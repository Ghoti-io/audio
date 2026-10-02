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
"""The CELT tables, out of the document that defines them.

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
    # The return value is the verdict. An earlier draft called emit() and
    # returned 0 regardless, so `--check` printed "is not what this script
    # produces" and exited successfully - a gate that says the right thing
    # and reports success is worse than no gate.
    return emit(tables, args.check)


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


def emit(tables, check):
    """Write the pair, or compare against what is committed."""
    header = [LICENSE, GENERATED, """
/**
 * @file
 *
 * The CELT tables, generated. tools/tables/gen_opus_tables.py says from
 * what, and which of them were checked against a second reading.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_TABLES_H
#define GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_TABLES_H

#include <ghoti.io/audio/macros.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
"""]
    body = [LICENSE, GENERATED, """
/**
 * @file
 *
 * The CELT tables themselves.
 */

#include "opus_tables.h"
"""]
    notes = {
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
    for name in tables:
        if name not in notes:
            raise SystemExit(f"gen_opus_tables: {name} has no note")
    for name, (kind, values) in tables.items():
        header.append(f"/** {notes[name]} */\n"
                      f"extern const {kind} gaud_opus_{name}[{len(values)}];"
                      "\n")
        body.append(f"/** {notes[name]} */\n"
                    + render(name, kind, values) + "\n")
    header.append("""
#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_TABLES_H
""")
    files = {
        "opus_tables.h": "\n".join(header),
        "opus_tables.c": "\n".join(body),
    }
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
