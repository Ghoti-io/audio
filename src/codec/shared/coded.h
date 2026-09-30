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
 * The pull decoder and the block writer for coded tracks, shared by every
 * container that carries one.
 *
 * `adpcm.h` turns bytes into samples for one block. This turns a block
 * decoder into a ::GAUD_Decoder: it owns the block cache, works out which
 * block a frame is in, and copies out of the cache into whatever buffer
 * the caller brought. WAV and AIFF-C differ only in how they found the
 * geometry, so neither of them contains any of this.
 *
 * ## Why the state is per decoder and not per track
 *
 * Phase 1's PCM decoders hang their state off the *track*, which was safe
 * because reading PCM changes nothing: two decoders on one track could
 * share it. A coded decoder holds a decoded block, and two decoders
 * reading different parts of one track would evict each other's cache and
 * return each other's samples. So this state is allocated in
 * `decoder_open` and freed by the vtable's `close`.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_SHARED_CODED_H
#define GHOTI_IO_GAUD_SRC_CODEC_SHARED_CODED_H

#include "adpcm.h"
#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/audio/macros.h>

/* The fuzz harnesses are C++ and link these objects directly, so the
 * declarations need C linkage or the harness asks the linker for a
 * mangled name nothing defines. */
#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Open a pull decoder over a coded track.
 *
 * @param track The track. Its frame count is the authority on where the
 *   data ends; see `wav_load.c` on the `fact` chunk for why that is not
 *   the same as the block arithmetic.
 * @param geometry Copied.
 * @param data_offset Where the first block starts in the stream.
 * @param data_length How many bytes of blocks there are.
 * @param out_decoder Receives the decoder. Written only on success.
 */
GAUD_Result gaud_coded_decoder_open(GAUD_Track * track,
    const GAUD_Coded_Geometry * geometry, uint64_t data_offset,
    uint64_t data_length, GAUD_Decoder ** out_decoder);

/**
 * @brief The encoder half: buffers frames and emits whole blocks.
 *
 * A container embeds one of these and calls push() from its `write` and
 * flush() from its `finish`, then patches its own header. Everything about
 * blocks is here; everything about chunk lengths is the container's.
 */
typedef struct {
  GAUD_Coded_Geometry geometry; ///< What to emit.
  int16_t * pending;            ///< Frames not yet in a whole block.
  size_t pending_frames;        ///< How many.
  unsigned char * raw;          ///< One block's bytes.
  uint64_t frames;              ///< True frames accepted, for `fact`.
  uint64_t bytes;               ///< Bytes emitted.
} GAUD_Coded_Writer;

/** @brief Allocate a writer's two buffers. */
GAUD_Result gaud_coded_writer_init(GAUD_Coded_Writer * writer,
    const GAUD_Coded_Geometry * geometry, const GAUD_Allocator * allocator);

/** @brief Release them. Safe on a zeroed writer. */
void gaud_coded_writer_free(
    GAUD_Coded_Writer * writer, const GAUD_Allocator * allocator);

/** @brief Accept @p frames of interleaved 16-bit samples. */
GAUD_Result gaud_coded_writer_push(GAUD_Coded_Writer * writer,
    GAUD_Stream * stream, const int16_t * samples, size_t frames);

/**
 * @brief Emit whatever is left as one final, padded block.
 *
 * The block goes out at its full length because the container's header
 * promised that length. The frames beyond the caller's data are padding,
 * and `writer->frames` is what the container writes as the true count.
 */
GAUD_Result gaud_coded_writer_flush(
    GAUD_Coded_Writer * writer, GAUD_Stream * stream);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_SHARED_CODED_H
