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
 * AIFF: private declarations, and the 80-bit float its sample rate is
 * stored as. Never installed.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_AIFF_AIFF_INTERNAL_H
#define GHOTI_IO_GAUD_SRC_CODEC_AIFF_AIFF_INTERNAL_H

#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/audio/macros.h>
#include <stdint.h>

/** What one track's decoder needs to find its samples again. */
typedef struct {
  uint64_t data_offset; ///< Where the sample bytes start.
  uint64_t data_length; ///< How many of them.
  size_t frame_size;    ///< Bytes per frame.
  bool needs_swap;      ///< Whether the file's order is not the host's.
} AIFF_Track_State;

/**
 * @brief Read an IEEE-754 80-bit extended float, big-endian.
 *
 * AIFF stores its sample rate this way and nothing else in either format
 * does. There is no C type for it on any platform this builds for - `long
 * double` is 80-bit on x86 but 64-bit on aarch64 and 128-bit on some others
 * - so it is decoded by hand into a double, which holds every rate anyone
 * will ever use exactly.
 *
 * @return The value, or 0.0 for a NaN or an infinity, which a sample rate
 *   cannot be and which the caller refuses.
 */
double gaud_aiff_read_extended(const unsigned char * p);

/** @brief Write one, from a non-negative finite double. */
void gaud_aiff_write_extended(unsigned char * p, double value);

GAUD_Result gaud_aiff_open(const GAUD_Codec * codec, GAUD_Stream * stream,
    const GAUD_Limits * limits, GAUD_Diagnostics * diagnostics,
    GAUD_Doc ** out_doc);
void gaud_aiff_close(const GAUD_Codec * codec, GAUD_Doc * doc);
GAUD_Result gaud_aiff_decoder_open(
    const GAUD_Codec * codec, GAUD_Track * track, GAUD_Decoder ** out_decoder);
GAUD_Result gaud_aiff_encoder_open(const GAUD_Codec * codec,
    GAUD_Stream * stream, const GAUD_Encode_Params * params,
    GAUD_Encoder ** out_encoder);

#endif // GHOTI_IO_GAUD_SRC_CODEC_AIFF_AIFF_INTERNAL_H
