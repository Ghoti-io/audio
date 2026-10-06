#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Audio.
#
# Ghoti.io Audio is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
"""The CELT and SILK tables, out of the document that defines them.

Usage:

    tools/tables/gen_opus_tables.py [--rfc PATH] [--url URL] [--check]

RFC 6716 is a different kind of specification from the others this
library reads, and the difference decides how this script works.

Section 6 says it plainly: "conformance is defined through the reference
implementation of the decoder provided in Appendix A", and "should the
description contradict the source code of the reference implementation,
the latter shall take precedence". The prose of section 4.3 describes
CELT thoroughly but names its tables rather than printing them - "the
e_prob_model table in quant_bands.c", "tf_select_table[][] in celt.c" -
so for a dozen arrays the normative text *is* that appendix. Appendix A
carries the whole reference implementation base64-encoded inside the RFC,
and A.1 publishes the SHA-1 of the archive it decodes to. So the tables
are extracted from the normative document, and the extraction is
verified against a hash the same document states.

That is the same arrangement gen_vorbis_tables.py has with the Vorbis
specification PDF, for the same reason: a table typed in by hand is a
table nobody can check, and these run to a thousand entries.

**What is computed instead of extracted.** Three of them have defining
formulas, and a formula that reproduces the reference's array is worth
far more than the array, because it is a second reading:

  - the band edges, whose bin counts per frame size are printed as
    Table 55 in the prose - so the prose and the appendix check each
    other, and this script fails if they disagree;
  - the 120-sample MDCT window, which is
    sin(pi/2 * sin(pi/2 * (n+1/2)/overlap)**2) in Q15;
  - logN, which is log2_frac of each band's width in eighths of a bit.

Each is computed here, compared against what the appendix holds, and
only then written out. A mismatch is a failure rather than a note: if
the formula and the table disagree, one of the two readings is wrong and
guessing which would be the whole mistake.

**SILK is the other way round, and that is worth exploiting.** Section
4.2 prints almost every table it uses - fifty of them, from the LBRR
flag distributions to the pulse-count splits - so the document holds two
independent copies of each, and they can be made to check each other.
That is what this script does with them: ninety-one tables are read once
out of section 4.2's ASCII tables and once out of Appendix A, converted
into a common form, and compared. A disagreement is a failure, because
one of the two readings would then be wrong and guessing which is the
whole mistake.

The conversion is where the content is. The prose prints probabilities
and the appendix stores inverse cumulative distributions; several tables
are also stored transposed, at a different scale, or with the
zero-probability symbols trimmed off the front. Each of those is a named
step rather than a tolerance, and three of them are not cosmetic:

  - the pitch contour codebooks are stored subframe-outermost and
    printed entry-per-row, so the comparison is against the transpose;
  - the four shell-code tables are numbered by partition size in the
    prose and by depth in the appendix, which puts Table 47 against
    `silk_shell_code_table3`;
  - the LSF cosine table is Q12 in the prose and Q13 in the appendix in
    spite of being called `silk_LSFCosTab_FIX_Q12`, and the two
    interpolation formulas differ by the matching shift - so what is
    checked is that they agree at all 32,768 inputs.

One thing in section 4.2 is simply wrong: Table 17 labels the row for
stage-1 index 6 with the letter "g". It is listed in `INDEX_ERRATA` so
that the index column stays a real check everywhere else.

`--check` regenerates into a temporary directory and compares, so a gate
can assert the committed tables are what this script produces.
"""

import argparse
import hashlib
import math
import os
import re
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT_DIR = os.path.join(ROOT, "src", "codec", "opus")

DEFAULT_URL = "https://www.rfc-editor.org/rfc/rfc6716.txt"

# RFC 6716 Appendix A.1 states this for the archive its base64 decodes to.
# It is the document's own claim about its own contents, which is what
# makes extracting from the appendix a verifiable step rather than a
# download nobody checked.
ARCHIVE_SHA1 = "86a927223e73d2476646a1b933fcd3fffb6ecc8c"

# Table 55 of the prose: MDCT bins per channel per band, for each of the
# four frame sizes. Transcribed from the document by hand - it is the
# only table in section 4.3 that is printed rather than named, and it
# exists here to contradict the appendix if the appendix is misread.
TABLE_55 = [
    #  2.5ms 5ms 10ms 20ms
    (1, 2, 4, 8),
    (1, 2, 4, 8),
    (1, 2, 4, 8),
    (1, 2, 4, 8),
    (1, 2, 4, 8),
    (1, 2, 4, 8),
    (1, 2, 4, 8),
    (1, 2, 4, 8),
    (2, 4, 8, 16),
    (2, 4, 8, 16),
    (2, 4, 8, 16),
    (2, 4, 8, 16),
    (4, 8, 16, 32),
    (4, 8, 16, 32),
    (4, 8, 16, 32),
    (6, 12, 24, 48),
    (6, 12, 24, 48),
    (8, 16, 32, 64),
    (12, 24, 48, 96),
    (18, 36, 72, 144),
    (22, 44, 88, 176),
]

# The start and stop frequency of each band, also from Table 55. Used to
# check the band edges against the sample rate rather than only against
# each other: band 20 must stop at 20 kHz in a 48 kHz mode.
TABLE_55_HZ = [
    (0, 200), (200, 400), (400, 600), (600, 800), (800, 1000),
    (1000, 1200), (1200, 1400), (1400, 1600), (1600, 2000),
    (2000, 2400), (2400, 2800), (2800, 3200), (3200, 4000),
    (4000, 4800), (4800, 5600), (5600, 6800), (6800, 8000),
    (8000, 9600), (9600, 12000), (12000, 15600), (15600, 20000),
]

LICENSE = """/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Audio.
 *
 * Ghoti.io Audio is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Audio is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
"""

GENERATED = """
/*
 * GENERATED by tools/tables/gen_opus_tables.py from RFC 6716. Do not edit.
 *
 * Every array here is either extracted from the reference implementation
 * in Appendix A - which section 6 makes normative, and whose archive the
 * RFC publishes a SHA-1 for - or computed from a defining formula and
 * checked against that reference. The generator says which is which, and
 * fails rather than choosing when the two disagree.
 */
"""


def fetch_rfc(path, url):
    """The RFC's text, from a local copy or from the editor's site."""
    if path and os.path.exists(path):
        with open(path, "rb") as handle:
            return handle.read()
    print(f"gen_opus_tables: fetching {url}")
    with urllib.request.urlopen(url, timeout=300) as source:
        payload = source.read()
    if path:
        with open(path, "wb") as handle:
            handle.write(payload)
    return payload


def reference_source(rfc_text, work):
    """Appendix A's archive, unpacked, after checking the RFC's own hash.

    Appendix A.1 gives the extraction as a shell pipeline over lines
    beginning with three spaces and '###'. That is followed literally
    here rather than reimplemented: the lines are the encoding, and a
    cleverer reading of them would be a second thing to get wrong.
    """
    lines = []
    for line in rfc_text.decode("ascii", "replace").splitlines():
        if line.startswith("   ###"):
            lines.append(line[6:])
    if not lines:
        raise SystemExit(
            "gen_opus_tables: the RFC text carries no '###' lines, so it "
            "is not the full document - Appendix A is where the tables "
            "are, and a copy without it cannot produce them.")
    import base64
    archive = base64.b64decode("".join(lines))
    got = hashlib.sha1(archive).hexdigest()
    if got != ARCHIVE_SHA1:
        raise SystemExit(
            "gen_opus_tables: Appendix A does not decode to the archive "
            f"the RFC says it does.\n  expected {ARCHIVE_SHA1}\n"
            f"  got      {got}")
    print(f"gen_opus_tables: Appendix A decodes to {got}, as A.1 states")
    tar_path = os.path.join(work, "opus.tar.gz")
    with open(tar_path, "wb") as handle:
        handle.write(archive)
    with tarfile.open(tar_path, "r:gz") as tar:
        for member in tar.getmembers():
            if not member.isfile():
                continue
            name = member.name
            if ".." in name or name.startswith("/"):
                continue
            target = os.path.join(work, name)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            extracted = tar.extractfile(member)
            if extracted is not None:
                with open(target, "wb") as handle:
                    handle.write(extracted.read())
    root = os.path.join(work, "opus-rfc6716")
    if not os.path.isdir(root):
        raise SystemExit("gen_opus_tables: the archive has no opus-rfc6716/")
    return root


def read_array(root, relative, name):
    """One C array's integers, by declared name.

    Deliberately a narrow reader: it finds the declaration, takes the
    text to the matching close brace, strips comments, and reads the
    integers. It does not evaluate expressions, so a table written as
    arithmetic would be refused rather than guessed at.
    """
    with open(os.path.join(root, relative), "r", encoding="utf-8",
              errors="replace") as handle:
        text = handle.read()
    match = re.search(
        r"\b" + re.escape(name) + r"\s*(\[[^\]]*\]\s*)+=\s*\{", text)
    if not match:
        raise SystemExit(
            f"gen_opus_tables: {relative} has no array called {name}. "
            "The reference's layout has changed and this script is "
            "reading the wrong thing.")
    start = match.end() - 1
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                body = text[start:index + 1]
                break
    else:
        raise SystemExit(f"gen_opus_tables: {name} has no closing brace")
    body = re.sub(r"/\*.*?\*/", " ", body, flags=re.S)
    body = re.sub(r"//[^\n]*", " ", body)
    if re.search(r"[A-Za-z_][A-Za-z_0-9]*", body):
        leftover = re.findall(r"[A-Za-z_][A-Za-z_0-9]*", body)
        raise SystemExit(
            f"gen_opus_tables: {name} is not a plain list of integers; it "
            f"mentions {sorted(set(leftover))}. Reading it as one would be "
            "a guess.")
    values = [int(piece) for piece in re.findall(r"-?\d+", body)]
    if not values:
        raise SystemExit(f"gen_opus_tables: {name} came out empty")
    return values


def log2_frac(value, frac):
    """The reference's integer log2, in units of 1/(1<<frac) of a bit.

    celt/cwrs.c's log2_frac, reproduced step for step. It is documented
    there as "guaranteed to return a conservatively large estimate", and
    the conservatism is the point: an exact logarithm does not give the
    same answers. Band 19 is 18 bins wide, whose true log2 is 33.36
    eighths of a bit and whose tabulated value is 34, so rounding the
    real logarithm would disagree with the format - the kind of near
    miss that decodes almost correctly.

    The first draft of this function was written from memory and got
    band 8 wrong, which is why it is now a transcription with the
    source's own loop structure, `do {} while (frac-- > 0)` included.
    """
    length = value.bit_length()  # EC_ILOG
    if value & (value - 1):
        if length > 16:
            shift = length - 16
            value = (value >> shift) + (
                ((value & ((1 << shift) - 1)) + (1 << shift) - 1) >> shift)
        else:
            value <<= 16 - length
        total = (length - 1) << frac
        # One iteration always runs: the rounding up above can change the
        # integer part of the logarithm, so there is always an adjustment
        # to make.
        here = frac
        while True:
            bit = value >> 16
            total += bit << here
            value = (value + bit) >> bit
            value = (value * value + 0x7FFF) >> 15
            more = here > 0
            here -= 1
            if not more:
                break
        # Anything but exactly 0x8000 left over rounds up.
        return total + (1 if value > 0x8000 else 0)
    # Exact powers of two need no rounding at all.
    return (length - 1) << frac



def table_57(rfc_text):
    """Table 57 of the prose: the static allocation, band by band.

    The prose prints the same 231 numbers the appendix holds, transposed
    - rows are bands and columns are the eleven quality steps, where the
    array in modes.c is quality-major. So this is a second reading of
    the most important table in the allocator, and it is parsed rather
    than transcribed because 231 numbers typed by hand is 231 chances to
    introduce the error this check exists to find.

    The table is found by its caption, read backwards, and its shape is
    asserted: a parse that silently picked up the wrong block of pipe
    characters would otherwise look like a disagreement in the data.
    """
    lines = rfc_text.decode("ascii", "replace").splitlines()
    caption = None
    for index, line in enumerate(lines):
        if "Table 57: CELT Static Allocation Table" in line:
            caption = index
            break
    if caption is None:
        raise SystemExit(
            "gen_opus_tables: the RFC text has no Table 57 caption, so the "
            "prose cannot be used to check the allocation table.")
    # The table spans a page break, so the scan cannot stop at the first
    # line that is not a row: it has to step over the RFC's running
    # header and footer. It collects 22 rows of eleven numbers - the
    # column headings and 21 bands - and stops when it has them.
    rows = []
    for line in reversed(lines[:caption]):
        stripped = line.strip()
        if not stripped.startswith("|"):
            continue
        numbers = [int(v) for v in re.findall(r"-?\d+", stripped)]
        if len(numbers) == 11:
            rows.append(numbers)
            if len(rows) == 22:
                break
    rows.reverse()
    # The first such row is the column heading 0..10, which is not data.
    if not rows or rows[0] != list(range(11)):
        raise SystemExit(
            "gen_opus_tables: Table 57 does not begin with its column "
            f"headings; got {rows[0] if rows else None}. The parse has "
            "found the wrong block.")
    rows = rows[1:]
    if len(rows) != 21:
        raise SystemExit(
            f"gen_opus_tables: Table 57 parsed as {len(rows)} bands, not 21")
    return rows


# --- the prose's own copy of the SILK tables ------------------------
#
# Section 4.2 is not written the way section 4.3 is. CELT's chapter
# names its tables and leaves them in the appendix; SILK's chapter
# *prints* almost every one of its own - fifty tables, from the LBRR
# flag PDFs to the pulse-count splits - so for SILK the document holds
# two independent copies of each table, and they can be made to check
# each other. That is worth more than either alone: a table extracted
# from the appendix is only as good as the extraction, and a table
# typed in from the prose is only as good as the typing.
#
# The prose prints probabilities and the appendix stores inverse
# cumulative distributions, so the comparison is through
# `pdf_to_icdf()` rather than literal. Several tables are also stored
# in a different order, a different scaling, or a different shape from
# the way they are printed; each of those is a named conversion below
# rather than a tolerance.


def is_box_line(line):
    """Whether a line is part of an ASCII table."""
    stripped = line.strip()
    return stripped.startswith("|") or stripped.startswith("+-")


def close_page_breaks(block):
    """The lines of `block` with the RFC's page furniture removed.

    A table longer than a page is interrupted by a form feed, a footer,
    a header, and blank lines either side. Dropping the two text lines
    leaves a run of blanks between two table lines, and dropping that
    run as well makes the table contiguous again. A blank run is only
    removed when a table line sits on both sides of it, so two separate
    tables - which always have a caption between them - never merge.
    """
    lines = []
    for line in block.replace("\x0c", "").split("\n"):
        stripped = line.strip()
        if stripped.startswith("Valin, et al.") or stripped.startswith(
                "RFC 6716 "):
            continue
        lines.append(line)
    out = []
    index = 0
    while index < len(lines):
        if not lines[index].strip():
            end = index
            while end < len(lines) and not lines[end].strip():
                end += 1
            previous = out[-1] if out else ""
            following = lines[end] if end < len(lines) else ""
            if is_box_line(previous) and is_box_line(following):
                index = end
                continue
            out.extend(lines[index:end])
            index = end
            continue
        out.append(lines[index])
        index += 1
    return out


def rfc_captions(rfc_text):
    """Where each `Table N:` caption begins and ends."""
    captions = {}
    for match in re.finditer(r"^ +Table (\d+):", rfc_text, re.M):
        captions[int(match.group(1))] = (match.start(), match.end())
    if 53 not in captions:
        raise SystemExit(
            "gen_opus_tables: the RFC text has no Table 53, so it is not "
            "the full document and the SILK tables cannot be checked "
            "against the prose.")
    return captions


# RFC 6716 misprints one index in Table 17: the row for stage-1 index 6
# is labelled "g", a letter out of the table's own body. Every other row
# of every other table carries the index it should, so this is listed
# here and the index check stays strict everywhere else.
INDEX_ERRATA = {(17, 6): "g"}


class Prose:
    """The tables the prose prints, read back out of the document."""

    def __init__(self, rfc_text):
        self.text = rfc_text
        self.captions = rfc_captions(rfc_text)

    def box(self, number):
        """The ASCII table immediately above `Table N:`."""
        previous = self.captions.get(number - 1)
        start = previous[1] if previous else 0
        lines = close_page_breaks(self.text[start:self.captions[number][0]])
        out = []
        for line in reversed(lines):
            if is_box_line(line):
                out.append(line)
            elif out:
                break
        if not out:
            raise SystemExit(f"gen_opus_tables: Table {number} has no box")
        return list(reversed(out))

    def rows(self, number, by_index):
        """The table's logical rows, as lists of cell strings.

        A logical row can span several physical lines. Which lines
        belong together is decided one of two ways: `by_index` starts a
        row wherever the first cell is non-empty, which is right for the
        tables indexed 0, 1, 2, ...; otherwise a row runs until a line
        whose cells are all blank, which is right for the tables whose
        row label itself wraps. The first is needed because a page break
        can fall exactly where a blank separator line would have been,
        and then the separator is simply not there - as happens in
        Table 41 between index 3 and index 4.
        """
        logical = []
        current = None
        for line in self.box(number):
            stripped = line.strip()
            if stripped.startswith("+"):
                if current:
                    logical.append(current)
                current = None
                continue
            cells = [cell.strip() for cell in stripped.strip("|").split("|")]
            if not any(cells):
                if current:
                    logical.append(current)
                current = None
                continue
            if current is None:
                current = cells
            elif by_index and cells[0]:
                logical.append(current)
                current = cells
            else:
                current = [(a + " " + b).strip()
                           for a, b in zip(current, cells)]
        if current:
            logical.append(current)
        return logical

    def indexed(self, number, skip):
        """Rows after `skip` header rows, with their index checked.

        The index column is the one thing in these tables that is
        redundant, so it is the one thing that can catch a row lost to a
        page break or a mis-joined continuation.
        """
        rows = self.rows(number, True)[skip:]
        for position, row in enumerate(rows):
            if row[0] != str(position) and INDEX_ERRATA.get(
                    (number, position)) != row[0]:
                raise SystemExit(
                    f"gen_opus_tables: Table {number} row {position} is "
                    f"indexed {row[0]!r}. The rows have been misread.")
        return rows

    def pdfs(self, number):
        """Every `{...}/256` the table prints, in order."""
        body = " ".join(" | ".join(row) for row in self.rows(number, False))
        out = []
        for group in re.findall(r"\{([^}]*)\}/256", body):
            values = [int(piece) for piece in group.split(",")]
            if sum(values) != 256:
                raise SystemExit(
                    f"gen_opus_tables: a PDF in Table {number} sums to "
                    f"{sum(values)}, not 256. A distribution that does not "
                    "tile the range is a value the encoder can write and "
                    "no decoder can read.")
            out.append(values)
        if not out:
            raise SystemExit(f"gen_opus_tables: Table {number} has no PDF")
        return out


def pdf_to_icdf(pdf):
    """The reference's spelling of a distribution the prose prints.

    `ec_dec_icdf` reads 256 minus the running sum, with the leading 256
    left implicit. Two consequences show up in the tables: a symbol the
    prose gives zero probability is simply absent from the front of the
    reference's array - the caller adds the offset back - and nothing is
    stored past the first zero, because the distribution is exhausted
    there.
    """
    cumulative = 0
    full = []
    for probability in pdf:
        cumulative += probability
        full.append(256 - cumulative)
    lead = 0
    while lead < len(pdf) and pdf[lead] == 0:
        lead += 1
    trimmed = full[lead:]
    for position, value in enumerate(trimmed):
        if value == 0:
            return trimmed[:position + 1]
    raise SystemExit("gen_opus_tables: an iCDF never reaches zero")


def silk_define(root, name):
    """One integer `#define` out of silk/define.h."""
    with open(os.path.join(root, "silk", "define.h"), encoding="utf-8",
              errors="replace") as handle:
        source = handle.read()
    match = re.search(r"^#define\s+" + re.escape(name) + r"\s+(-?\d+)\s*$",
                      source, re.M)
    if not match:
        raise SystemExit(f"gen_opus_tables: silk/define.h has no {name}")
    return int(match.group(1))


class SilkCheck:
    """Appendix A's SILK tables, each held against the prose's copy."""

    OTHER = "silk/tables_other.c"
    GAIN = "silk/tables_gain.c"
    LAG = "silk/tables_pitch_lag.c"
    LTP = "silk/tables_LTP.c"
    PULSES = "silk/tables_pulses_per_block.c"
    NB_MB = "silk/tables_NLSF_CB_NB_MB.c"
    WB = "silk/tables_NLSF_CB_WB.c"
    PITCH = "silk/pitch_est_tables.c"
    COS = "silk/table_LSF_cos.c"

    def __init__(self, root, rfc_text):
        self.root = root
        self.prose = Prose(rfc_text)
        self.tables = {}
        self.checked = 0

    def array(self, relative, name):
        return read_array(self.root, relative, name)

    def agree(self, label, mine, theirs, source):
        """Fail unless the two readings of one table are identical."""
        if list(mine) != list(theirs):
            raise SystemExit(
                f"gen_opus_tables: {label} reads differently in the two "
                f"places RFC 6716 states it.\n  {source}: {list(mine)}\n"
                f"  Appendix A: {list(theirs)}\nOne of the two is being "
                "misread; do not pick one.")
        self.checked += 1

    def emit(self, name, kind, values):
        self.tables["silk_" + name] = (kind, list(values))

    def take(self, name, kind, relative, reference):
        """Extract an array and emit it under our own name."""
        values = self.array(relative, reference)
        self.emit(name, kind, values)
        return values

    # -- section 4.2.3 to 4.2.7.3: the frame header ------------------

    def header(self):
        prose = self.prose
        lbrr = prose.pdfs(4)
        for frames, suffix in ((2, "2"), (3, "3")):
            name = f"silk_LBRR_flags_{suffix}_iCDF"
            values = self.take(f"lbrr_flags_{suffix}_icdf", "uint8_t",
                               self.OTHER, name)
            self.agree(name, pdf_to_icdf(lbrr[frames - 2]), values,
                       f"Table 4, {20 * frames} ms")

        stereo = prose.pdfs(6)
        joint = self.take("stereo_pred_joint_icdf", "uint8_t", self.OTHER,
                          "silk_stereo_pred_joint_iCDF")
        self.agree("silk_stereo_pred_joint_iCDF", pdf_to_icdf(stereo[0]),
                   joint, "Table 6 stage 1")
        # The stage-2 and stage-3 stereo PDFs are the uniform tables the
        # reference shares with several other fields, so this is also
        # where two of those get checked.
        for stage, size in ((1, 3), (2, 5)):
            self.agree(f"silk_uniform{size}_iCDF", pdf_to_icdf(stereo[stage]),
                       self.array(self.OTHER, f"silk_uniform{size}_iCDF"),
                       f"Table 6 stage {stage + 1}")
        for size in (3, 4, 5, 6, 8):
            self.take(f"uniform{size}_icdf", "uint8_t", self.OTHER,
                      f"silk_uniform{size}_iCDF")

        weights = self.take("stereo_pred_quant_q13", "int16_t", self.OTHER,
                            "silk_stereo_pred_quant_Q13")
        self.agree("silk_stereo_pred_quant_Q13",
                   [int(row[1]) for row in prose.indexed(7, 1)], weights,
                   "Table 7")
        mid = self.take("stereo_only_code_mid_icdf", "uint8_t", self.OTHER,
                        "silk_stereo_only_code_mid_iCDF")
        self.agree("silk_stereo_only_code_mid_iCDF",
                   pdf_to_icdf(prose.pdfs(8)[0]), mid, "Table 8")

        # Table 9 pads both frame-type distributions out to six symbols
        # so that they can be printed side by side; the reference drops
        # the impossible ones off the front and the caller adds the
        # offset back. pdf_to_icdf does the same trimming.
        frame_type = prose.pdfs(9)
        for index, (name, label) in enumerate((
                ("silk_type_offset_no_VAD_iCDF", "inactive"),
                ("silk_type_offset_VAD_iCDF", "active"))):
            values = self.take(name.lower()[5:], "uint8_t", self.OTHER, name)
            self.agree(name, pdf_to_icdf(frame_type[index]), values,
                       f"Table 9, {label}")

        gains = self.take("gain_icdf", "uint8_t", self.GAIN, "silk_gain_iCDF")
        for index, label in enumerate(("inactive", "unvoiced", "voiced")):
            self.agree(f"silk_gain_iCDF[{label}]",
                       pdf_to_icdf(prose.pdfs(11)[index]),
                       gains[8 * index:8 * index + 8], f"Table 11, {label}")
        self.agree("silk_uniform8_iCDF", pdf_to_icdf(prose.pdfs(12)[0]),
                   self.array(self.OTHER, "silk_uniform8_iCDF"), "Table 12")
        delta = self.take("delta_gain_icdf", "uint8_t", self.GAIN,
                          "silk_delta_gain_iCDF")
        self.agree("silk_delta_gain_iCDF", pdf_to_icdf(prose.pdfs(13)[0]),
                   delta, "Table 13")

    # -- section 4.2.7.5: the normalized LSFs ------------------------

    def nlsf(self):
        prose = self.prose
        stage1 = prose.pdfs(14)
        for half, (source, name) in enumerate(((self.NB_MB,
                                                "silk_NLSF_CB1_iCDF_NB_MB"),
                                               (self.WB,
                                                "silk_NLSF_CB1_iCDF_WB"))):
            values = self.take(name.lower()[5:], "uint8_t", source, name)
            for voiced in (0, 1):
                self.agree(f"{name}[{voiced}]",
                           pdf_to_icdf(stage1[2 * half + voiced]),
                           values[32 * voiced:32 * voiced + 32],
                           f"Table 14 row {2 * half + voiced}")

        for table, source, name in ((15, self.NB_MB, "silk_NLSF_CB2_iCDF_NB_MB"),
                                    (16, self.WB, "silk_NLSF_CB2_iCDF_WB")):
            values = self.take(name.lower()[5:], "uint8_t", source, name)
            for letter, pdf in enumerate(prose.pdfs(table)):
                self.agree(f"{name}[{letter}]", pdf_to_icdf(pdf),
                           values[9 * letter:9 * letter + 9],
                           f"Table {table} row {letter}")

        # The reference packs two things into each byte of the select
        # table, and the prose prints them as two separate tables: which
        # of the eight stage-2 distributions a coefficient uses (as a
        # letter), and which of the two prediction weight lists it reads
        # (as a letter again, in a different alphabet). Reassembling
        # them is what checks both at once.
        for ec_table, weight_table, source, select, order, first in (
                (17, 21, self.NB_MB, "silk_NLSF_CB2_SELECT_NB_MB", 10, "a"),
                (18, 22, self.WB, "silk_NLSF_CB2_SELECT_WB", 16, "i")):
            distributions = self.letters(ec_table, order, first, 8)
            # The prose prints one fewer column here than there are
            # coefficients, and that is not an omission: the reference
            # indexes pred_Q8[i + sel*(order-1) + 1], so a set bit on
            # the last coefficient would read one past the end of an
            # array that is exactly 2*(order-1) long. The last
            # coefficient's selector cannot be anything but zero.
            selectors = self.letters(weight_table, order - 1,
                                     "A" if order == 10 else "C", 2)
            packed = []
            for row_ec, row_sel in zip(distributions, selectors):
                row_sel = row_sel + [0]
                for index in range(0, order, 2):
                    packed.append((row_ec[index] << 1) | row_sel[index]
                                  | (row_ec[index + 1] << 5)
                                  | (row_sel[index + 1] << 4))
            values = self.take(select.lower()[5:], "uint8_t", source, select)
            self.agree(select, packed, values,
                       f"Tables {ec_table} and {weight_table}, packed")
            if any(byte & 0x10 for byte in values[order // 2 - 1::order // 2]):
                raise SystemExit(
                    f"gen_opus_tables: {select} sets the prediction weight "
                    "bit on a last coefficient, which would read past the "
                    "end of the weight list.")

        weights = [self.column(20, column) for column in (1, 2, 3, 4)]
        for low, high, source, name in ((0, 1, self.NB_MB,
                                         "silk_NLSF_PRED_NB_MB_Q8"),
                                        (2, 3, self.WB,
                                         "silk_NLSF_PRED_WB_Q8")):
            values = self.take(name.lower()[5:], "uint8_t", source, name)
            self.agree(name, weights[low] + weights[high], values,
                       f"Table 20 columns {low + 1} and {high + 1}")

        for table, source, name, order in (
                (23, self.NB_MB, "silk_NLSF_CB1_NB_MB_Q8", 10),
                (24, self.WB, "silk_NLSF_CB1_WB_Q8", 16)):
            flat = []
            for position, row in enumerate(prose.indexed(table, 2)):
                vector = [int(piece) for piece in row[1].split()]
                if len(vector) != order:
                    raise SystemExit(
                        f"gen_opus_tables: Table {table} row {position} has "
                        f"{len(vector)} coefficients, not {order}")
                flat += vector
            values = self.take(name.lower()[5:], "uint8_t", source, name)
            self.agree(name, flat, values, f"Table {table}")

        for column, source, name in ((1, self.NB_MB,
                                      "silk_NLSF_DELTA_MIN_NB_MB_Q15"),
                                     (2, self.WB,
                                      "silk_NLSF_DELTA_MIN_WB_Q15")):
            values = self.take(name.lower()[5:], "int16_t", source, name)
            self.agree(name, self.column(25, column), values,
                       f"Table 25 column {column}")

        extension = self.take("nlsf_ext_icdf", "uint8_t", self.OTHER,
                              "silk_NLSF_EXT_iCDF")
        self.agree("silk_NLSF_EXT_iCDF", pdf_to_icdf(prose.pdfs(19)[0]),
                   extension, "Table 19")
        interpolation = self.take("nlsf_interpolation_factor_icdf", "uint8_t",
                                  self.OTHER,
                                  "silk_NLSF_interpolation_factor_iCDF")
        self.agree("silk_NLSF_interpolation_factor_iCDF",
                   pdf_to_icdf(prose.pdfs(26)[0]), interpolation, "Table 26")

    def letters(self, table, width, base, span):
        """A grid of single letters, as indices into its own alphabet.

        The prose names these choices with letters rather than numbers,
        and uses a different run of the alphabet for each table so that
        a NB/MB distribution can never be confused with a WB one.
        """
        rows = []
        for position, row in enumerate(self.prose.indexed(table, 2)):
            cells = row[1].split()
            if len(cells) != width or any(
                    len(cell) != 1 or not 0 <= ord(cell) - ord(base) < span
                    for cell in cells):
                raise SystemExit(
                    f"gen_opus_tables: Table {table} row {position} is "
                    f"{cells}, which is not {width} letters from "
                    f"{base!r} to {chr(ord(base) + span - 1)!r}")
            rows.append(cells)
        return [[ord(cell) - ord(base) for cell in row] for row in rows]

    def column(self, table, column, skip=1):
        """One numeric column, skipping the rows that leave it blank."""
        out = []
        for row in self.prose.indexed(table, skip):
            if row[column]:
                out.append(int(row[column]))
        return out

    # -- section 4.2.7.5.6: the LSFs become LPC coefficients ---------

    def lsf_to_lpc(self):
        # Table 27's ordering is not in a table file at all; it is two
        # local arrays inside silk_NLSF2A().
        with open(os.path.join(self.root, "silk", "NLSF2A.c"),
                  encoding="utf-8", errors="replace") as handle:
            source = handle.read()
        for column, order in ((1, 10), (2, 16)):
            match = re.search(
                r"ordering%d\s*\[\s*%d\s*\]\s*=\s*\{([^}]*)\}" % (order, order),
                source)
            if not match:
                raise SystemExit(
                    f"gen_opus_tables: silk/NLSF2A.c has no ordering{order}")
            values = [int(piece) for piece in re.findall(r"\d+",
                                                         match.group(1))]
            self.agree(f"ordering{order}", self.column(27, column), values,
                       f"Table 27 column {column}")
            self.emit(f"nlsf_ordering{order}", "uint8_t", values)

        # The reference calls this table Q12 and stores it in Q13; the
        # prose prints the Q12 values its own formula wants. Neither is
        # wrong, because the two interpolation formulas differ by the
        # same bit - so what gets checked is that they agree at every
        # one of the 32,768 inputs, not that the tables match.
        cos13 = self.take("lsf_cos_q13", "int16_t", self.COS,
                          "silk_LSFCosTab_FIX_Q12")
        cos12 = []
        for position, row in enumerate(self.prose.rows(28, True)[1:]):
            if row[0] != str(4 * position):
                raise SystemExit(
                    f"gen_opus_tables: Table 28 row {position} is indexed "
                    f"{row[0]!r}, not {4 * position}")
            cos12 += [int(cell) for cell in row[1:] if cell]
        if len(cos12) != len(cos13):
            raise SystemExit(
                f"gen_opus_tables: Table 28 has {len(cos12)} cosines and "
                f"silk_LSFCosTab_FIX_Q12 has {len(cos13)}")
        for index in range(len(cos13) - 1):
            for fraction in range(256):
                prose = (cos12[index] * 256
                         + (cos12[index + 1] - cos12[index]) * fraction
                         + 4) >> 3
                appendix = ((cos13[index] << 8)
                            + (cos13[index + 1] - cos13[index]) * fraction
                            + 8) >> 4
                if prose != appendix:
                    raise SystemExit(
                        "gen_opus_tables: the prose's Q12 cosine table and "
                        f"Appendix A's Q13 one disagree at index {index}, "
                        f"fraction {fraction}: {prose} against {appendix}")
        self.checked += 1

    # -- section 4.2.7.6: the long-term predictor -------------------

    def ltp(self):
        prose = self.prose
        lag = self.take("pitch_lag_icdf", "uint8_t", self.LAG,
                        "silk_pitch_lag_iCDF")
        self.agree("silk_pitch_lag_iCDF", pdf_to_icdf(prose.pdfs(29)[0]), lag,
                   "Table 29")
        for index, size in enumerate((4, 6, 8)):
            self.agree(f"silk_uniform{size}_iCDF",
                       pdf_to_icdf(prose.pdfs(30)[index]),
                       self.array(self.OTHER, f"silk_uniform{size}_iCDF"),
                       f"Table 30 row {index}")
        delta = self.take("pitch_delta_icdf", "uint8_t", self.LAG,
                          "silk_pitch_delta_iCDF")
        self.agree("silk_pitch_delta_iCDF", pdf_to_icdf(prose.pdfs(31)[0]),
                   delta, "Table 31")
        contours = prose.pdfs(32)
        for index, name in enumerate(("silk_pitch_contour_10_ms_NB_iCDF",
                                      "silk_pitch_contour_NB_iCDF",
                                      "silk_pitch_contour_10_ms_iCDF",
                                      "silk_pitch_contour_iCDF")):
            values = self.take(name.lower()[5:], "uint8_t", self.LAG, name)
            self.agree(name, pdf_to_icdf(contours[index]), values,
                       f"Table 32 row {index}")

        # The reference stores these with the subframe as the outer
        # index and the codebook entry as the inner one; the prose
        # prints one codebook entry per row. So the comparison is
        # against the transpose, and reading it the other way round
        # would be a decoder that moves the right offsets to the wrong
        # subframes.
        for table, name, subframes in (
                (33, "silk_CB_lags_stage2_10_ms", 2),
                (34, "silk_CB_lags_stage2", 4),
                (35, "silk_CB_lags_stage3_10_ms", 2),
                (36, "silk_CB_lags_stage3", 4)):
            entries = []
            for position, row in enumerate(prose.indexed(table, 1)):
                offsets = [int(piece) for piece in row[1].split()]
                if len(offsets) != subframes:
                    raise SystemExit(
                        f"gen_opus_tables: Table {table} row {position} has "
                        f"{len(offsets)} offsets, not {subframes}")
                entries.append(offsets)
            transposed = [entry[subframe] for subframe in range(subframes)
                          for entry in entries]
            values = self.take(name.lower()[5:], "int8_t", self.PITCH, name)
            self.agree(name, transposed, values, f"Table {table}, transposed")

        periodicity = self.take("ltp_per_index_icdf", "uint8_t", self.LTP,
                                "silk_LTP_per_index_iCDF")
        self.agree("silk_LTP_per_index_iCDF", pdf_to_icdf(prose.pdfs(37)[0]),
                   periodicity, "Table 37")
        sizes = []
        for index, pdf in enumerate(prose.pdfs(38)):
            name = f"silk_LTP_gain_iCDF_{index}"
            values = self.take(name.lower()[5:], "uint8_t", self.LTP, name)
            self.agree(name, pdf_to_icdf(pdf), values,
                       f"Table 38 row {index}")
            sizes.append(len(values))
        for table, index in ((39, 0), (40, 1), (41, 2)):
            flat = []
            for position, row in enumerate(prose.indexed(table, 1)):
                taps = [int(piece) for piece in row[1].split()]
                if len(taps) != 5:
                    raise SystemExit(
                        f"gen_opus_tables: Table {table} row {position} has "
                        f"{len(taps)} taps, not 5")
                flat += taps
            name = f"silk_LTP_gain_vq_{index}"
            values = self.take(name.lower()[5:], "int8_t", self.LTP, name)
            self.agree(name, flat, values, f"Table {table}")
        self.agree("silk_LTP_vq_sizes", sizes,
                   self.array(self.LTP, "silk_LTP_vq_sizes"),
                   "the lengths of the three distributions in Table 38")

        scaling = self.take("ltpscale_icdf", "uint8_t", self.OTHER,
                            "silk_LTPscale_iCDF")
        self.agree("silk_LTPscale_iCDF", pdf_to_icdf(prose.pdfs(42)[0]),
                   scaling, "Table 42")
        # Section 4.2.7.6.3 prints the three scale factors in a
        # sentence rather than a table.
        match = re.search(r"Q14 scale factors of\s+([\d,\s]+?),?\s+and\s+"
                          r"(\d+)", self.prose.text)
        if not match:
            raise SystemExit(
                "gen_opus_tables: section 4.2.7.6.3 no longer names the LTP "
                "scale factors in the sentence this reads them from")
        printed = [int(piece) for piece in re.findall(r"\d+", match.group(1))]
        printed.append(int(match.group(2)))
        factors = self.take("ltp_scales_q14", "int16_t", self.OTHER,
                            "silk_LTPScales_table_Q14")
        self.agree("silk_LTPScales_table_Q14", printed, factors,
                   "section 4.2.7.6.3")
        self.agree("silk_uniform4_iCDF", pdf_to_icdf(prose.pdfs(43)[0]),
                   self.array(self.OTHER, "silk_uniform4_iCDF"), "Table 43")

    # -- section 4.2.7.8: the excitation ----------------------------

    def excitation(self):
        prose = self.prose
        levels = self.take("rate_levels_icdf", "uint8_t", self.PULSES,
                           "silk_rate_levels_iCDF")
        for index, label in enumerate(("inactive or unvoiced", "voiced")):
            self.agree(f"silk_rate_levels_iCDF[{label}]",
                       pdf_to_icdf(prose.pdfs(45)[index]),
                       levels[9 * index:9 * index + 9], f"Table 45, {label}")

        counts = prose.pdfs(46)
        per_block = self.take("pulses_per_block_icdf", "uint8_t", self.PULSES,
                              "silk_pulses_per_block_iCDF")
        stored = len(per_block) // 18
        for level in range(stored):
            self.agree(f"silk_pulses_per_block_iCDF[{level}]",
                       pdf_to_icdf(counts[level]),
                       per_block[18 * level:18 * level + 18],
                       f"Table 46 rate level {level}")
        # The prose prints one more distribution than the reference
        # stores, and says why: rate level 10 "is just a shifted version
        # of that for 9 and thus does not require any additional
        # storage". That is a claim about the numbers, so it is checked
        # rather than believed - and it is what caps the number of extra
        # LSBs at ten, because reading a 17 has probability zero there.
        if len(counts) != stored + 1:
            raise SystemExit(
                f"gen_opus_tables: Table 46 prints {len(counts)} rate "
                f"levels and Appendix A stores {stored}")
        self.agree("silk_pulses_per_block_iCDF[10]",
                   pdf_to_icdf(counts[stored]),
                   per_block[18 * (stored - 1) + 1:18 * stored],
                   "Table 46 rate level 9, shifted by one")

        # The prose numbers the split tables by the size of the
        # partition being split, largest first; the reference numbers
        # them by depth, finest first. Lining them up the other way
        # round would put every split's distribution one level out.
        offsets = self.take("shell_code_table_offsets", "uint8_t",
                            self.PULSES, "silk_shell_code_table_offsets")
        for table, index in ((47, 3), (48, 2), (49, 1), (50, 0)):
            built = []
            for pulses, pdf in enumerate(prose.pdfs(table), start=1):
                if offsets[pulses] != len(built):
                    raise SystemExit(
                        f"gen_opus_tables: Table {table} puts the "
                        f"distribution for {pulses} pulses at "
                        f"{len(built)} and silk_shell_code_table_offsets "
                        f"says {offsets[pulses]}")
                built += pdf_to_icdf(pdf)
            name = f"silk_shell_code_table{index}"
            values = self.take(name.lower()[5:], "uint8_t", self.PULSES, name)
            self.agree(name, built, values,
                       f"Table {table}, {2 ** (index + 1)}-sample partitions")

        lsb = self.take("lsb_icdf", "uint8_t", self.OTHER, "silk_lsb_iCDF")
        self.agree("silk_lsb_iCDF", pdf_to_icdf(prose.pdfs(51)[0]), lsb,
                   "Table 51")
        # Each sign is one binary decision and the reference keeps only
        # the first entry of each two-entry distribution;
        # silk_encode_signs builds the pair on the stack with the zero
        # after it.
        signs = self.take("sign_icdf", "uint8_t", self.PULSES,
                          "silk_sign_iCDF")
        self.agree("silk_sign_iCDF",
                   [pdf_to_icdf(pdf)[0] for pdf in prose.pdfs(52)], signs,
                   "Table 52, the first entry of each")

        # The prose prints these offsets on a scale six bits below the
        # one the reference holds them on and four below the one it
        # uses them on, so the two differ by a factor of four. The
        # emitted table is the reference's Q10.
        rows = prose.rows(53, True)[1:]
        printed = [int(row[2]) for row in rows]
        if printed[0] != printed[2] or printed[1] != printed[3]:
            raise SystemExit(
                "gen_opus_tables: Table 53 no longer gives inactive and "
                "unvoiced frames the same quantization offsets, so the "
                "reference's two-row table cannot hold it")
        reference = [silk_define(self.root, name) for name in
                     ("OFFSET_UVL_Q10", "OFFSET_UVH_Q10",
                      "OFFSET_VL_Q10", "OFFSET_VH_Q10")]
        self.agree("silk_Quantization_Offsets_Q10",
                   [4 * printed[index] for index in (0, 1, 4, 5)], reference,
                   "Table 53, times four")
        self.emit("quantization_offsets_q10", "int16_t", reference)

    def expression_array(self, relative, name):
        """An array whose entries are sums, as `39083 - 65536` is.

        read_array refuses these on purpose, because reading the two
        numbers of "39083 - 65536" as a list would be silently wrong.
        This one takes only integers, plus and minus, and evaluates each
        entry.
        """
        text = open(os.path.join(self.root, relative), encoding="utf-8",
                    errors="replace").read()
        match = re.search(
            r"\b" + re.escape(name) + r"\s*(\[[^\]]*\]\s*)+=\s*\{", text)
        if not match:
            raise SystemExit(f"gen_opus_tables: {relative} has no {name}")
        start = match.end()
        end = text.index("};", start)
        body = re.sub(r"/\*.*?\*/", " ", text[start:end], flags=re.S)
        values = []
        for entry in re.split(r"[,{}]", body):
            entry = entry.strip()
            if not entry:
                continue
            if not re.fullmatch(r"[-+0-9 ]+", entry):
                raise SystemExit(
                    f"gen_opus_tables: {name} has an entry {entry!r} that is "
                    "not integers and signs")
            values.append(sum(int(term.replace(" ", "")) for term in
                              re.findall(r"[-+]?\s*\d+", entry)))
        return values

    # -- section 4.2.10: the resampler -------------------------------

    def resampler(self):
        """The two tables the decoder's 48 kHz resampler needs.

        Section 4.2.10 says the resampler is not normative, so there is
        no prose copy to hold these against. What can be checked is what
        the numbers are for: each pair of rows of the interpolation
        table is one polyphase filter's eight taps, and a filter that
        passes DC at unity has taps summing to 32768 in Q15, to within
        the rounding of its own design.
        """
        rom = "silk/resampler_rom.c"
        for name in ("silk_resampler_up2_hq_0", "silk_resampler_up2_hq_1"):
            values = self.expression_array(rom, name)
            if len(values) != 3 or values[0] <= 0 or values[1] <= 0 \
                    or values[2] >= 0:
                raise SystemExit(
                    f"gen_opus_tables: {name} is {values}, not two "
                    "positive coefficients and a negative one")
            self.emit(name[5:], "int16_t", values)
        table = self.expression_array(rom, "silk_resampler_frac_FIR_12")
        if len(table) != 48:
            raise SystemExit(
                f"gen_opus_tables: the interpolation table has {len(table)} "
                "entries, not 12 rows of 4")
        for row in range(12):
            taps = table[4 * row:4 * row + 4] \
                + table[4 * (11 - row):4 * (11 - row) + 4]
            if abs(sum(taps) - 32768) > 8:
                raise SystemExit(
                    f"gen_opus_tables: interpolation row {row} sums to "
                    f"{sum(taps)}, not 32768: a coefficient was misread")
        self.emit("resampler_frac_fir_12", "int16_t", table)

    def run(self):
        self.header()
        self.nlsf()
        self.lsf_to_lpc()
        self.ltp()
        self.excitation()
        self.resampler()
        print(f"gen_opus_tables: {self.checked} SILK tables read twice - "
              "once from section 4.2's prose and once from Appendix A - "
              "and the two readings agree")
        return self.tables


def main(argv):
    parser = argparse.ArgumentParser()
    parser.add_argument("--rfc", default=os.path.join(
        os.environ.get("TMPDIR", "/tmp"), "rfc6716.txt"))
    parser.add_argument("--url", default=DEFAULT_URL)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args(argv)

    rfc_text = fetch_rfc(args.rfc, args.url)
    with tempfile.TemporaryDirectory() as work:
        root = reference_source(rfc_text, work)
        tables = extract(root, rfc_text)
        silk = SilkCheck(root, rfc_text.decode("ascii", "replace")).run()
    # The return value is the verdict. An earlier draft called emit() and
    # returned 0 regardless, so `--check` printed "is not what this script
    # produces" and exited successfully - a gate that says the right thing
    # and reports success is worse than no gate.
    return emit(tables, silk, args.check)


def extract(root, rfc_text):
    """Every table, extracted or computed, each checked where it can be."""
    tables = {}

    # --- the band edges, in 2.5 ms units -------------------------------
    eband5ms = read_array(root, "celt/modes.c", "eband5ms")
    if len(eband5ms) != 22:
        raise SystemExit(
            f"gen_opus_tables: eband5ms has {len(eband5ms)} entries, not 22")
    widths = [eband5ms[i + 1] - eband5ms[i] for i in range(21)]

    # Table 55 of the prose against Appendix A's array, for all four
    # frame sizes. Two independent readings of the same fact.
    for band, row in enumerate(TABLE_55):
        for size, multiplier in enumerate((1, 2, 4, 8)):
            want = row[size]
            got = widths[band] * multiplier
            if want != got:
                raise SystemExit(
                    f"gen_opus_tables: band {band} at frame size "
                    f"{multiplier} is {got} bins from Appendix A and {want} "
                    "from Table 55 of the prose. The two readings of the "
                    "band layout disagree; do not pick one.")
    # And against the frequencies the same table states. A 2.5 ms frame
    # at 48 kHz has 120 bins over 20 kHz, so one bin is 400/3 Hz... no:
    # 120 bins span 0 to 20 kHz only because the mode's own Nyquist is
    # 24 kHz and band 20 stops at 20 kHz. One 2.5 ms bin is 200 Hz.
    for band, (low, high) in enumerate(TABLE_55_HZ):
        if eband5ms[band] * 200 != low or eband5ms[band + 1] * 200 != high:
            raise SystemExit(
                f"gen_opus_tables: band {band} runs "
                f"{eband5ms[band] * 200}-{eband5ms[band + 1] * 200} Hz from "
                f"Appendix A and {low}-{high} Hz from Table 55.")
    print("gen_opus_tables: the band layout agrees with Table 55 at all "
          "four frame sizes and in Hz")
    tables["eband5ms"] = ("int16_t", eband5ms)

    # --- logN, checked against the reference's own log2 ----------------
    log_n = read_array(root, "celt/static_modes_fixed.h", "logN400")
    if len(log_n) != 21:
        raise SystemExit("gen_opus_tables: logN400 is not 21 entries")
    for band, width in enumerate(widths):
        computed = log2_frac(width, 3)
        if computed != log_n[band]:
            raise SystemExit(
                f"gen_opus_tables: logN for band {band} (width {width}) is "
                f"{log_n[band]} in Appendix A and {computed} from the "
                "reference's own log2_frac. One of the two is misread.")
    print("gen_opus_tables: logN reproduces from log2_frac of the band "
          "widths")
    tables["logN400"] = ("int16_t", log_n)

    # --- the window, checked against its formula -----------------------
    import math
    window = read_array(root, "celt/static_modes_fixed.h", "window120")
    if len(window) != 120:
        raise SystemExit("gen_opus_tables: window120 is not 120 entries")
    computed = []
    saturated = 0
    for n in range(120):
        inner = math.sin(math.pi / 2.0 * (n + 0.5) / 120.0)
        value = math.sin(math.pi / 2.0 * inner * inner)
        exact = int(math.floor(0.5 + 32768.0 * value))
        # Q15 cannot hold 1.0, and the window reaches it. The last five
        # entries round to 32768 and the reference holds 32767, which is
        # Q15ONE - so the clamp is part of the format's arithmetic rather
        # than a discrepancy to tolerate. Checked as a clamp, and the
        # count of clamped entries is asserted so that a formula which
        # saturated everywhere could not pass.
        if exact > 32767:
            saturated += 1
            exact = 32767
        computed.append(exact)
    if window != computed:
        bad = [(n, window[n], computed[n])
               for n in range(120) if window[n] != computed[n]]
        raise SystemExit(
            "gen_opus_tables: the window in Appendix A is not "
            "sin(pi/2 sin^2(pi/2 (n+1/2)/120)) in Q15 saturated at "
            f"Q15ONE; {len(bad)} of 120 entries differ, first {bad[0]}")
    if saturated != 5:
        raise SystemExit(
            f"gen_opus_tables: {saturated} window entries saturated, not "
            "the five at the top of the rise that this was measured to "
            "clamp. The formula or the scale has moved.")
    print("gen_opus_tables: the 120-sample window reproduces from its "
          f"formula, with {saturated} entries at Q15ONE where it reaches "
          "one")
    tables["window120"] = ("int16_t", window)

    # --- the allocation table, checked against Table 57 ---------------
    allocation = read_array(root, "celt/modes.c", "band_allocation")
    if len(allocation) != 11 * 21:
        raise SystemExit(
            f"gen_opus_tables: band_allocation has {len(allocation)} "
            "entries, not 231")
    prose = table_57(rfc_text)
    for band in range(21):
        for quality in range(11):
            want = prose[band][quality]
            got = allocation[quality * 21 + band]
            if want != got:
                raise SystemExit(
                    f"gen_opus_tables: allocation for band {band} at "
                    f"quality {quality} is {got} in Appendix A and {want} "
                    "in Table 57 of the prose. The two readings of the "
                    "allocation table disagree; do not pick one.")
    print("gen_opus_tables: all 231 allocation entries agree between "
          "Appendix A and Table 57 of the prose")
    tables["band_allocation"] = ("unsigned char", allocation)

    # --- the uniform-coding cost table, which log2_frac reproduces ----
    log2_frac_table = read_array(root, "celt/rate.c", "LOG2_FRAC_TABLE")
    if len(log2_frac_table) != 24:
        raise SystemExit(
            f"gen_opus_tables: LOG2_FRAC_TABLE has "
            f"{len(log2_frac_table)} entries, not 24")
    for index, value in enumerate(log2_frac_table):
        # Entry k is what it costs to code one of k+1 equally likely
        # values, in eighths of a bit - so it is log2_frac of k+1, with
        # the same conservative rounding the allocator relies on.
        want = log2_frac(index + 1, 3)
        if want != value:
            raise SystemExit(
                f"gen_opus_tables: LOG2_FRAC_TABLE[{index}] is {value} in "
                f"Appendix A and {want} from log2_frac({index + 1}). One "
                "of the two is misread.")
    print("gen_opus_tables: the uniform-coding costs reproduce from "
          "log2_frac")
    tables["log2_frac_table"] = ("unsigned char", log2_frac_table)

    # --- extracted, with no second reading available -------------------
    # These have no formula and are not printed in the prose. The check
    # available is their shape, which is asserted rather than assumed.
    for relative, name, kind, expect in (
            ("celt/celt.c", "tf_select_table", "int8_t", 4 * 8),
            ("celt/celt.c", "trim_icdf", "unsigned char", 11),
            ("celt/celt.c", "spread_icdf", "unsigned char", 4),
            ("celt/celt.c", "tapset_icdf", "unsigned char", 3),
            ("celt/static_modes_fixed.h", "cache_index50", "int16_t", 105),
            ("celt/static_modes_fixed.h", "cache_bits50",
             "unsigned char", 392),
            ("celt/static_modes_fixed.h", "cache_caps50",
             "unsigned char", 168),
            ("celt/quant_bands.c", "e_prob_model",
             "unsigned char", 4 * 2 * 42),
            ("celt/quant_bands.c", "eMeans", "int8_t", 25),
            # The inverse MDCT's twiddles, and the FFT it is built on.
            # 481 entries of a quarter cosine, and 480 complex twiddles
            # written as 960 integers.
            ("celt/static_modes_fixed.h", "mdct_twiddles960", "int16_t", 481),
            ("celt/static_modes_fixed.h", "fft_twiddles48000_960",
             "int16_t", 960),
            ("celt/static_modes_fixed.h", "fft_bitrev480", "int16_t", 480),
            ("celt/static_modes_fixed.h", "fft_bitrev240", "int16_t", 240),
            ("celt/static_modes_fixed.h", "fft_bitrev120", "int16_t", 120),
            ("celt/static_modes_fixed.h", "fft_bitrev60", "int16_t", 60),
    ):
        values = read_array(root, relative, name)
        # The reference's one camel-case array name; everything else here
        # is already in the shape this library spells its symbols.
        name = "e_means" if name == "eMeans" else name
        if len(values) != expect:
            raise SystemExit(
                f"gen_opus_tables: {name} has {len(values)} entries, not "
                f"{expect}. Either the reference changed or the shape this "
                "script assumes is wrong.")
        tables[name] = (kind, values)

    # pred_coef and beta_coef appear twice in quant_bands.c, once for the
    # fixed-point build and once divided by 32768 for the float one. The
    # integer decoder wants the first, and reading the file top to bottom
    # finds it first - but relying on that would be relying on the order
    # of two #ifdef arms, so the values are checked against the float
    # spelling's numerators instead.
    text = open(os.path.join(root, "celt/quant_bands.c"),
                encoding="utf-8", errors="replace").read()
    for name, expect in (("pred_coef", 4), ("beta_coef", 4)):
        found = re.findall(
            re.escape(name) + r"\s*\[\s*4\s*\]\s*=\s*\{([^}]*)\}", text)
        if len(found) != 2:
            raise SystemExit(
                f"gen_opus_tables: expected two spellings of {name}, the "
                f"fixed-point one and the float one, and found {len(found)}")
        integers = [int(v) for v in re.findall(r"-?\d+", found[0])]
        floats = [int(v) for v in re.findall(r"(-?\d+)/32768", found[1])]
        if integers != floats or len(integers) != expect:
            raise SystemExit(
                f"gen_opus_tables: the two spellings of {name} disagree: "
                f"{integers} against {floats}")
        tables[name] = ("int16_t", integers)
    # The four FFT configurations are struct initialisers rather than
    # arrays, so they need their own reader. Each gives a transform size,
    # a twiddle stride and a factorisation; the sizes are asserted
    # against the frame sizes they belong to, because a factorisation
    # read against the wrong size would still look like a list of
    # integers.
    modes = open(os.path.join(root, "celt/static_modes_fixed.h"),
                 encoding="utf-8", errors="replace").read()
    nffts = []
    shifts = []
    factors = []
    for level in range(4):
        found = re.search(
            r"fft_state48000_960_%d\s*=\s*\{(.*?)\};" % level, modes, re.S)
        if not found:
            raise SystemExit(
                f"gen_opus_tables: no fft_state48000_960_{level}")
        body = re.sub(r"/\*.*?\*/", " ", found.group(1), flags=re.S)
        head, _, rest = body.partition("{")
        numbers = [int(v) for v in re.findall(r"-?\d+", head)]
        if len(numbers) != 2:
            raise SystemExit(
                f"gen_opus_tables: fft_state48000_960_{level} starts with "
                f"{numbers}, not a size and a shift")
        inner = [int(v) for v in re.findall(r"-?\d+", rest.split("}")[0])]
        if len(inner) != 16:
            raise SystemExit(
                f"gen_opus_tables: fft_state48000_960_{level} has "
                f"{len(inner)} factors, not 16")
        nffts.append(numbers[0])
        shifts.append(numbers[1])
        factors.extend(inner)
    # 480 is the longest frame's N/4, and each shorter frame halves it.
    if nffts != [480, 240, 120, 60]:
        raise SystemExit(
            f"gen_opus_tables: the FFT sizes are {nffts}, not the "
            "480/240/120/60 the four frame sizes need")
    if shifts != [-1, 1, 2, 3]:
        raise SystemExit(f"gen_opus_tables: the FFT shifts are {shifts}")
    for level, size in enumerate(nffts):
        product = 1
        for pair in range(8):
            radix = factors[level * 16 + 2 * pair]
            if radix == 0:
                break
            product *= radix
        if product != size:
            raise SystemExit(
                f"gen_opus_tables: fft_state48000_960_{level}'s factors "
                f"multiply to {product}, not its size {size}")
    tables["fft_nfft"] = ("int16_t", nffts)
    tables["fft_shift"] = ("int16_t", shifts)
    tables["fft_factors"] = ("int16_t", factors)
    print("gen_opus_tables: each FFT configuration's factors multiply to "
          "its transform size")

    beta_intra = re.search(r"beta_intra\s*=\s*(\d+)\s*;", text)
    if not beta_intra:
        raise SystemExit("gen_opus_tables: no beta_intra")
    tables["beta_intra"] = ("int16_t", [int(beta_intra.group(1))])
    print("gen_opus_tables: the prediction coefficients agree between the "
          "fixed-point and floating-point spellings")
    check_fft_tables(tables)
    return tables



def celt_cos_norm(x):
    """RFC 6716's fixed-point cosine, in Python, for checking a table.

    A second transcription of celt/mathops.c's `celt_cos_norm`, written
    here so that the MDCT twiddles can be checked against the formula
    `clt_mdct_init` computes them with rather than only copied. Two
    transcriptions agreeing is worth more than one being careful.
    """

    def s16(value):
        value &= 0xFFFF
        return value - 0x10000 if value >= 0x8000 else value

    def frac_mul_p15(a, b):
        return (16384 + s16(a) * s16(b)) >> 15

    def cos_pi_2(x):
        x2 = frac_mul_p15(x, x)
        inner = 8277 + frac_mul_p15(-626, x2)
        inner = -7651 + frac_mul_p15(x2, inner)
        value = s16(32767 - x2) + frac_mul_p15(x2, inner)
        return s16(1 + min(value, 32766))

    x &= 0x0001FFFF
    if x > 65536:
        x = 131072 - x
    if x & 0x00007FFF:
        if x < 32768:
            return cos_pi_2(s16(x))
        return -cos_pi_2(s16(65536 - x))
    if x & 0x0000FFFF:
        return 0
    if x & 0x0001FFFF:
        return -32767
    return 32767


def check_fft_tables(tables):
    """The MDCT and FFT twiddles against the formulas that make them."""
    # clt_mdct_init: trig[i] = celt_cos_norm((i<<17 + N2) / N), N = 1920.
    mdct = tables["mdct_twiddles960"][1]
    for index, have in enumerate(mdct):
        want = celt_cos_norm(((index << 17) + 960) // 1920)
        if want != have:
            raise SystemExit(
                f"gen_opus_tables: mdct_twiddles960[{index}] is {have}, but "
                f"the formula in clt_mdct_init gives {want}")
    print(f"gen_opus_tables: all {len(mdct)} MDCT twiddles reproduce from "
          "celt_cos_norm of the formula clt_mdct_init uses")

    # The FFT twiddles are exp(-2*pi*i*k/480), and they are *not* a
    # rounded real cosine - `floor(.5 + 32767*cos(phase))`, which is
    # what kiss_fft's own KISS_FFT_COS would give, is wrong by up to two
    # for a third of them. They are celt_cos_norm's output, in the units
    # where 32768 is a quarter turn: a phase of 2*pi*k/480 is
    # (k << 17) / 480, and the sine is the same a quarter turn along.
    #
    # That is worth having rather than a tolerance, because it says the
    # tables and the arithmetic came from one place.
    twiddles = tables["fft_twiddles48000_960"][1]
    for index in range(len(twiddles) // 2):
        phase = (index << 17) // 480
        want_r = celt_cos_norm(phase)
        want_i = celt_cos_norm(phase + 32768)
        if (want_r, want_i) != (twiddles[2 * index], twiddles[2 * index + 1]):
            raise SystemExit(
                f"gen_opus_tables: fft_twiddles48000_960[{index}] is "
                f"({twiddles[2*index]},{twiddles[2*index+1]}), but "
                f"celt_cos_norm of ({phase}, {phase + 32768}) gives "
                f"({want_r},{want_i})")
    print(f"gen_opus_tables: all {len(twiddles)//2} FFT twiddles reproduce "
          "from celt_cos_norm of their phase")

    # Each bit-reversal table must be a permutation of its own range,
    # which is the one property that makes it usable at all.
    for size in (480, 240, 120, 60):
        values = tables[f"fft_bitrev{size}"][1]
        if sorted(values) != list(range(size)):
            raise SystemExit(
                f"gen_opus_tables: fft_bitrev{size} is not a permutation "
                f"of 0..{size - 1}")
    print("gen_opus_tables: each FFT bit-reversal table is a permutation of "
          "its own range")


def render(name, kind, values, per_line=12):
    """One C array, wrapped."""
    out = [f"const {kind} gaud_opus_{name}[{len(values)}] = {{"]
    for start in range(0, len(values), per_line):
        chunk = values[start:start + per_line]
        out.append("    " + " ".join(f"{v}," for v in chunk))
    out.append("};")
    return "\n".join(out)


CELT_NOTES = {
    "eband5ms": "Band edges in 2.5 ms bins; checked against Table 55.",
    "logN400": "Band widths as log2 in eighths of a bit.",
    "window120": "The MDCT overlap window in Q15.",
    "band_allocation": "Allocation in 1/32 bit per sample, 11 by 21.",
    "tf_select_table": "Time-frequency resolution changes, 4 by 8.",
    "trim_icdf": "The allocation trim's distribution.",
    "spread_icdf": "The spreading decision's distribution.",
    "tapset_icdf": "The post-filter tap set's distribution.",
    "cache_index50": "Where each band's pulse cache starts.",
    "cache_bits50": "Bits needed for K pulses in N bins.",
    "cache_caps50": "The allocation ceiling per band and channel count.",
    "e_prob_model": "Laplace parameters for coarse energy, 4 by 2 by 42.",
    "pred_coef": "Coarse energy's time prediction, per frame size, Q15.",
    "beta_coef": "Coarse energy's frequency prediction, Q15.",
    "beta_intra": "The same, for a frame coded without history.",
    "log2_frac_table": "Cost of coding one of k+1 values, in eighths of a bit.",
    "e_means": "The mean energy per band, in Q4 decibels; 21 used of 25.",
    "mdct_twiddles960": "The inverse MDCT's rotation, a quarter cosine in Q15.",
    "fft_twiddles48000_960": "The FFT's twiddles: 480 complex pairs in Q15.",
    "fft_bitrev480": "Input permutation for the 480-point FFT.",
    "fft_bitrev240": "Input permutation for the 240-point FFT.",
    "fft_bitrev120": "Input permutation for the 120-point FFT.",
    "fft_bitrev60": "Input permutation for the 60-point FFT.",
    "fft_nfft": "Each frame size's FFT length.",
    "fft_shift": "Each frame size's twiddle stride, as a shift.",
    "fft_factors": "Each frame size's radix factorisation, 4 by 16.",
}


SILK_NOTES = {
    "silk_resampler_up2_hq_0": "The even output's three all-pass coefficients, Q16.",
    "silk_resampler_up2_hq_1": "The odd output's three all-pass coefficients, Q16.",
    "silk_resampler_frac_fir_12": "Twelve polyphase rows of four taps; a row and its mirror make eight, Q15.",
    "silk_lbrr_flags_2_icdf": "Which of a 40 ms frame's two LBRR frames are coded.",
    "silk_lbrr_flags_3_icdf": "The same for 60 ms, over three frames.",
    "silk_stereo_pred_joint_icdf": "The stereo weights' shared high-order index.",
    "silk_uniform3_icdf": "Three equally likely values.",
    "silk_uniform4_icdf": "Four equally likely values.",
    "silk_uniform5_icdf": "Five equally likely values.",
    "silk_uniform6_icdf": "Six equally likely values.",
    "silk_uniform8_icdf": "Eight equally likely values.",
    "silk_stereo_pred_quant_q13": "The sixteen stereo prediction weights, Q13.",
    "silk_stereo_only_code_mid_icdf": "Whether the side channel is coded at all.",
    "silk_type_offset_no_vad_icdf": "Frame type in an inactive frame; add 0.",
    "silk_type_offset_vad_icdf": "Frame type in an active frame; add 2.",
    "silk_gain_icdf": "The independent gain's top three bits, by signal type.",
    "silk_delta_gain_icdf": "A gain coded against the previous subframe's.",
    "silk_nlsf_cb1_icdf_nb_mb": "NB and MB stage-1 LSF index, unvoiced then voiced.",
    "silk_nlsf_cb1_icdf_wb": "The same for WB.",
    "silk_nlsf_cb2_icdf_nb_mb": "NB and MB stage-2 residual, eight distributions of nine.",
    "silk_nlsf_cb2_icdf_wb": "The same for WB.",
    "silk_nlsf_cb2_select_nb_mb": "Per NB/MB coefficient: stage-2 distribution and weight list,\n    two per byte.",
    "silk_nlsf_cb2_select_wb": "The same for WB.",
    "silk_nlsf_pred_nb_mb_q8": "NB and MB backwards prediction weights, two lists of nine, Q8.",
    "silk_nlsf_pred_wb_q8": "The same for WB, two lists of fifteen.",
    "silk_nlsf_cb1_nb_mb_q8": "The NB and MB stage-1 codebook, 32 vectors of 10, Q8.",
    "silk_nlsf_cb1_wb_q8": "The WB stage-1 codebook, 32 vectors of 16, Q8.",
    "silk_nlsf_delta_min_nb_mb_q15": "Minimum spacing between NB and MB LSFs, Q15.",
    "silk_nlsf_delta_min_wb_q15": "The same for WB.",
    "silk_nlsf_ext_icdf": "The extension coded when a stage-2 index saturates.",
    "silk_nlsf_interpolation_factor_icdf": "How far a 20 ms frame interpolates towards the previous LSFs.",
    "silk_nlsf_ordering10": "The order 10 LSFs are evaluated in, which is chosen for accuracy.",
    "silk_nlsf_ordering16": "The same for 16.",
    "silk_lsf_cos_q13": "Cosine of an LSF, 129 points in Q13 despite the reference's name for it.",
    "silk_pitch_lag_icdf": "The primary pitch lag's high part.",
    "silk_pitch_delta_icdf": "A pitch lag coded against the previous frame's.",
    "silk_pitch_contour_10_ms_nb_icdf": "Subframe pitch contour, NB, 10 ms.",
    "silk_pitch_contour_nb_icdf": "Subframe pitch contour, NB, 20 ms.",
    "silk_pitch_contour_10_ms_icdf": "Subframe pitch contour, MB or WB, 10 ms.",
    "silk_pitch_contour_icdf": "Subframe pitch contour, MB or WB, 20 ms.",
    "silk_cb_lags_stage2_10_ms": "The NB 10 ms contour offsets, subframe by subframe.",
    "silk_cb_lags_stage2": "The NB 20 ms contour offsets, subframe by subframe.",
    "silk_cb_lags_stage3_10_ms": "The MB and WB 10 ms contour offsets.",
    "silk_cb_lags_stage3": "The MB and WB 20 ms contour offsets.",
    "silk_ltp_per_index_icdf": "Which of the three LTP filter codebooks a frame uses.",
    "silk_ltp_gain_icdf_0": "LTP filter index, periodicity 0.",
    "silk_ltp_gain_icdf_1": "LTP filter index, periodicity 1.",
    "silk_ltp_gain_icdf_2": "LTP filter index, periodicity 2.",
    "silk_ltp_gain_vq_0": "Eight five-tap LTP filters, Q7.",
    "silk_ltp_gain_vq_1": "Sixteen five-tap LTP filters, Q7.",
    "silk_ltp_gain_vq_2": "Thirty-two five-tap LTP filters, Q7.",
    "silk_ltpscale_icdf": "Which LTP scale factor a voiced frame uses.",
    "silk_ltp_scales_q14": "The three LTP scale factors, Q14.",
    "silk_rate_levels_icdf": "The excitation's rate level, by signal type.",
    "silk_pulses_per_block_icdf": "Pulses in a shell block, ten rate levels of eighteen.",
    "silk_shell_code_table_offsets": "Where each pulse count's split distribution starts.",
    "silk_shell_code_table3": "Splitting a 16-sample partition.",
    "silk_shell_code_table2": "Splitting an 8-sample partition.",
    "silk_shell_code_table1": "Splitting a 4-sample partition.",
    "silk_shell_code_table0": "Splitting a 2-sample partition.",
    "silk_lsb_icdf": "One extra excitation bit.",
    "silk_sign_icdf": "An excitation sign: six contexts by seven pulse counts.",
    "silk_quantization_offsets_q10": "The excitation's quantization offset, Q10.",
}


def render_pair(stem, guard, headline, body_line, tables, notes):
    """One generated .h/.c pair, as text."""
    header = [LICENSE, GENERATED, f"""
/**
 * @file
 *
{headline} tools/tables/gen_opus_tables.py says from
 * what, and which of them were checked against a second reading.
 */

#ifndef {guard}
#define {guard}

#include <ghoti.io/audio/macros.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {{
#endif
"""]
    body = [LICENSE, GENERATED, f"""
/**
 * @file
 *
{body_line}
 */

#include "{stem}.h"
"""]
    for name in tables:
        if name not in notes:
            raise SystemExit(f"gen_opus_tables: {name} has no note")
    for name, (kind, values) in tables.items():
        header.append(f"/** {notes[name]} */\n"
                      f"extern const {kind} gaud_opus_{name}[{len(values)}];"
                      "\n")
        body.append(f"/** {notes[name]} */\n"
                    + render(name, kind, values) + "\n")
    header.append(f"""
#ifdef __cplusplus
}}
#endif

#endif // {guard}
""")
    return {f"{stem}.h": "\n".join(header), f"{stem}.c": "\n".join(body)}


def emit(tables, silk, check):
    """Write both pairs, or compare against what is committed."""
    files = {}
    files.update(render_pair(
        "opus_tables", "GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_TABLES_H",
        " * The CELT tables, generated.",
        " * The CELT tables themselves.", tables, CELT_NOTES))
    files.update(render_pair(
        "opus_silk_tables",
        "GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_SILK_TABLES_H",
        " * The SILK tables, generated.",
        " * The SILK tables themselves.", silk, SILK_NOTES))
    if check:
        bad = 0
        for name, text in files.items():
            path = os.path.join(OUT_DIR, name)
            have = open(path, encoding="utf-8").read() if os.path.exists(
                path) else ""
            if have != text:
                bad += 1
                print(f"gen_opus_tables: {name} is not what this script "
                      "produces", file=sys.stderr)
        if bad:
            return 1
        print("gen_opus_tables: the committed tables are what this script "
              "produces")
        return 0
    for name, text in files.items():
        path = os.path.join(OUT_DIR, name)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(text)
        print(f"gen_opus_tables: wrote {path} ({len(text)} bytes)")
    return 0
if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
