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
 * The energy envelope, and the Laplace distribution it is coded with.
 *
 * RFC 6716 section 4.3.2 calls this a "three-step coarse-fine-fine
 * strategy", and the three steps are in three different places in the
 * bitstream with the whole bit allocation computed between the first
 * and the second. That ordering is the reason this file exists
 * separately from the allocator: the coarse pass must be decodable
 * before anything knows how many bits the fine pass will have.
 *
 * **Why the energy is predicted twice.** Coarse energy is coded at six
 * decibels of resolution, which would be far too coarse on its own, and
 * then predicted both from the same band in the previous frame and from
 * the previous band in this one. Section 4.3.2.1 gives the prediction
 * as a two-dimensional filter and the decoder runs it in the base-two
 * log domain, so the arithmetic is all addition. The time half can be
 * switched off per frame - an "intra" frame - which is what stops a
 * lost packet from colouring everything after it indefinitely.
 *
 * **The clamps are part of the format.** Section 4.3.2.1 says the
 * prediction "is clamped internally so that fixed-point
 * implementations with limited dynamic range always remain in the same
 * state as floating point implementations". So the two bounds below
 * are not defensive programming and must not be loosened: they are how
 * the two configurations of the reference stay in step, and a decoder
 * that omitted them would drift from both.
 */

#include "opus_celt.h"
#include <string.h>

/** The smallest probability the Laplace distribution's tail falls to. */
#define LAPLACE_MINP 1

/** Its base-two logarithm, which the tail arithmetic shifts by. */
#define LAPLACE_LOG_MINP 0

/** How many values at each end are held at that floor. */
#define LAPLACE_NMIN 16

/**
 * The frequency of the first non-zero value.
 *
 * Section 4.3.2.1 leaves this to the reference; it is the total
 * probability not already spent on zero and on the floor, scaled by
 * the decay.
 */
static uint32_t laplace_freq1(uint32_t fs0, int decay) {
  uint32_t ft = 32768u - LAPLACE_MINP * (2u * LAPLACE_NMIN) - fs0;
  return (ft * (uint32_t)(16384 - decay)) >> 15;
}

int gaud_celt_laplace_decode(OPUS_Range * range, uint32_t fs, int decay) {
  int value = 0;
  uint32_t fl = 0;
  uint32_t fm = gaud_opus_decode_bin(range, 15);

  if (fm >= fs) {
    ++value;
    fl = fs;
    fs = laplace_freq1(fs, decay) + LAPLACE_MINP;
    /* The decaying part: each step halves what is left, so the loop
     * runs at most as many times as the decay allows. */
    while (fs > LAPLACE_MINP && fm >= fl + 2u * fs) {
      fs *= 2u;
      fl += fs;
      fs = ((fs - 2u * LAPLACE_MINP) * (uint32_t)decay) >> 15;
      fs += LAPLACE_MINP;
      ++value;
    }
    /*
     * Past the decaying part every remaining value has the same floor
     * probability, so the rest of the distance is divided rather than
     * walked. This is what lets one very large prediction error be
     * coded at all instead of the distribution simply ending.
     */
    if (fs <= LAPLACE_MINP) {
      uint32_t di = (fm - fl) >> (LAPLACE_LOG_MINP + 1);
      value += (int)di;
      fl += 2u * di * LAPLACE_MINP;
    }
    if (fm < fl + fs) {
      value = -value;
    } else {
      fl += fs;
    }
  }
  gaud_opus_dec_update(
      range, fl, (uint32_t)gaud_celt_min32((int32_t)(fl + fs), 32768), 32768u);
  return value;
}

bool gaud_celt_mode_init(CELT_Mode * mode, unsigned lm) {
  if (lm > CELT_MAX_LM) {
    return false;
  }
  mode->lm = lm;
  mode->shorts = 1u << lm;
  mode->size = (uint32_t)CELT_SHORT_MDCT << lm;
  mode->edges = gaud_opus_eband5ms;
  mode->bands = CELT_BANDS;
  return true;
}

/** The distribution for a band with almost no bits left to spend. */
static const unsigned char small_energy_icdf[3] = {2, 1, 0};

void gaud_celt_decode_coarse_energy(OPUS_Range * range,
    const CELT_Mode * mode, int16_t * energy, uint32_t start, uint32_t end,
    bool intra, uint32_t channels) {
  const unsigned char * model
      = gaud_opus_e_prob_model + ((size_t)mode->lm * 2u + (intra ? 1u : 0u))
      * 42u;
  int32_t previous[2] = {0, 0};
  int32_t coefficient;
  int32_t beta;

  if (intra) {
    coefficient = 0;
    beta = gaud_opus_beta_intra[0];
  } else {
    coefficient = gaud_opus_pred_coef[mode->lm];
    beta = gaud_opus_beta_coef[mode->lm];
  }

  /*
   * The budget is the whole frame in bits. Three fallbacks below read
   * progressively cheaper symbols as it runs out, and the last reads
   * nothing at all - a frame can end before its energy is fully coded,
   * and the format says what the rest of it then is rather than
   * treating it as corrupt.
   */
  int32_t budget = (int32_t)range->size * 8;

  for (uint32_t band = start; band < end; ++band) {
    for (uint32_t channel = 0; channel < channels; ++channel) {
      int32_t qi;
      int32_t tell = (int32_t)gaud_opus_tell(range);
      if (budget - tell >= 15) {
        uint32_t at = 2u * (band < 20u ? band : 20u);
        qi = gaud_celt_laplace_decode(range,
            (uint32_t)model[at] << 7, (int)model[at + 1u] << 6);
      } else if (budget - tell >= 2) {
        qi = gaud_opus_dec_icdf(range, small_energy_icdf, 2);
        /* Zig-zag: 0, -1, 1 becomes 0, -1, +1 around zero. */
        qi = (qi >> 1) ^ -(qi & 1);
      } else if (budget - tell >= 1) {
        qi = -gaud_opus_dec_bit_logp(range, 1);
      } else {
        qi = -1;
      }

      size_t slot = (size_t)channel * mode->bands + band;
      int32_t q = gaud_celt_shl32(qi, CELT_DB_SHIFT);

      /* Section 4.3.2.1's clamps, both of them. The first keeps the
       * previous frame's value from dragging this one arbitrarily low;
       * the second is the one the specification names explicitly, and
       * is what keeps a fixed-point decoder in the same state as a
       * floating-point one. */
      energy[slot] = (int16_t)gaud_celt_max32(
          -(9 << CELT_DB_SHIFT), energy[slot]);
      /*
       * A 16-by-16 multiply into 32 bits, which is what the format's
       * arithmetic is defined as and what the smallest target it names
       * can do. Widening either operand first would be a different
       * computation on an energy that had left 16-bit range.
       */
      int32_t tmp = gaud_celt_pshr32(
                        (int32_t)(int16_t)coefficient * (int32_t)energy[slot],
                        8)
          + previous[channel] + gaud_celt_shl32(q, 7);
      tmp = gaud_celt_max32(-(28 << (CELT_DB_SHIFT + 7)), tmp);
      energy[slot] = (int16_t)gaud_celt_pshr32(tmp, 7);
      previous[channel] = previous[channel] + gaud_celt_shl32(q, 7)
          - (int32_t)(int16_t)beta * gaud_celt_pshr32(q, 8);
    }
  }
}

void gaud_celt_decode_fine_energy(OPUS_Range * range,
    const CELT_Mode * mode, int16_t * energy, const int * fine,
    uint32_t start, uint32_t end, uint32_t channels) {
  for (uint32_t band = start; band < end; ++band) {
    if (fine[band] <= 0) {
      continue;
    }
    for (uint32_t channel = 0; channel < channels; ++channel) {
      uint32_t q2 = gaud_opus_dec_bits(range, (unsigned)fine[band]);
      /*
       * Section 4.3.2.2 states the correction as (f + 1/2)/2**B - 1/2,
       * which in Q10 is this: shift the value up, add a half, shift
       * down by the number of bits, and subtract the half back.
       */
      int32_t offset
          = (gaud_celt_shl32((int32_t)q2, CELT_DB_SHIFT)
                + (1 << (CELT_DB_SHIFT - 1)))
              >> fine[band];
      offset -= 1 << (CELT_DB_SHIFT - 1);
      size_t slot = (size_t)channel * mode->bands + band;
      energy[slot] = (int16_t)(energy[slot] + offset);
    }
  }
}

void gaud_celt_decode_final_energy(OPUS_Range * range,
    const CELT_Mode * mode, int16_t * energy, const int * fine,
    const int * priority, int32_t left, uint32_t start, uint32_t end,
    uint32_t channels) {
  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t band = start;
        band < end && left >= (int32_t)channels; ++band) {
      if (fine[band] >= CELT_MAX_FINE_BITS || priority[band] != pass) {
        continue;
      }
      for (uint32_t channel = 0; channel < channels; ++channel) {
        uint32_t q2 = gaud_opus_dec_bits(range, 1u);
        int32_t offset
            = (gaud_celt_shl32((int32_t)q2, CELT_DB_SHIFT)
                  - (1 << (CELT_DB_SHIFT - 1)))
            >> (fine[band] + 1);
        size_t slot = (size_t)channel * mode->bands + band;
        energy[slot] = (int16_t)(energy[slot] + offset);
        --left;
      }
    }
  }
}
