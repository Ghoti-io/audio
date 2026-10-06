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
 * This file is generated and must not be edited. See `mp3enc_psy_tables.c`.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3ENC_PSY_TABLES_H
#define GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3ENC_PSY_TABLES_H

#include <ghoti.io/audio/macros.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Most partitions of the long (1024-point) spectrum. */
#define MP3E_MAX_LONG_PARTS 72
/** Most partitions of the short (256-point) spectrum. */
#define MP3E_MAX_SHORT_PARTS 48
/** The spreading table's entries per Bark. */
#define MP3E_SPREAD_STEP 8
/** How many Bark below the masker the spreading table reaches; index 0. */
#define MP3E_SPREAD_LOW 8
/** And its length. */
#define MP3E_SPREAD_COUNT 193

/** cos of 2 pi k / 1024, Q30. */
extern const int32_t gaud_mp3enc_fft_cos[512];
/** sin of 2 pi k / 1024, Q30. */
extern const int32_t gaud_mp3enc_fft_sin[512];
/** The 1024-point Hann window, Q15. */
extern const int16_t gaud_mp3enc_hann_long[1024];
/** The 256-point Hann window, Q15. */
extern const int16_t gaud_mp3enc_hann_short[256];
/** Schroeder's spreading function as linear power, times 65536. */
extern const uint32_t gaud_mp3enc_spread[193];

/** Long partitions per row (version * 3 + rate index). */
extern const uint8_t gaud_mp3enc_long_parts[9];
/** Short partitions per row. */
extern const uint8_t gaud_mp3enc_short_parts[9];

/** The first bin of each partition. (long) */
extern const uint16_t gaud_mp3enc_long_lo[9][72];
/** One past the last bin of each partition. (long) */
extern const uint16_t gaud_mp3enc_long_hi[9][72];
/** The Bark scale at each partition's centre, Q8. (long) */
extern const uint16_t gaud_mp3enc_long_bark[9][72];
/** The threshold of hearing in each partition, per bin, in power units. (long) */
extern const uint64_t gaud_mp3enc_long_ath[9][72];
/** The least signal-to-noise ratio each partition is held to, dB Q8. (long) */
extern const uint16_t gaud_mp3enc_long_minsnr[9][72];

/** The first bin of each partition. (short) */
extern const uint16_t gaud_mp3enc_short_lo[9][48];
/** One past the last bin of each partition. (short) */
extern const uint16_t gaud_mp3enc_short_hi[9][48];
/** The Bark scale at each partition's centre, Q8. (short) */
extern const uint16_t gaud_mp3enc_short_bark[9][48];
/** The threshold of hearing in each partition, per bin, in power units. (short) */
extern const uint64_t gaud_mp3enc_short_ath[9][48];
/** The least signal-to-noise ratio each partition is held to, dB Q8. (short) */
extern const uint16_t gaud_mp3enc_short_minsnr[9][48];

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3ENC_PSY_TABLES_H
