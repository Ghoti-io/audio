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
 * Huffman coding of one granule's quantised spectrum: where the regions
 * go, which table each uses, and the bits themselves.
 *
 * **The decoder's own region rules are used, not restated.** A granule's
 * spectrum is three regions of pairs, a run of quadruples and a tail of
 * zeros, and where the first two regions end is stated by two small
 * numbers whose meaning depends on the block type and the sampling
 * frequency. The encoder decides those two numbers and the decoder
 * interprets them; if the two disagreed about what they mean, every file
 * would decode into noise. So there is exactly one function that turns the
 * side information into region boundaries (`regions_of`), and the encoder
 * calls it both when it chooses a split and when it writes the data.
 *
 * **The search is exhaustive and cheap.** For every table and every band
 * the bits that band would cost are computed once; a region's cost with a
 * given table is then a difference of two prefix sums, and trying every
 * legal split (up to 16 x 8) is a few thousand lookups. LAME limits its own
 * search because its tables are not laid out for this; the result here is
 * the minimum, not an approximation of it.
 */

#include "mp3enc_internal.h"

/** A cost larger than any real one, small enough that sums cannot wrap. */
#define COST_INF (1u << 24)

/** The largest value table @p t can carry, or -1 when the standard marks
 *  it unused. */
static int table_capacity(unsigned t) {
  const MP3_Huff * table = &gaud_mp3_huff[t];
  if (table->unused) {
    return -1;
  }
  if (table->width == 0u) {
    return 0;
  }
  if (table->linbits == 0u) {
    return (int)table->width - 1;
  }
  return 15 + (int)((1u << table->linbits) - 1u);
}

/**
 * Where the three pair regions end, as line positions, exactly as
 * `decode_spectrum` computes them. @p big is the pair region's end.
 */
static void regions_of(const MP3_Granule * side, unsigned row, unsigned big,
    unsigned region[3]) {
  const uint16_t * boundaries = gaud_mp3_sfb_long[row];
  unsigned bands = gaud_mp3_sfb_long_bands[row];
  if (side->window_switching) {
    region[0] = side->block_type == 2u && !side->mixed_block
        ? 3u * (unsigned)gaud_mp3_sfb_short[row][3]
        : boundaries[8];
    region[1] = 576u;
  }
  else {
    unsigned first = side->region0_count + 1u;
    unsigned second = first + side->region1_count + 1u;
    if (first > bands) {
      first = bands;
    }
    if (second > bands) {
      second = bands;
    }
    region[0] = boundaries[first];
    region[1] = boundaries[second];
  }
  region[2] = big;
  for (unsigned i = 0; i < 2u; ++i) {
    if (region[i] > big) {
      region[i] = big;
    }
  }
  if (region[1] < region[0]) {
    region[1] = region[0];
  }
}

/** Bits of pairs [lo, hi) coded with table @p t, or COST_INF. */
static uint32_t pairs_cost(
    const int32_t * is, unsigned lo, unsigned hi, unsigned t) {
  const MP3_Huff * table = &gaud_mp3_huff[t];
  int cap = table_capacity(t);
  if (cap < 0) {
    return COST_INF;
  }
  uint32_t bits = 0;
  if (table->width == 0u) {
    for (unsigned i = lo; i < hi; ++i) {
      if (is[i] != 0) {
        return COST_INF;
      }
    }
    return 0;
  }
  unsigned width = table->width;
  unsigned linbits = table->linbits;
  const uint8_t * len = gaud_mp3enc_huff_len + gaud_mp3enc_huff_start[t];
  for (unsigned i = lo; i < hi; i += 2u) {
    int32_t x = is[i] < 0 ? -is[i] : is[i];
    int32_t y = is[i + 1u] < 0 ? -is[i + 1u] : is[i + 1u];
    if (x > cap || y > cap) {
      return COST_INF;
    }
    unsigned cx = x >= 15 && width == 16u ? 15u : (unsigned)x;
    unsigned cy = y >= 15 && width == 16u ? 15u : (unsigned)y;
    bits += len[cx * width + cy];
    if (x >= 15) {
      bits += linbits;
    }
    if (y >= 15) {
      bits += linbits;
    }
    bits += (x != 0) + (y != 0);
  }
  return bits;
}

/** Bits of the quadruples [lo, hi) with count1 table A (0) or B (1). */
static uint32_t quads_cost(
    const int32_t * is, unsigned lo, unsigned hi, unsigned which) {
  uint32_t bits = 0;
  for (unsigned i = lo; i < hi; i += 4u) {
    unsigned index = 0;
    unsigned nonzero = 0;
    for (unsigned k = 0; k < 4u; ++k) {
      unsigned v = is[i + k] != 0;
      index = (index << 1) | v;
      nonzero += v;
    }
    bits += gaud_mp3enc_quad_len[which * 16u + index] + nonzero;
  }
  return bits;
}

/**
 * Find the cheapest regions and tables for a granule whose pair and
 * quadruple extents are already known.
 *
 * The layout's runs are the units a region boundary can fall on, in
 * bitstream order, which is what makes one routine serve the long block's
 * bands and the short block's band-by-window runs alike. @p r0_run and
 * @p r1_run are, for a granule with fixed regions, the run counts they
 * end at; for a free one the search tries every split.
 */
static void choose_tables(MP3E_Granule * granule, const MP3E_Layout * layout,
    unsigned row, unsigned big_end) {
  MP3_Granule * side = &granule->side;
  const int32_t * is = granule->is;
  unsigned runs = layout->count;

  /* cost[t][k]: bits of runs 0..k-1 under table t, counting only the runs
   * the table can carry; bad[t][k]: how many runs it cannot. A region is
   * usable with a table when no run in it is bad, and costs a difference
   * of two prefix sums. Runs are clipped to the pair region. */
  uint32_t cost[32][MP3E_MAX_BANDS + 1u];
  uint8_t bad[32][MP3E_MAX_BANDS + 1u];
  for (unsigned t = 0; t < 32u; ++t) {
    cost[t][0] = 0;
    bad[t][0] = 0;
  }
  for (unsigned k = 0; k < runs; ++k) {
    unsigned lo = layout->band[k].start;
    unsigned hi = lo + layout->band[k].width;
    if (hi > big_end) {
      hi = big_end;
    }
    for (unsigned t = 0; t < 32u; ++t) {
      uint32_t c = lo < hi ? pairs_cost(is, lo, hi, t) : 0u;
      bool usable = c < COST_INF;
      cost[t][k + 1u] = cost[t][k] + (usable ? c : 0u);
      bad[t][k + 1u] = (uint8_t)(bad[t][k] + (usable ? 0u : 1u));
    }
  }

  /* Cheapest table over runs [from, to). */
  #define REGION_BEST(from, to, out_table, out_cost) \
    do { \
      uint32_t best_ = COST_INF; \
      unsigned best_t_ = 0; \
      for (unsigned t_ = 0; t_ < 32u; ++t_) { \
        if (bad[t_][to] != bad[t_][from]) { \
          continue; \
        } \
        uint32_t c_ = cost[t_][to] - cost[t_][from]; \
        if (c_ < best_) { \
          best_ = c_; \
          best_t_ = t_; \
        } \
      } \
      out_table = best_t_; \
      out_cost = best_; \
    } while (0)

  if (side->window_switching) {
    /* The two regions' end is stated by the standard: after the first
     * three short bands in every window, or after the eighth long band. */
    unsigned region[3];
    regions_of(side, row, big_end, region);
    unsigned split = 0;
    while (split < runs && layout->band[split].start < region[0]) {
      ++split;
    }
    unsigned end = 0;
    while (end < runs && layout->band[end].start < big_end) {
      ++end;
    }
    if (split > end) {
      split = end;
    }
    unsigned t0 = 0;
    unsigned t1 = 0;
    uint32_t c0;
    uint32_t c1;
    REGION_BEST(0u, split, t0, c0);
    REGION_BEST(split, end, t1, c1);
    (void)c0;
    (void)c1;
    side->table_select[0] = (uint8_t)t0;
    side->table_select[1] = (uint8_t)t1;
    side->table_select[2] = 0;
    return;
  }

  /* A free split of the long bands. The first region is bands
   * 0..r0, the second r0+1..r0+r1+1 and the third the rest of the pair
   * region; the runs of a long layout are its bands one for one. */
  unsigned end = 0;
  while (end < runs && layout->band[end].start < big_end) {
    ++end;
  }
  uint32_t best_total = UINT32_MAX;
  unsigned best_r0 = 0;
  unsigned best_r1 = 0;
  unsigned best_t[3] = {0, 0, 0};
  unsigned bands = gaud_mp3_sfb_long_bands[row];
  for (unsigned r0 = 0; r0 < 16u; ++r0) {
    unsigned first = r0 + 1u;
    if (first > bands) {
      break;
    }
    for (unsigned r1 = 0; r1 < 8u; ++r1) {
      unsigned second = first + r1 + 1u;
      if (second > bands) {
        break;
      }
      unsigned f = first < end ? first : end;
      unsigned s = second < end ? second : end;
      unsigned t0;
      unsigned t1;
      unsigned t2;
      uint32_t c0;
      uint32_t c1;
      uint32_t c2;
      REGION_BEST(0u, f, t0, c0);
      REGION_BEST(f, s, t1, c1);
      REGION_BEST(s, end, t2, c2);
      uint32_t total = c0 + c1 + c2;
      if (total < best_total) {
        best_total = total;
        best_r0 = r0;
        best_r1 = r1;
        best_t[0] = t0;
        best_t[1] = t1;
        best_t[2] = t2;
      }
    }
  }
  side->region0_count = (uint8_t)best_r0;
  side->region1_count = (uint8_t)best_r1;
  for (unsigned i = 0; i < 3u; ++i) {
    side->table_select[i] = (uint8_t)best_t[i];
  }
  #undef REGION_BEST
}

void gaud_mp3e_huffman_plan(MP3E_Granule * granule,
    const MP3E_Layout * layout, unsigned row) {
  const int32_t * is = granule->is;
  MP3_Granule * side = &granule->side;

  /* The end of the non-zero part, and of the part that needs pairs. */
  unsigned nonzero_end = 0;
  unsigned big_end = 0;
  for (unsigned i = 0; i < MP3E_LINES; ++i) {
    int32_t magnitude = is[i] < 0 ? -is[i] : is[i];
    if (magnitude != 0) {
      nonzero_end = i + 1u;
    }
    if (magnitude > 1) {
      big_end = i + 1u;
    }
  }
  big_end = (big_end + 1u) & ~1u;
  /* Quadruples start where the pairs end and must be whole: if the last
   * non-zero lines cannot be reached by whole quadruples inside 576, the
   * pair region has to grow to cover them. */
  for (;;) {
    unsigned end = big_end;
    while (end < nonzero_end && end + 4u <= MP3E_LINES) {
      end += 4u;
    }
    if (end >= nonzero_end) {
      granule->count1_end = (uint16_t)end;
      break;
    }
    big_end += 2u;
  }
  granule->big_end = (uint16_t)big_end;
  side->big_values = big_end / 2u;

  unsigned a = (unsigned)quads_cost(is, big_end, granule->count1_end, 0);
  unsigned b = (unsigned)quads_cost(is, big_end, granule->count1_end, 1);
  side->count1table_select = b < a;

  choose_tables(granule, layout, row, big_end);
  granule->part3_bits = gaud_mp3e_huffman_bits(granule, row);
}

uint32_t gaud_mp3e_huffman_bits(const MP3E_Granule * granule, unsigned row) {
  const MP3_Granule * side = &granule->side;
  unsigned region[3];
  regions_of(side, row, granule->big_end, region);
  uint32_t bits = 0;
  unsigned at = 0;
  for (unsigned part = 0; part < 3u; ++part) {
    if (region[part] > at) {
      bits += pairs_cost(
          granule->is, at, region[part], side->table_select[part]);
      at = region[part];
    }
  }
  bits += quads_cost(granule->is, granule->big_end, granule->count1_end,
      side->count1table_select ? 1u : 0u);
  return bits;
}

void gaud_mp3e_huffman_write(
    MP3E_Bits * bits, const MP3E_Granule * granule, unsigned row) {
  const MP3_Granule * side = &granule->side;
  const int32_t * is = granule->is;
  unsigned region[3];
  regions_of(side, row, granule->big_end, region);
  unsigned at = 0;
  for (unsigned part = 0; part < 3u; ++part) {
    unsigned t = side->table_select[part];
    const MP3_Huff * table = &gaud_mp3_huff[t];
    for (; at < region[part]; at += 2u) {
      if (table->width == 0u) {
        continue;
      }
      int32_t x = is[at] < 0 ? -is[at] : is[at];
      int32_t y = is[at + 1u] < 0 ? -is[at + 1u] : is[at + 1u];
      unsigned width = table->width;
      unsigned cx = x >= 15 && width == 16u ? 15u : (unsigned)x;
      unsigned cy = y >= 15 && width == 16u ? 15u : (unsigned)y;
      unsigned index = gaud_mp3enc_huff_start[t] + cx * width + cy;
      gaud_mp3e_bits_put(
          bits, gaud_mp3enc_huff_code[index], gaud_mp3enc_huff_len[index]);
      if (x >= 15) {
        gaud_mp3e_bits_put(bits, (uint32_t)(x - 15), table->linbits);
      }
      if (x != 0) {
        gaud_mp3e_bits_put(bits, is[at] < 0 ? 1u : 0u, 1u);
      }
      if (y >= 15) {
        gaud_mp3e_bits_put(bits, (uint32_t)(y - 15), table->linbits);
      }
      if (y != 0) {
        gaud_mp3e_bits_put(bits, is[at + 1u] < 0 ? 1u : 0u, 1u);
      }
    }
  }
  unsigned which = side->count1table_select ? 1u : 0u;
  for (unsigned i = granule->big_end; i < granule->count1_end; i += 4u) {
    unsigned index = 0;
    for (unsigned k = 0; k < 4u; ++k) {
      index = (index << 1) | (is[i + k] != 0);
    }
    gaud_mp3e_bits_put(bits, gaud_mp3enc_quad_code[which * 16u + index],
        gaud_mp3enc_quad_len[which * 16u + index]);
    for (unsigned k = 0; k < 4u; ++k) {
      if (is[i + k] != 0) {
        gaud_mp3e_bits_put(bits, is[i + k] < 0 ? 1u : 0u, 1u);
      }
    }
  }
}
