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
 * CELT's fixed-point arithmetic: the few functions it needs that C does
 * not have. Never installed.
 *
 * These are reciprocals, square roots, a cosine and a power of two, each
 * a short polynomial over a normalised range with the exponent handled
 * by shifting. They exist because CELT has to run on hardware with no
 * floating-point unit, and this library uses them for the separate
 * reason that planning/audio.md section 11.1 requires byte-identical
 * output on every architecture.
 *
 * **They are approximations, and which approximation is part of the
 * format.** ::gaud_celt_rsqrt_norm is not "the reciprocal square root";
 * it is one particular minimax quadratic followed by one particular
 * Householder step, with a relative error of about 1.05e-4. Replacing
 * it with a better one changes the decoder's output. So the tests here
 * check two different things and both matter: that each function tracks
 * the real function it names to the error bound RFC 6716's own source
 * states, and that it returns the exact integers the reference returns.
 * The first says the transcription is not nonsense; only the second
 * says it is right.
 *
 * **Two of them are bit-exact in the floating-point configuration too.**
 * ::gaud_celt_bitexact_cos and ::gaud_celt_bitexact_log2tan are integer
 * in both of RFC 6716's builds, and deliberately so: their results feed
 * the stereo split's bit allocation, which section 4.3.3 requires both
 * ends to agree on exactly. Everything else here is the fixed-point
 * build's answer to something the normative build does in `float`.
 *
 * **The domains are small enough to check completely.** All but two of
 * these take an argument whose useful range is a few tens of thousands
 * of values wide, so "tested against the reference" can mean every
 * input rather than a sample. notes/audio/opus.md records which ones
 * that was done for and what the remaining two were sampled over.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_CELT_MATH_H
#define GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_CELT_MATH_H

#include "opus_range.h"
#include <ghoti.io/audio/macros.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Add as 16 bits, wrapping, which the approximations rely on.
 *
 * RFC 6716's `ADD16`. The intermediate results of the polynomials below
 * are declared 16-bit, so the reference's answer where one overflows is
 * the wrapped one, and this cannot widen to 32 bits even though doing so
 * would look like a repair.
 *
 * **No argument here reaches the wrap.** Counting them across the 11.2
 * million inputs the comparison in notes/audio/opus.md sweeps gives zero,
 * and widening this is the one mutation of eighteen that no test catches.
 * It stays as it is because it is what Appendix A says and because the
 * next caller may not be so lucky, not because anything demonstrates it.
 *
 * @param a First addend.
 * @param b Second addend.
 * @return The low 16 bits of their sum, as a signed value.
 */
static inline int16_t gaud_celt_add16(int32_t a, int32_t b) {
  return (int16_t)(uint16_t)((uint32_t)(int32_t)(int16_t)a
      + (uint32_t)(int32_t)(int16_t)b);
}

/**
 * @brief Subtract as 16 bits, wrapping.
 *
 * RFC 6716's `SUB16` does *not* wrap - it is the one member of the pair
 * written without the outer cast that `ADD16` has, so it widens to `int`
 * instead. **This one does wrap, and it is reachable**: 22,354 times over
 * the same sweep, all of them in ::gaud_celt_bitexact_cos and all of them
 * for arguments below the 64 its caller's smallest step produces. The
 * results still agree everywhere, because the wider value is truncated
 * again before it is used, so the difference cannot escape the function.
 * Measured rather than argued; notes/audio/opus.md has the counts.
 *
 * @param a The minuend.
 * @param b The subtrahend.
 * @return The low 16 bits of the difference, as a signed value.
 */
static inline int16_t gaud_celt_sub16(int32_t a, int32_t b) {
  return (int16_t)(uint16_t)((uint32_t)(int32_t)(int16_t)a
      - (uint32_t)(int32_t)(int16_t)b);
}

/**
 * @brief Shift right by @p shift, or left if it is negative.
 *
 * RFC 6716's `VSHR32`. The normalise-then-approximate shape of every
 * function here means the exponent correction can go either way, and
 * writing it as one call keeps the two directions from drifting apart.
 *
 * @param value What to shift.
 * @param shift Positive to shift right, negative to shift left.
 * @return The shifted value.
 */
static inline int32_t gaud_celt_vshr32(int32_t value, int shift) {
  return shift > 0 ? value >> shift
                   : (int32_t)((uint32_t)value << (unsigned)-shift);
}

/**
 * @brief Add as 32 bits, wrapping.
 *
 * RFC 6716's `ADD32` is a plain `+` on `opus_int32`, and the transform
 * does overflow it: a bitstream that decodes to an extreme energy
 * drives the FFT's accumulators past 2^31, and the reference wraps
 * there on every two's-complement machine. Signed overflow is
 * undefined behaviour rather than merely implementation-defined, so
 * these go through an unsigned type - which computes the same answer
 * the reference computes, and lets the sanitizer gate stay on.
 *
 * @param a First addend.
 * @param b Second addend.
 * @return Their sum, wrapped to 32 bits.
 */
static inline int32_t gaud_celt_add32(int32_t a, int32_t b) {
  return (int32_t)((uint32_t)a + (uint32_t)b);
}

/**
 * @brief Subtract as 32 bits, wrapping.
 *
 * @param a The minuend.
 * @param b The subtrahend.
 * @return Their difference, wrapped to 32 bits.
 */
static inline int32_t gaud_celt_sub32(int32_t a, int32_t b) {
  return (int32_t)((uint32_t)a - (uint32_t)b);
}

/**
 * @brief Multiply two 16-bit values into 32 bits.
 *
 * @param a First factor; only its low 16 bits are read.
 * @param b Second factor; likewise.
 * @return Their product.
 */
static inline int32_t gaud_celt_mult16_16(int32_t a, int32_t b) {
  return (int32_t)(int16_t)a * (int32_t)(int16_t)b;
}

/**
 * @brief Multiply two Q15 values, truncating.
 *
 * @param a First factor.
 * @param b Second factor.
 * @return The product shifted right 15, rounding towards minus infinity.
 */
static inline int32_t gaud_celt_mult16_16_q15(int32_t a, int32_t b) {
  return gaud_celt_mult16_16(a, b) >> 15;
}

/**
 * @brief Multiply two Q15 values, rounding to nearest.
 *
 * @param a First factor.
 * @param b Second factor.
 * @return The product shifted right 15 with a half added first.
 */
static inline int32_t gaud_celt_mult16_16_p15(int32_t a, int32_t b) {
  return (16384 + gaud_celt_mult16_16(a, b)) >> 15;
}

/**
 * @brief RFC 6716's `FRAC_MUL16`, whose exactness it calls out.
 *
 * The same arithmetic as ::gaud_celt_mult16_16_p15. It has its own name
 * in the reference because its two callers are the two functions that
 * must agree bit for bit between the encoder and the decoder, and that
 * distinction is worth keeping.
 *
 * @param a First factor.
 * @param b Second factor.
 * @return The rounded Q15 product.
 */
static inline int32_t gaud_celt_frac_mul16(int32_t a, int32_t b) {
  return (16384 + gaud_celt_mult16_16(a, b)) >> 15;
}

/**
 * @brief Multiply a 32-bit value by a Q15 one, RFC 6716's `MULT16_32_Q15`.
 *
 * @param twiddle The Q15 factor; only its low 16 bits are read.
 * @param value The 32-bit one.
 * @return Their product, in the second factor's scale.
 */
static inline int32_t gaud_celt_mult16_32_q15(int32_t twiddle, int32_t value) {
  return gaud_celt_add32(
      (int32_t)((uint32_t)gaud_celt_mult16_16(twiddle, value >> 16) << 1),
      ((int32_t)(int16_t)twiddle * (int32_t)(uint16_t)(uint32_t)value) >> 15);
}

/**
 * @brief Multiply two Q31 values, as three 16-bit partial products.
 *
 * RFC 6716's `MULT32_32_Q31`. It drops the lowest partial product
 * rather than computing the full 64-bit result, so it is not the same
 * as `(int64_t)a * b >> 31` and must not be written that way.
 *
 * @param a First factor.
 * @param b Second factor.
 * @return Their product in Q31.
 */
static inline int32_t gaud_celt_mult32_32_q31(int32_t a, int32_t b) {
  int32_t high = (int32_t)((uint32_t)gaud_celt_mult16_16(a >> 16, b >> 16)
      << 1);
  int32_t mid_a = ((int32_t)(int16_t)(a >> 16)
                      * (int32_t)(uint16_t)(uint32_t)b)
      >> 15;
  int32_t mid_b = ((int32_t)(int16_t)(b >> 16)
                      * (int32_t)(uint16_t)(uint32_t)a)
      >> 15;
  return high + mid_a + mid_b;
}

/**
 * @brief Floor of the base-two logarithm. Undefined at and below zero.
 *
 * RFC 6716's `celt_ilog2`, one less than ::gaud_opus_ilog because that
 * one counts bits rather than naming an exponent.
 *
 * @param x Strictly positive.
 * @return The index of its highest set bit.
 */
static inline int gaud_celt_ilog2(int32_t x) {
  return (int)gaud_opus_ilog((uint32_t)x) - 1;
}

/**
 * @brief ::gaud_celt_ilog2, with zero and negatives answering zero.
 *
 * @param x Anything.
 * @return Zero if @p x is not positive, its floor-log2 otherwise.
 */
static inline int gaud_celt_zlog2(int32_t x) {
  return x <= 0 ? 0 : gaud_celt_ilog2(x);
}

/**
 * @brief Exact integer square root.
 *
 * Not an approximation: this is `floor(sqrt(value))` for every 32-bit
 * input, found one binary digit at a time. The theta split in section
 * 4.3.4.4 inverts a triangular number with it, so a result one out
 * changes which symbol is read next.
 *
 * @param value The radicand. Zero answers zero; RFC 6716's own version
 *   is undefined there and is never called with it.
 * @return The largest integer whose square does not exceed @p value.
 */
unsigned gaud_celt_isqrt32(uint32_t value);

/**
 * @brief Reciprocal approximation: Q15 in, Q16 out.
 *
 * A linear first guess refined by two Newton steps, with a maximum
 * relative error of 7.05e-5.
 *
 * @param x Strictly positive.
 * @return One over @p x, with both scaled as the summary says.
 */
int32_t gaud_celt_rcp(int32_t x);

/**
 * @brief Divide, through ::gaud_celt_rcp rather than by dividing.
 *
 * RFC 6716's `celt_div`. The quotient inherits that function's error,
 * which is the point: a real division here would give a different and
 * better answer, and so a different decoder.
 *
 * @param a The dividend.
 * @param b The divisor; strictly positive.
 * @return Their quotient.
 */
int32_t gaud_celt_div(int32_t a, int32_t b);

/**
 * @brief Reciprocal square root over [0.25,1): Q16 in, Q14 out.
 *
 * A minimax quadratic followed by one second-order Householder
 * iteration. RFC 6716's source states the result: a relative error of
 * at most 1.04956e-4, an RMS relative error of 2.80979e-5, and a peak
 * absolute error of 2.26591/16384.
 *
 * @param x In Q16, between 16384 and 65535.
 * @return One over its square root, in Q14.
 */
int16_t gaud_celt_rsqrt_norm(int32_t x);

/**
 * @brief Square root approximation: QX in, QX/2 out.
 *
 * @param x Not negative. Zero answers zero.
 * @return Its square root, at half the input's fractional precision.
 */
int32_t gaud_celt_sqrt(int32_t x);

/**
 * @brief Cosine of a quarter turn per unit: Q15 argument, Q15 result.
 *
 * Periodic with period four in the argument's units, so only the low 17
 * bits are read; the quarter-turn cases answer exactly rather than
 * through the polynomial.
 *
 * @param x The angle, where 32768 is a quarter turn.
 * @return Its cosine in Q15, between -32767 and 32767.
 */
int16_t gaud_celt_cos_norm(int32_t x);

/**
 * @brief Base-two exponential: Q10 in, Q16 out.
 *
 * @param x The exponent in Q10. Above 14 saturates and below -15 is zero.
 * @return Two to that power, in Q16.
 */
int32_t gaud_celt_exp2(int16_t x);

/**
 * @brief The cosine the stereo split's bit allocation is agreed on.
 *
 * Integer in both of RFC 6716's configurations, because the number it
 * produces decides how the split's bits are divided and section 4.3.3
 * requires the two ends to reach the same division. Distinct from
 * ::gaud_celt_cos_norm, which is the fixed-point build's answer to
 * something the normative build computes in `float`.
 *
 * **Its domain is 64 to 16320.** `itheta` is formed as `itheta*16384/qn`
 * with `qn` at most 256, and the two ends are answered before the call,
 * so nothing smaller than 64 arrives. Below that the squared argument
 * rounds to zero and the final `+1` carries the 16-bit result over to
 * -32768. RFC 6716 reaches the same value and guards it with an
 * assertion; this transcribes the behaviour rather than repairing it,
 * and a test pins it so that a later repair has to argue the case.
 *
 * @param x A quarter turn is 16384; 64 to 16320 is the useful range.
 * @return Its cosine in Q15.
 */
int16_t gaud_celt_bitexact_cos(int16_t x);

/**
 * @brief Base-two log of a tangent given as a sine and cosine, in Q11.
 *
 * The split's `delta`: how far the bits lean towards one half. Bit-exact
 * in both configurations for the same reason as ::gaud_celt_bitexact_cos.
 *
 * @param isin The sine, strictly positive.
 * @param icos The cosine, strictly positive.
 * @return `log2(isin/icos)` in Q11.
 */
int32_t gaud_celt_bitexact_log2tan(int32_t isin, int32_t icos);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_CELT_MATH_H
