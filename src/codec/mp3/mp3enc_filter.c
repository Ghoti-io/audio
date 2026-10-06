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
 * The analysis filterbank: the decoder's synthesis run backwards.
 *
 * Two stages, as in the decoder and for the same reason. The **polyphase**
 * stage turns each 32 input samples into one sample of each of 32
 * subbands, using the window of 11172-3 Table 3-B.3 (the encoder's
 * coefficients are that table divided by 32, which is why the decoder's
 * and the encoder's tables are one table here) and a 32 x 64 cosine matrix.
 * The **hybrid** stage then takes 18 consecutive slots of each subband
 * through an MDCT - 36 long, or three of 12 - and undoes, with a rotation,
 * the aliasing butterflies the decoder applies after it.
 *
 * **Scale.** The decoder's inverse transforms carry no normalisation, so
 * the forward ones do: 1/9 for the 36-point transform and 1/3 for the
 * 12-point, which is what makes analysis followed by synthesis the identity
 * (the factor a TDAC pair needs is 4/N). The decoder's Q28 unit is 1.0 =
 * full scale = 32768 in a 16-bit sample, so the analysis scales its input
 * by 2^13 on the way through; `mp3_decode.c` removes the same 2^13.
 *
 * **Word widths.** An input sample is 16 bits, the window is Q28, so one
 * tap is 44 bits and the eight-tap sum 47. It is brought down to a Q10
 * subband input (31 bits) before the 64-term matrix, whose accumulator is
 * then 59 bits; nothing here overflows an int64 whatever the input.
 */

#include "mp3enc_internal.h"

/** Round(2^28 / 9) and Round(2^28 / 3): the forward transforms' scales. */
#define SCALE_LONG 29826162
#define SCALE_SHORT 89478485

static int32_t mul(int32_t a, int32_t b) {
  int64_t product = (int64_t)a * b;
  return (int32_t)((product + (1 << (MP3_Q - 1))) >> MP3_Q);
}

/**
 * A transform's accumulated sum, scaled by 1/9 or 1/3 (Q28) into a line.
 *
 * **The scale has to be applied before the value is narrowed.** A full-scale
 * low-frequency wave puts the same sign in all 36 inputs of a subband and
 * its first coefficient is their sum, which is eleven times the largest
 * input: 2^32.5 where an int32 holds 2^31, before the 1/9 that brings it
 * back inside. A first draft narrowed first. Sine tones at half scale and
 * everything in the unit tests fit; a full-scale square wave came back as
 * something else, and the validity check that gave it one found it.
 */
static int32_t scale(int64_t sum, int32_t factor) {
  int64_t line = (sum + (1 << 27)) >> MP3_Q;
  return (int32_t)((line * factor + (1 << 27)) >> MP3_Q);
}

void gaud_mp3e_filter_reset(MP3E_Filter * filter) {
  memset(filter, 0, sizeof(*filter));
}

/** One slot: 32 input samples in, 32 subband samples out. */
static void analyze_slot(MP3E_Filter * filter, const int16_t * samples,
    size_t stride, int32_t out[32]) {
  int32_t * x = filter->x;
  memmove(x + 32, x, 480u * sizeof(x[0]));
  for (unsigned i = 0; i < 32u; ++i) {
    x[31u - i] = samples[(size_t)i * stride];
  }
  int32_t y[64];
  for (unsigned i = 0; i < 64u; ++i) {
    int64_t sum = 0;
    for (unsigned j = 0; j < 8u; ++j) {
      sum += (int64_t)x[i + 64u * j] * gaud_mp3_window[i + 64u * j];
    }
    /* /32 for C = D/32, and down to Q10. */
    y[i] = (int32_t)((sum + (1 << 22)) >> 23);
  }
  for (unsigned k = 0; k < 32u; ++k) {
    int64_t sum = 0;
    for (unsigned i = 0; i < 64u; ++i) {
      sum += (int64_t)gaud_mp3enc_ana_cos[k][i] * y[i];
    }
    out[k] = (int32_t)((sum + (1 << 24)) >> 25);
  }
}

void gaud_mp3e_filter_analyze(
    MP3E_Filter * filter, const int16_t * samples, size_t stride) {
  filter->current ^= 1u;
  for (unsigned slot = 0; slot < 18u; ++slot) {
    int32_t * out = filter->slot[filter->current][slot];
    analyze_slot(filter, samples + (size_t)slot * 32u * stride, stride, out);
    /* Frequency inversion, the decoder's last step before the polyphase
     * run backwards: every second sample of every second subband. */
    for (unsigned sb = 1; sb < 32u; sb += 2u) {
      if (slot & 1u) {
        out[sb] = -out[sb];
      }
    }
  }
}

void gaud_mp3e_filter_transform(
    const MP3E_Filter * filter, unsigned block_type, int32_t out[MP3E_LINES]) {
  const int32_t (*older)[32] = filter->slot[filter->current ^ 1u];
  const int32_t (*newer)[32] = filter->slot[filter->current];
  for (unsigned sb = 0; sb < 32u; ++sb) {
    int32_t t[36];
    for (unsigned i = 0; i < 18u; ++i) {
      t[i] = older[i][sb];
      t[18u + i] = newer[i][sb];
    }
    int32_t * lines = out + sb * 18u;
    if (block_type == 2u) {
      for (unsigned w = 0; w < 3u; ++w) {
        int32_t z[12];
        for (unsigned i = 0; i < 12u; ++i) {
          z[i] = mul(t[6u + w * 6u + i], gaud_mp3_short_window[i]);
        }
        for (unsigned k = 0; k < 6u; ++k) {
          int64_t sum = 0;
          for (unsigned i = 0; i < 12u; ++i) {
            sum += (int64_t)z[i] * gaud_mp3_imdct12[i][k];
          }
          lines[w * 6u + k] = scale(sum, SCALE_SHORT);
        }
      }
    }
    else {
      const int32_t * window = gaud_mp3_block_window[block_type];
      int32_t z[36];
      for (unsigned i = 0; i < 36u; ++i) {
        z[i] = mul(t[i], window[i]);
      }
      for (unsigned k = 0; k < 18u; ++k) {
        int64_t sum = 0;
        for (unsigned i = 0; i < 36u; ++i) {
          sum += (int64_t)z[i] * gaud_mp3_imdct36[i][k];
        }
        lines[k] = scale(sum, SCALE_LONG);
      }
    }
  }
  if (block_type == 2u) {
    return;
  }
  /* The aliasing butterflies, inverted. The decoder rotates each pair by
   * an angle; a rotation is undone by its transpose. */
  for (unsigned boundary = 1; boundary <= 31u; ++boundary) {
    int32_t * lower = out + 18u * boundary;
    for (unsigned i = 0; i < 8u; ++i) {
      int32_t above = lower[i];
      int32_t below = lower[-1 - (int)i];
      lower[-1 - (int)i]
          = mul(below, gaud_mp3_cs[i]) + mul(above, gaud_mp3_ca[i]);
      lower[i] = mul(above, gaud_mp3_cs[i]) - mul(below, gaud_mp3_ca[i]);
    }
  }
}
