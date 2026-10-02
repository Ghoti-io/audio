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
 * SILK's fixed-point primitives. RFC 6716 Appendix A. Never installed.
 *
 * SILK and CELT do not share these. They were written by different
 * people for different signal models and they round differently, so
 * `opus_celt_math.h` and this file both define a multiply-and-shift
 * and the two are not the same function. Mixing them is the kind of
 * mistake that produces audio which is almost right.
 *
 * **The partial-product rounding is the whole content of the wide
 * multiplies.** `silk_SMULWB` is not `(a * b) >> 16`: it splits the
 * 32-bit operand and shifts the low half's product down *before*
 * adding, which discards bits a 64-bit multiply would have kept. That
 * is a different number, by up to one unit, and the difference
 * accumulates through the LPC recurrence. Each is transcribed here
 * exactly as Appendix A spells it rather than simplified.
 *
 * **Rounding right shifts are written the reference's way on purpose.**
 * `(a >> (s-1)) + 1) >> 1` and `(a + (1 << (s-1))) >> s` are provably
 * the same value for every `a` and every `s >= 1` - but only the first
 * cannot overflow, and the LPC coefficients get shifted while they are
 * still near the top of their range.
 *
 * **Right shifts of negative values are arithmetic here**, which C
 * leaves implementation-defined and every target this library supports
 * implements that way; RFC 6716's own code relies on it throughout.
 * Left shifts of negative values are undefined rather than
 * implementation-defined, so those go through an unsigned type.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_SILK_MATH_H
#define GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_SILK_MATH_H

#include <ghoti.io/audio/macros.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Left shift that is defined for negative values. */
static inline int32_t gaud_silk_shl32(int32_t value, unsigned shift) {
  return (int32_t)((uint32_t)value << shift);
}

/** The same for 64 bits. */
static inline int64_t gaud_silk_shl64(int64_t value, unsigned shift) {
  return (int64_t)((uint64_t)value << shift);
}

/** Wrapping 32-bit addition, which several accumulators rely on. */
static inline int32_t gaud_silk_add32(int32_t a, int32_t b) {
  return (int32_t)((uint32_t)a + (uint32_t)b);
}

/**
 * Arithmetic right shift, rounding to nearest, halves away from zero.
 *
 * Written as the reference writes it: shifting first and then rounding
 * cannot overflow, where adding half first can.
 */
static inline int32_t gaud_silk_rshift_round(int32_t value, unsigned shift) {
  return shift == 1u ? (value >> 1) + (value & 1)
                     : ((value >> (shift - 1u)) + 1) >> 1;
}

/** The same over 64 bits. */
static inline int64_t gaud_silk_rshift_round64(int64_t value, unsigned shift) {
  return shift == 1u ? (value >> 1) + (value & 1)
                     : ((value >> (shift - 1u)) + 1) >> 1;
}

/** Saturate to 16 bits. */
static inline int32_t gaud_silk_sat16(int32_t value) {
  return value > 32767 ? 32767 : (value < -32768 ? -32768 : value);
}

/** Clamp to a range. */
static inline int32_t gaud_silk_limit(int32_t value, int32_t low,
    int32_t high) {
  return value < low ? low : (value > high ? high : value);
}

/** Sixteen by sixteen, both operands truncated to signed 16 bits. */
static inline int32_t gaud_silk_smulbb(int32_t a, int32_t b) {
  return (int32_t)((int16_t)a) * (int32_t)((int16_t)b);
}

/** @ref gaud_silk_smulbb accumulated onto @p a. */
static inline int32_t gaud_silk_smlabb(int32_t a, int32_t b, int32_t c) {
  return gaud_silk_add32(a, gaud_silk_smulbb(b, c));
}

/**
 * Thirty-two by the low sixteen of another, kept at the wide operand's
 * scale: `(a * (int16)b) >> 16`, computed in halves.
 *
 * The halves are what make this exact to transcribe: the low half's
 * product is shifted down before the add, so the result can differ by
 * one from the same expression evaluated in 64 bits.
 */
static inline int32_t gaud_silk_smulwb(int32_t a, int32_t b) {
  int32_t low = (int32_t)(((uint32_t)a & 0xFFFFu));
  return (a >> 16) * (int32_t)((int16_t)b)
      + ((low * (int32_t)((int16_t)b)) >> 16);
}

/** @ref gaud_silk_smulwb accumulated onto @p a. */
static inline int32_t gaud_silk_smlawb(int32_t a, int32_t b, int32_t c) {
  return gaud_silk_add32(a, gaud_silk_smulwb(b, c));
}

/**
 * Thirty-two by thirty-two at the first's scale, in two steps.
 *
 * The second step's product is formed unsigned because it genuinely
 * overflows: a subframe gain can be large enough that the filter
 * state times it leaves 32 bits, and the reference wraps there rather
 * than saturating. Wrapping is what the format does; signed overflow
 * is merely what C calls it.
 */
static inline int32_t gaud_silk_smulww(int32_t a, int32_t b) {
  return gaud_silk_add32(gaud_silk_smulwb(a, b),
      (int32_t)((uint32_t)a * (uint32_t)gaud_silk_rshift_round(b, 16)));
}

/** @ref gaud_silk_smulww accumulated onto @p a. */
static inline int32_t gaud_silk_smlaww(int32_t a, int32_t b, int32_t c) {
  return gaud_silk_add32(gaud_silk_smlawb(a, b, c),
      (int32_t)((uint32_t)b * (uint32_t)gaud_silk_rshift_round(c, 16)));
}

/** The top 32 bits of a 64-bit product. */
static inline int32_t gaud_silk_smmul(int32_t a, int32_t b) {
  return (int32_t)(((int64_t)a * (int64_t)b) >> 32);
}

/** Leading zeros, with 32 for an input of zero. */
static inline int gaud_silk_clz32(uint32_t value) {
  int count = 0;
  if (value == 0u) {
    return 32;
  }
  while ((value & 0x80000000u) == 0u) {
    value <<= 1;
    ++count;
  }
  return count;
}

/** Rotate right. */
static inline uint32_t gaud_silk_ror32(uint32_t value, int rotation) {
  unsigned by = (unsigned)(rotation & 31);
  return by == 0u ? value : ((value >> by) | (value << (32u - by)));
}

/**
 * @brief Square root, to about nine bits, in Q(input/2).
 *
 * RFC 6716's `silk_SQRT_APPROX`: normalise, pick one of two seeds by
 * the parity of the exponent, and correct once with the mantissa.
 *
 * @param value Non-negative; zero and below return zero.
 * @return The approximate root.
 */
int32_t gaud_silk_sqrt_approx(int32_t value);

/**
 * @brief Two to the power of a Q7 logarithm.
 *
 * RFC 6716's `silk_log2lin`, which the quantization gains are read
 * through. A parabola corrects the straight exponential, and the
 * correction is applied before or after the shift depending on whether
 * the result still has room - the two arms are not the same
 * computation and the boundary is at 16 in the log domain.
 *
 * **The argument has a ceiling of 4095** and the reference does not
 * check it: one more and the shift that forms the exponent is
 * undefined. The only caller caps it at 3967, which is 31 in Q7, so
 * the ceiling is the caller's to keep - as it is in Appendix A.
 *
 * @param log_q7 The logarithm, at most 4095; negative returns zero.
 * @return The linear value.
 */
int32_t gaud_silk_log2lin(int32_t log_q7);

/**
 * @brief `(1 << q) / value`, to about 29 bits.
 *
 * RFC 6716's `silk_INVERSE32_varQ`: a 14-bit reciprocal from a
 * division, then one Newton step.
 *
 * @param value The denominator, which must not be zero.
 * @param q The scale wanted, which must be positive.
 * @return The reciprocal, or zero when @p q asks for more than fits.
 */
int32_t gaud_silk_inverse32_varq(int32_t value, int q);

/**
 * @brief `(numerator << q) / denominator`, to about 29 bits.
 *
 * RFC 6716's `silk_DIV32_varQ`. The residual it refines with is
 * allowed to overflow - the reference says so, and the final value is
 * small enough that two wraps cancel - so that step goes through an
 * unsigned type here.
 *
 * @param numerator Any value.
 * @param denominator Must not be zero.
 * @param q The scale wanted, which must not be negative.
 * @return The quotient, saturated if it does not fit.
 */
int32_t gaud_silk_div32_varq(int32_t numerator, int32_t denominator, int q);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_SILK_MATH_H
