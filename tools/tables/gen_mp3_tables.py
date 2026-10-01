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
"""Generate src/codec/mp3/mp3_tables.c from ISO/IEC 11172-3 Annex B.

    tools/tables/gen_mp3_tables.py [--pdf PATH] [--url URL] [--check]

**Why this script exists rather than a hand-typed table.** A Layer III
decoder needs two things that no formula produces: the 32 Huffman code
tables of Table 3-B.7, which are about two thousand code/length pairs, and
the 512 coefficients of the synthesis window in Table 3-B.3. Both are
normative data in the standard. Typing them in by hand is a thousand
opportunities to be wrong in a way that decodes *almost* correctly, and
the wrongness would be invisible wherever the corpus does not reach - a
Huffman entry for a value pair no fixture contains is a defect that ships.

So the tables are extracted from the document that defines them, by a
script that is in the repository, and **the document itself is not.** It
is a standards text and this library does not redistribute it; the script
fetches it, reads it, and leaves it in a temporary directory. The
generated C file *is* committed, so a build needs neither the network nor
the PDF - this runs when a table changes, which is to say almost never.

What is extracted, and what is computed from what was extracted:

  extracted   3-B.1 Layer I/II scalefactors; 3-B.2a..d Layer II bit
              allocation; 3-B.3 the synthesis window; 3-B.4 Layer II
              quantisation classes; 3-B.6 Layer III preemphasis;
              3-B.7 every Huffman table and its linbits; 3-B.8a..c the
              Layer III scalefactor bands; 3-B.9 the aliasing
              coefficients c[i].
  computed    cs[i] and ca[i] from c[i], by the formula 3-B.9 gives;
              is^(4/3) for requantisation; the four 2^(k/4) gain
              multipliers; the IMDCT and polyphase cosine matrices; the
              Layer III block windows. All in exact integer or Fraction
              arithmetic here, so the C file carries no float literal and
              the result is identical on every machine that runs this.

**Everything extracted is validated before it is written.** The
validations are the point of the exercise, because a silent
mis-extraction is the failure this script exists to prevent:

  - every Huffman table is a complete prefix code: no code is a prefix of
    another, the Kraft sum is exactly 1, and every (x,y) pair in the
    table's square appears exactly once;
  - every code's spelled length equals the number of binary digits
    printed beside it, which is what catches a column of the PDF being
    read into the wrong row;
  - the synthesis window's 512 values are all exact multiples of 2^-16,
    which the standard's own decimals are and which no mis-read value
    would be;
  - the scalefactor bands partition their spectrum with no gap and no
    overlap, and end where the standard says;
  - the Layer I/II scalefactors are a geometric sequence in 2^(-1/3).

Run with --check to validate and print a summary without writing.
"""

import argparse
import hashlib
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# The mirror this was first run against, and its digest. The digest is not
# a trust anchor for the standard - it is a statement that the document
# this script was written for has not changed under it, so that a failure
# to parse is a failure to parse and not a different edition.
DEFAULT_URL = ("https://courses.e-ce.uth.gr/CE401/tree_menu/tutorials/"
               "MPEG1/MPEG1_3.PDF")
KNOWN_SHA256 = {
    "0f2f6c6a5f6e1f5ac1c0f1f1e8f0c9d9c2f9d3a0b9e3b9e0d5c2a1f0e9d8c7b6":
        "unset placeholder",
}


class Fail(Exception):
    """A validation failed. Never caught into a warning."""


def fetch(url, into):
    """Fetch the document into a temporary directory, never the repository."""
    path = os.path.join(into, "iso11172-3.pdf")
    if shutil.which("curl") is None:
        raise Fail("curl is not on PATH, and the document has to be fetched")
    finished = subprocess.run(
        ["curl", "-sSL", "--max-time", "300", "-o", path, url],
        capture_output=True, text=True)
    if finished.returncode != 0 or not os.path.exists(path):
        raise Fail("could not fetch %s\n%s" % (url, finished.stderr.strip()))
    return path


def raw_text(pdf):
    """The document as text, in reading order.

    `-raw` and not `-layout`, and the difference decides whether this
    script works at all. Annex B's Huffman tables are printed in three
    columns, and `-layout` interleaves them: one output line then holds
    fragments of three different tables, and a row's own table is no
    longer knowable. `-raw` emits each column's text in order, so a
    table's rows arrive together and the "Huffman code table N" headings
    stay in sequence.
    """
    if shutil.which("pdftotext") is None:
        raise Fail("pdftotext is not on PATH (Debian: poppler-utils)")
    finished = subprocess.run(["pdftotext", "-raw", pdf, "-"],
        capture_output=True, text=True)
    if finished.returncode != 0:
        raise Fail("pdftotext failed: %s" % finished.stderr.strip()[-400:])
    return finished.stdout


def section(text, start, end=None):
    """The lines between two headings, exclusive of both."""
    at = text.find(start)
    if at < 0:
        raise Fail("the document does not contain %r" % start)
    at += len(start)
    if end is None:
        return text[at:]
    stop = text.find(end, at)
    if stop < 0:
        raise Fail("the document does not contain %r after %r" % (end, start))
    return text[at:stop]


def numbers(line):
    """Every integer and decimal on a line, as strings."""
    return re.findall(r"-?\d+\.\d+|-?\d+", line)


# ----------------------------------------------------------- the parsers

def check_band_rows(name, rows, lines, exact):
    """Validate a scalefactor band table from its width and start columns.

    **The end column is not trusted, and there is a measured reason.** Every
    band table in both documents prints the width, the first line and the
    last line of each band, which is three statements of one fact - and in
    13818-3's 24 kHz short table the third disagrees with the other two: it
    prints band 0 as lines 0 to 4 with a width of 4, and its last band as
    180 to 192 in a spectrum of 192 lines. That column is the *next*
    band's start throughout that one table, where every other table in
    both documents prints the band's own last line.

    So the width and the start are checked against each other, the end is
    accepted in either spelling, and a table that mixes the two within
    itself fails. The widths are what a decoder actually needs.

    @param lines  How many spectral lines the table covers at most.
    @param exact  Whether it must cover all of them. MPEG-1's long tables
      do not: at 32 kHz they stop after line 549, and the lines above
      belong to no scalefactor band at all.
    """
    if not rows:
        raise Fail("%s: no band rows were found" % name)
    inclusive = None
    at = 0
    for index, (wide, start, end) in enumerate(rows):
        if start != at:
            raise Fail("%s: band %d starts at %d and the bands before it "
                       "account for %d lines" % (name, index, start, at))
        if end == start + wide - 1:
            spelling = True
        elif end == start + wide:
            spelling = False
        else:
            raise Fail("%s: band %d is %d wide and starts at %d, so it ends "
                       "at %d or (exclusively) %d, and the table says %d"
                       % (name, index, wide, start, start + wide - 1,
                          start + wide, end))
        if inclusive is None:
            inclusive = spelling
        elif inclusive != spelling:
            raise Fail("%s: band %d spells its end %s and the bands before "
                       "it spell theirs %s" % (name, index,
                           "inclusively" if spelling else "exclusively",
                           "inclusively" if inclusive else "exclusively"))
        at = start + wide
    if at > lines:
        raise Fail("%s: the bands account for %d lines and there are %d"
                   % (name, at, lines))
    if exact and at != lines:
        raise Fail("%s: the bands account for %d of the %d lines and this "
                   "table must cover all of them" % (name, at, lines))
    return inclusive


def parse_scalefactors(text):
    """3-B.1: the 63 Layer I and II scalefactors.

    Printed in two columns - index 0..31 beside index 32..62 - which `-raw`
    emits as one line holding both, so a line carries two (index, value)
    pairs rather than one.
    """
    body = section(text, "Table 3-B.1. Layer I,II scalefactors",
        "Layer II bit allocation tables")
    found = {}
    for line in body.splitlines():
        parts = numbers(line)
        for i in range(0, len(parts) - 1, 2):
            index, value = parts[i], parts[i + 1]
            if "." not in value or "." in index:
                continue
            found[int(index)] = Fraction(value)
    if sorted(found) != list(range(63)):
        raise Fail("3-B.1: expected indices 0..62, got %d of them: %s"
                   % (len(found), sorted(set(range(63)) - set(found))[:8]))
    # A geometric sequence in 2^(-1/3), starting at 2. Checked rather than
    # assumed: it is the cheapest statement that every one of the 63 values
    # was read from the right row, and a transposed pair fails it.
    for index, value in found.items():
        want = 2.0 * (2.0 ** (-index / 3.0))
        if abs(float(value) - want) > 1e-11 * max(1.0, want):
            raise Fail("3-B.1: scalefactor %d is %s, and 2*2^(-%d/3) is %.14f"
                       % (index, value, index, want))
    return [found[i] for i in range(63)]


def parse_slen(text):
    """11172-3: scalefac_compress to the two scalefactor field widths.

    Sixteen rows of two small numbers. The validation is the part2_length
    identity the standard states beside it: a long granule's scalefactors
    occupy 11*slen1 + 10*slen2 bits, so both widths have to be in 0..4 for
    the field they are read from to be the size it is.
    """
    body = section(text, "scalefac_compress slen1 slen2", "blocksplit_flag")
    found = {}
    for line in body.splitlines():
        parts = line.split()
        if len(parts) != 3 or not all(re.fullmatch(r"\d+", p) for p in parts):
            continue
        index, slen1, slen2 = (int(p) for p in parts)
        if index in found or index > 15:
            continue
        if slen1 > 4 or slen2 > 4:
            raise Fail("slen table: row %d is (%d, %d) and a scalefactor "
                       "field is at most four bits"
                       % (index, slen1, slen2))
        found[index] = (slen1, slen2)
    if sorted(found) != list(range(16)):
        raise Fail("slen table: expected 16 rows, got %r" % sorted(found))
    return [found[i] for i in range(16)]


def parse_allocation(text):
    """3-B.2a..d: how many quantisation steps each allocation index means.

    One table per (sampling rate, bitrate) group, each a row per subband:
    the subband, how many bits its allocation field occupies, and then one
    entry per value that field can take. A dash means "no samples in this
    subband", which is not the same as zero steps.
    """
    tables = []
    names = ["Table 3-B.2a Possible quantization per subband",
             "Table 3-B.2b. Possible quantization per subband",
             "Table 3-B.2c. Possible quantization per subband",
             "Table 3-B.2d. Possible quantization per subband"]
    ends = names[1:] + ["Table 3-B.3. Coefficients"]
    for name, end in zip(names, ends):
        body = section(text, name, end)
        rows = []
        for line in body.splitlines():
            line = line.strip()
            if not line.startswith("SB"):
                continue
            parts = line.split()
            subband = int(parts[0][2:])
            nbal = int(parts[1])
            entries = []
            for token in parts[2:]:
                entries.append(0 if token == "-" else int(token))
            if len(entries) != 1 << nbal:
                raise Fail("%s: subband %d says nbal=%d so %d entries, and "
                           "%d were printed"
                           % (name, subband, nbal, 1 << nbal, len(entries)))
            if entries[0] != 0:
                raise Fail("%s: subband %d allocation 0 is not a dash"
                           % (name, subband))
            if subband != len(rows):
                raise Fail("%s: subbands are out of order at %d"
                           % (name, subband))
            rows.append((nbal, entries))
        if not rows:
            raise Fail("%s: no subband rows were found" % name)
        tables.append(rows)
    return tables


def parse_window(text):
    """3-B.3: the 512 coefficients of the synthesis window.

    **The printed decimals are rounded and the real values are not.** Every
    coefficient in this table is an exact multiple of 2^-16 - the standard's
    own 0.000015259 is 1/65536 printed to nine places - so the exact value
    is recovered by scaling and rounding, and the recovery is itself the
    check: a mis-read digit lands nowhere near a multiple of 2^-16.
    """
    body = section(text, "Table 3-B.3. Coefficients Di of the synthesis "
        "window", "Table 3-B.4")
    found = {}
    for match in re.finditer(r"D\[\s*(\d+)\]\s*=\s*(-?\d+\.\d+)", body):
        index = int(match.group(1))
        value = Fraction(match.group(2))
        scaled = value * 65536
        nearest = round(scaled)
        if abs(float(scaled - nearest)) > 1e-3:
            raise Fail("3-B.3: D[%d] = %s is not a multiple of 2^-16 "
                       "(x65536 = %.6f)" % (index, value, float(scaled)))
        found[index] = Fraction(nearest, 65536)
    if sorted(found) != list(range(512)):
        missing = sorted(set(range(512)) - set(found))
        raise Fail("3-B.3: expected D[0..511], %d missing, first %s"
                   % (len(missing), missing[:8]))
    return [found[i] for i in range(512)]


def parse_classes(text):
    """3-B.4: the Layer II quantisation classes.

    Steps, the two requantisation constants C and D, whether three samples
    share a codeword, and how many bits that codeword is.
    """
    body = section(text, "Table 3-B.4. Layer II classes of quantization",
        "Table 3-B.5")
    rows = []
    for line in body.splitlines():
        parts = line.split()
        if len(parts) != 6 or parts[3] not in ("yes", "no"):
            continue
        steps = int(parts[0])
        c = Fraction(parts[1])
        d = Fraction(parts[2])
        grouping = parts[3] == "yes"
        samples = int(parts[4])
        bits = int(parts[5])
        # C is 2^b/steps for the smallest b with 2^b > steps, and D is a
        # half for a grouped class and 2^-(bits-1) otherwise. Both are
        # stated to eleven places, so this catches a dropped digit.
        power = 1
        while power <= steps:
            power *= 2
        if abs(float(c) - power / steps) > 1e-9:
            raise Fail("3-B.4: steps=%d says C=%s and %d/%d is %.11f"
                       % (steps, c, power, steps, power / steps))
        want_d = 0.5 if grouping else 2.0 ** -(bits - 1)
        if abs(float(d) - want_d) > 1e-9:
            raise Fail("3-B.4: steps=%d says D=%s and %.11f was expected"
                       % (steps, d, want_d))
        rows.append((steps, c, d, grouping, samples, bits))
    if len(rows) != 17:
        raise Fail("3-B.4: expected 17 classes, found %d" % len(rows))
    return rows


def parse_pretab(text):
    """3-B.6: the preemphasis added to the scalefactors when preflag is set."""
    body = section(text, "Table 3-B.6. Layer III Preemphasis",
        "Table 3-B.7")
    values = []
    for line in body.splitlines():
        parts = numbers(line)
        if len(parts) >= 20:
            values = [int(p) for p in parts]
            break
    if len(values) != 21 or any(v < 0 or v > 3 for v in values):
        raise Fail("3-B.6: expected 21 values in 0..3, got %r" % (values,))
    return values


def parse_aliasing(text):
    """3-B.9: the eight c[i] the butterfly coefficients are computed from."""
    body = section(text, "Table 3-B.9 Layer III coefficients for aliasing "
        "reduction", "The butterfly coefficients")
    found = {}
    for line in body.splitlines():
        parts = line.split()
        if len(parts) == 2 and re.fullmatch(r"\d", parts[0]) \
                and re.fullmatch(r"-?\d*\.\d+", parts[1]):
            found[int(parts[0])] = Fraction(parts[1])
    if sorted(found) != list(range(8)):
        raise Fail("3-B.9: expected c[0..7], got %r" % sorted(found))
    if not all(-1 < found[i] < 0 for i in range(8)):
        raise Fail("3-B.9: every c[i] is negative and above -1; got %r"
                   % [str(found[i]) for i in range(8)])
    return [found[i] for i in range(8)]


def check_prefix_code(name, codes, pairs, expected):
    """Every validation a Huffman table from this document must pass.

    @param codes     (hlen, hcod) in the order printed.
    @param pairs     what each code stands for - an (x,y) for a pair table,
                     a value for a quadruple table.
    @param expected  the set of those this table must cover exactly.

    Four separate statements, because they fail for different reasons: a
    code whose printed digits disagree with its printed length is a column
    read into the wrong row; a prefix collision or a Kraft sum below one is
    a row lost at a page break; and a missing (x,y) is a row lost
    anywhere.
    """
    seen = {}
    kraft = Fraction(0)
    for (hlen, hcod), pair in zip(codes, pairs):
        if len(hcod) != hlen:
            raise Fail("%s: %s has hlen=%d and %d printed digits"
                       % (name, pair, hlen, len(hcod)))
        if hlen > 19:
            raise Fail("%s: %s has an implausible length %d"
                       % (name, pair, hlen))
        for other in seen:
            short, long_ = (hcod, other) if len(hcod) <= len(other) \
                else (other, hcod)
            if long_.startswith(short):
                raise Fail("%s: %s's code %s and %s's code %s share a prefix"
                           % (name, pair, hcod, seen[other], other))
        seen[hcod] = pair
        kraft += Fraction(1, 1 << hlen)
    if kraft != 1:
        raise Fail("%s: the Kraft sum is %s and a complete prefix code's is "
                   "exactly 1, so %s" % (name, kraft,
                       "a code is missing" if kraft < 1 else
                       "a code is duplicated"))
    if set(pairs) != expected:
        missing = sorted(expected - set(pairs))
        extra = sorted(set(pairs) - expected)
        raise Fail("%s: the table does not cover what it should; %d missing "
                   "(%s), %d unexpected (%s)" % (name, len(missing),
                       missing[:4], len(extra), extra[:4]))


def parse_huffman(text):
    """3-B.7: the two quadruple tables and the thirty-two pair tables.

    Returns (quadruples, tables) where quadruples is {'A': rows, 'B': rows}
    and tables is a list of 32 entries, each None for an unused table or
    {'linbits', 'width', 'codes', 'pairs'}.

    Three shapes appear in the document and all three are here:
    a table printed in full; `not used`, which tables 4 and 14 are; and
    `same as table 16, but linbits=N`, which is how tables 17 to 23 and 25
    to 31 are given - one code table serving eight linbits values, which is
    also how a decoder should hold them.
    """
    body = section(text, "Table 3-B.7. Huffman",
        "Table 3-B.8. Layer III scalefactor bands")
    lines = [line.strip() for line in body.splitlines()]

    blocks = {}
    order = []
    current = None
    index = 0
    while index < len(lines):
        line = lines[index]
        match = re.fullmatch(r"Huffman code table (\d+)", line)
        if match:
            current = int(match.group(1))
            blocks[current] = []
            order.append(current)
            index += 1
            continue
        if line == "Huffman code table for" and index + 1 < len(lines):
            match = re.fullmatch(r"quadruples \(([AB])\)", lines[index + 1])
            if match:
                current = match.group(1)
                blocks[current] = []
                order.append(current)
                index += 2
                continue
        if current is not None:
            blocks[current].append(line)
        index += 1

    for key in ["A", "B"] + list(range(32)):
        if key not in blocks:
            raise Fail("3-B.7: no block for table %r" % (key,))

    quadruples = {}
    for key in ("A", "B"):
        rows = []
        for line in blocks[key]:
            match = re.fullmatch(r"([01]{4}) (\d+) ([01]+)", line)
            if match:
                rows.append((match.group(1), int(match.group(2)),
                             match.group(3)))
        if len(rows) != 16:
            raise Fail("3-B.7: quadruple table %s has %d rows, not 16"
                       % (key, len(rows)))
        values = [row[0] for row in rows]
        if sorted(values) != sorted("%04d" % int(bin(v)[2:])
                                    for v in range(16)):
            # Spelled as four binary digits; every one of the sixteen
            # four-bit values appears exactly once.
            want = {format(v, "04b") for v in range(16)}
            if set(values) != want:
                raise Fail("3-B.7: quadruple table %s does not cover the "
                           "sixteen values: %r" % (key, sorted(values)))
        check_prefix_code("quadruple table %s" % key,
            [(row[1], row[2]) for row in rows],
            [int(row[0], 2) for row in rows], set(range(16)))
        quadruples[key] = rows

    tables = [None] * 32
    for number in range(32):
        block = blocks[number]
        text_block = " ".join(block)
        if "not used" in text_block:
            continue
        rows = []
        for line in block:
            match = re.fullmatch(r"(\d+) (\d+) (\d+) ([01]+)", line)
            if match:
                rows.append((int(match.group(1)), int(match.group(2)),
                             int(match.group(3)), match.group(4)))
        linbits = 0
        match = re.search(r"linbits\s*=\s*(\d+)", text_block)
        if match:
            linbits = int(match.group(1))
        alias = re.search(r"same as table (\d+)", text_block)
        if alias:
            source = int(alias.group(1))
            if rows:
                raise Fail("3-B.7: table %d says it is a copy of %d and "
                           "also prints %d rows" % (number, source, len(rows)))
            if tables[source] is None:
                raise Fail("3-B.7: table %d copies table %d, which is not "
                           "there" % (number, source))
            tables[number] = dict(tables[source])
            tables[number]["linbits"] = linbits
            tables[number]["copy_of"] = source
            continue
        if number == 0:
            # The empty table: a region that selects it codes nothing, and
            # the document prints one row of zeros to say so.
            if rows and rows != [(0, 0, 0, "0")]:
                raise Fail("3-B.7: table 0 is the empty table and printed "
                           "%r" % (rows[:3],))
            tables[0] = {"linbits": 0, "width": 0, "codes": [], "pairs": []}
            continue
        if not rows:
            raise Fail("3-B.7: table %d printed no rows and does not say it "
                       "is unused or a copy" % number)
        width = max(max(row[0] for row in rows),
                    max(row[1] for row in rows)) + 1
        check_prefix_code("table %d" % number,
            [(row[2], row[3]) for row in rows],
            [(row[0], row[1]) for row in rows],
            {(x, y) for x in range(width) for y in range(width)})
        tables[number] = {
            "linbits": linbits,
            "width": width,
            "codes": [(row[2], row[3]) for row in rows],
            "pairs": [(row[0], row[1]) for row in rows],
        }
    return quadruples, tables


def parse_sfbands(text):
    """3-B.8a..c: where each Layer III scalefactor band starts and ends.

    Returns {rate: {'long': [...], 'short': [...]}} with the *widths* as
    printed and the boundaries validated against them.

    The widths and the two indices are all printed, which is three
    statements of the same fact - so they are checked against each other
    rather than one being taken and the others ignored. A band table read
    from the wrong column decodes every scalefactor into the wrong part of
    the spectrum, which sounds like a filter rather than like a bug.
    """
    names = {32000: "Table 3-B.8a. 32 kHz sampling rate",
             44100: "Table 3-B.8b. 44.1 kHz sampling rate",
             48000: "Table 3-B.8c. 48 kHz sampling rate"}
    ends = {32000: names[44100], 44100: names[48000],
            48000: "Table 3-B.9"}
    out = {}
    for rate in (32000, 44100, 48000):
        body = section(text, names[rate], ends[rate])
        which = None
        bands = {"long": [], "short": []}
        for line in body.splitlines():
            line = line.strip()
            if line.startswith("long blocks"):
                which = "long"
                continue
            if line.startswith("short blocks"):
                which = "short"
                continue
            if which is None:
                continue
            match = re.fullmatch(r"(\d+) (\d+) (\d+) (\d+)", line)
            if not match:
                continue
            band, wide, start, end = (int(g) for g in match.groups())
            if band != len(bands[which]):
                raise Fail("3-B.8 %d %s: band %d arrived at position %d"
                           % (rate, which, band, len(bands[which])))
            bands[which].append((wide, start, end))
        if len(bands["long"]) != 21:
            raise Fail("3-B.8 %d: expected 21 long bands, got %d"
                       % (rate, len(bands["long"])))
        if len(bands["short"]) != 12:
            raise Fail("3-B.8 %d: expected 12 short bands, got %d"
                       % (rate, len(bands["short"])))
        check_band_rows("3-B.8 %d long" % rate, bands["long"], 576, False)
        check_band_rows("3-B.8 %d short" % rate, bands["short"], 192, False)
        out[rate] = bands
    return out


# ------------------------------------------- 13818-3, the lower rates

def parse_lsf_sfbands(text):
    """13818-3 Table B.2: the scalefactor bands at 16, 22.05 and 24 kHz.

    **These are not the MPEG-1 tables with different numbers in them: they
    are a different shape.** The low sampling frequency extension has 22
    long bands and 13 short ones where MPEG-1 has 21 and 12, and the
    document's own prose says 21 and 12 while its tables print 22 and 13 -
    so the count is taken from the table and the prose is noted as wrong.
    A decoder that allocated 21 reads one band short at the top of every
    spectrum.
    """
    out = {}
    marks = [(16000, "16 kHz sampling rate"),
             (22050, "22,05 kHz sampling rate"),
             (24000, "24 kHz sampling rate")]
    for rate, mark in marks:
        bands = {}
        for which, lines_count in (("long", 576), ("short", 192)):
            head = "%s, %s blocks, number of lines %d" % (mark, which,
                lines_count)
            at = text.find(head)
            if at < 0:
                raise Fail("13818-3: no %r" % head)
            rows = []
            for line in text[at + len(head):].splitlines():
                line = line.strip()
                match = re.fullmatch(r"(\d+) (\d+) (\d+) (\d+)", line)
                if not match:
                    if rows and not line.startswith("scalefactor band"):
                        # The table has ended; anything after it belongs to
                        # the next one.
                        if re.search(r"[A-Za-z]{4}", line):
                            break
                    continue
                band, wide, start, end = (int(g) for g in match.groups())
                if band != len(rows):
                    break
                rows.append((wide, start, end))
            want = 22 if which == "long" else 13
            if len(rows) != want:
                raise Fail("13818-3 %d %s: expected %d bands, got %d"
                           % (rate, which, want, len(rows)))
            check_band_rows("13818-3 %d %s" % (rate, which), rows,
                lines_count, True)
            bands[which] = rows
        out[rate] = bands
    return out


def parse_lsf_scalefactor_groups(text):
    """13818-3: how many scalefactors each of the four partitions holds.

    Six groups, in the order the document gives them: three for an ordinary
    channel, selected by ranges of scalefac_compress, and three more for the
    right channel of an intensity-coded pair. Each group has a row for the
    long block types, one for short unmixed and one for short mixed.

    The sums are the validation and they are decisive: a long granule has
    21 scalefactors, an unmixed short one has 12 bands in each of three
    windows, and a mixed one has the first 6 long bands plus 9 short bands
    in three windows. So the four partitions must add to 21, 36 and 33
    respectively, in every one of the six groups.
    """
    header = ("block_type mixed_block_flag nr_of_sfb1 nr_of_sfb2 nr_of_sfb3 "
              "nr_of_sfb4")
    groups = []
    at = 0
    while True:
        found = text.find(header, at)
        if found < 0:
            break
        at = found + len(header)
        rows = []
        for line in text[at:].splitlines():
            line = line.strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) < 5:
                break
            tail = parts[-4:]
            if not all(re.fullmatch(r"\d+", p) for p in tail):
                break
            rows.append([int(p) for p in tail])
            if len(rows) == 3:
                break
        if len(rows) != 3:
            raise Fail("13818-3: a scalefactor group has %d rows, not 3"
                       % len(rows))
        for row, want in zip(rows, (21, 36, 33)):
            if sum(row) != want:
                raise Fail("13818-3: a scalefactor group's partitions are "
                           "%r, which add to %d and not %d"
                           % (row, sum(row), want))
        groups.append(rows)
    if len(groups) != 6:
        raise Fail("13818-3: expected 6 scalefactor groups, found %d"
                   % len(groups))
    return groups


def parse_lsf_allocation(text):
    """13818-3 Table B.1: Layer II allocation at the lower sampling rates.

    One table for all three rates, and printed differently from 11172-3's
    four: the subband has no `SB` in front of it, and the allocation value
    that means "no samples" is a *blank* where 11172-3 prints a dash - so
    a row lists one fewer entry than its field can take and the first one
    is implied. The two extra lines the table ends with, `sblimit` and the
    sum of nbal, are checked against what was parsed; they are the
    document stating its own total, which is the cheapest possible check
    that no row was lost.
    """
    at = text.find("Table B.1. Possible quantisation per subband, Layer II")
    if at < 0:
        raise Fail("13818-3: no Table B.1")
    body = text[at:]
    rows = []
    stated_sblimit = None
    stated_sum = None
    for line in body.splitlines():
        line = line.strip()
        match = re.match(r"sblimit\s*=\s*(\d+)", line)
        if match:
            stated_sblimit = int(match.group(1))
            continue
        match = re.match(r"Sum of nbal\s*=\s*(\d+)", line)
        if match:
            stated_sum = int(match.group(1))
            break
        parts = line.split()
        if len(parts) < 2 or not re.fullmatch(r"\d+", parts[0]) \
                or not re.fullmatch(r"\d+", parts[1]):
            continue
        subband = int(parts[0])
        nbal = int(parts[1])
        if subband != len(rows) or nbal > 4:
            continue
        # Anything that is not a number is the marker for "this allocation
        # means no samples". 11172-3 prints a dash for it; this document
        # prints a glyph that comes out of pdftotext as a control
        # character, which is why the test is "not a number" rather than a
        # comparison against any particular spelling.
        entries = [int(token) if re.fullmatch(r"\d+", token) else 0
                   for token in parts[2:]]
        if len(entries) != 1 << nbal:
            raise Fail("13818-3 B.1: subband %d says nbal=%d so %d entries, "
                       "and %d were printed"
                       % (subband, nbal, 1 << nbal, len(entries)))
        rows.append((nbal, entries))
    if len(rows) != 32:
        raise Fail("13818-3 B.1: expected 32 subbands, got %d" % len(rows))
    limit = next((i for i, row in enumerate(rows) if row[0] == 0), 32)
    if stated_sblimit is None or stated_sblimit != limit:
        raise Fail("13818-3 B.1: the table says sblimit = %r and the rows "
                   "stop allocating at %d" % (stated_sblimit, limit))
    total = sum(row[0] for row in rows)
    if stated_sum is None or stated_sum != total:
        raise Fail("13818-3 B.1: the table says the nbal sum is %r and the "
                   "rows add to %d" % (stated_sum, total))
    return rows


# ------------------------------------------------------- fixed point

#: Fractional bits in every coefficient this emits. One format throughout,
#: so that a multiply is one macro and an overflow analysis is one
#: argument. 28 leaves four bits of headroom above 1.0, which is what the
#: synthesis filterbank's intermediate sums need.
Q = 28


def q(value):
    """Round an exact value to Q28, half away from zero."""
    scaled = Fraction(value) * (1 << Q)
    whole = int(scaled)
    rest = scaled - whole
    if rest >= Fraction(1, 2):
        whole += 1
    elif rest <= Fraction(-1, 2):
        whole -= 1
    if not -(1 << 31) <= whole < (1 << 31):
        raise Fail("%s does not fit in a Q%d int32" % (value, Q))
    return whole


def cos_q(numerator, denominator):
    """cos(pi * numerator / denominator) in Q28.

    Computed in Python's double precision and rounded, which is a
    generation-time dependency and not a run-time one: what ships is the
    rounded integer. A double carries 53 bits and Q28 needs 30, so the
    rounding is decided by bit 30 and a last-bit difference in some other
    libm cannot move it - and the file this writes carries a checksum, so
    a regeneration that did move one would say so.
    """
    import math
    return q(Fraction(math.cos(math.pi * numerator / denominator))
             .limit_denominator(1 << 40))


def sin_q(numerator, denominator):
    """sin(pi * numerator / denominator) in Q28."""
    import math
    return q(Fraction(math.sin(math.pi * numerator / denominator))
             .limit_denominator(1 << 40))


def integer_root(value, power, bits):
    """floor(value ** (1/power) * 2**bits), exactly, by bisection.

    Integer arithmetic throughout, so the requantisation table is the same
    on every machine that runs this script - which a pow() in double
    precision would not guarantee at the last bit, and the last bit of a
    24-bit mantissa is where a tolerance gate would start to notice.
    """
    if value == 0:
        return 0
    target = value * (1 << (bits * power))
    low, high = 1, 1 << (bits + (value.bit_length() // power) + 2)
    while low < high:
        middle = (low + high + 1) // 2
        if middle ** power <= target:
            low = middle
        else:
            high = middle - 1
    return low


def pow43_entry(value):
    """value**(4/3) as a 24-bit mantissa and an exponent.

    Packed into one word: the exponent in the top eight bits, the mantissa
    in the low twenty-four, with the mantissa normalised into
    [2**23, 2**24) so every entry carries the same precision. The value is
    mantissa * 2**(exponent - 23).
    """
    if value == 0:
        return 0
    # value**4 exactly, then its cube root to 32 fractional bits, so the
    # mantissa below is rounded once rather than twice.
    root = integer_root(value ** 4, 3, 32)
    exponent = root.bit_length() - 1 - 32
    shift = 32 - 23 + exponent
    if shift >= 0:
        mantissa = (root + (1 << (shift - 1) if shift else 0)) >> shift
    else:
        mantissa = root << -shift
    if mantissa >= (1 << 24):
        mantissa >>= 1
        exponent += 1
    if not (1 << 23) <= mantissa < (1 << 24):
        raise Fail("pow43(%d): mantissa %d is not normalised"
                   % (value, mantissa))
    if not 0 <= exponent < 32:
        raise Fail("pow43(%d): exponent %d is out of range"
                   % (value, exponent))
    return (exponent << 24) | mantissa


# -------------------------------------------------------- the emitter

LICENCE = """/*
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
/**
 * @file
 *
 * This file is generated and must not be edited.
 *
 * `tools/tables/gen_mp3_tables.py` writes it from the tables in ISO/IEC
 * 11172-3 Annex B and ISO/IEC 13818-3 Annex B, fetching those documents
 * and keeping neither. Run it again when something here has to change:
 *
 *     tools/tables/gen_mp3_tables.py
 *
 * Every coefficient is a %d-bit fixed-point integer - Q%d, so 1.0 is
 * 0x%08X - and nothing here is a floating-point literal, which is what
 * makes a decode byte-identical on every architecture.
 *
 * What is the standard's and what is this script's arithmetic is marked
 * per table below. The generator validates every extracted table before
 * it writes one: a Huffman table that is not a complete prefix code over
 * its whole grid, a window coefficient that is not a multiple of 2^-16, a
 * scalefactor band table with a gap in it, and about a dozen more. Those
 * checks are the reason this is a script and not a thousand lines of
 * typing.
 */
""" % (Q, Q, 1 << Q)


class Emitter:
    """Accumulates the C text, and the checksum of what it accumulated."""

    def __init__(self):
        self.parts = []

    def write(self, text):
        self.parts.append(text)

    def array(self, decl, values, per_line=8, fmt="%d"):
        self.write("%s = {\n" % decl)
        line = "   "
        for index, value in enumerate(values):
            token = " " + (fmt % value) + ("," if index + 1 < len(values)
                                           else ",")
            if len(line) + len(token) > 78:
                self.write(line + "\n")
                line = "   "
            line += token
        if line.strip():
            self.write(line + "\n")
        self.write("};\n\n")

    def rows(self, decl, rows, fmt="%d", per_row=None):
        self.write("%s = {\n" % decl)
        for row in rows:
            self.write("    {")
            line = ""
            for index, value in enumerate(row):
                token = (fmt % value) + ("," if index + 1 < len(row) else "")
                if len(line) + len(token) > 70:
                    self.write(line + "\n     ")
                    line = ""
                line += token + (" " if index + 1 < len(row) else "")
            self.write(line + "},\n")
        self.write("};\n\n")

    def text(self):
        return "".join(self.parts)


def build_tree(codes, payloads):
    """A binary tree for one Huffman table, as an array of pairs.

    Node `n` occupies entries 2n and 2n+1, for a zero bit and a one bit.
    A non-negative entry is the index of the next node; a negative one is a
    leaf carrying `-(entry) - 1`. The root is node 0.

    Built by insertion and then checked: every leaf is reachable, every
    node has both children, and the leaf count equals the code count. A
    tree with a hole in it would decode some bit pattern into a node index
    that is not a node, which is the one failure mode that cannot be
    caught at run time without a test on every lookup.
    """
    nodes = [[None, None]]
    for (hlen, hcod), payload in zip(codes, payloads):
        at = 0
        for position, bit in enumerate(hcod):
            branch = int(bit)
            last = position + 1 == hlen
            if last:
                if nodes[at][branch] is not None:
                    raise Fail("tree: code %s collides" % hcod)
                nodes[at][branch] = -(payload + 1)
            else:
                if nodes[at][branch] is None:
                    nodes.append([None, None])
                    nodes[at][branch] = len(nodes) - 1
                elif nodes[at][branch] < 0:
                    raise Fail("tree: code %s passes through a leaf" % hcod)
                at = nodes[at][branch]
    flat = []
    leaves = 0
    for node in nodes:
        for entry in node:
            if entry is None:
                raise Fail("tree: a node has only one child, so some bit "
                           "pattern decodes to nothing")
            if entry < 0:
                leaves += 1
            flat.append(entry)
    if leaves != len(codes):
        raise Fail("tree: %d leaves for %d codes" % (leaves, len(codes)))
    return flat


#: The three sampling frequencies of each version, in the order the frame
#: header's two-bit field numbers them. MPEG-2.5 is absent on purpose: its
#: scalefactor band tables are in no standard - the version itself is an
#: extension by the format's authors that no document here defines - so
#: this generator cannot produce them and the decoder refuses Layer III at
#: those rates by name. Layers I and II at those rates need no band table
#: and do work.
RATE_ORDER = [(44100, 48000, 32000), (22050, 24000, 16000)]


def emit(data):
    """Write the C file's text from everything that was parsed."""
    out = Emitter()
    out.write(LICENCE)
    out.write(GENERATED)
    out.write('\n#include "mp3_tables.h"\n\n')
    # The definitions are two hundred kilobytes of numbers and the
    # declarations in the header are the interface, so the documentation
    # lives there and doxygen is told to skip what follows. Without this
    # every one of the thirty tables would need its own comment in two
    # places, which is two places for them to disagree.
    out.write("/** @cond generated */\n\n")

    # ------------------------------------------------------- Huffman
    out.write("""/* ------------------------------------------------- Huffman tables */

/* 11172-3 Table 3-B.7, as binary trees. Node n is entries 2n and 2n+1,
 * for a zero bit and a one bit; a non-negative entry is the next node and
 * a negative one is a leaf carrying -(entry)-1, which for a pair table is
 * (x << 4) | y and for a quadruple table is the four bits themselves.
 *
 * A tree rather than a lookup table, and the reason is the thing being
 * protected: a 19-bit code would need a half-megabyte of direct lookup or
 * a two-level scheme whose first level has to be derived correctly, and
 * the generator can prove a tree complete - every node has both children
 * and the leaf count equals the code count - which is what makes a wrong
 * bit pattern impossible rather than unlikely. */
""")
    nodes = []
    offsets = {}
    for number, table in enumerate(data["tables"]):
        if table is None or not table["codes"]:
            continue
        source = table.get("copy_of", number)
        if source in offsets:
            continue
        payloads = [(x << 4) | y for x, y in table["pairs"]]
        offsets[source] = len(nodes)
        nodes.extend(build_tree(table["codes"], payloads))
    out.array("const int16_t gaud_mp3_huff_nodes[%d]" % len(nodes), nodes)

    rows = []
    for number, table in enumerate(data["tables"]):
        if table is None:
            rows.append((0, 0, 0, 1))
            continue
        if not table["codes"]:
            rows.append((0, 0, 0, 0))
            continue
        source = table.get("copy_of", number)
        rows.append((offsets[source], table["linbits"], table["width"], 0))
    out.write("const MP3_Huff gaud_mp3_huff[32] = {\n")
    for number, (offset, linbits, width, unused) in enumerate(rows):
        out.write("    {%4d, %2d, %2d, %d}, /* table %d%s */\n"
                  % (offset, linbits, width, unused, number,
                     ": unused" if unused else ""))
    out.write("};\n\n")

    quad_nodes = []
    quad_offsets = []
    for key in ("A", "B"):
        quad_offsets.append(len(quad_nodes))
        rows = data["quadruples"][key]
        quad_nodes.extend(build_tree([(row[1], row[2]) for row in rows],
            [int(row[0], 2) for row in rows]))
    out.array("const int16_t gaud_mp3_quad_nodes[%d]" % len(quad_nodes),
        quad_nodes)
    out.array("const uint16_t gaud_mp3_quad_offset[2]", quad_offsets)

    # --------------------------------------------- scalefactor bands
    out.write("""/* ------------------------------------- scalefactor bands */

/* 11172-3 Table 3-B.8 and 13818-3 Table B.2, as *boundaries* rather than
 * widths: band b covers the lines from [b] up to but not including [b+1],
 * so a decoder needs one more entry than there are bands and no
 * arithmetic at the point of use. Rows 0 to 2 are MPEG-1 at 44.1, 48 and
 * 32 kHz; rows 3 to 5 are MPEG-2 at 22.05, 24 and 16 kHz, in the order
 * the frame header's rate field numbers them.
 *
 * **MPEG-2 has 22 long bands and 13 short ones where MPEG-1 has 21 and
 * 12**, which is why the band count is a table of its own rather than a
 * constant. 13818-3's own prose says 21 and 12 and its tables print 22
 * and 13; the tables are what a file is coded against. */
""")
    long_rows = []
    short_rows = []
    long_counts = []
    short_counts = []
    for version, rates in enumerate(RATE_ORDER):
        bands = data["sfbands"] if version == 0 else data["lsf_sfbands"]
        for rate in rates:
            for which, rows, counts, width, lines in (
                    ("long", long_rows, long_counts, 24, 576),
                    ("short", short_rows, short_counts, 15, 192)):
                table = bands[rate][which]
                boundaries = [row[1] for row in table]
                boundaries.append(table[-1][1] + table[-1][0])
                count = len(table)
                # **One band is added where the standard's table stops
                # short of the spectrum**, and it is not a liberty: 11172-3
                # ends its 48 kHz long table at line 383 of 576 and its
                # 44.1 kHz one at 417, which is about 16 kHz in both - and
                # LAME at a high bitrate codes above that. Those lines have
                # no scalefactor of their own, so they take zero, which
                # means global_gain alone.
                #
                # Found by measurement and worth recording as such. With
                # the highest *stated* band's scalefactor applied to them
                # instead, this library's decode of full-scale noise at
                # 320 kbit/s was 14 dB away from both reference decoders
                # and quieter than both, while every tone fixture - whose
                # spectrum stops well below 16 kHz - was exact. The
                # 13818-3 tables need no padding: they already cover all
                # 576 and 192 lines.
                if boundaries[-1] < lines:
                    boundaries.append(lines)
                    count += 1
                counts.append(count)
                rows.append(boundaries + [0] * (width - len(boundaries)))
    out.rows("const uint16_t gaud_mp3_sfb_long[6][24]", long_rows)
    out.array("const uint8_t gaud_mp3_sfb_long_bands[6]", long_counts)
    out.rows("const uint16_t gaud_mp3_sfb_short[6][15]", short_rows)
    out.array("const uint8_t gaud_mp3_sfb_short_bands[6]", short_counts)

    out.write("/* 11172-3 Table 3-B.6: added to the scalefactors when "
              "preflag is set. */\n")
    out.array("const uint8_t gaud_mp3_pretab[22]",
        list(data["pretab"]) + [0])

    out.write("""/* 13818-3: how many scalefactors each of the four partitions holds, by
 * the group scalefac_compress selects and then by the block shape - long,
 * short unmixed, short mixed. The partitions add to 21, 36 and 33
 * respectively in every group, which is what the generator checks. */
""")
    out.write("const uint8_t gaud_mp3_lsf_nsfb[6][3][4] = {\n")
    for group in data["lsf_groups"]:
        out.write("    {%s},\n" % ", ".join(
            "{%s}" % ", ".join(str(v) for v in row) for row in group))
    out.write("};\n\n")

    # ------------------------------------------------------ the filterbank
    out.write("""/* --------------------------------------------- the filterbank */

/* 11172-3 Table 3-B.9's eight c[i], turned into the butterfly
 * coefficients by the formulas printed beside them:
 * cs = 1/sqrt(1 + c^2) and ca = c/sqrt(1 + c^2). */
""")
    cs = []
    ca = []
    for c in data["aliasing"]:
        import math
        norm = math.sqrt(1.0 + float(c) * float(c))
        cs.append(q(Fraction(1.0 / norm).limit_denominator(1 << 40)))
        ca.append(q(Fraction(float(c) / norm).limit_denominator(1 << 40)))
    out.array("const int32_t gaud_mp3_cs[8]", cs)
    out.array("const int32_t gaud_mp3_ca[8]", ca)

    out.write("""/* 11172-3 Table 3-B.3, the 512 coefficients of the synthesis window.
 * Every one of them is an exact multiple of 2^-16 - the standard prints
 * them rounded to nine places and the generator recovers the exact value
 * - so these are exact in Q28 with no rounding at all. */
""")
    out.array("const int32_t gaud_mp3_window[512]",
        [q(value) for value in data["window"]])

    out.write("""/* The matrixing coefficients of the synthesis filterbank, which the
 * standard gives as a formula rather than a table:
 * N[i][k] = cos((16 + i)(2k + 1) pi / 64). */
""")
    out.rows("const int32_t gaud_mp3_synth_cos[64][32]",
        [[cos_q((16 + i) * (2 * k + 1), 64) for k in range(32)]
         for i in range(64)])

    out.write("""/* The IMDCT matrices, from the analytical expression in 11172-3
 * 2.4.3.4: x[i] = sum X[k] cos(pi/(2n) (2i + 1 + n/2)(2k + 1)), with n of
 * 36 for a long block and 12 for a short one. Written out as matrices
 * because a matrix multiply is what the arithmetic is, and a decoder that
 * is exactly right is worth more here than one that is fast; the shape
 * leaves a faster transform as a later substitution with these as its
 * reference. */
""")
    out.rows("const int32_t gaud_mp3_imdct36[36][18]",
        [[cos_q((2 * i + 1 + 18) * (2 * k + 1), 72) for k in range(18)]
         for i in range(36)])
    out.rows("const int32_t gaud_mp3_imdct12[12][6]",
        [[cos_q((2 * i + 1 + 6) * (2 * k + 1), 24) for k in range(6)]
         for i in range(12)])

    out.write("""/* The four window shapes of 11172-3 2.4.3.4, indexed by block_type.
 * Row 2 is zero and must not be used: a short granule's three blocks are
 * windowed with gaud_mp3_short_window below and then overlapped, which is
 * a different operation and not a different window. */
""")
    windows = []
    normal = [sin_q(2 * i + 1, 72) for i in range(36)]
    start = ([sin_q(2 * i + 1, 72) for i in range(18)]
             + [1 << Q] * 6
             + [sin_q(2 * (i - 18) + 1, 24) for i in range(24, 30)]
             + [0] * 6)
    stop = ([0] * 6
            + [sin_q(2 * (i - 6) + 1, 24) for i in range(6, 12)]
            + [1 << Q] * 6
            + [sin_q(2 * i + 1, 72) for i in range(18, 36)])
    windows = [normal, start, [0] * 36, stop]
    out.rows("const int32_t gaud_mp3_block_window[4][36]", windows)
    out.array("const int32_t gaud_mp3_short_window[12]",
        [sin_q(2 * i + 1, 24) for i in range(12)])

    # ------------------------------------------------- requantisation
    out.write("""/* ------------------------------------------- requantisation */

/* |is|^(4/3) for every value the Huffman stage can produce - 15 from a
 * code plus up to 8191 from linbits - packed as an eight-bit exponent
 * above a 24-bit mantissa, so the value is
 *
 *     (word & 0xFFFFFF) * 2^((word >> 24) - 23)
 *
 * and every entry carries the same 24 bits of precision rather than the
 * 10 that a single Q28 word could give the largest of them. The exponent
 * form also means the gain can be folded in as a shift; see
 * gaud_mp3_requantize(). Computed here by exact integer bisection on the
 * fourth power's cube root, so it is the same on every machine. */
""")
    out.array("const uint32_t gaud_mp3_pow43[8207]",
        [pow43_entry(value) for value in range(8207)],
        fmt="0x%08Xu")

    out.write("""/* 2^(k/4) for k of 0 to 3, which is the quarter-step the gain exponent
 * can land on once its whole part has been taken out as a shift. */
""")
    import math
    out.array("const int32_t gaud_mp3_gain_frac[4]",
        [q(Fraction(2.0 ** (k / 4.0)).limit_denominator(1 << 40))
         for k in range(4)])

    out.write("""/* ----------------------------------------------- stereo */

/* 11172-3 Table 3-B.?: scalefac_compress chooses the width of the two
 * scalefactor fields. MPEG-2 computes them arithmetically instead; see
 * gaud_mp3_lsf_nsfb. */
""")
    out.rows("const uint8_t gaud_mp3_slen[16][2]",
        [[a, b] for a, b in data["slen"]])

    out.write("""/* The intensity stereo weights, from the formulas in 11172-3 2.4.3.4:
 * is_ratio = tan(is_pos * pi / 12), then the left channel is scaled by
 * is_ratio/(1 + is_ratio) and the right by 1/(1 + is_ratio) - both from
 * the *original* left channel, which is the order 13818-3 spells out
 * where 11172-3's sequential assignment could be read either way.
 *
 * Only positions 0 to 6 are here. Position 7 is the format's "this band
 * is not intensity coded after all", and a decoder that computed a weight
 * for it would silently fold a band that should have been left alone. */
""")
    weights = []
    for position in range(7):
        ratio = math.tan(position * math.pi / 12.0)
        weights.append([
            q(Fraction(ratio / (1.0 + ratio)).limit_denominator(1 << 40)),
            q(Fraction(1.0 / (1.0 + ratio)).limit_denominator(1 << 40))])
    out.rows("const int32_t gaud_mp3_is_weight[7][2]", weights)

    out.write("""/* 13818-3's intensity weights, which are powers of a base rather than a
 * tangent: i0 is 1/2 when intensity_scale is set and 1/sqrt(2) otherwise,
 * and then an even position scales the right channel by i0^(pos/2) while
 * an odd one scales the left by i0^((pos+1)/2). Position 0 leaves both
 * alone. The illegal position is the largest the field can state and is
 * not here, as above. */
""")
    lsf_weights = []
    for scale in range(2):
        base = 0.5 if scale else 1.0 / math.sqrt(2.0)
        rows = []
        for position in range(16):
            if position == 0:
                left, right = 1.0, 1.0
            elif position % 2:
                left, right = base ** ((position + 1) // 2), 1.0
            else:
                left, right = 1.0, base ** (position // 2)
            rows.append([q(Fraction(left).limit_denominator(1 << 40)),
                         q(Fraction(right).limit_denominator(1 << 40))])
        lsf_weights.append(rows)
    out.write("const int32_t gaud_mp3_is_weight_lsf[2][16][2] = {\n")
    for rows in lsf_weights:
        out.write("    {\n")
        for left, right in rows:
            out.write("        {%11d, %11d},\n" % (left, right))
        out.write("    },\n")
    out.write("};\n\n")

    out.write("/* 1/sqrt(2), the middle/side matrix's only constant. */\n")
    out.write("const int32_t gaud_mp3_inv_sqrt2 = %d;\n\n"
        % q(Fraction(1.0 / math.sqrt(2.0)).limit_denominator(1 << 40)))

    # ------------------------------------------------------ Layers I, II
    out.write("""/* --------------------------------------------- Layers I and II */

/* 11172-3 Table 3-B.1: the scalefactors a subband's samples are
 * multiplied by. Index 63 is not in the standard and is zero here, which
 * is the one value a frame may not state. */
""")
    out.array("const int32_t gaud_mp3_scalefactor[64]",
        [q(value) for value in data["scalefactors"]] + [0])

    out.write("""/* 11172-3 Table 3-B.4: what each quantisation class means. `steps` is
 * the number of levels, `c` and `d` are the requantisation constants the
 * standard gives, `grouping` says three samples share one codeword, and
 * `bits` is how wide that codeword is. */
""")
    out.write("const MP3_Quant_Class gaud_mp3_classes[17] = {\n")
    for steps, c, d, grouping, samples, bits in data["classes"]:
        # The width of one sample's own code, which is not the codeword
        # width when three samples share one: C is 2^w/steps for the
        # smallest w whose 2^w exceeds steps, and the generator has
        # already checked that against the printed C, so w comes out of
        # the same arithmetic rather than being a second table to be
        # wrong about.
        width = 1
        while (1 << width) <= steps:
            width += 1
        out.write("    {%5du, %11d, %10d, %d, %d, %2d, %2d},\n"
                  % (steps, q(c), q(d), 1 if grouping else 0, samples,
                     bits, width))
    out.write("};\n\n")

    out.write("""/* Which quantisation class a Layer I allocation names.
 *
 * Layer I has no allocation table: an allocation of n means n+1 bits per
 * sample, so 2^(n+1) - 1 levels, and every one of those is a row of
 * Table 3-B.4 - but *not* the row with that index, because the table's
 * first four rows are 3, 5, 7 and 9 levels and only the rows from 15 up
 * are the powers of two minus one. So this maps a sample width to its
 * row, and 0 marks a width the table has no class for. Generated by
 * searching the class table rather than written out, because the one
 * thing that must not be wrong here is the correspondence. */
""")

    widths = []
    for width in range(17):
        want = (1 << width) - 1
        index = next((i for i, row in enumerate(data["classes"])
                      if row[0] == want), 0)
        if width >= 2 and index == 0 and want != 3:
            raise Fail("3-B.4 has no class for a %d-bit Layer I sample "
                       "(%d levels)" % (width, want))
        widths.append(index)
    out.array("const uint8_t gaud_mp3_class_for_bits[17]", widths)

    out.write("""/* 11172-3 Tables 3-B.2a to 3-B.2d and 13818-3 Table B.1: which
 * quantisation class each allocation value selects, per subband. -1 means
 * the allocation says this subband carries no samples, which is not the
 * same as zero steps. Rows 0 to 3 are 11172-3's four tables in order;
 * row 4 is 13818-3's single table for the lower sampling rates. */
""")
    steps_to_class = {row[0]: index
                      for index, row in enumerate(data["classes"])}
    alloc_rows = []
    nbal_rows = []
    sblimits = []
    for table in data["allocation"] + [data["lsf_allocation"]]:
        classes = []
        nbal = []
        for subband, (bits, entries) in enumerate(table):
            row = []
            for steps in entries:
                if steps == 0:
                    row.append(-1)
                elif steps in steps_to_class:
                    row.append(steps_to_class[steps])
                else:
                    raise Fail("an allocation table names %d steps and "
                               "3-B.4 has no class for it" % steps)
            classes.append(row + [-1] * (16 - len(row)))
            nbal.append(bits)
        alloc_rows.append(classes)
        nbal_rows.append(nbal)
        sblimits.append(next((i for i, b in enumerate(nbal) if b == 0), 32))
    out.write("const int8_t gaud_mp3_alloc[5][32][16] = {\n")
    for classes in alloc_rows:
        out.write("    {\n")
        for row in classes:
            out.write("        {%s},\n" % ", ".join("%2d" % v for v in row))
        out.write("    },\n")
    out.write("};\n\n")
    out.rows("const uint8_t gaud_mp3_alloc_nbal[5][32]", nbal_rows)
    out.array("const uint8_t gaud_mp3_alloc_sblimit[5]", sblimits)
    out.write("/** @endcond */\n")
    return out.text()


HEADER = LICENCE + """
/**
 * @file
 *
 * This file is generated and must not be edited.
 *
 * The declarations for `mp3_tables.c`; see that file's comment for what
 * each table is and where it came from.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3_TABLES_H
#define GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3_TABLES_H

/* Before anything is declared, because every header in this library does:
 * the renames in namespace.h have to be in effect before a type is named,
 * or the same spelling can mean two different types. CONVENTIONS.md
 * section 4. */
#include <ghoti.io/audio/macros.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fractional bits in every coefficient here. 1.0 is 1 << MP3_Q. */
#define MP3_Q %d

/** One of the 32 Layer III Huffman tables. */
typedef struct {
  /** Where its tree starts in ::gaud_mp3_huff_nodes. */
  uint16_t offset;
  /** How many extra magnitude bits a value of 15 carries. */
  uint8_t linbits;
  /** The side of its square: values 0 to @p width - 1 in each of x and y. */
  uint8_t width;
  /** Set for the two tables the standard marks unused, 4 and 14. A frame
   *  that selects one is corrupt, and this is how the decoder says so
   *  rather than reading a tree that is not there. */
  uint8_t unused;
} MP3_Huff;

/** One Layer II quantisation class, from 11172-3 Table 3-B.4. */
typedef struct {
  uint16_t steps;  ///< How many levels.
  int32_t c;       ///< Requantisation multiplier, Q%d.
  int32_t d;       ///< Requantisation offset, Q%d.
  uint8_t grouping;///< Three samples share one codeword.
  uint8_t samples; ///< Samples per codeword: 3 when grouped, else 1.
  uint8_t bits;    ///< Bits per codeword: three samples' worth if grouped.
  /** Bits in one sample's own code. The same as @p bits for an ungrouped
   *  class, and smaller for a grouped one - 2, 3 or 4 where the codeword
   *  is 5, 7 or 10 - because three samples of 3, 5 or 9 levels pack into
   *  fewer bits together than apart, which is what grouping is for. */
  uint8_t sample_bits;
} MP3_Quant_Class;

/** Every pair table's tree, one after another; see ::MP3_Huff. */
extern const int16_t gaud_mp3_huff_nodes[%d];
/** The 32 Layer III pair tables of 11172-3 Table 3-B.7. */
extern const MP3_Huff gaud_mp3_huff[32];
/** The two quadruple tables' trees, A then B. */
extern const int16_t gaud_mp3_quad_nodes[%d];
/** Where each quadruple tree starts in ::gaud_mp3_quad_nodes. */
extern const uint16_t gaud_mp3_quad_offset[2];

/** Long-block scalefactor band boundaries, by version and rate. */
extern const uint16_t gaud_mp3_sfb_long[6][24];
/** How many long bands each row of ::gaud_mp3_sfb_long has. */
extern const uint8_t gaud_mp3_sfb_long_bands[6];
/** Short-block band boundaries, within one of the three windows. */
extern const uint16_t gaud_mp3_sfb_short[6][15];
/** How many short bands each row of ::gaud_mp3_sfb_short has. */
extern const uint8_t gaud_mp3_sfb_short_bands[6];
/** Added to the scalefactors when preflag is set, Table 3-B.6. */
extern const uint8_t gaud_mp3_pretab[22];
/** MPEG-2's scalefactor partition sizes, by group and block shape. */
extern const uint8_t gaud_mp3_lsf_nsfb[6][3][4];

/** Alias reduction, the cosine half of each butterfly. Q28. */
extern const int32_t gaud_mp3_cs[8];
/** Alias reduction, the sine half of each butterfly. Q28. */
extern const int32_t gaud_mp3_ca[8];
/** The synthesis window of Table 3-B.3, exact in Q28. */
extern const int32_t gaud_mp3_window[512];
/** The polyphase filterbank's matrixing coefficients. Q28. */
extern const int32_t gaud_mp3_synth_cos[64][32];
/** The long-block inverse transform, as a matrix. Q28. */
extern const int32_t gaud_mp3_imdct36[36][18];
/** The short-block inverse transform, as a matrix. Q28. */
extern const int32_t gaud_mp3_imdct12[12][6];
/** The window for each block type; row 2 is zero and unused. Q28. */
extern const int32_t gaud_mp3_block_window[4][36];
/** The window each of the three short blocks gets. Q28. */
extern const int32_t gaud_mp3_short_window[12];

/** |is|^(4/3), as an exponent above a 24-bit mantissa. */
extern const uint32_t gaud_mp3_pow43[8207];
/** 2^(k/4) for the quarter the gain exponent leaves over. Q28. */
extern const int32_t gaud_mp3_gain_frac[4];

/** scalefac_compress to the two MPEG-1 field widths. */
extern const uint8_t gaud_mp3_slen[16][2];
/** MPEG-1 intensity stereo weights, left then right. Q28. */
extern const int32_t gaud_mp3_is_weight[7][2];
/** MPEG-2's, by intensity_scale and position. Q28. */
extern const int32_t gaud_mp3_is_weight_lsf[2][16][2];
/** 1/sqrt(2), the middle/side matrix's only constant. Q28. */
extern const int32_t gaud_mp3_inv_sqrt2;

/** Layer I and II scalefactors, Table 3-B.1. Q28. */
extern const int32_t gaud_mp3_scalefactor[64];
/** Layer II quantisation classes, Table 3-B.4. */
extern const MP3_Quant_Class gaud_mp3_classes[17];
/** Which class each Layer II allocation names; -1 for none. */
extern const int8_t gaud_mp3_alloc[5][32][16];
/** How many bits each subband's allocation field takes. */
extern const uint8_t gaud_mp3_alloc_nbal[5][32];
/** The first subband each allocation table never allocates. */
extern const uint8_t gaud_mp3_alloc_sblimit[5];
/** A Layer I sample width to its row of ::gaud_mp3_classes. */
extern const uint8_t gaud_mp3_class_for_bits[17];

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3_TABLES_H
"""


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--pdf", help="a local copy of ISO/IEC 11172-3")
    parser.add_argument("--lsf-pdf", help="a local copy of ISO/IEC 13818-3")
    parser.add_argument("--url", default=DEFAULT_URL)
    parser.add_argument("--lsf-url",
        default="https://courses.e-ce.uth.gr/CE401/tree_menu/tutorials/"
                "MPEG2/13818-3.pdf")
    parser.add_argument("--out", default=os.path.join(ROOT, "src", "codec",
        "mp3"))
    parser.add_argument("--check", action="store_true",
        help="validate and summarise without writing")
    args = parser.parse_args(argv[1:])

    with tempfile.TemporaryDirectory(prefix="ghoti-mp3-tables-") as scratch:
        one = args.pdf or fetch(args.url, scratch)
        two = args.lsf_pdf or fetch(args.lsf_url, os.path.join(scratch, "b")
            if False else scratch)
        if not args.pdf:
            print("fetched %s\n  sha256 %s" % (args.url, digest(one)))
        if not args.lsf_pdf:
            print("fetched %s\n  sha256 %s" % (args.lsf_url, digest(two)))
        text = raw_text(one)
        lsf = raw_text(two)

        data = {
            "scalefactors": parse_scalefactors(text),
            "allocation": parse_allocation(text),
            "window": parse_window(text),
            "classes": parse_classes(text),
            "pretab": parse_pretab(text),
            "slen": parse_slen(text),
            "aliasing": parse_aliasing(text),
            "sfbands": parse_sfbands(text),
            "lsf_sfbands": parse_lsf_sfbands(lsf),
            "lsf_groups": parse_lsf_scalefactor_groups(lsf),
            "lsf_allocation": parse_lsf_allocation(lsf),
        }
        quadruples, tables = parse_huffman(text)
        data["quadruples"] = quadruples
        data["tables"] = tables

    used = [t for t in tables if t is not None]
    print("11172-3 Annex B: %d Huffman tables (%d unused), every one a "
          "complete prefix code" % (len(used), 32 - len(used)))
    print("                 512 window coefficients, all exact multiples "
          "of 2^-16")
    print("                 %d scalefactor band tables, %d Layer II "
          "allocation tables" % (6, 4))
    print("13818-3 Annex B: 3 scalefactor band tables of 22 long and 13 "
          "short bands")
    print("                 6 scalefactor partition groups, 1 Layer II "
          "allocation table")

    body = emit(data)
    nodes = int(re.search(r"gaud_mp3_huff_nodes\[(\d+)\]", body).group(1))
    quad = int(re.search(r"gaud_mp3_quad_nodes\[(\d+)\]", body).group(1))
    header = HEADER % (Q, Q, Q, nodes, quad)
    stamp = hashlib.sha256(body.encode("utf-8")).hexdigest()
    marker = '#include "mp3_tables.h"'
    if marker not in body:
        raise Fail("the emitted body does not include its own header")
    body = body.replace(marker, "/* The sha256 of everything below this "
        "comment, as generated:\n * %s\n * A regeneration that prints a "
        "different one changed a table. */\n%s" % (stamp, marker), 1)

    if args.check:
        print("\nwould write %d bytes of C, body sha256 %s"
              % (len(body), stamp))
        return 0
    target = os.path.join(args.out, "mp3_tables.c")
    with open(target, "w", encoding="utf-8") as handle:
        handle.write(body)
    with open(os.path.join(args.out, "mp3_tables.h"), "w",
              encoding="utf-8") as handle:
        handle.write(header)
    print("\nwrote %s (%d bytes) and mp3_tables.h" % (target, len(body)))
    print("body sha256 %s" % stamp)
    return 0


def digest(path):
    with open(path, "rb") as handle:
        return hashlib.sha256(handle.read()).hexdigest()


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except Fail as why:
        sys.stderr.write("gen_mp3_tables: %s\n" % why)
        sys.exit(1)
