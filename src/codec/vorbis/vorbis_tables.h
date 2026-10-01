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
 * This file is generated and must not be edited. The generator is
 * `tools/tables/gen_vorbis_tables.py`.
 *
 * The declarations for `vorbis_tables.c`; see that file's comment for
 * what each table is and where it came from. Never installed.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_VORBIS_VORBIS_TABLES_H
#define GHOTI_IO_GAUD_SRC_CODEC_VORBIS_VORBIS_TABLES_H

/* Before anything is declared, because every header in this library does:
 * the renames in namespace.h have to be in effect before a type is named,
 * or the same spelling can mean two different types. CONVENTIONS.md
 * section 4, and `make check-symbols` enforces it. */
#include <ghoti.io/audio/macros.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fractional bits in the window and the rotation tables. */
#define VORBIS_TABLE_Q 30

/** Bits of mantissa in a floor entry. */
#define VORBIS_FLOOR_MANTISSA_BITS 31

/** How many block sizes the format permits. */
#define VORBIS_BLOCK_SIZE_COUNT 8

/** The smallest block size, and the base of the index into the arrays. */
#define VORBIS_LOG2_MIN_BLOCK 6

/** How many twiddles gaud_vorbis_fft_cos and _sin hold. */
#define VORBIS_FFT_TWIDDLES 1024

/** The transform size those twiddles are a full turn of. */
#define VORBIS_FFT_TURN 2048

/** The inverse decibel table: {mantissa, shift}, value = mantissa >> shift. */
extern const int32_t gaud_vorbis_floor_db[256][2];

extern const int32_t gaud_vorbis_window_64[32];
extern const int32_t gaud_vorbis_window_128[64];
extern const int32_t gaud_vorbis_window_256[128];
extern const int32_t gaud_vorbis_window_512[256];
extern const int32_t gaud_vorbis_window_1024[512];
extern const int32_t gaud_vorbis_window_2048[1024];
extern const int32_t gaud_vorbis_window_4096[2048];
extern const int32_t gaud_vorbis_window_8192[4096];
extern const int32_t gaud_vorbis_rotation_64[16][2];
extern const int32_t gaud_vorbis_rotation_128[32][2];
extern const int32_t gaud_vorbis_rotation_256[64][2];
extern const int32_t gaud_vorbis_rotation_512[128][2];
extern const int32_t gaud_vorbis_rotation_1024[256][2];
extern const int32_t gaud_vorbis_rotation_2048[512][2];
extern const int32_t gaud_vorbis_rotation_4096[1024][2];
extern const int32_t gaud_vorbis_rotation_8192[2048][2];

/** exp(-2*pi*i*k/::VORBIS_FFT_TURN), real part, in Q::VORBIS_TABLE_Q. */
extern const int32_t gaud_vorbis_fft_cos[1024];

/** The same, imaginary part. */
extern const int32_t gaud_vorbis_fft_sin[1024];

/** The window of each block size, indexed by log2 of it minus six. */
extern const int32_t * const
    gaud_vorbis_windows[VORBIS_BLOCK_SIZE_COUNT];

/** The rotation of each block size, the same way. */
extern const int32_t (* const
    gaud_vorbis_rotations[VORBIS_BLOCK_SIZE_COUNT])[2];

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_VORBIS_VORBIS_TABLES_H
