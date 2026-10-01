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
 * This file is generated and must not be edited.
 *
 * The declarations for `mp3_tables.c`; see that file's comment for what
 * each table is and where it came from.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3_TABLES_H
#define GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3_TABLES_H

/* Before anything is declared, because every header in this library does:
 * the renames in namespace.h have to be in effect before a type is named,
 * or the same spelling can mean two different types. CONVENTIONS.md
 * section 4. */
#include <ghoti.io/audio/macros.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fractional bits in every coefficient here. 1.0 is 1 << MP3_Q. */
#define MP3_Q 28

/** One of the 32 Layer III Huffman tables. */
typedef struct {
  /** Where its tree starts in ::gaud_mp3_huff_nodes. */
  uint16_t offset;
  /** How many extra magnitude bits a value of 15 carries. */
  uint8_t linbits;
  /** The side of its square: values 0 to @p width - 1 in each of x and y. */
  uint8_t width;
  /** Set for the two tables the standard marks unused, 4 and 14. A frame
   *  that selects one is corrupt, and this is how the decoder says so
   *  rather than reading a tree that is not there. */
  uint8_t unused;
} MP3_Huff;

/** One Layer II quantisation class, from 11172-3 Table 3-B.4. */
typedef struct {
  uint16_t steps;  ///< How many levels.
  int32_t c;       ///< Requantisation multiplier, Q28.
  int32_t d;       ///< Requantisation offset, Q28.
  uint8_t grouping;///< Three samples share one codeword.
  uint8_t samples; ///< Samples per codeword: 3 when grouped, else 1.
  uint8_t bits;    ///< Bits per codeword: three samples' worth if grouped.
  /** Bits in one sample's own code. The same as @p bits for an ungrouped
   *  class, and smaller for a grouped one - 2, 3 or 4 where the codeword
   *  is 5, 7 or 10 - because three samples of 3, 5 or 9 levels pack into
   *  fewer bits together than apart, which is what grouping is for. */
  uint8_t sample_bits;
} MP3_Quant_Class;

/** Every pair table's tree, one after another; see ::MP3_Huff. */
extern const int16_t gaud_mp3_huff_nodes[2726];
/** The 32 Layer III pair tables of 11172-3 Table 3-B.7. */
extern const MP3_Huff gaud_mp3_huff[32];
/** The two quadruple tables' trees, A then B. */
extern const int16_t gaud_mp3_quad_nodes[60];
/** Where each quadruple tree starts in ::gaud_mp3_quad_nodes. */
extern const uint16_t gaud_mp3_quad_offset[2];

/** Long-block scalefactor band boundaries, by version and rate. */
extern const uint16_t gaud_mp3_sfb_long[6][24];
/** How many long bands each row of ::gaud_mp3_sfb_long has. */
extern const uint8_t gaud_mp3_sfb_long_bands[6];
/** Short-block band boundaries, within one of the three windows. */
extern const uint16_t gaud_mp3_sfb_short[6][15];
/** How many short bands each row of ::gaud_mp3_sfb_short has. */
extern const uint8_t gaud_mp3_sfb_short_bands[6];
/** Added to the scalefactors when preflag is set, Table 3-B.6. */
extern const uint8_t gaud_mp3_pretab[22];
/** MPEG-2's scalefactor partition sizes, by group and block shape. */
extern const uint8_t gaud_mp3_lsf_nsfb[6][3][4];

/** Alias reduction, the cosine half of each butterfly. Q28. */
extern const int32_t gaud_mp3_cs[8];
/** Alias reduction, the sine half of each butterfly. Q28. */
extern const int32_t gaud_mp3_ca[8];
/** The synthesis window of Table 3-B.3, exact in Q28. */
extern const int32_t gaud_mp3_window[512];
/** The polyphase filterbank's matrixing coefficients. Q28. */
extern const int32_t gaud_mp3_synth_cos[64][32];
/** The long-block inverse transform, as a matrix. Q28. */
extern const int32_t gaud_mp3_imdct36[36][18];
/** The short-block inverse transform, as a matrix. Q28. */
extern const int32_t gaud_mp3_imdct12[12][6];
/** The window for each block type; row 2 is zero and unused. Q28. */
extern const int32_t gaud_mp3_block_window[4][36];
/** The window each of the three short blocks gets. Q28. */
extern const int32_t gaud_mp3_short_window[12];

/** |is|^(4/3), as an exponent above a 24-bit mantissa. */
extern const uint32_t gaud_mp3_pow43[8207];
/** 2^(k/4) for the quarter the gain exponent leaves over. Q28. */
extern const int32_t gaud_mp3_gain_frac[4];

/** scalefac_compress to the two MPEG-1 field widths. */
extern const uint8_t gaud_mp3_slen[16][2];
/** MPEG-1 intensity stereo weights, left then right. Q28. */
extern const int32_t gaud_mp3_is_weight[7][2];
/** MPEG-2's, by intensity_scale and position. Q28. */
extern const int32_t gaud_mp3_is_weight_lsf[2][16][2];
/** 1/sqrt(2), the middle/side matrix's only constant. Q28. */
extern const int32_t gaud_mp3_inv_sqrt2;

/** Layer I and II scalefactors, Table 3-B.1. Q28. */
extern const int32_t gaud_mp3_scalefactor[64];
/** Layer II quantisation classes, Table 3-B.4. */
extern const MP3_Quant_Class gaud_mp3_classes[17];
/** Which class each Layer II allocation names; -1 for none. */
extern const int8_t gaud_mp3_alloc[5][32][16];
/** How many bits each subband's allocation field takes. */
extern const uint8_t gaud_mp3_alloc_nbal[5][32];
/** The first subband each allocation table never allocates. */
extern const uint8_t gaud_mp3_alloc_sblimit[5];
/** A Layer I sample width to its row of ::gaud_mp3_classes. */
extern const uint8_t gaud_mp3_class_for_bits[17];

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3_TABLES_H
