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
 * Putting the energy back: RFC 6716 sections 4.3.5 and 4.3.6.
 * Never installed.
 *
 * Section 4.3.4 produced unit-norm shape vectors and section 4.3.2
 * decoded the envelope separately; these two put them back together.
 * Between them sits anti-collapse, which exists because the separation
 * has one failure mode.
 *
 * **A band's shape can be silent where its envelope is not.** In a
 * transient frame a band is divided into time blocks, and if the
 * allocator could not afford the band then some of those blocks come
 * out of the shape coder as exact zeros. A zero between two loud blocks
 * is a hole in time - and a hole in time is precisely the artifact the
 * transient machinery exists to avoid, so hearing one there is worse
 * than hearing the quantisation noise that would otherwise be there.
 * Section 4.3.5 fills the hole with noise, at a level taken from how
 * far the band's energy has risen above the two frames before: a band
 * that just got much louder is one where a hole would be audible.
 */

#include "opus_celt.h"
#include "opus_celt_math.h"
#include <string.h>

void gaud_celt_anti_collapse(const CELT_Mode * mode, int16_t * x,
    const unsigned char * collapse, uint32_t channels, uint32_t size,
    uint32_t start, uint32_t end, const int16_t * energy,
    const int16_t * previous1, const int16_t * previous2,
    const int32_t * pulses, uint32_t seed) {
  for (uint32_t band = start; band < end; ++band) {
    int32_t width = mode->edges[band + 1u] - mode->edges[band];
    int32_t depth;
    int16_t threshold;
    int16_t inverse_root;
    int shift;
    // Depth in eighths of a bit per bin: how finely this band was coded.
    depth = (1 + pulses[band]) / (width << mode->lm);
    threshold = (int16_t)gaud_celt_mult16_16_q15(16384,
        gaud_celt_min32(32767,
            gaud_celt_exp2((int16_t)-(depth << (10 - CELT_BITRES))) >> 1));
    {
      int32_t bins = width << mode->lm;
      shift = gaud_celt_ilog2(bins) >> 1;
      inverse_root = gaud_celt_rsqrt_norm(
          gaud_celt_shl32(bins, (unsigned)((7 - shift) << 1)));
    }
    for (uint32_t channel = 0; channel < channels; ++channel) {
      int16_t * band_x = x + channel * size
          + ((uint32_t)mode->edges[band] << mode->lm);
      int16_t one_ago = previous1[channel * CELT_BANDS + band];
      int16_t two_ago = previous2[channel * CELT_BANDS + band];
      int32_t risen;
      int16_t level;
      bool refilled = false;
      if (channels == 1u) {
        // A mono frame still carries two channels of history, because
        // the stream may have been stereo a frame ago.
        one_ago = (int16_t)gaud_celt_max32(one_ago, previous1[CELT_BANDS + band]);
        two_ago = (int16_t)gaud_celt_max32(two_ago, previous2[CELT_BANDS + band]);
      }
      risen = (int32_t)energy[channel * CELT_BANDS + band]
          - gaud_celt_min32(one_ago, two_ago);
      risen = gaud_celt_max32(0, risen);
      if (risen < 16384) {
        level = (int16_t)(2
            * gaud_celt_min32(16383, gaud_celt_exp2((int16_t)-risen) >> 1));
      } else {
        level = 0;
      }
      // A 20 ms frame's short blocks hold less energy each, so the
      // noise has to be louder by the square root of two.
      if (mode->lm == 3u) {
        level = (int16_t)(gaud_celt_mult16_16(23170,
                              gaud_celt_min32(23169, level))
            >> 14);
      }
      level = (int16_t)(gaud_celt_min32(threshold, level) >> 1);
      level = (int16_t)(gaud_celt_mult16_16_q15(inverse_root, level) >> shift);
      for (uint32_t block = 0; block < (1u << mode->lm); ++block) {
        if ((collapse[band * channels + channel] & (1u << block)) != 0u) {
          continue;
        }
        for (int32_t j = 0; j < width; ++j) {
          seed = gaud_celt_lcg_rand(seed);
          band_x[((uint32_t)j << mode->lm) + block] = (seed & 0x8000u)
              ? level
              : (int16_t)-level;
        }
        refilled = true;
      }
      if (refilled) {
        // Energy was added, so the band is no longer unit-norm.
        gaud_celt_renormalise_vector(band_x,
            (uint32_t)(width << mode->lm), CELT_Q15ONE);
      }
    }
  }
}

void gaud_celt_log2_amp(int32_t * amplitude, const int16_t * energy,
    uint32_t start, uint32_t end, uint32_t channels) {
  for (uint32_t channel = 0; channel < channels; ++channel) {
    uint32_t base = channel * CELT_BANDS;
    for (uint32_t band = 0; band < CELT_BANDS; ++band) {
      if (band < start || band >= end) {
        amplitude[base + band] = 0;
        continue;
      }
      // The envelope is relative to a per-band mean, which is in Q4
      // decibels and so needs six more bits to reach DB_SHIFT.
      int16_t level = gaud_celt_add16(energy[base + band],
          (int16_t)((uint16_t)(uint32_t)(int32_t)gaud_opus_e_means[band] << 6));
      amplitude[base + band] = gaud_celt_pshr32(gaud_celt_exp2(level), 4);
    }
  }
}

void gaud_celt_denormalise_bands(const CELT_Mode * mode, const int16_t * x,
    int32_t * out, const int32_t * amplitude, uint32_t end,
    uint32_t channels) {
  uint32_t frame = mode->size;
  for (uint32_t channel = 0; channel < channels; ++channel) {
    const int16_t * shape = x + channel * frame;
    int32_t * target = out + channel * frame;
    uint32_t bin = 0;
    for (uint32_t band = 0; band < end; ++band) {
      // Halved here and shifted back by two below, which keeps the
      // 16-by-32 multiply from overflowing on a loud band.
      int32_t gain = amplitude[channel * CELT_BANDS + band] >> 1;
      uint32_t band_end = (uint32_t)mode->edges[band + 1u] << mode->lm;
      for (; bin < band_end; ++bin) {
        int32_t scaled = gaud_celt_shl32(
            gaud_celt_mult16_16(shape[bin], gain >> 16), 1)
            + (((int32_t)(int16_t)shape[bin]
                   * (int32_t)(uint16_t)(uint32_t)gain)
                >> 15);
        target[bin] = gaud_celt_shl32(scaled, 2);
      }
    }
    // Above the last coded band there is nothing, and saying so is what
    // band-limits the output.
    for (; bin < frame; ++bin) {
      target[bin] = 0;
    }
  }
}
