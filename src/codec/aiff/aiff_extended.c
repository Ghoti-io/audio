/*
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

/**
 * @file
 *
 * The 80-bit extended float AIFF states its sample rate in.
 *
 * One sign bit, a 15-bit exponent biased by 16383, and a 64-bit significand
 * whose top bit is **explicit** - which is the difference from every other
 * IEEE format and the thing a hand-rolled decoder gets wrong.
 */

#include "aiff_internal.h"
#include <math.h>
#include <string.h>

/** @brief Decode an 80-bit big-endian extended float into a double. */
double gaud_aiff_read_extended(const unsigned char * p) {
  uint32_t sign = (p[0] & 0x80u) ? 1u : 0u;
  int32_t exponent = (int32_t)(((uint32_t)(p[0] & 0x7Fu) << 8) | p[1]);

  uint64_t significand = 0;
  for (int i = 0; i < 8; ++i) {
    significand = (significand << 8) | p[2 + i];
  }

  if (exponent == 0 && significand == 0) {
    return sign ? -0.0 : 0.0;
  }
  if (exponent == 0x7FFF) {
    /* Infinity or NaN. A sample rate is neither, and the caller refuses 0. */
    return 0.0;
  }

  /* ldexp rather than pow(2, n): exact, and it cannot introduce a rounding
   * error into a value that is going to be compared for equality with the
   * rate a file was written with. The significand's top bit is explicit, so
   * the scale is exponent - 16383 - 63 rather than the -52 a double uses. */
  double value = ldexp((double)significand, exponent - 16383 - 63);
  return sign ? -value : value;
}

/** @brief Encode a non-negative finite double as an 80-bit extended. */
void gaud_aiff_write_extended(unsigned char * p, double value) {
  memset(p, 0, 10);
  if (!(value > 0.0)) {
    /* Zero, negative or NaN all write as zero. A caller has already refused
     * a rate of zero, so this is the unreachable-but-defined arm. */
    return;
  }

  int exponent = 0;
  double fraction = frexp(value, &exponent); /* 0.5 <= fraction < 1.0 */

  /* Shift the fraction up so its top bit lands in bit 63, which is where
   * this format keeps the explicit leading one. frexp gives [0.5, 1), so
   * scaling by 2^64 puts the leading bit at 63 and the exponent adjusts by
   * the same amount. */
  uint64_t significand = (uint64_t)ldexp(fraction, 64);
  int biased = exponent + 16383 - 1;
  if (biased < 0 || biased > 0x7FFE) {
    return; /* Outside the format; unreachable for any real sample rate. */
  }

  p[0] = (unsigned char)((biased >> 8) & 0x7Fu);
  p[1] = (unsigned char)(biased & 0xFFu);
  for (int i = 0; i < 8; ++i) {
    p[2 + i] = (unsigned char)((significand >> (56 - 8 * i)) & 0xFFu);
  }
}
