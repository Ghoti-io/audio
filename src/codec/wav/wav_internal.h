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
 * WAV: private declarations. Never installed.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_WAV_WAV_INTERNAL_H
#define GHOTI_IO_GAUD_SRC_CODEC_WAV_WAV_INTERNAL_H

#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/audio/macros.h>
#include <stdint.h>

/** `WAVE_FORMAT_PCM`: integer samples. */
#define WAV_FORMAT_PCM 0x0001u
/** `WAVE_FORMAT_IEEE_FLOAT`: 32- or 64-bit float samples. */
#define WAV_FORMAT_IEEE_FLOAT 0x0003u
/** `WAVE_FORMAT_EXTENSIBLE`: the real tag is a GUID in the extension. */
#define WAV_FORMAT_EXTENSIBLE 0xFFFEu

/** What one track's decoder needs to find its samples again. */
typedef struct {
  uint64_t data_offset; ///< Where the sample bytes start.
  uint64_t data_length; ///< How many of them.
  size_t frame_size;    ///< Bytes per frame.
  bool needs_swap;      ///< Whether the file's order is not the host's.
} WAV_Track_State;

GAUD_Result gaud_wav_open(const GAUD_Codec * codec, GAUD_Stream * stream,
    const GAUD_Limits * limits, GAUD_Diagnostics * diagnostics,
    GAUD_Doc ** out_doc);
void gaud_wav_close(const GAUD_Codec * codec, GAUD_Doc * doc);
GAUD_Result gaud_wav_decoder_open(
    const GAUD_Codec * codec, GAUD_Track * track, GAUD_Decoder ** out_decoder);
GAUD_Result gaud_wav_encoder_open(const GAUD_Codec * codec,
    GAUD_Stream * stream, const GAUD_Encode_Params * params,
    GAUD_Encoder ** out_encoder);

#endif // GHOTI_IO_GAUD_SRC_CODEC_WAV_WAV_INTERNAL_H
