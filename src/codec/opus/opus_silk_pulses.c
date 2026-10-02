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
 * SILK's excitation, off the bitstream. RFC 6716 4.2.7.8. Never installed.
 *
 * The excitation is coded in blocks of sixteen samples, and a block is
 * coded as **a total and then a shape**, which is the same idea CELT
 * uses for a band and is the reason the two coders can share a range
 * decoder and nothing else. The total is the sum of the pulse
 * magnitudes; the shape is recovered by repeatedly splitting that
 * total between two halves, four times, until each half is one sample.
 *
 * Three things here are worth stating before reading the code.
 *
 * **The split is free of the position.** Each split codes how many of
 * the parent's pulses went to the first child; the second child gets
 * the rest, with no symbol of its own. So a block of p pulses costs
 * fifteen symbols whatever p is, and the distributions differ only by
 * how deep the split is - which is why there are four tables and not
 * sixteen.
 *
 * **A block can carry more than sixteen pulses, and says so by
 * escaping.** A pulse count of seventeen is not a count; it means
 * every sample in the block has one more low-order bit to come, and
 * another count follows under a different distribution. The escape can
 * repeat, and at ten repetitions the distribution shifts by one so the
 * escape has probability zero. That shift is the only thing bounding
 * the loop, and it is the reason the eleventh distribution in Table 46
 * needs no storage of its own.
 *
 * **Signs are coded last and only for the samples that have one.** The
 * sign distribution is chosen by the signal type, the quantization
 * offset, and how many pulses the *block* has - capped at six, and
 * read from the low five bits of a total that has had the shift count
 * packed into it. That packing is the reference's, and reproducing it
 * is not optional: it is what makes a block with extra LSBs use the
 * six-pulse distribution rather than its own count's.
 */

#include "opus_silk.h"
#include "opus_silk_tables.h"
#include <string.h>

/** Rate levels in Table 46; the last is the one the escape uses. */
#define SILK_RATE_LEVELS 10

/** Symbols in one pulse-count distribution: 0 to 16, plus the escape. */
#define SILK_PULSE_SYMBOLS 18

/**
 * @brief Split @p pulses between two halves, or give both zero.
 *
 * @param children Receives the two counts.
 * @param range The range decoder.
 * @param pulses The parent's count.
 * @param table The distribution for this depth.
 */
static void decode_split(int * children, OPUS_Range * range, int pulses,
    const unsigned char * table) {
  if (pulses <= 0) {
    children[0] = 0;
    children[1] = 0;
    return;
  }
  children[0] = gaud_opus_dec_icdf(
      range, table + gaud_opus_silk_shell_code_table_offsets[pulses], 8);
  children[1] = pulses - children[0];
}

/**
 * @brief One sixteen-sample block's pulse magnitudes.
 *
 * The tree is walked breadth-first by level but depth-first within it,
 * and the order is part of the format rather than an implementation
 * choice: it is the order the encoder wrote the symbols in.
 *
 * @param out Receives sixteen non-negative magnitudes.
 * @param range The range decoder.
 * @param total The block's pulse count, which is at least one.
 */
static void decode_shell(int * out, OPUS_Range * range, int total) {
  int half[2];
  int quarter[4];
  int eighth[8];
  decode_split(half, range, total, gaud_opus_silk_shell_code_table3);
  decode_split(quarter, range, half[0], gaud_opus_silk_shell_code_table2);
  decode_split(eighth, range, quarter[0], gaud_opus_silk_shell_code_table1);
  decode_split(out, range, eighth[0], gaud_opus_silk_shell_code_table0);
  decode_split(out + 2, range, eighth[1], gaud_opus_silk_shell_code_table0);
  decode_split(
      eighth + 2, range, quarter[1], gaud_opus_silk_shell_code_table1);
  decode_split(out + 4, range, eighth[2], gaud_opus_silk_shell_code_table0);
  decode_split(out + 6, range, eighth[3], gaud_opus_silk_shell_code_table0);
  decode_split(
      quarter + 2, range, half[1], gaud_opus_silk_shell_code_table2);
  decode_split(
      eighth + 4, range, quarter[2], gaud_opus_silk_shell_code_table1);
  decode_split(out + 8, range, eighth[4], gaud_opus_silk_shell_code_table0);
  decode_split(out + 10, range, eighth[5], gaud_opus_silk_shell_code_table0);
  decode_split(
      eighth + 6, range, quarter[3], gaud_opus_silk_shell_code_table1);
  decode_split(out + 12, range, eighth[6], gaud_opus_silk_shell_code_table0);
  decode_split(out + 14, range, eighth[7], gaud_opus_silk_shell_code_table0);
}

void gaud_silk_decode_pulses(SILK_Channel * channel, OPUS_Range * range) {
  const SILK_Indices * indices = &channel->indices;
  int totals[SILK_MAX_SHELL_BLOCKS];
  int shifts[SILK_MAX_SHELL_BLOCKS];
  int blocks = channel->shell_blocks;
  int * pulses = channel->pulses;

  // Section 4.2.7.8.1. The rate level only chooses which distribution
  // the pulse counts are read under; it is not itself a count.
  int level = gaud_opus_dec_icdf(range,
      gaud_opus_silk_rate_levels_icdf + 9 * (indices->signal_type >> 1), 8);

  // Section 4.2.7.8.2.
  const unsigned char * counts =
      gaud_opus_silk_pulses_per_block_icdf + SILK_PULSE_SYMBOLS * level;
  const unsigned char * escape = gaud_opus_silk_pulses_per_block_icdf
      + SILK_PULSE_SYMBOLS * (SILK_RATE_LEVELS - 1);
  for (int block = 0; block < blocks; ++block) {
    shifts[block] = 0;
    totals[block] = gaud_opus_dec_icdf(range, counts, 8);
    while (totals[block] == SILK_MAX_PULSES + 1) {
      ++shifts[block];
      // At ten shifts the distribution moves on by one entry, which
      // takes the escape's probability to zero. Nothing else stops
      // this loop, so the shift is load-bearing rather than an
      // optimisation.
      totals[block] =
          gaud_opus_dec_icdf(range, escape + (shifts[block] == 10 ? 1 : 0), 8);
    }
  }

  // Section 4.2.7.8.3.
  for (int block = 0; block < blocks; ++block) {
    int * out = pulses + block * SILK_SHELL_BLOCK;
    if (totals[block] > 0) {
      decode_shell(out, range, totals[block]);
    } else {
      memset(out, 0, SILK_SHELL_BLOCK * sizeof(*out));
    }
  }

  // Section 4.2.7.8.4. Every sample of a block that escaped gets the
  // same number of extra bits, zero ones included - the escape is a
  // property of the block, not of the sample.
  for (int block = 0; block < blocks; ++block) {
    if (shifts[block] == 0) {
      continue;
    }
    int * out = pulses + block * SILK_SHELL_BLOCK;
    for (int k = 0; k < SILK_SHELL_BLOCK; ++k) {
      int magnitude = out[k];
      for (int bit = 0; bit < shifts[block]; ++bit) {
        magnitude = (magnitude << 1)
            + gaud_opus_dec_icdf(range, gaud_opus_silk_lsb_icdf, 8);
      }
      out[k] = magnitude;
    }
    // The sign step reads the low five bits of this as a pulse count
    // and anything above as "there were extra bits", so a block that
    // escaped is marked here even when its own count came back zero.
    totals[block] |= shifts[block] << 5;
  }

  // Section 4.2.7.8.5. Six contexts - three signal types by two
  // quantization offsets - each with seven entries indexed by the
  // block's pulse count capped at six.
  const unsigned char * signs = gaud_opus_silk_sign_icdf
      + 7 * (indices->quant_offset_type + 2 * indices->signal_type);
  for (int block = 0; block < blocks; ++block) {
    if (totals[block] <= 0) {
      continue;
    }
    int capped = totals[block] & 0x1F;
    unsigned char pair[2];
    pair[0] = signs[capped < 6 ? capped : 6];
    pair[1] = 0;
    int * out = pulses + block * SILK_SHELL_BLOCK;
    for (int k = 0; k < SILK_SHELL_BLOCK; ++k) {
      if (out[k] > 0) {
        // The symbol is 0 for negative and 1 for positive, so the
        // multiplier is twice it less one.
        out[k] *= 2 * gaud_opus_dec_icdf(range, pair, 8) - 1;
      }
    }
  }
}
