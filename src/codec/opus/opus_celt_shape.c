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
 * One band's shape: the pulses, normalised, and then rotated.
 * RFC 6716 sections 4.3.4.2 and 4.3.4.3. Never installed.
 *
 * **The rotation is the only part of CELT the prose states completely.**
 * Section 4.3.4.3 gives the gain, the angle, the 2-D rotation, the order
 * the 2-D rotations are applied in, and the condition and stride for the
 * second pass - enough to write from, and therefore enough to check the
 * transcription against. It is worth saying why the rotation is there at
 * all: a band given four pulses across forty bins is four spikes, and
 * four spikes in the frequency domain are a comb filter in the time
 * domain, which is heard as a metallic ring. Spreading the energy back
 * out across the band turns the artifact into noise, which is what the
 * ear expects a quiet band to sound like.
 *
 * **The vector is normalised before it is rotated, not after.** The
 * rotation is orthogonal, so in exact arithmetic the order would not
 * matter; in 16 bits it does, and the reference normalises first.
 *
 * **The collapse mask is an output of this stage and is used two stages
 * later.** Section 4.3.5's anti-collapse needs to know which time blocks
 * ended up with no energy at all, and the only place that is cheap to
 * see is here, in the pulse vector, before the rotation smears a zero
 * block into its neighbours.
 */

#include "opus_celt.h"
#include "opus_celt_math.h"

/**
 * @brief Apply the chain of 2-D rotations, out and then back.
 *
 * Section 4.3.4.3's `R(x_1,x_2), ..., R(x_N-1,x_N), ..., R(x_1,x_2)`.
 * Going out and back makes the whole thing orthogonal in a way that a
 * single pass is not.
 *
 * @param x The block, adjusted in place.
 * @param len How many entries.
 * @param stride Which pairs to rotate: entries @p stride apart.
 * @param c The cosine of the angle, in Q15.
 * @param s Its sine, in Q15.
 */
static void rotate(int16_t * x, int len, int stride, int16_t c, int16_t s) {
  for (int i = 0; i < len - stride; ++i) {
    int16_t x1 = x[i];
    int16_t x2 = x[i + stride];
    x[i + stride] = (int16_t)((gaud_celt_mult16_16(c, x2)
                                  + gaud_celt_mult16_16(s, x1))
        >> 15);
    x[i] = (int16_t)((gaud_celt_mult16_16(c, x1) - gaud_celt_mult16_16(s, x2))
        >> 15);
  }
  // The reference forms `&X[len-2*stride-1]` before testing it, which is
  // a pointer one before the array when the loop will not run. Indexing
  // instead keeps that pointer from existing; the arithmetic is the same.
  for (int i = len - 2 * stride - 1; i >= 0; --i) {
    int16_t x1 = x[i];
    int16_t x2 = x[i + stride];
    x[i + stride] = (int16_t)((gaud_celt_mult16_16(c, x2)
                                  + gaud_celt_mult16_16(s, x1))
        >> 15);
    x[i] = (int16_t)((gaud_celt_mult16_16(c, x1) - gaud_celt_mult16_16(s, x2))
        >> 15);
  }
}

/**
 * @brief Section 4.3.4.3, in the decoder's direction only.
 *
 * @param x The band, adjusted in place.
 * @param len How many bins.
 * @param blocks How many time blocks it spans.
 * @param k How many pulses it was given.
 * @param spread Which of Table 59's four values.
 */
static void spread_band(
    int16_t * x, int len, int blocks, uint32_t k, unsigned spread) {
  // Table 59. A band with pulses in half its bins or more is already
  // dense enough that rotating it would only blur it.
  static const int kFactor[3] = {15, 10, 5};
  int factor;
  int16_t gain;
  int16_t theta;
  int16_t c;
  int16_t s;
  int stride2 = 0;
  if (2 * (int)k >= len || spread == CELT_SPREAD_NONE) {
    return;
  }
  factor = kFactor[spread - 1u];
  // g_r = N / (N + f_r*K), in Q15, and through celt_div rather than a
  // division - see opus_celt_math.h for why that distinction is real.
  gain = (int16_t)gaud_celt_div(gaud_celt_mult16_16(CELT_Q15ONE, len),
      len + factor * (int)k);
  // theta = pi*g_r*g_r/4, which in celt_cos_norm's units - where 32768
  // is a quarter turn - is just half of g_r squared.
  theta = (int16_t)(gaud_celt_mult16_16_q15(gain, gain) >> 1);
  c = gaud_celt_cos_norm(theta);
  s = gaud_celt_cos_norm(CELT_Q15ONE - (int32_t)theta);
  if (len >= 8 * blocks) {
    // round(sqrt(len/blocks)), counted up to rather than computed:
    // the largest stride2 with (stride2+0.5)^2 below len/blocks.
    stride2 = 1;
    while ((stride2 * stride2 + stride2) * blocks + (blocks >> 2) < len) {
      ++stride2;
    }
  }
  len /= blocks;
  for (int i = 0; i < blocks; ++i) {
    // The wide-block pass comes first and uses the complementary angle,
    // which is what swapping the sine and cosine amounts to.
    if (stride2 != 0) {
      rotate(x + i * len, len, stride2, s, c);
    }
    rotate(x + i * len, len, 1, c, s);
  }
}

/**
 * @brief Scale an integer pulse vector to @p gain and store it as Q15.
 *
 * @param y The pulses.
 * @param x Receives the result.
 * @param n How many.
 * @param ryy The sum of the squares of @p y, which is never zero.
 * @param gain The length to give the result, in Q15.
 */
static void normalise_residual(
    const int * y, int16_t * x, uint32_t n, int32_t ryy, int16_t gain) {
  // Normalising to 1/sqrt(ryy) through the Q16-in approximation means
  // first shifting ryy into [0.25,1), and the halved exponent is then
  // the shift the result needs back.
  int k = gaud_celt_ilog2(ryy) >> 1;
  int32_t t = gaud_celt_vshr32(ryy, 2 * (k - 7));
  int16_t g = (int16_t)gaud_celt_mult16_16_p15(gaud_celt_rsqrt_norm(t), gain);
  for (uint32_t i = 0; i < n; ++i) {
    x[i] = (int16_t)gaud_celt_pshr32(
        gaud_celt_mult16_16(g, y[i]), (unsigned)(k + 1));
  }
}

/**
 * @brief Which time blocks got at least one pulse.
 *
 * @param y The pulse vector.
 * @param n How many entries.
 * @param blocks How many time blocks they divide into.
 * @return One bit per block, set where that block has a nonzero entry.
 */
static unsigned collapse_mask(const int * y, uint32_t n, uint32_t blocks) {
  uint32_t per_block;
  unsigned mask = 0;
  if (blocks <= 1u) {
    return 1u;
  }
  per_block = n / blocks;
  for (uint32_t i = 0; i < blocks; ++i) {
    for (uint32_t j = 0; j < per_block; ++j) {
      mask |= (unsigned)(y[i * per_block + j] != 0) << i;
    }
  }
  return mask;
}

void gaud_celt_renormalise_vector(int16_t * x, uint32_t n, int16_t gain) {
  // EPSILON, so that a band of all zeros still has a logarithm.
  int32_t energy = 1;
  int k;
  int32_t t;
  int16_t g;
  for (uint32_t i = 0; i < n; ++i) {
    energy += gaud_celt_mult16_16(x[i], x[i]);
  }
  k = gaud_celt_ilog2(energy) >> 1;
  t = gaud_celt_vshr32(energy, 2 * (k - 7));
  g = (int16_t)gaud_celt_mult16_16_p15(gaud_celt_rsqrt_norm(t), gain);
  for (uint32_t i = 0; i < n; ++i) {
    x[i] = (int16_t)gaud_celt_pshr32(
        gaud_celt_mult16_16(g, x[i]), (unsigned)(k + 1));
  }
}

unsigned gaud_celt_alg_unquant(int16_t * x, uint32_t n, uint32_t k,
    unsigned spread, uint32_t blocks, OPUS_Range * range, int16_t gain) {
  int y[CELT_MAX_BAND_BINS];
  int32_t ryy = 0;
  gaud_celt_decode_pulses(range, y, n, k);
  for (uint32_t i = 0; i < n; ++i) {
    ryy += gaud_celt_mult16_16(y[i], y[i]);
  }
  normalise_residual(y, x, n, ryy, gain);
  spread_band(x, (int)n, (int)blocks, k, spread);
  return collapse_mask(y, n, blocks);
}
