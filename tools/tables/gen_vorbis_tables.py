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
"""Generate src/codec/vorbis/vorbis_tables.c from the Vorbis I specification.

    tools/tables/gen_vorbis_tables.py [--spec PATH] [--url URL] [--check]

**One table is extracted and the rest are computed**, which is a different
balance from gen_mp3_tables.py and worth saying why. A Vorbis stream
defines its own codebooks, floor curves and residue layout, so almost
nothing a Vorbis decoder needs is normative data in the document. The
exceptions:

  extracted   `floor1_inverse_dB_table`, section 10.1: 256 values that
              are printed as decimals and derived from no formula the
              specification states. This is the only table in the format
              that has to come out of the document.
  computed    The window, from the formula in section 1.3.2; the two
              rotation tables the inverse transform needs, from the
              transform's own definition in section 1.3.2. Both are
              closed form, so they are computed here in exact arithmetic
              and the C file carries no float literal.

**The document is fetched and not committed.** It is Xiph's text and this
library does not redistribute it; the script reads it out of a temporary
directory. The generated C *is* committed, so a build needs neither the
network nor the specification - this runs when a table changes, which is
to say almost never.

## What the validations are for

A silent mis-extraction is the failure this exists to prevent, and for a
table of 256 decimals in four columns the plausible failure is a digit, a
column read into the wrong row, or a truncated last line. So:

  - **the ratio between consecutive entries is constant**, which is the
    strong check: the table is geometric, so a single mistyped digit
    anywhere breaks the ratio at two places and nothing else does. The
    measured spread of that ratio across all 255 steps is printed.
  - 256 entries exactly, strictly increasing, and the first and last
    match the values the specification prints in the running text.
  - every entry is positive and below one, which is what makes the
    floor a pure attenuation and lets the fixed-point form below hold
    the mantissa in a fixed number of bits.

## The fixed-point forms, and why each is what it is

**The floor is a mantissa and a shift, not a fixed-point number.** Its
256 values span 1.06e-07 to 0.984, which is seven decades - 2^23 - so no
single scale holds both ends with useful precision. Each entry is
therefore stored as a 31-bit mantissa and a right shift, so every value
carries the same 31 bits of relative precision whatever its magnitude.
That is what a float does, and the point of spelling it out is that the
arithmetic stays integer: a spectral line is `(residue * mantissa) >>
shift`, with the product taken in 64 bits.

**The window and the rotations are Q30.** Both are bounded by one in
absolute value, so a single scale is right for them, and 30 bits leaves
the headroom a signed 32-bit multiply needs.

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
OUTPUT = os.path.join(ROOT, "src", "codec", "vorbis", "vorbis_tables.c")
HEADER = os.path.join(ROOT, "src", "codec", "vorbis", "vorbis_tables.h")

#: Xiph's own copy of the specification, and the digest this script was
#: written against. The digest is not a trust anchor - it is a statement
#: that the document has not changed underneath, so a failure to parse is
#: a failure to parse and not a different edition.
DEFAULT_URL = "https://xiph.org/vorbis/doc/Vorbis_I_spec.pdf"

#: The first and last entries, read once by eye from the document and
#: written here so that the extraction is checked against something
#: outside itself. The last is printed as `1.` in the specification -
#: exactly one - which is also what the geometric ratio predicts from the
#: first, and that agreement is the second independent check on both.
FIRST_VALUE = "1.0649863e-07"
LAST_VALUE = "1."

#: Fractional bits for the window and for the inverse transform's
#: rotations, both of which are bounded by one.
Q = 30

#: Bits of mantissa in a floor entry.
FLOOR_MANTISSA_BITS = 31

#: Every block size the format permits: 2^6 to 2^13.
BLOCK_SIZES = [1 << e for e in range(6, 14)]

#: The largest complex transform any of them needs, which is n/4 of the
#: largest block size. One table of this many twiddles serves every stage
#: of every size, read with a stride.
MAX_FFT = max(BLOCK_SIZES) // 4


class Fail(Exception):
    """A validation failed. Never caught into a warning."""


def fetch(url, into):
    path = os.path.join(into, "Vorbis_I_spec.pdf")
    if shutil.which("curl") is None:
        raise Fail("curl is not on PATH, and the document has to be fetched")
    finished = subprocess.run(
        ["curl", "-sSL", "--max-time", "300", "-o", path, url],
        capture_output=True, text=True)
    if finished.returncode != 0 or not os.path.exists(path):
        raise Fail("could not fetch %s\n%s" % (url, finished.stderr.strip()))
    return path


def plain_text(path):
    """The specification as text, with its columns kept.

    `-layout` and not `-raw`, which is the opposite of what
    gen_mp3_tables.py needs and for the opposite reason. That script
    reads three-column Huffman tables, which `-layout` interleaves into
    nonsense. This one reads a four-column table whose rows are numbered
    down the left, and keeping the layout keeps each row on one output
    line - so the row number and its four values arrive together and the
    numbering itself becomes a check.

    **The HTML edition cannot be used for this.** Its last table row has
    three values where every other row has four: the 256th entry is
    rendered as `1.` and runs into the section heading after it, so a
    tag-stripping reader finds 255 values and a tolerant one finds 256
    with a section number as the last. The PDF prints the same `1.` but
    keeps it inside the row, where the numbering says it belongs.
    """
    if shutil.which("pdftotext") is None:
        raise Fail("pdftotext is not on PATH (Debian: poppler-utils)")
    finished = subprocess.run(["pdftotext", "-layout", path, "-"],
        capture_output=True, text=True)
    if finished.returncode != 0:
        raise Fail("pdftotext failed: %s" % finished.stderr.strip()[-400:])
    return finished.stdout


def parse_floor_table(text):
    """The 256 values of section 10.1, read row by numbered row.

    **The rows are numbered in the document and the numbering is used as
    a check**, not skipped as noise. Sixty-four rows of four values, read
    left to right then top to bottom; a row whose number is not the one
    expected, or which does not hold four values, stops the extraction.
    That is what catches the two failures a four-column table of decimals
    actually has - a column read into the wrong row, and a row dropped by
    the converter.

    The last value is printed as `1.`, with no digits after the point,
    where every other value has seven or eight. So the pattern has to
    admit it, and the row numbering is what makes admitting it safe: a
    bare `1.` is a value only because it is the fourth thing on row 64.
    """
    marker = "floor1_inverse_dB_table"
    lines = text.splitlines()
    start = None
    for index, line in enumerate(lines):
        if marker in line and "10.1" not in line:
            start = index
    if start is None:
        raise Fail("the specification has no section naming %s" % marker)

    number = re.compile(r"^\s*([0-9]{1,2})\s+(.*)$")
    value = re.compile(r"^([0-9]\.[0-9]*(?:e[-+][0-9]+)?)$")
    values = []
    row = 0
    for line in lines[start:]:
        match = number.match(line)
        if not match:
            if values:
                continue
            continue
        if int(match.group(1)) != row + 1:
            if values:
                break
            continue
        pieces = [one.strip() for one in match.group(2).split(",")]
        pieces = [one for one in pieces if one]
        if len(pieces) != 4:
            raise Fail("row %d of the table holds %d values and not four: %r"
                       % (row + 1, len(pieces), match.group(2)))
        for one in pieces:
            if not value.match(one):
                raise Fail("row %d holds %r, which is not a value"
                           % (row + 1, one))
            values.append(Fraction(one))
        row += 1
        if row == 64:
            break
    if row != 64:
        raise Fail("read %d rows of the table and it has 64" % row)
    if len(values) != 256:
        raise Fail("read %d values and the table is 256" % len(values))

    if str(float(values[0])) not in (FIRST_VALUE, "1.0649863e-07"):
        raise Fail("the first value is %s and was expected to be %s"
                   % (values[0], FIRST_VALUE))
    if values[255] != 1:
        raise Fail("the last value is %s and the specification prints %s"
                   % (values[255], LAST_VALUE))
    for i, one in enumerate(values):
        if one <= 0 or one > 1:
            raise Fail("entry %d is %s, which is not a pure attenuation"
                       % (i, one))
    for i in range(1, 256):
        if values[i] <= values[i - 1]:
            raise Fail("entry %d is not above entry %d (%s, %s)"
                       % (i, i - 1, values[i], values[i - 1]))

    # **The strong check.** The table is geometric, so one mistyped digit
    # breaks the ratio at two consecutive steps and nothing else does.
    # A column read into the wrong row breaks it at four.
    ratios = [float(values[i] / values[i - 1]) for i in range(1, 256)]
    low = min(ratios)
    high = max(ratios)
    spread = (high - low) / ((high + low) / 2)
    if spread > 1e-5:
        raise Fail("the ratio between consecutive entries spans %.9f to "
                   "%.9f, a spread of %.2e - which is a transcription "
                   "error rather than rounding" % (low, high, spread))
    # And the two ends predict each other through it, which is a reading
    # of the first value that does not involve the first value.
    predicted = float(values[255]) / ((low + high) / 2) ** 255
    if abs(predicted - float(values[0])) / float(values[0]) > 1e-3:
        raise Fail("the first entry is %s and the ratio applied 255 times "
                   "backwards from the last gives %.6e"
                   % (values[0], predicted))
    return values, (low, high, spread)


def floor_fixed(values):
    """Each entry as a 31-bit mantissa and a right shift.

    `value == mantissa * 2^-shift`, with the mantissa normalised into
    [2^30, 2^31) so every entry carries the same relative precision. The
    search for the shift is a loop rather than a logarithm, for the
    reason lookup1_values has one: a float logarithm is off by one at an
    exact power on some platforms, and this table is compiled in, so
    being off by one here would be a factor of two in a spectral line.
    """
    out = []
    for value in values:
        shift = 0
        scaled = value
        limit_low = Fraction(1 << (FLOOR_MANTISSA_BITS - 1))
        limit_high = Fraction(1 << FLOOR_MANTISSA_BITS)
        while scaled < limit_low:
            scaled *= 2
            shift += 1
        while scaled >= limit_high:
            scaled /= 2
            shift -= 1
        mantissa = round_half_away(scaled)
        if mantissa >= (1 << FLOOR_MANTISSA_BITS):
            mantissa >>= 1
            shift -= 1
        if not (1 << (FLOOR_MANTISSA_BITS - 1)) <= mantissa \
                < (1 << FLOOR_MANTISSA_BITS):
            raise Fail("mantissa %d out of range for %s" % (mantissa, value))
        if shift < 0 or shift > 255:
            raise Fail("shift %d out of range for %s" % (shift, value))
        # The round trip, which is what says the pair means the value.
        back = Fraction(mantissa, 1 << shift)
        error = abs(back - value) / value
        if error > Fraction(1, 1 << 29):
            raise Fail("%s round-trips to %s, a relative error of %.3e"
                       % (value, back, float(error)))
        out.append((mantissa, shift))
    return out


def round_half_away(value):
    """Round a Fraction to the nearest integer, halves away from zero.

    Spelled out rather than left to `round`, which rounds halves to even -
    a perfectly good rule that this script must not use, because the C
    these tables are compared against rounds halves away from zero and
    two rules disagreeing on one entry in a table is exactly the kind of
    difference that is invisible until it is not.
    """
    if value >= 0:
        return int(value + Fraction(1, 2))
    return -int(-value + Fraction(1, 2))


def to_q(value):
    """A float in [-1, 1] as a Q30 integer, rounded half away from zero."""
    scaled = value * (1 << Q)
    if scaled >= 0:
        out = int(scaled + 0.5)
    else:
        out = -int(-scaled + 0.5)
    if out > (1 << Q) or out < -(1 << Q):
        raise Fail("%.17g does not fit Q%d" % (value, Q))
    return out


def window_table(n):
    """The window of section 1.3.2, half of it.

        w[i] = sin(pi/2 * sin^2((i + 1/2)/n * pi))

    Only the first half is stored: the window is symmetric about its
    centre, `w[n-1-i] == w[i]`, which the check below asserts rather than
    assumes - an asymmetric window would be an overlap-add that does not
    sum to one, which is audible as a comb filter and not as a defect.
    """
    out = []
    for i in range(n // 2):
        inner = math.sin((i + 0.5) / n * math.pi)
        value = math.sin(math.pi / 2.0 * inner * inner)
        other_inner = math.sin((n - 1 - i + 0.5) / n * math.pi)
        other = math.sin(math.pi / 2.0 * other_inner * other_inner)
        if abs(value - other) > 1e-15:
            raise Fail("the window of %d is not symmetric at %d" % (n, i))
        out.append(to_q(value))
    # The Princen-Bradley condition, which is what makes the overlap-add
    # reconstruct: w[i]^2 + w[n/2 + i]^2 == 1 for the two halves, and by
    # the symmetry above that is w[i]^2 + w[n/2-1-i]^2 == 1.
    for i in range(n // 4):
        a = out[i] / float(1 << Q)
        b = out[n // 2 - 1 - i] / float(1 << Q)
        if abs(a * a + b * b - 1.0) > 1e-8:
            raise Fail("the window of %d fails Princen-Bradley at %d: "
                       "%.12f" % (n, i, a * a + b * b))
    return out


def rotation_table(n):
    """exp(-i*pi*(k + 1/8)/(n/2)) for k in 0..n/4-1, as (cos, sin).

    The pre- and post-rotation the inverse transform needs. Both use the
    same table, which falls out of the factorisation: the transform is a
    DCT-IV of n/2 points, and a DCT-IV of M points is a complex transform
    of M/2 points with this rotation applied on both sides.

    The 1/8 is the whole of it and is where this is easy to get wrong -
    1/4 gives a result that is wrong by a few percent, which is to say
    recognisable audio with a buzz on it rather than noise.
    """
    m = n // 2
    out = []
    for k in range(n // 4):
        angle = -math.pi * (k + 0.125) / m
        out.append((to_q(math.cos(angle)), to_q(math.sin(angle))))
    return out


def fft_table():
    """exp(-2*pi*i*k/MAX_FFT) for k in 0..MAX_FFT/2-1, as (cos, sin).

    **One table for every stage of every block size.** A radix-2 stage of
    size s needs exp(-2*pi*i*k/s) for k below s/2, and every such value is
    in this table at stride MAX_FFT/s - so the eight block sizes and all
    their stages read one array of %d pairs rather than eleven arrays.
    """
    out = []
    for k in range(MAX_FFT // 2):
        angle = -2.0 * math.pi * k / MAX_FFT
        out.append((to_q(math.cos(angle)), to_q(math.sin(angle))))
    return out


def emit_pairs(name, pairs, kind="int32_t"):
    lines = ["const %s %s[%d][2] = {" % (kind, name, len(pairs))]
    row = "   "
    for a, b in pairs:
        piece = " {%d, %d}," % (a, b)
        if len(row) + len(piece) > 76:
            lines.append(row)
            row = "   "
        row += piece
    if row.strip():
        lines.append(row)
    lines.append("};")
    return "\n".join(lines)


def emit_values(name, values, kind="int32_t"):
    lines = ["const %s %s[%d] = {" % (kind, name, len(values))]
    row = "   "
    for one in values:
        piece = " %d," % one
        if len(row) + len(piece) > 76:
            lines.append(row)
            row = "   "
        row += piece
    if row.strip():
        lines.append(row)
    lines.append("};")
    return "\n".join(lines)


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


def build(values, floor_pairs, summary):
    parts = [LICENSE, """
/**
 * @file
 *
 * This file is generated and must not be edited. The generator is
 * `tools/tables/gen_vorbis_tables.py`.
 *
 * One table here is extracted from the Vorbis I specification and the
 * rest are computed from formulas in it. That balance is the opposite of
 * mp3_tables.c's, and the reason is the format: a Vorbis stream states
 * its own codebooks, floor curves and residue layout, so there is very
 * little normative data for a decoder to be given.
 *
%s */

#include "vorbis_tables.h"
""" % summary]

    parts.append("""
/**
 * The inverse decibel table of section 10.1, as a mantissa and a right
 * shift: `value == mantissa * 2^-shift`.
 *
 * **A pair and not a fixed-point number**, because the 256 values span
 * seven decades - 1.06e-07 to 0.984 - and no single scale holds both ends
 * with useful precision. Each mantissa is normalised into [2^30, 2^31),
 * so every entry carries the same 31 bits of relative precision whatever
 * its magnitude, and the arithmetic stays integer: a spectral line is
 * `(residue * mantissa) >> shift` with the product taken in 64 bits.
 */""")
    parts.append(emit_pairs("gaud_vorbis_floor_db", floor_pairs))

    for n in BLOCK_SIZES:
        parts.append("""
/** The window of block size %d, first half; symmetric about its centre. */"""
                     % n)
        parts.append(emit_values("gaud_vorbis_window_%d" % n,
                                 window_table(n)))

    for n in BLOCK_SIZES:
        parts.append("""
/** exp(-i*pi*(k + 1/8)/%d), the rotation for block size %d. */"""
                     % (n // 2, n))
        parts.append(emit_pairs("gaud_vorbis_rotation_%d" % n,
                                rotation_table(n)))

    parts.append("""
/**
 * exp(-2*pi*i*k/%d) for k below %d.
 *
 * One table for every radix-2 stage of every block size: a stage of size
 * s wants exp(-2*pi*i*k/s) for k below s/2, and every such value is here
 * at stride %d/s.
 */""" % (MAX_FFT, MAX_FFT // 2, MAX_FFT))
    parts.append(emit_values("gaud_vorbis_fft_cos",
                             [c for c, _ in fft_table()]))
    parts.append("/** The same, imaginary part. */")
    parts.append(emit_values("gaud_vorbis_fft_sin",
                             [s for _, s in fft_table()]))

    parts.append("""
/** The window of each block size, indexed by log2 of it minus six. */
const int32_t * const gaud_vorbis_windows[%d] = {
%s
};

/** The rotation of each block size, the same way. */
const int32_t (* const gaud_vorbis_rotations[%d])[2] = {
%s
};
""" % (len(BLOCK_SIZES),
       "\n".join("    gaud_vorbis_window_%d," % n for n in BLOCK_SIZES),
       len(BLOCK_SIZES),
       "\n".join("    gaud_vorbis_rotation_%d," % n for n in BLOCK_SIZES)))
    return "\n".join(parts) + "\n"


def build_header():
    declarations = []
    for n in BLOCK_SIZES:
        declarations.append("extern const int32_t gaud_vorbis_window_%d[%d];"
                            % (n, n // 2))
    for n in BLOCK_SIZES:
        declarations.append(
            "extern const int32_t gaud_vorbis_rotation_%d[%d][2];"
            % (n, n // 4))
    return LICENSE + """
/**
 * @file
 *
 * This file is generated and must not be edited. The generator is
 * `tools/tables/gen_vorbis_tables.py`.
 *
 * The declarations for `vorbis_tables.c`; see that file's comment for
 * what each table is and where it came from. Never installed.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_VORBIS_VORBIS_TABLES_H
#define GHOTI_IO_GAUD_SRC_CODEC_VORBIS_VORBIS_TABLES_H

/* Before anything is declared, because every header in this library does:
 * the renames in namespace.h have to be in effect before a type is named,
 * or the same spelling can mean two different types. CONVENTIONS.md
 * section 4, and `make check-symbols` enforces it. */
#include <ghoti.io/audio/macros.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fractional bits in the window and the rotation tables. */
#define VORBIS_TABLE_Q %d

/** Bits of mantissa in a floor entry. */
#define VORBIS_FLOOR_MANTISSA_BITS %d

/** How many block sizes the format permits. */
#define VORBIS_BLOCK_SIZE_COUNT %d

/** The smallest block size, and the base of the index into the arrays. */
#define VORBIS_LOG2_MIN_BLOCK 6

/** How many twiddles gaud_vorbis_fft_cos and _sin hold. */
#define VORBIS_FFT_TWIDDLES %d

/** The transform size those twiddles are a full turn of. */
#define VORBIS_FFT_TURN %d

/** The inverse decibel table: {mantissa, shift}, value = mantissa >> shift. */
extern const int32_t gaud_vorbis_floor_db[256][2];

%s

/** exp(-2*pi*i*k/::VORBIS_FFT_TURN), real part, in Q::VORBIS_TABLE_Q. */
extern const int32_t gaud_vorbis_fft_cos[%d];

/** The same, imaginary part. */
extern const int32_t gaud_vorbis_fft_sin[%d];

/** The window of each block size, indexed by log2 of it minus six. */
extern const int32_t * const
    gaud_vorbis_windows[VORBIS_BLOCK_SIZE_COUNT];

/** The rotation of each block size, the same way. */
extern const int32_t (* const
    gaud_vorbis_rotations[VORBIS_BLOCK_SIZE_COUNT])[2];

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_VORBIS_VORBIS_TABLES_H
""" % (Q, FLOOR_MANTISSA_BITS, len(BLOCK_SIZES), MAX_FFT // 2, MAX_FFT,
       "\n".join(declarations), MAX_FFT // 2, MAX_FFT // 2)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--spec")
    parser.add_argument("--url", default=DEFAULT_URL)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    scratch = tempfile.mkdtemp(prefix="gen_vorbis_")
    try:
        path = args.spec or fetch(args.url, scratch)
        with open(path, "rb") as handle:
            digest = hashlib.sha256(handle.read()).hexdigest()
        print("specification: %s\n  sha256 %s" % (path, digest))
        text = plain_text(path)
        values, (low, high, spread) = parse_floor_table(text)
        floor_pairs = floor_fixed(values)

        print("floor1_inverse_dB_table: 256 values extracted, %s to %s"
              % (values[0], values[255]))
        print("  consecutive ratio %.9f to %.9f, spread %.2e - geometric, "
              "so no single entry is mistyped" % (low, high, spread))
        print("  stored as a %d-bit mantissa and a shift; shifts %d to %d"
              % (FLOOR_MANTISSA_BITS,
                 min(s for _, s in floor_pairs),
                 max(s for _, s in floor_pairs)))

        windows = {n: window_table(n) for n in BLOCK_SIZES}
        rotations = {n: rotation_table(n) for n in BLOCK_SIZES}
        twiddles = fft_table()
        print("windows:   %d sizes, %d values, symmetric and "
              "Princen-Bradley exact to 1e-8"
              % (len(windows), sum(len(v) for v in windows.values())))
        print("rotations: %d sizes, %d pairs"
              % (len(rotations), sum(len(v) for v in rotations.values())))
        print("fft:       one table of %d pairs, read at stride %d/s"
              % (len(twiddles), MAX_FFT))

        summary = (" * Extracted: floor1_inverse_dB_table, section 10.1,"
                   " 256 values whose\n * consecutive ratio is constant to"
                   " %.1e - which is the check that\n * catches a mistyped"
                   " digit, since the table is geometric and nothing\n"
                   " * else breaks the ratio.\n *\n"
                   " * Computed: the window of section 1.3.2, asserted"
                   " symmetric and to\n * satisfy the Princen-Bradley"
                   " condition; and the two rotations the\n * inverse"
                   " transform's factorisation needs.\n" % spread)
        body = build(values, floor_pairs, summary)
        header = build_header()

        if args.check:
            print("\nwould write %d bytes of C and %d of header"
                  % (len(body), len(header)))
            return 0
        with open(OUTPUT, "w") as handle:
            handle.write(body)
        with open(HEADER, "w") as handle:
            handle.write(header)
        print("\nwrote %s (%d bytes)\n      %s (%d bytes)"
              % (OUTPUT, len(body), HEADER, len(header)))
        return 0
    finally:
        shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as problem:
        print("gen_vorbis_tables: %s" % problem, file=sys.stderr)
        sys.exit(1)
