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
"""Generate src/codec/mp3/mp3enc_tables.c for the MP3 encoder.

    tools/tables/gen_mp3enc_tables.py [--check]

**Nothing here is read from a standard.** The decoder's tables are the
standard's, extracted once by gen_mp3_tables.py and committed; this script
starts from them, and derives what an *encoder* needs and a decoder does
not:

  - **Huffman code words.** The decoder holds each table as a tree to be
    walked. The encoder needs the inverse, a (value pair) to (code, length)
    lookup, and the way to get it that cannot disagree with the decoder is
    to walk the decoder's own trees. Every table is then decoded back
    through the same trees before it is written, and the unit test does
    it again through the shipped decoder.
  - **The analysis matrix**, cos((2k+1)(i-16) pi/64), which 11172-3 gives
    as a formula in Annex C.
  - **The quantiser's decision thresholds**, (q + 0.5946)^(4/3), so that
    "which integer does this value round to" is a search in an integer
    table and not a floating-point power. 0.5946 is 1 - 0.4054, the
    rounding offset every ISO-derived encoder uses for x^(3/4).
  - **2^(-k/4)**, the reciprocals of the decoder's gain fractions.

As in the decoder's generator, the arithmetic is Python's and happens
when a table changes. What ships is rounded integers, so the encoder has
no floating point in it and its output is the same on every machine.
"""

import math
import os
import re
import sys
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SRC = os.path.join(ROOT, "src", "codec", "mp3", "mp3_tables.c")
OUT = os.path.join(ROOT, "src", "codec", "mp3", "mp3enc_tables.c")
HDR = os.path.join(ROOT, "src", "codec", "mp3", "mp3enc_tables.h")

Q = 28
THRESHOLD_OFFSET = Fraction(5946, 10000)
QUANT_MAX = 8206


class Fail(Exception):
    pass


def array(text, name):
    """The integers of `name[...] = { ... };` in mp3_tables.c."""
    m = re.search(r"\b" + re.escape(name) + r"\[[^\]]*\](?:\[[^\]]*\])?\s*=\s*\{(.*?)\n\};",
                  text, re.S)
    if not m:
        raise Fail("no array named " + name)
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    return [int(v) for v in re.findall(r"-?\d+", body)]


def huff_rows(text):
    m = re.search(r"gaud_mp3_huff\[32\]\s*=\s*\{(.*?)\n\};", text, re.S)
    rows = []
    for line in m.group(1).splitlines():
        r = re.match(r"\s*\{\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\}", line)
        if r:
            rows.append(tuple(int(v) for v in r.groups()))
    if len(rows) != 32:
        raise Fail("expected 32 Huffman rows, found %d" % len(rows))
    return rows


def walk(nodes, offset, path, out):
    """Every leaf under `offset`: payload -> (code, length)."""
    stack = [(offset, 0, 0)]
    while stack:
        at, code, length = stack.pop()
        for bit in (0, 1):
            entry = nodes[at + bit]
            c = (code << 1) | bit
            if entry < 0:
                payload = -entry - 1
                if payload in out:
                    raise Fail("payload %d reached twice" % payload)
                out[payload] = (c, length + 1)
            else:
                stack.append((offset + 2 * entry, c, length + 1))


def decode(nodes, offset, code, length):
    at = offset
    for i in range(length):
        bit = (code >> (length - 1 - i)) & 1
        entry = nodes[at + bit]
        if entry < 0:
            return -entry - 1 if i == length - 1 else None
        at = offset + 2 * entry
    return None


def q28(value):
    scaled = Fraction(value) * (1 << Q)
    whole = int(scaled)
    rest = scaled - whole
    if rest >= Fraction(1, 2):
        whole += 1
    elif rest <= Fraction(-1, 2):
        whole -= 1
    return whole


def cos_q(num, den):
    return q28(Fraction(math.cos(math.pi * num / den)).limit_denominator(1 << 40))


def integer_root(value, power, bits):
    """floor(value ** (1/power) * 2**bits), exactly."""
    target = value << (power * bits)
    lo, hi = 0, 1 << ((target.bit_length() + power - 1) // power + 1)
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if mid ** power <= target:
            lo = mid
        else:
            hi = mid - 1
    return lo


def threshold(i):
    """(i + 0.5946)^(4/3) in Q28, rounded down: the smallest value that
    quantises to i + 1. Exact rational arithmetic: the fourth power of the
    base, then a cube root."""
    base = Fraction(i) + THRESHOLD_OFFSET
    fourth = base ** 4
    # cube root of a rational, to Q28: scale into an integer first.
    scaled = fourth * (1 << (3 * Q))
    n = scaled.numerator // scaled.denominator
    return integer_root(n, 3, 0)


def inverse_gain(k):
    """2 ** (-k/4) in Q28."""
    value = Fraction(math.pow(2.0, -k / 4.0)).limit_denominator(1 << 40)
    return q28(value)


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


def emit_array(lines, decl, values, per_line=10):
    lines.append(decl + " = {")
    for i in range(0, len(values), per_line):
        lines.append("    " + ", ".join(str(v) for v in values[i:i + per_line]) + ",")
    lines.append("};\n")


def emit_rows(lines, decl, rows, per_line=8):
    lines.append(decl + " = {")
    for row in rows:
        lines.append("    {")
        for i in range(0, len(row), per_line):
            lines.append("        " + ", ".join(str(v) for v in row[i:i + per_line]) + ",")
        lines.append("    },")
    lines.append("};\n")


def build():
    text = open(SRC).read()
    nodes = array(text, "gaud_mp3_huff_nodes")
    quad_nodes = array(text, "gaud_mp3_quad_nodes")
    quad_offset = array(text, "gaud_mp3_quad_offset")
    gain_frac = array(text, "gaud_mp3_gain_frac")
    rows = huff_rows(text)

    code, length, start = [], [], []
    for t, (offset, linbits, width, unused) in enumerate(rows):
        start.append(len(code))
        if width == 0:
            continue
        found = {}
        walk(nodes, offset, [], found)
        if len(found) != width * width:
            raise Fail("table %d: %d leaves for a %dx%d grid"
                       % (t, len(found), width, width))
        for x in range(width):
            for y in range(width):
                c, n = found[(x << 4) | y]
                if n > 19:
                    raise Fail("table %d: a code of %d bits" % (t, n))
                if decode(nodes, offset, c, n) != (x << 4) | y:
                    raise Fail("table %d (%d,%d) does not decode back" % (t, x, y))
                code.append(c)
                length.append(n)

    qcode, qlen = [], []
    for k in range(2):
        found = {}
        walk(quad_nodes, quad_offset[k], [], found)
        if sorted(found) != list(range(16)):
            raise Fail("quad table %d does not cover 16 values" % k)
        for v in range(16):
            c, n = found[v]
            if decode(quad_nodes, quad_offset[k], c, n) != v:
                raise Fail("quad table %d value %d does not decode back" % (k, v))
            qcode.append(c)
            qlen.append(n)

    ana = []
    for k in range(32):
        for i in range(64):
            ana.append(cos_q((2 * k + 1) * (i - 16), 64))

    thr = [threshold(i) for i in range(QUANT_MAX + 1)]
    for a, b in zip(thr, thr[1:]):
        if not a < b:
            raise Fail("thresholds are not increasing")

    log2_frac = [int(math.floor(256 * math.log2(1 + i / 256.0) + 0.5)) for i in range(256)]
    exp2_frac = [int(math.floor(65536 * math.pow(2.0, i / 256.0) + 0.5)) for i in range(256)]
    inv = [inverse_gain(k) for k in range(4)]
    for k in range(4):
        # 2^(k/4) * 2^(-k/4) = 1, to the rounding of two Q28 values.
        product = (gain_frac[k] * inv[k] + (1 << (Q - 1))) >> Q
        if abs(product - (1 << Q)) > 2:
            raise Fail("gain %d and its inverse multiply to %d" % (k, product))

    out = [LICENCE, """/**
 * @file
 *
 * This file is generated and must not be edited.
 *
 * `tools/tables/gen_mp3enc_tables.py` writes it from the decoder's tables
 * in `mp3_tables.c`; run it again when those change:
 *
 *     tools/tables/gen_mp3enc_tables.py
 *
 * What each table is, and why it is a table, is in that script.
 */

#include "mp3enc_tables.h"
"""]
    emit_array(out, "const uint16_t gaud_mp3enc_huff_start[32]", start)
    emit_array(out, "const uint32_t gaud_mp3enc_huff_code[%d]" % len(code), code)
    emit_array(out, "const uint8_t gaud_mp3enc_huff_len[%d]" % len(length), length, 20)
    emit_array(out, "const uint8_t gaud_mp3enc_quad_code[32]", qcode, 16)
    emit_array(out, "const uint8_t gaud_mp3enc_quad_len[32]", qlen, 16)
    emit_rows(out, "const int32_t gaud_mp3enc_ana_cos[32][64]",
              [ana[k * 64:(k + 1) * 64] for k in range(32)])
    emit_array(out, "const uint64_t gaud_mp3enc_quant_threshold[8207]",
               [str(v) + "ull" for v in thr], 4)
    emit_array(out, "const int32_t gaud_mp3enc_gain_inverse[4]", inv)
    emit_array(out, "const uint16_t gaud_mp3enc_log2_frac[256]", log2_frac, 12)
    emit_array(out, "const uint32_t gaud_mp3enc_exp2_frac[256]", exp2_frac, 8)

    hdr = [LICENCE, """/**
 * @file
 *
 * This file is generated and must not be edited. See `mp3enc_tables.c`.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3ENC_TABLES_H
#define GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3ENC_TABLES_H

#include <ghoti.io/audio/macros.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where each Huffman table's code words start in ::gaud_mp3enc_huff_code;
 *  the table is `width * width` entries, row x, column y. */
extern const uint16_t gaud_mp3enc_huff_start[32];
/** Code words of every pair table, one after another. */
extern const uint32_t gaud_mp3enc_huff_code[%d];
/** Their lengths in bits. */
extern const uint8_t gaud_mp3enc_huff_len[%d];
/** The two quadruple tables' code words, 16 each, indexed by the four
 *  bits (v, w, x, y) with v the most significant. */
extern const uint8_t gaud_mp3enc_quad_code[32];
/** Their lengths. */
extern const uint8_t gaud_mp3enc_quad_len[32];
/** The analysis matrixing, cos((2k+1)(i-16)pi/64). Q28. */
extern const int32_t gaud_mp3enc_ana_cos[32][64];
/** The smallest value, Q28, that quantises to index + 1. */
extern const uint64_t gaud_mp3enc_quant_threshold[8207];
/** 2^(-k/4), the reciprocal of ::gaud_mp3_gain_frac. Q28. */
extern const int32_t gaud_mp3enc_gain_inverse[4];
/** 256 * log2(1 + i/256), rounded: the fractional part of a logarithm. */
extern const uint16_t gaud_mp3enc_log2_frac[256];
/** 65536 * 2^(i/256), rounded. */
extern const uint32_t gaud_mp3enc_exp2_frac[256];

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3ENC_TABLES_H
""" % (len(code), len(length))]
    return "\n".join(out), "\n".join(hdr)


def main(argv):
    try:
        c, h = build()
    except Fail as why:
        sys.stderr.write("gen_mp3enc_tables: %s\n" % why)
        return 1
    if "--check" in argv:
        ok = open(OUT).read() == c and open(HDR).read() == h
        print("mp3enc_tables is %s" % ("current" if ok else "STALE"))
        return 0 if ok else 1
    open(OUT, "w").write(c)
    open(HDR, "w").write(h)
    print("wrote %s and %s" % (OUT, HDR))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
