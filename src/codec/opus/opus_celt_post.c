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
 * What happens to CELT's samples after the transform: the post-filter
 * and the de-emphasis. RFC 6716 sections 4.3.7.1 and 4.3.7.2.
 * Never installed.
 *
 * **These two are stated completely in the prose**, coefficients and
 * all - nine filter taps as decimals and one `alpha_p` - so the
 * constants here are checkable against the document rather than only
 * against Appendix A. They agree: every tap's Q15 value divided by
 * 32768 is exactly the decimal the prose prints.
 *
 * **The post-filter is recursive and the decoder runs it in place.**
 * Section 4.3.7.1 says so in as many words - "It is important that
 * values of y(n) be interpolated one at a time such that the past
 * value of y(n) used is interpolated" - and the reference passes the
 * same pointer for both arguments. Writing it with separate buffers
 * would be a different filter that looks like the same one.
 *
 * The prose's formula for it has a typo, incidentally: it prints
 * `g1*(y(n-T+1)+y(n-T+1))` where it means `y(n-T-1)+y(n-T+1)`, and the
 * same for `g2`. The source taps both sides, which is the only reading
 * under which the filter is symmetric and the only one that matches
 * the description of a comb filter.
 */

#include "opus_celt.h"
#include "opus_celt_math.h"

/**
 * @brief Multiply a 32-bit value by a Q15 one, RFC 6716's `MULT16_32_Q15`.
 *
 * @param twiddle The Q15 factor.
 * @param value The 32-bit one.
 * @return Their product, in the second factor's scale.
 */
static inline int32_t mult16_32_q15(int32_t twiddle, int32_t value) {
  return gaud_celt_shl32(gaud_celt_mult16_16(twiddle, value >> 16), 1)
      + (((int32_t)(int16_t)twiddle * (int32_t)(uint16_t)(uint32_t)value)
          >> 15);
}

/**
 * The three tap sets, in Q15.
 *
 * Section 4.3.7.1 prints these as decimals and each of these integers
 * divided by 32768 is exactly that decimal, which is a check the
 * document supplies on its own appendix.
 */
static const int16_t kTaps[3][3] = {
  {10048, 7112, 4248},
  {15200, 8784, 0},
  {26208, 3280, 0},
};

void gaud_celt_comb_filter(int32_t * out, const int32_t * in,
    int period_old, int period, int n, int16_t gain_old, int16_t gain,
    unsigned tapset_old, unsigned tapset, const int16_t * window,
    uint32_t overlap) {
  int16_t old0 = (int16_t)gaud_celt_mult16_16_q15(gain_old, kTaps[tapset_old][0]);
  int16_t old1 = (int16_t)gaud_celt_mult16_16_q15(gain_old, kTaps[tapset_old][1]);
  int16_t old2 = (int16_t)gaud_celt_mult16_16_q15(gain_old, kTaps[tapset_old][2]);
  int16_t new0 = (int16_t)gaud_celt_mult16_16_q15(gain, kTaps[tapset][0]);
  int16_t new1 = (int16_t)gaud_celt_mult16_16_q15(gain, kTaps[tapset][1]);
  int16_t new2 = (int16_t)gaud_celt_mult16_16_q15(gain, kTaps[tapset][2]);
  for (int i = 0; i < (int)overlap; ++i) {
    // The square of the window, so the two filters cross over smoothly
    // rather than the pitch jumping between frames.
    int16_t f = (int16_t)gaud_celt_mult16_16_q15(window[i], window[i]);
    int16_t inverse = (int16_t)(CELT_Q15ONE - f);
    out[i] = in[i]
        + mult16_32_q15(gaud_celt_mult16_16_q15(inverse, old0), in[i - period_old])
        + mult16_32_q15(gaud_celt_mult16_16_q15(inverse, old1),
              in[i - period_old - 1])
        + mult16_32_q15(gaud_celt_mult16_16_q15(inverse, old1),
              in[i - period_old + 1])
        + mult16_32_q15(gaud_celt_mult16_16_q15(inverse, old2),
              in[i - period_old - 2])
        + mult16_32_q15(gaud_celt_mult16_16_q15(inverse, old2),
              in[i - period_old + 2])
        + mult16_32_q15(gaud_celt_mult16_16_q15(f, new0), in[i - period])
        + mult16_32_q15(gaud_celt_mult16_16_q15(f, new1), in[i - period - 1])
        + mult16_32_q15(gaud_celt_mult16_16_q15(f, new1), in[i - period + 1])
        + mult16_32_q15(gaud_celt_mult16_16_q15(f, new2), in[i - period - 2])
        + mult16_32_q15(gaud_celt_mult16_16_q15(f, new2), in[i - period + 2]);
  }
  for (int i = (int)overlap; i < n; ++i) {
    out[i] = in[i] + mult16_32_q15(new0, in[i - period])
        + mult16_32_q15(new1, in[i - period - 1])
        + mult16_32_q15(new1, in[i - period + 1])
        + mult16_32_q15(new2, in[i - period - 2])
        + mult16_32_q15(new2, in[i - period + 2]);
  }
}

void gaud_celt_deemphasis(const int32_t * const * in, int16_t * pcm, int n,
    uint32_t channels, int downsample, int32_t * memory) {
  // alpha_p = 0.8500061035, and the output scaling the reference folds
  // into the same pass: coef[3] of 8192 is a quarter, undone by the
  // shift of two below.
  static const int16_t kCoef0 = 27853;
  static const int16_t kCoef1 = 0;
  static const int16_t kCoef3 = 8192;
  for (uint32_t c = 0; c < channels; ++c) {
    const int32_t * source = in[c];
    int16_t * target = pcm + c;
    int32_t state = memory[c];
    int count = 0;
    for (int j = 0; j < n; ++j) {
      int32_t sum = source[j] + state;
      int32_t sample;
      state = mult16_32_q15(kCoef0, sum) - mult16_32_q15(kCoef1, source[j]);
      sample = gaud_celt_shl32(mult16_32_q15(kCoef3, sum), 2);
      // Back from the 12-bit headroom the synthesis carries, clamped
      // rather than wrapped: a loud frame that overflows should be
      // loud, not inverted.
      sample = gaud_celt_pshr32(sample, 12);
      sample = gaud_celt_max32(sample, -32768);
      sample = gaud_celt_min32(sample, 32767);
      if (count == 0) {
        *target = (int16_t)sample;
      }
      if (++count == downsample) {
        target += channels;
        count = 0;
      }
    }
    memory[c] = state;
  }
}
