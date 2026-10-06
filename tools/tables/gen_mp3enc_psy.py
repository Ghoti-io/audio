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
"""Generate src/codec/mp3/mp3enc_psy_tables.c: the psychoacoustic model's data.

    tools/tables/gen_mp3enc_psy.py [--check]

**What is generated, and why it is generated and not computed.** The model
needs a Fourier transform's twiddle factors, two Hann windows, the
partitioning of the spectrum into bands that are narrower than the ear's
critical bands, the absolute threshold of hearing on that partitioning, and
the spreading function that says how far a loud tone's masking reaches. All
of those are real-valued functions of frequency. The encoder is
integer-only and so cannot evaluate them; this script evaluates them once,
in Python's arithmetic, rounds them, and the C file carries the integers.
Rounding happens here, in one place, so that the encoder's output is the
same bytes on every architecture: the table is data, not an expression.

The formulas are the published ones and are the *model's* choices, not a
standard's:

  - **Bark scale**, Zwicker and Terhardt: 13 atan(0.00076 f) + 3.5
    atan((f / 7500)^2).
  - **Absolute threshold of hearing**, Terhardt: 3.64 (f/1000)^-0.8 - 6.5
    exp(-0.6 (f/1000 - 3.3)^2) + 10^-3 (f/1000)^4, in dB SPL, floored at
    -20 dB below which no listener is credited, capped at 90 dB, and referred to the
    encoder's digital scale by taking a full-scale sine to be 96 dB SPL.
  - **Spreading function**, Schroeder: 15.81 + 7.5 (dz + 0.474) - 17.5
    sqrt(1 + (dz + 0.474)^2) dB for a masked band dz Bark above the masker.
  - **Partitions** are at most a third of a Bark wide and at least one
    FFT bin.

The units of the thresholds are the power of the integer FFT the C code
runs: a sample of x enters as (x * window_Q15) >> 10, so a full-scale sine
at a bin centre has power 2^56 in the 1024-point transform and 2^52 in the
256-point one.
"""

import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "src", "codec", "mp3", "mp3enc_psy_tables.c")
HDR = os.path.join(ROOT, "src", "codec", "mp3", "mp3enc_psy_tables.h")

#: Row order: MPEG-1 44.1, 48, 32; MPEG-2 22.05, 24, 16; MPEG-2.5 11.025,
#: 12, 8 - version * 3 + the header's rate index.
RATES = [44100, 48000, 32000, 22050, 24000, 16000, 11025, 12000, 8000]

LONG_N = 1024
SHORT_N = 256
MAX_LONG_PARTS = 72
MAX_SHORT_PARTS = 48

SPREAD_STEP = 8           # entries per Bark
SPREAD_LOW = 8            # Bark below the masker
SPREAD_HIGH = 16          # Bark above it
SPREAD_COUNT = (SPREAD_LOW + SPREAD_HIGH) * SPREAD_STEP + 1


def bark(f):
    return 13.0 * math.atan(0.00076 * f) + 3.5 * math.atan((f / 7500.0) ** 2)


def ath_db(f):
    k = max(f, 20.0) / 1000.0
    v = 3.64 * k ** -0.8 - 6.5 * math.exp(-0.6 * (k - 3.3) ** 2) + 1e-3 * k ** 4
    # The formula's last term grows without bound; no real threshold of
    # hearing rises past what a loud sound is, so it is held at 90 dB.
    return min(max(v, -20.0), 90.0)


def partitions(rate, n):
    """Partition boundaries over bins 0..n/2 inclusive: [(lo, hi)), bins
    lo..hi-1."""
    bins = n // 2 + 1
    parts = []
    start = 0
    start_bark = bark(0.0)
    for k in range(1, bins):
        f = k * rate / n
        if bark(f) - start_bark >= 1.0 / 3.0:
            parts.append((start, k))
            start = k
            start_bark = bark(f)
    parts.append((start, bins))
    return parts


def table_for(rate, n, full_scale_power):
    parts = partitions(rate, n)
    lo = [p[0] for p in parts]
    hi = [p[1] for p in parts]
    centre_bark = []
    ath = []
    for a, b in parts:
        f = (a + b - 1) / 2.0 * rate / n
        centre_bark.append(int(round(bark(f) * 256)))
        # The quietest bin of the partition, in power units.
        best = None
        for k in range(a, b):
            db = ath_db(k * rate / n)
            power = full_scale_power * 10.0 ** ((db - 96.0) / 10.0)
            best = power if best is None else min(best, power)
        ath.append(max(1, int(best)))
    return lo, hi, centre_bark, ath


def spread_table():
    out = []
    for i in range(SPREAD_COUNT):
        dz = (i - SPREAD_LOW * SPREAD_STEP) / float(SPREAD_STEP)
        x = dz + 0.474
        db = 15.81 + 7.5 * x - 17.5 * math.sqrt(1.0 + x * x)
        out.append(int(round(65536.0 * 10.0 ** (db / 10.0))))
    return out


LICENCE = open(os.path.join(HERE, "gen_mp3enc_tables.py")).read().split('LICENCE = """')[1].split('"""')[0]


def arr(lines, decl, values, per=8):
    lines.append(decl + " = {")
    for i in range(0, len(values), per):
        lines.append("    " + ", ".join(str(v) for v in values[i:i + per]) + ",")
    lines.append("};\n")


def build():
    out = [LICENCE, """/**
 * @file
 *
 * This file is generated and must not be edited.
 *
 * `tools/tables/gen_mp3enc_psy.py` writes it; run it again when the model's
 * data has to change. See that script for the formulas and the units.
 */

#include "mp3enc_psy_tables.h"
"""]
    cos = []
    sin = []
    for k in range(LONG_N // 2):
        cos.append(int(round(math.cos(2 * math.pi * k / LONG_N) * (1 << 30))))
        sin.append(int(round(math.sin(2 * math.pi * k / LONG_N) * (1 << 30))))
    arr(out, "const int32_t gaud_mp3enc_fft_cos[512]", cos)
    arr(out, "const int32_t gaud_mp3enc_fft_sin[512]", sin)
    hl = [int(round(32767 * 0.5 * (1 - math.cos(2 * math.pi * (i + 0.5) / LONG_N)))) for i in range(LONG_N)]
    hs = [int(round(32767 * 0.5 * (1 - math.cos(2 * math.pi * (i + 0.5) / SHORT_N)))) for i in range(SHORT_N)]
    arr(out, "const int16_t gaud_mp3enc_hann_long[1024]", hl, 12)
    arr(out, "const int16_t gaud_mp3enc_hann_short[256]", hs, 12)
    arr(out, "const uint32_t gaud_mp3enc_spread[%d]" % SPREAD_COUNT, spread_table())

    rows = []
    for rate in RATES:
        rows.append((table_for(rate, LONG_N, 2.0 ** 56), table_for(rate, SHORT_N, 2.0 ** 52)))
    for (name, idx, maxp, fs) in (("long", 0, MAX_LONG_PARTS, 56), ("short", 1, MAX_SHORT_PARTS, 52)):
        counts = []
        for r in rows:
            if len(r[idx][0]) > maxp:
                raise SystemExit("too many %s partitions: %d" % (name, len(r[idx][0])))
            counts.append(len(r[idx][0]))
        arr(out, "const uint8_t gaud_mp3enc_%s_parts[9]" % name, counts, 9)
        for field, pos, ctype in (("lo", 0, "uint16_t"), ("hi", 1, "uint16_t"),
                                  ("bark", 2, "uint16_t"), ("ath", 3, "uint64_t")):
            out.append("const %s gaud_mp3enc_%s_%s[9][%d] = {" % (ctype, name, field, maxp))
            for r in rows:
                vals = list(r[idx][pos]) + [0] * (maxp - len(r[idx][pos]))
                if field == "ath":
                    vals = [str(v) + "ull" for v in vals]
                out.append("    {" + ", ".join(str(v) for v in vals) + "},")
            out.append("};\n")

    hdr = [LICENCE, """/**
 * @file
 *
 * This file is generated and must not be edited. See `mp3enc_psy_tables.c`.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3ENC_PSY_TABLES_H
#define GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3ENC_PSY_TABLES_H

#include <ghoti.io/audio/macros.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Most partitions of the long (1024-point) and short (256-point) spectra. */
#define MP3E_MAX_LONG_PARTS %d
#define MP3E_MAX_SHORT_PARTS %d
/** The spreading table: entries per Bark, Bark below the masker it covers,
 *  and its length. Index 0 is SPREAD_LOW Bark below. */
#define MP3E_SPREAD_STEP %d
#define MP3E_SPREAD_LOW %d
#define MP3E_SPREAD_COUNT %d

/** cos and sin of 2 pi k / 1024, Q30. */
extern const int32_t gaud_mp3enc_fft_cos[512];
extern const int32_t gaud_mp3enc_fft_sin[512];
/** Hann windows, Q15. */
extern const int16_t gaud_mp3enc_hann_long[1024];
extern const int16_t gaud_mp3enc_hann_short[256];
/** Schroeder's spreading function as linear power, times 65536. */
extern const uint32_t gaud_mp3enc_spread[%d];

/** Partitions per row (version * 3 + rate index), long and short. */
extern const uint8_t gaud_mp3enc_long_parts[9];
extern const uint8_t gaud_mp3enc_short_parts[9];
""" % (MAX_LONG_PARTS, MAX_SHORT_PARTS, SPREAD_STEP, SPREAD_LOW, SPREAD_COUNT, SPREAD_COUNT)]
    for name, maxp in (("long", MAX_LONG_PARTS), ("short", MAX_SHORT_PARTS)):
        hdr.append("/** The %s partitions' first bin, one past their last, Bark centre (Q8)\n * and quiet threshold per bin (power units). */" % name)
        hdr.append("extern const uint16_t gaud_mp3enc_%s_lo[9][%d];" % (name, maxp))
        hdr.append("extern const uint16_t gaud_mp3enc_%s_hi[9][%d];" % (name, maxp))
        hdr.append("extern const uint16_t gaud_mp3enc_%s_bark[9][%d];" % (name, maxp))
        hdr.append("extern const uint64_t gaud_mp3enc_%s_ath[9][%d];\n" % (name, maxp))
    hdr.append("""#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3ENC_PSY_TABLES_H
""")
    return "\n".join(out), "\n".join(hdr)


def main(argv):
    c, h = build()
    if "--check" in argv:
        ok = open(OUT).read() == c and open(HDR).read() == h
        print("mp3enc_psy_tables is %s" % ("current" if ok else "STALE"))
        return 0 if ok else 1
    open(OUT, "w").write(c)
    open(HDR, "w").write(h)
    print("wrote", OUT, HDR)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
