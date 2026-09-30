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
 * The coded-block layer: geometry, decode and encode for every coding that
 * is not plain PCM.
 *
 * WAV and AIFF-C disagree about byte order, chunk names and which codings
 * they can spell, and agree about everything below that line. This header
 * is that line. A container reads its own header, works out the geometry,
 * and then never touches a nibble.
 *
 * ## Why one geometry covers G.711 too
 *
 * G.711 has no blocks - every byte is independent - so treating it as one
 * is a fiction. It is a useful fiction: it lets the pull decoder in
 * `coded.c` have exactly one loop instead of two, and a stateless coding
 * simply takes whatever block size is convenient. ::GAUD_CODED_RUN_FRAMES
 * is that size. For the ADPCM codings the block is the container's, and
 * the size is not ours to choose.
 *
 * ## Blocks are self-contained, and that is what makes seeking exact
 *
 * Every block of every coding here carries its own predictor and step
 * state in its header. Nothing is carried across a block boundary. So
 * seeking to frame N means decoding the one block containing N and
 * discarding what precedes it inside that block - never decoding from the
 * start of the file - and gaud_decoder_seek() lands exactly on N rather
 * than on a block boundary. MP3's bit reservoir in phase 5 is where
 * `out_landed` stops being the frame that was asked for.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_SHARED_ADPCM_H
#define GHOTI_IO_GAUD_SRC_CODEC_SHARED_ADPCM_H

#include <ghoti.io/audio/coding.h>
#include <ghoti.io/audio/core.h>
#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The fuzz harnesses are C++ and link these objects directly, so the
 * declarations need C linkage or the harness asks the linker for a
 * mangled name nothing defines. */
#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief How many frames a stateless coding decodes per notional block.
 *
 * Only G.711 uses it. Any value is correct; this one is large enough that
 * the per-block overhead disappears and small enough that the scratch
 * buffer it implies stays off the interesting end of the allocator.
 */
#define GAUD_CODED_RUN_FRAMES 2048u

/** @brief The largest block any coding here allows, in bytes. */
#define GAUD_CODED_MAX_BLOCK_BYTES 65535u

/** @brief How many coefficient pairs an MS ADPCM header may carry. */
#define GAUD_MS_MAX_COEF 32u

/**
 * @brief The most channels a coded block may interleave.
 *
 * The block decoders keep one predictor, step index and delta per channel,
 * and keeping them on the stack is what makes a block decode allocation
 * free. That means a bound, and a bound means a refusal rather than a
 * silent truncation: gaud_coded_geometry() answers ::GAUD_ERR_LIMIT above
 * this, so no decoder is ever handed a geometry it cannot hold.
 *
 * `GAUD_Limits::max_channels` defaults to 256 and applies to PCM, where
 * there is no per-channel state to keep. 64 is well past anything either
 * of these containers is used for - WAVE_FORMAT_EXTENSIBLE's channel mask
 * has 18 defined positions - and is stated here rather than shared with
 * the PCM limit because the two are bounded for different reasons.
 */
#define GAUD_CODED_MAX_CHANNELS 64u

/**
 * @brief Everything the block layer needs, derived once when the container
 *   header is read.
 *
 * `block_bytes` and `block_frames` are both exact and both required: a
 * reader cannot compute one from the other without knowing the coding, and
 * a header that states them inconsistently is the thing
 * gaud_coded_geometry() refuses.
 */
typedef struct {
  GAUD_Sample_Coding coding; ///< Which coding.
  uint32_t channels;         ///< Interleaved channel count.
  uint32_t block_bytes;      ///< Bytes of one block in the file.
  uint32_t block_frames;     ///< Sample frames one whole block yields.
  /** MS ADPCM's predictor coefficients, as the header stated them. */
  int16_t coef[2u * GAUD_MS_MAX_COEF];
  /** How many pairs @p coef holds. Zero for every other coding. */
  uint16_t coef_count;
} GAUD_Coded_Geometry;

/**
 * @brief The seven coefficient pairs every MS ADPCM writer emits.
 *
 * A file may state its own and some do, which is why this is a default and
 * not a constant the decoder reaches for. Confirmed byte for byte against
 * the `fmt ` extension ffmpeg 7.1.5 and libsndfile 1.2.2 both write.
 */
extern const int16_t gaud_ms_adpcm_default_coef[14];

/**
 * @brief Work out and check a block geometry.
 *
 * @param coding Which coding.
 * @param channels Interleaved channels; refused if zero.
 * @param block_bytes The container's block size. Ignored, and computed,
 *   for the codings whose block size is fixed by the format
 *   (::GAUD_CODING_ADPCM_IMA_QT) or absent from it (G.711).
 * @param stated_frames What the container claimed one block yields, or 0
 *   when it did not say. When it did and it disagrees with the arithmetic,
 *   this is ::GAUD_ERR_CORRUPT rather than a silent preference for one of
 *   them - the two numbers describe the same bytes and a file where they
 *   differ is a file whose length cannot be computed.
 * @param out Filled in on success.
 */
GAUD_Result gaud_coded_geometry(GAUD_Sample_Coding coding, uint32_t channels,
    uint32_t block_bytes, uint32_t stated_frames, GAUD_Coded_Geometry * out);

/**
 * @brief Whether @p coding may be WRITTEN with @p channels channels.
 *
 * Read and write are deliberately asymmetric here. The three ADPCM
 * codings are mono and stereo formats in practice: ffmpeg 7.1.5 refuses
 * to *encode* any of them above two channels ("Specified channel layout
 * `5.1` is not supported", and the same for 2.1) and refuses to *decode*
 * above two ("Invalid number of channels"). libsndfile refuses the WAV
 * framings and will decode a six-channel `ima4`.
 *
 * So a six-channel ADPCM file is one this library could write and the
 * most widely deployed reader could not open. Refusing keeps the rule
 * that what this library writes, the references read - which is the
 * whole of planning/audio.md 11.3's first gate. Decoding stays liberal:
 * a file someone else wrote is still read, because refusing to read it
 * helps nobody.
 *
 * G.711 has no such bound. It is a per-sample mapping with no block and
 * no channel interleave, and six-channel mu-law round-trips through
 * ffmpeg, sox and libsndfile unchanged.
 */
bool gaud_coded_writable(GAUD_Sample_Coding coding, uint32_t channels);

/**
 * @brief How many frames a final block of only @p bytes bytes yields.
 *
 * A file whose data length is not a whole number of blocks ends in a
 * short one. Asking this rather than decoding it is what lets a loader
 * state the track's length before any samples are read, which is the
 * whole contract of ::GAUD_Codec_Open_Fn.
 *
 * @return 0 when @p bytes cannot hold even a block header.
 */
uint32_t gaud_coded_tail_frames(
    const GAUD_Coded_Geometry * geometry, size_t bytes);

/**
 * @brief Decode one whole or partial block into interleaved 16-bit samples.
 *
 * @param geometry From gaud_coded_geometry().
 * @param in The block's bytes.
 * @param in_bytes How many are actually present. A short final block is
 *   normal and decodes to proportionally fewer frames; it is not an error.
 * @param out At least `geometry->block_frames * channels` samples of room.
 * @return How many frames were written. Zero means the input held none.
 */
size_t gaud_coded_decode_block(const GAUD_Coded_Geometry * geometry,
    const unsigned char * in, size_t in_bytes, int16_t * out);

/**
 * @brief Encode up to one block's worth of frames.
 *
 * Fewer than `block_frames` is allowed and is what the last block of a
 * file does. The block is still written at its full `block_bytes` length,
 * because that is what the container's header promised: the tail is
 * padding, and the true frame count is carried elsewhere (WAV's `fact`).
 *
 * @param geometry From gaud_coded_geometry().
 * @param in Interleaved 16-bit samples, `frames` of them.
 * @param frames How many to take. More than `block_frames` is clamped.
 * @param out At least `geometry->block_bytes` of room.
 * @return How many frames were consumed, which is `frames` clamped to
 *   `block_frames`.
 */
size_t gaud_coded_encode_block(const GAUD_Coded_Geometry * geometry,
    const int16_t * in, size_t frames, unsigned char * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_SHARED_ADPCM_H
