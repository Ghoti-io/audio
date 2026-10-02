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
 * The bit allocation: RFC 6716 section 4.3.3.
 *
 * **This is the file where being nearly right is worth nothing.** The
 * specification states the standard in its first paragraph: the
 * allocation "MUST be recovered exactly so that identical coding
 * decisions are made in the encoder and decoder", and "any deviation
 * from the reference's resulting bit allocation will result in
 * corrupted output". The reason is structural rather than a matter of
 * quality. Almost nothing about the allocation is transmitted: it is
 * computed from the frame size, the channel count, a handful of decoded
 * flags and the number of bits left over - and the result then decides
 * how many symbols are read next. An allocator one eighth of a bit out
 * in one band does not make that band sound slightly worse. It makes
 * the range decoder read the wrong number of symbols, and everything
 * after it in the frame is noise.
 *
 * So this is a transcription, and deliberately a close one. Where a
 * clearer formulation existed it was not taken, because "produces
 * identical results" is the whole specification and a rearrangement
 * that is obviously equivalent is a rearrangement whose equivalence
 * nobody has checked over the whole input space.
 *
 * The shape of it, in the order section 4.3.3 lists:
 *
 *  1. the per-band ceiling, from the cache table;
 *  2. the boosts, which *are* transmitted, one band at a time, with a
 *     probability that drops as soon as any band is boosted;
 *  3. the trim, which tilts the whole curve towards high or low bands;
 *  4. a search of the eleven-row static table for the richest row that
 *     fits, then a second, finer search interpolating between that row
 *     and the next in 64 steps;
 *  5. band skipping, decided from the top down, each skip reclaiming
 *     its band's bits for the bands below - and each costing a bit that
 *     has to have been reserved in advance;
 *  6. the intensity and dual-stereo parameters, if two channels and if
 *     room was reserved for them;
 *  7. the split between fine energy and shape, band by band, with a
 *     running balance carried forward.
 *
 * **Bits are reserved before they are spent.** Steps 5 and 6 each need
 * a symbol that can only be read if the budget allowed for it, so the
 * budget is reduced up front and the reservation handed back if the
 * decision turns out not to be coded. Getting that order wrong is the
 * easiest way to be one bit out, and one bit is enough.
 */

#include "opus_celt.h"
#include <stddef.h>
#include <string.h>

/** Steps in the fine interpolation between two table rows: 2**6. */
#define CELT_ALLOC_STEPS 6

/** How far the fine-energy split is biased, in eighths of a bit. */
#define CELT_FINE_OFFSET 21

/** Rows in the static allocation table. */
#define CELT_ALLOC_VECTORS 11


/**
 * A band's width in *short-MDCT* bins, which is to say unscaled.
 *
 * **The allocator works in two units and they are easy to confuse.**
 * The band edge table is in 2.5 ms bins; a frame of `1 << lm` short
 * MDCTs has `1 << lm` times as many real bins. Some of the arithmetic
 * below wants the real count - the caps and the boost quanta, which are
 * about how much signal there is - and most of it wants the unscaled
 * one, because the scaling is then applied explicitly a line later.
 *
 * Writing all of it in scaled units applies `<< lm` twice, which is how
 * the first draft of this file was wrong. It got the caps and the
 * boosts right, because those two want the scaled width, and then
 * allocated roughly `1 << lm` times too much to every band, skipped the
 * top five of them to pay for it, and produced an allocation that
 * agreed with the reference on none of its outputs.
 */
static inline int32_t short_width(const CELT_Mode * mode, uint32_t band) {
  return mode->edges[band + 1u] - mode->edges[band];
}

/** A band edge in short-MDCT bins, for the running totals below. */
static inline int32_t short_edge(const CELT_Mode * mode, uint32_t band) {
  return mode->edges[band];
}

void gaud_celt_init_caps(const CELT_Mode * mode, int32_t * cap,
    uint32_t channels) {
  for (uint32_t band = 0; band < mode->bands; ++band) {
    int32_t n = (int32_t)(gaud_celt_band_start(mode, band + 1u)
        - gaud_celt_band_start(mode, band));
    /* The cache is indexed by frame size and channel count together,
     * eight combinations of 21 bands. */
    size_t at = (size_t)mode->bands * (2u * mode->lm + channels - 1u) + band;
    cap[band] = ((int32_t)gaud_opus_cache_caps50[at] + 64)
        * (int32_t)channels * n >> 2;
  }
}

void gaud_celt_tf_decode(OPUS_Range * range, const CELT_Mode * mode,
    uint32_t start, uint32_t end, bool transient, int * tf_res) {
  uint32_t budget = range->size * 8u;
  uint32_t tell = gaud_opus_tell(range);
  int logp = transient ? 2 : 4;
  /*
   * The select flag is reserved before the per-band flags are read,
   * because whether there is room for it depends on the budget before
   * they are read rather than after. It is then only actually read if
   * it could change the answer - see below.
   */
  int select_rsv = (mode->lm > 0 && tell + (uint32_t)logp + 1u <= budget)
      ? 1 : 0;
  budget -= (uint32_t)select_rsv;
  int changed = 0;
  int current = 0;
  for (uint32_t band = start; band < end; ++band) {
    if (tell + (uint32_t)logp <= budget) {
      current ^= gaud_opus_dec_bit_logp(range, (unsigned)logp);
      tell = gaud_opus_tell(range);
      changed |= current;
    }
    tf_res[band] = current;
    logp = transient ? 4 : 5;
  }
  int select = 0;
  /*
   * Section 4.3.1: the flag "is only decoded if it can have an impact
   * on the result knowing the value of all per-band tf_change flags".
   * So the two rows of the table are compared first, and when they
   * agree no bit is read at all - a decoder that read it unconditionally
   * would be one bit out on exactly the frames where the table happens
   * to be flat.
   */
  size_t row = (size_t)mode->lm * 8u;
  if (select_rsv
      && gaud_opus_tf_select_table[row + 4u * (transient ? 1u : 0u)
             + 0u + (uint32_t)changed]
          != gaud_opus_tf_select_table[row + 4u * (transient ? 1u : 0u)
              + 2u + (uint32_t)changed]) {
    select = gaud_opus_dec_bit_logp(range, 1);
  }
  for (uint32_t band = start; band < end; ++band) {
    tf_res[band] = gaud_opus_tf_select_table[row
        + 4u * (transient ? 1u : 0u) + 2u * (uint32_t)select
        + (uint32_t)tf_res[band]];
  }
}

int32_t gaud_celt_decode_boosts(OPUS_Range * range, const CELT_Mode * mode,
    uint32_t start, uint32_t end, uint32_t channels, const int32_t * cap,
    int32_t total_bits, int32_t * offsets) {
  int dynalloc_logp = 6;
  uint32_t tell = gaud_opus_tell_frac(range);
  memset(offsets, 0, (size_t)mode->bands * sizeof(*offsets));
  for (uint32_t band = start; band < end; ++band) {
    int32_t width = (int32_t)channels
        * (int32_t)(gaud_celt_band_start(mode, band + 1u)
            - gaud_celt_band_start(mode, band));
    /* Six bits per boost, but never more than one bit per sample and
     * never less than an eighth of one. */
    int32_t quanta = gaud_celt_min32(width << CELT_BITRES,
        gaud_celt_max32(6 << CELT_BITRES, width));
    int loop_logp = dynalloc_logp;
    int32_t boost = 0;
    while ((int32_t)tell + (loop_logp << CELT_BITRES) < total_bits
        && boost < cap[band]) {
      int flag = gaud_opus_dec_bit_logp(range, (unsigned)loop_logp);
      tell = gaud_opus_tell_frac(range);
      if (!flag) {
        break;
      }
      boost += quanta;
      total_bits -= quanta;
      /* After the first, further boosts of the same band are cheap. */
      loop_logp = 1;
    }
    offsets[band] = boost;
    /* And a band having been boosted makes the next one likelier. */
    if (boost > 0) {
      dynalloc_logp = (int)gaud_celt_max32(2, dynalloc_logp - 1);
    }
  }
  return total_bits;
}

/**
 * The second half of the allocation, once the two candidate rows are
 * known: interpolate, skip, and split into fine energy and shape.
 *
 * Kept as its own function because that is how the reference divides
 * it, and because the division is where the transmitted decisions
 * live - everything above this point is arithmetic, and everything
 * inside it can read a symbol.
 */
static uint32_t interp_bits2pulses(OPUS_Range * range,
    const CELT_Mode * mode, uint32_t start, uint32_t end,
    uint32_t skip_start, const int32_t * bits1, const int32_t * bits2,
    const int32_t * thresh, const int32_t * cap, int32_t total,
    int32_t * out_balance, int32_t skip_rsv, uint32_t * intensity,
    int32_t intensity_rsv, bool * dual_stereo, int32_t dual_stereo_rsv,
    int32_t * bits, int * ebits, int * fine_priority, uint32_t channels) {
  int32_t alloc_floor = (int32_t)channels << CELT_BITRES;
  int stereo = channels > 1 ? 1 : 0;
  int32_t log_m = (int32_t)mode->lm << CELT_BITRES;
  int32_t psum;
  int32_t left;
  int32_t percoeff;

  /* The fine search: 64 steps between the two rows the coarse search
   * bracketed, by bisection rather than by scanning. */
  int32_t lo = 0;
  int32_t hi = 1 << CELT_ALLOC_STEPS;
  for (int step = 0; step < CELT_ALLOC_STEPS; ++step) {
    int32_t mid = (lo + hi) >> 1;
    psum = 0;
    int done = 0;
    for (uint32_t j = end; j-- > start;) {
      int32_t tmp = bits1[j] + ((mid * bits2[j]) >> CELT_ALLOC_STEPS);
      if (tmp >= thresh[j] || done) {
        done = 1;
        psum += gaud_celt_min32(tmp, cap[j]);
      } else if (tmp >= alloc_floor) {
        psum += alloc_floor;
      }
    }
    if (psum > total) {
      hi = mid;
    } else {
      lo = mid;
    }
  }

  psum = 0;
  {
    int done = 0;
    for (uint32_t j = end; j-- > start;) {
      int32_t tmp = bits1[j] + ((lo * bits2[j]) >> CELT_ALLOC_STEPS);
      if (tmp < thresh[j] && !done) {
        tmp = tmp >= alloc_floor ? alloc_floor : 0;
      } else {
        done = 1;
      }
      tmp = gaud_celt_min32(tmp, cap[j]);
      bits[j] = tmp;
      psum += tmp;
    }
  }

  /*
   * Skipping, from the top band downwards. Each skipped band hands its
   * bits back to the bands below, which is why this cannot be done in
   * the other direction: how much a band would receive depends on what
   * the bands above it gave up.
   */
  uint32_t coded_bands = end;
  for (;; --coded_bands) {
    uint32_t j = coded_bands - 1u;
    /* The first band is never skipped - that would spend a bit to say
     * the rest of them are wasted - and neither is a boosted one,
     * which would spend a bit to undo a boost just transmitted. */
    if (j <= skip_start) {
      /* Handing the reserved skip bit back, which happens only when the
       * loop walks all the way to the bottom - no real stream does, so
       * this arm is unreached by the vectors too. Same measurement and
       * same reason as the fine-energy clamp below. */
      total += skip_rsv;
      break;
    }
    left = total - psum;
    int32_t span = short_edge(mode, coded_bands) - short_edge(mode, start);
    percoeff = left / span;
    left -= span * percoeff;
    int32_t rem = gaud_celt_max32(
        left - (short_edge(mode, j) - short_edge(mode, start)), 0);
    int32_t band_width
        = short_edge(mode, coded_bands) - short_edge(mode, j);
    int32_t band_bits = bits[j] + percoeff * band_width + rem;
    /* A skip is only *coded* when the band is above the threshold;
     * below it the band is skipped regardless, which is what
     * guarantees there is room for the flag. */
    if (band_bits
        >= gaud_celt_max32(thresh[j], alloc_floor + (1 << CELT_BITRES))) {
      if (gaud_opus_dec_bit_logp(range, 1)) {
        break;
      }
      psum += 1 << CELT_BITRES;
      band_bits -= 1 << CELT_BITRES;
    }
    psum -= bits[j] + intensity_rsv;
    if (intensity_rsv > 0) {
      intensity_rsv = gaud_opus_log2_frac_table[j - start];
    }
    psum += intensity_rsv;
    if (band_bits >= alloc_floor) {
      psum += alloc_floor;
      bits[j] = alloc_floor;
    } else {
      bits[j] = 0;
    }
  }

  /* The two stereo parameters, each read only if reserved for. */
  if (intensity_rsv > 0) {
    *intensity = start + gaud_opus_dec_uint(range, coded_bands + 1u - start);
  } else {
    *intensity = 0;
  }
  if (*intensity <= start) {
    total += dual_stereo_rsv;
    dual_stereo_rsv = 0;
  }
  *dual_stereo = dual_stereo_rsv > 0
      ? gaud_opus_dec_bit_logp(range, 1) != 0
      : false;

  /* Whatever is left is spread over the coded bands, by width. */
  left = total - psum;
  int32_t span = short_edge(mode, coded_bands) - short_edge(mode, start);
  percoeff = left / span;
  left -= span * percoeff;
  for (uint32_t j = start; j < coded_bands; ++j) {
    bits[j] += percoeff * short_width(mode, j);
  }
  for (uint32_t j = start; j < coded_bands; ++j) {
    int32_t take = gaud_celt_min32(left, short_width(mode, j));
    bits[j] += take;
    left -= take;
  }

  /*
   * And the split: how much of each band's allowance goes to refining
   * its energy and how much to its shape. The balance carries forward,
   * so a band that could not use everything it was given passes the
   * excess to the next one.
   */
  int32_t balance = 0;
  uint32_t j = start;
  for (; j < coded_bands; ++j) {
    int32_t n0 = short_width(mode, j);
    int32_t n = n0 << mode->lm;
    int32_t excess;
    bits[j] += balance;

    if (n > 1) {
      excess = gaud_celt_max32(bits[j] - cap[j], 0);
      bits[j] -= excess;
      /* A coupled stereo pair has one more degree of freedom than two
       * independent channels, and the split has to know. */
      int32_t den = (int32_t)channels * n
          + ((channels == 2 && n > 2 && !*dual_stereo && j < *intensity)
                  ? 1
                  : 0);
      int32_t nclogn = den * (gaud_opus_logN400[j] + log_m);
      int32_t offset = (nclogn >> 1) - den * CELT_FINE_OFFSET;
      /* Two bins is the one width the curve above does not describe. */
      if (n == 2) {
        offset += den << CELT_BITRES >> 2;
      }
      if (bits[j] + offset < den * 2 << CELT_BITRES) {
        offset += nclogn >> 2;
      } else if (bits[j] + offset < den * 3 << CELT_BITRES) {
        offset += nclogn >> 3;
      }
      ebits[j] = (int)gaud_celt_max32(0,
          (bits[j] + offset + (den << (CELT_BITRES - 1)))
              / (den << CELT_BITRES));
      /* Never spend more on fine energy than the band actually has. */
      if ((int32_t)channels * ebits[j] > (bits[j] >> CELT_BITRES)) {
        ebits[j] = (int)(bits[j] >> stereo >> CELT_BITRES);
      }
      /*
       * **Measured unreachable, and kept anyway.** The bound above -
       * never more fine energy than the band's own bits can pay for -
       * already holds this at eight, and the rebalancing below adds
       * `MAX_FINE_BITS - ebits[j]`, which cannot push it past eight
       * either. Deleting this line changes nothing across 1,272
       * synthetic configurations spanning every frame size, both
       * channel counts and the whole byte range, with and without band
       * boosts, and nothing across 11,211 CELT frames of the
       * conformance vectors compared against the reference.
       *
       * It stays because this file is a transcription of the normative
       * reference and "produces identical results" is the whole
       * specification: a line that is unreachable in every case anyone
       * here can construct is not a line anyone here can prove is
       * unreachable in all of them.
       */
      ebits[j] = (int)gaud_celt_min32(ebits[j], CELT_MAX_FINE_BITS);
      /* A band that was rounded down or capped is a candidate for the
       * leftover bits at the very end of the frame. */
      fine_priority[j]
          = (int32_t)ebits[j] * (den << CELT_BITRES) >= bits[j] + offset;
      bits[j] -= (int32_t)channels * ebits[j] << CELT_BITRES;
    } else {
      /* One bin: everything goes to fine energy but the sign bit. */
      excess = gaud_celt_max32(0, bits[j] - ((int32_t)channels
                                                << CELT_BITRES));
      bits[j] -= excess;
      ebits[j] = 0;
      fine_priority[j] = 1;
    }

    /* Fine energy cannot take part in the rebalancing the shape coder
     * does, so anything over the cap is rebalanced here instead. */
    if (excess > 0) {
      int32_t extra_fine = gaud_celt_min32(excess >> (stereo + CELT_BITRES),
          CELT_MAX_FINE_BITS - ebits[j]);
      ebits[j] += (int)extra_fine;
      int32_t extra_bits = extra_fine * (int32_t)channels << CELT_BITRES;
      fine_priority[j] = extra_bits >= excess - balance;
      excess -= extra_bits;
    }
    balance = excess;
  }
  *out_balance = balance;

  /* A skipped band spends everything it has on fine energy. */
  for (; j < end; ++j) {
    ebits[j] = (int)(bits[j] >> stereo >> CELT_BITRES);
    bits[j] = 0;
    fine_priority[j] = ebits[j] < 1;
  }
  return coded_bands;
}

uint32_t gaud_celt_compute_allocation(OPUS_Range * range,
    const CELT_Mode * mode, uint32_t start, uint32_t end,
    const int32_t * offsets, const int32_t * cap, int alloc_trim,
    uint32_t * intensity, bool * dual_stereo, int32_t total,
    int32_t * out_balance, int32_t * pulses, int * ebits,
    int * fine_priority, uint32_t channels) {
  int32_t bits1[CELT_BANDS];
  int32_t bits2[CELT_BANDS];
  int32_t thresh[CELT_BANDS];
  int32_t trim_offset[CELT_BANDS];

  total = gaud_celt_max32(total, 0);
  uint32_t skip_start = start;

  /*
   * Reserve, in this order, before anything is spent: one bit to end
   * the run of skipped bands, then the intensity parameter's cost,
   * then one bit for dual stereo. Each is handed back later if its
   * decision turns out not to be coded. The order is the format's and
   * a different one is a different budget.
   */
  int32_t skip_rsv = total >= 1 << CELT_BITRES ? 1 << CELT_BITRES : 0;
  total -= skip_rsv;
  int32_t intensity_rsv = 0;
  int32_t dual_stereo_rsv = 0;
  if (channels == 2) {
    intensity_rsv = gaud_opus_log2_frac_table[end - start];
    if (intensity_rsv > total) {
      intensity_rsv = 0;
    } else {
      total -= intensity_rsv;
      dual_stereo_rsv = total >= 1 << CELT_BITRES ? 1 << CELT_BITRES : 0;
      total -= dual_stereo_rsv;
    }
  }

  for (uint32_t j = start; j < end; ++j) {
    int32_t width = short_width(mode, j);
    /* Below this a band certainly gets no shape bits at all. */
    thresh[j] = gaud_celt_max32((int32_t)channels << CELT_BITRES,
        (3 * width << mode->lm << CELT_BITRES) >> 4);
    /* The trim tilts the curve: positive favours high bands. */
    trim_offset[j] = (int32_t)channels * width * (alloc_trim - 5
                         - (int)mode->lm)
        * (int32_t)(end - j - 1u) * (1 << (mode->lm + CELT_BITRES)) >> 6;
    /* A band of a single bin benefits more from one coarse value per
     * coefficient than from any shape at all. */
    if ((width << mode->lm) == 1) {
      trim_offset[j] -= (int32_t)channels << CELT_BITRES;
    }
  }

  /* The coarse search: the richest of the eleven rows that still fits. */
  int32_t lo = 1;
  int32_t hi = CELT_ALLOC_VECTORS - 1;
  do {
    int done = 0;
    int32_t psum = 0;
    int32_t mid = (lo + hi) >> 1;
    for (uint32_t j = end; j-- > start;) {
      int32_t width = short_width(mode, j);
      int32_t bitsj = (int32_t)channels * width
              * gaud_opus_band_allocation[(size_t)mid * mode->bands + j]
          << mode->lm >> 2;
      if (bitsj > 0) {
        bitsj = gaud_celt_max32(0, bitsj + trim_offset[j]);
      }
      bitsj += offsets[j];
      if (bitsj >= thresh[j] || done) {
        done = 1;
        psum += gaud_celt_min32(bitsj, cap[j]);
      } else if (bitsj >= (int32_t)channels << CELT_BITRES) {
        psum += (int32_t)channels << CELT_BITRES;
      }
    }
    if (psum > total) {
      hi = mid - 1;
    } else {
      lo = mid + 1;
    }
  } while (lo <= hi);
  hi = lo--;

  for (uint32_t j = start; j < end; ++j) {
    int32_t width = short_width(mode, j);
    int32_t bits1j = (int32_t)channels * width
            * gaud_opus_band_allocation[(size_t)lo * mode->bands + j]
        << mode->lm >> 2;
    int32_t bits2j = hi >= CELT_ALLOC_VECTORS
        ? cap[j]
        : (int32_t)channels * width
                * gaud_opus_band_allocation[(size_t)hi * mode->bands + j]
            << mode->lm >> 2;
    if (bits1j > 0) {
      bits1j = gaud_celt_max32(0, bits1j + trim_offset[j]);
    }
    if (bits2j > 0) {
      bits2j = gaud_celt_max32(0, bits2j + trim_offset[j]);
    }
    /* Row zero is the all-zero row, and a boost applied to it would
     * make a band that the table says gets nothing get something. */
    if (lo > 0) {
      bits1j += offsets[j];
    }
    bits2j += offsets[j];
    if (offsets[j] > 0) {
      skip_start = j;
    }
    bits2[j] = gaud_celt_max32(0, bits2j - bits1j);
    bits1[j] = bits1j;
  }

  return interp_bits2pulses(range, mode, start, end, skip_start, bits1,
      bits2, thresh, cap, total, out_balance, skip_rsv, intensity,
      intensity_rsv, dual_stereo, dual_stereo_rsv, pulses, ebits,
      fine_priority, channels);
}

const unsigned char * gaud_celt_pulse_cache(int lm, uint32_t band) {
  // The index is by frame size and then by band, with one extra row at
  // the front for the -1 that a split hands down.
  int16_t index = gaud_opus_cache_index50[(uint32_t)(lm + 1) * CELT_BANDS
      + band];
  return index < 0 ? NULL : gaud_opus_cache_bits50 + index;
}

int gaud_celt_bits_to_pulses(int lm, uint32_t band, int32_t bits) {
  const unsigned char * cache = gaud_celt_pulse_cache(lm, band);
  int low = 0;
  int high = cache[0];
  --bits;
  for (int i = 0; i < CELT_LOG_MAX_PSEUDO; ++i) {
    int mid = (low + high + 1) >> 1;
    if ((int32_t)cache[mid] >= bits) {
      high = mid;
    } else {
      low = mid;
    }
  }
  // Whichever of the two the search closed on is nearer. The low end
  // reads as -1 rather than 0 because coding nothing costs nothing.
  return bits - (low == 0 ? -1 : (int32_t)cache[low])
          <= (int32_t)cache[high] - bits
      ? low
      : high;
}

int32_t gaud_celt_pulses_to_bits(int lm, uint32_t band, int pulses) {
  const unsigned char * cache = gaud_celt_pulse_cache(lm, band);
  return pulses == 0 ? 0 : (int32_t)cache[pulses] + 1;
}
