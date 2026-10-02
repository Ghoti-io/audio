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
 * The three SILK primitives too large to inline. Never installed.
 *
 * Each is an approximation whose error is part of the format: the
 * encoder made its decisions with the same wrong answer, so a more
 * accurate root or reciprocal here decodes to different audio.
 */

#include "opus_silk_math.h"

int32_t gaud_silk_sqrt_approx(int32_t value) {
  if (value <= 0) {
    return 0;
  }
  int leading = gaud_silk_clz32((uint32_t)value);
  // The seven bits below the leading one, which is the mantissa the
  // correction step reads.
  int32_t fraction =
      (int32_t)(gaud_silk_ror32((uint32_t)value, 24 - leading) & 0x7Fu);
  // sqrt(2) in Q15 for an odd exponent, one for an even one: half an
  // exponent cannot be represented by the shift alone.
  int32_t root = (leading & 1) ? 32768 : 46214;
  root >>= leading >> 1;
  // 213/32768 is close to ln(2)/2 over the mantissa's range, which is
  // the slope the square root has there.
  return gaud_silk_smlawb(root, root, gaud_silk_smulbb(213, fraction));
}

int32_t gaud_silk_log2lin(int32_t log_q7) {
  if (log_q7 < 0) {
    return 0;
  }
  int32_t out = gaud_silk_shl32(1, (unsigned)(log_q7 >> 7));
  int32_t fraction = log_q7 & 0x7F;
  // The parabola `f + f*(128-f)*(-174/65536)` is the correction from a
  // straight line to 2^f over one octave.
  int32_t correction =
      gaud_silk_smlawb(fraction, gaud_silk_smulbb(fraction, 128 - fraction),
          -174);
  if (log_q7 < 2048) {
    // Below 2^16 the product still fits, so the correction is applied
    // at full width and shifted afterwards.
    return gaud_silk_add32(out, (out * correction) >> 7);
  }
  // Above it the shift has to come first. This is a different
  // computation, not a rearrangement of the same one.
  return gaud_silk_add32(out, (out >> 7) * correction);
}

int32_t gaud_silk_inverse32_varq(int32_t value, int q) {
  int headroom = gaud_silk_clz32((uint32_t)(value > 0 ? value : -value)) - 1;
  int32_t normalised = gaud_silk_shl32(value, (unsigned)headroom);
  // Fourteen bits of reciprocal out of one 32-by-16 division.
  int32_t approximate = (int32_t)(0x1FFFFFFF / (normalised >> 16));
  int32_t result = gaud_silk_shl32(approximate, 16);
  // One Newton step: the residual of the first approximation, scaled
  // back up and multiplied by the approximation again.
  int32_t error = gaud_silk_shl32(
      (1 << 29) - gaud_silk_smulwb(normalised, approximate), 3);
  result = gaud_silk_smlaww(result, error, approximate);
  int shift = 61 - headroom - q;
  if (shift <= 0) {
    // The reference clamps to what the shift can carry and then
    // shifts, which is not the same as clamping the result: it leaves
    // the low bits zero.
    unsigned by = (unsigned)(-shift);
    if (by >= 32u) {
      return result == 0 ? 0 : (result > 0 ? INT32_MAX : INT32_MIN);
    }
    return gaud_silk_shl32(
        gaud_silk_limit(result, INT32_MIN >> by, INT32_MAX >> by), by);
  }
  // Above 32 the shift would be undefined, and the answer is zero
  // anyway.
  return shift < 32 ? result >> shift : 0;
}

int32_t gaud_silk_div32_varq(int32_t numerator, int32_t denominator, int q) {
  int top = gaud_silk_clz32(
                (uint32_t)(numerator > 0 ? numerator : -numerator))
      - 1;
  int bottom = gaud_silk_clz32(
                   (uint32_t)(denominator > 0 ? denominator : -denominator))
      - 1;
  int32_t a = gaud_silk_shl32(numerator, (unsigned)top);
  int32_t b = gaud_silk_shl32(denominator, (unsigned)bottom);
  int32_t reciprocal = (int32_t)(0x1FFFFFFF / (b >> 16));
  int32_t result = gaud_silk_smulwb(a, reciprocal);
  // One Newton step. The reference notes that this subtraction may
  // overflow and that it does not matter, because what is left is
  // small; it is spelt unsigned so that it is defined rather than
  // merely usual.
  a = (int32_t)((uint32_t)a
      - (uint32_t)gaud_silk_shl32(gaud_silk_smmul(b, result), 3));
  result = gaud_silk_smlawb(result, a, reciprocal);
  int shift = 29 + top - bottom - q;
  if (shift < 0) {
    unsigned by = (unsigned)(-shift);
    if (by >= 32u) {
      return result == 0 ? 0 : (result > 0 ? INT32_MAX : INT32_MIN);
    }
    return gaud_silk_shl32(
        gaud_silk_limit(result, INT32_MIN >> by, INT32_MAX >> by), by);
  }
  return shift < 32 ? result >> shift : 0;
}
