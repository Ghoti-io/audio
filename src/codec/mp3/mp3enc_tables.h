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
 * This file is generated and must not be edited. See `mp3enc_tables.c`.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3ENC_TABLES_H
#define GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3ENC_TABLES_H

#include <ghoti.io/audio/macros.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Where each Huffman table's code words start in ::gaud_mp3enc_huff_code;
 *  the table is `width * width` entries, row x, column y. */
extern const uint16_t gaud_mp3enc_huff_start[32];
/** Code words of every pair table, one after another. */
extern const uint32_t gaud_mp3enc_huff_code[4962];
/** Their lengths in bits. */
extern const uint8_t gaud_mp3enc_huff_len[4962];
/** The two quadruple tables' code words, 16 each, indexed by the four
 *  bits (v, w, x, y) with v the most significant. */
extern const uint8_t gaud_mp3enc_quad_code[32];
/** Their lengths. */
extern const uint8_t gaud_mp3enc_quad_len[32];
/** The analysis matrixing, cos((2k+1)(i-16)pi/64). Q28. */
extern const int32_t gaud_mp3enc_ana_cos[32][64];
/** The smallest value, Q28, that quantises to index + 1. */
extern const uint64_t gaud_mp3enc_quant_threshold[8207];
/** 2^(-k/4), the reciprocal of ::gaud_mp3_gain_frac. Q28. */
extern const int32_t gaud_mp3enc_gain_inverse[4];
/** 256 * log2(1 + i/256), rounded: the fractional part of a logarithm. */
extern const uint16_t gaud_mp3enc_log2_frac[256];
/** 65536 * 2^(i/256), rounded. */
extern const uint32_t gaud_mp3enc_exp2_frac[256];

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3ENC_TABLES_H
