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
 * @file codec_sdk.h
 *
 * What a codec calls, as distinct from what a caller calls.
 *
 * codec.h says how a codec is registered and found. This says how one builds
 * a document, describes a track, and hands back a decoder or an encoder. It
 * is **installed**, for the same reason codec.h is: a codec in another
 * repository has to be able to do all of it, and a header that lived in
 * `src/` would make "third parties can write codecs" false while looking
 * true.
 *
 * Nothing here is useful to a caller who is only reading audio, which is why
 * it is a separate header and why `audio.h` does not include it. Including it
 * is how a translation unit says it is implementing a codec.
 */

#ifndef GHOTI_IO_GAUD_CODEC_SDK_H
#define GHOTI_IO_GAUD_CODEC_SDK_H

#include <ghoti.io/audio/allocator.h>
#include <ghoti.io/audio/buffer.h>
#include <ghoti.io/audio/codec.h>
#include <ghoti.io/audio/coding.h>
#include <ghoti.io/audio/core.h>
#include <ghoti.io/audio/decoder.h>
#include <ghoti.io/audio/doc.h>
#include <ghoti.io/audio/macros.h>
#include <ghoti.io/audio/stream.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Everything a codec knows about a track once it has read the header.
 *
 * Passed by value to gaud_doc_add_track(). Zero-initialise it and fill in
 * what the container stated: a field left zero means "not stated", except
 * where noted, and that is a fact a caller can act on rather than a default
 * pretending to be one.
 */
typedef struct {
  /** What this track's decoder will put in a buffer. Required. */
  GAUD_Sample_Format format;
  /**
   * How the container codes them, or ::GAUD_CODING_PCM (zero) when it does
   * not. A codec that decodes µ-law sets @p format to ::GAUD_SAMPLE_S16 and
   * this to ::GAUD_CODING_G711_ULAW: the first is what the caller receives,
   * the second is what the file said, and gaud_track_coding() reports it so
   * a round trip can put it back.
   */
  GAUD_Sample_Coding coding;
  /** Frames per second. Required, and refused if zero. */
  uint32_t sample_rate;
  /** Which speakers. Its `channels` is required and refused if zero. */
  GAUD_Channel_Layout layout;
  /** Interleaved or planar, as this track's decoder will produce. */
  GAUD_Sample_Layout sample_layout;
  /** Frames the container claims, or `UINT64_MAX` when it did not say. */
  uint64_t frames;
  /** Encoder delay and padding. Leave zeroed where the format has none. */
  GAUD_Trim trim;
  /** Where this track's samples begin in the stream, for the decoder's use. */
  uint64_t data_offset;
  /** How many bytes of them there are, or `UINT64_MAX` when unknown. */
  uint64_t data_length;
  /** The codec's own, freed by the codec's close. */
  void * codec_private;
} GAUD_Track_Desc;

/**
 * @brief Create an empty document for this codec's stream.
 *
 * @param codec The codec creating it, which is stored and reported by
 *   gaud_doc_codec_name().
 * @param stream Borrowed; must outlive the document.
 * @param allocator NULL for the default. Everything the document owns comes
 *   from here, including the tracks.
 * @param out_doc Receives the document. Written only on success.
 */
GAUD_API GAUD_Result gaud_doc_create_internal(const GAUD_Codec * codec,
    GAUD_Stream * stream, const GAUD_Allocator * allocator,
    GAUD_Doc ** out_doc);

/**
 * @brief Add a track described by @p desc.
 *
 * @param doc A document from gaud_doc_create_internal().
 * @param desc What the container said. Copied.
 * @param out_track Receives the track, owned by the document. May be NULL if
 *   the codec does not need it back.
 * @return ::GAUD_OK; ::GAUD_ERR_INVALID for a rate or channel count of zero,
 *   or a layout whose mask and count disagree; ::GAUD_ERR_OOM.
 */
GAUD_API GAUD_Result gaud_doc_add_track(GAUD_Doc * doc,
    const GAUD_Track_Desc * desc, GAUD_Track ** out_track);

/** @brief Attach codec-private state to a document. */
GAUD_API void gaud_doc_set_private(GAUD_Doc * doc, void * codec_private);

/** @brief Read it back. */
GAUD_API void * gaud_doc_private(const GAUD_Doc * doc);

/** @brief The codec-private state of one track, from its ::GAUD_Track_Desc. */
GAUD_API void * gaud_track_private(const GAUD_Track * track);

/** @brief Where this track's samples start, as the codec said. */
GAUD_API uint64_t gaud_track_data_offset(const GAUD_Track * track);

/** @brief How many bytes of them, or `UINT64_MAX`. */
GAUD_API uint64_t gaud_track_data_length(const GAUD_Track * track);

/** @brief How each sample is laid out relative to the others. */
GAUD_API GAUD_Sample_Layout gaud_track_sample_layout(const GAUD_Track * track);

/**
 * @brief What a codec's decoder must do.
 *
 * The library owns the ::GAUD_Decoder and keeps the position; this is the
 * part only the codec can do.
 */
typedef struct {
  /**
   * Fill @p buffer with up to its capacity, and set its frame count.
   *
   * Zero frames means the end of the track, and is not an error. The library
   * has already checked that @p buffer matches the track's shape.
   */
  GAUD_Result (*read)(GAUD_Decoder * decoder, GAUD_Buffer * buffer);
  /**
   * Position at @p frame, and report where that landed in @p out_landed.
   *
   * May be NULL, which means the track cannot be sought and
   * gaud_decoder_seek() answers ::GAUD_ERR_UNSUPPORTED.
   */
  GAUD_Result (*seek)(
      GAUD_Decoder * decoder, uint64_t frame, uint64_t * out_landed);
  /** Release the codec's own decoder state. May be NULL. */
  void (*close)(GAUD_Decoder * decoder);
} GAUD_Decoder_Vtable;

/**
 * @brief Build a decoder around a codec's vtable.
 *
 * @param track The track being decoded.
 * @param vtable Borrowed; must outlive the decoder, so a file-scope constant.
 * @param codec_private The codec's state, reachable with
 *   gaud_decoder_private().
 * @param out_decoder Receives the decoder. Written only on success.
 */
GAUD_API GAUD_Result gaud_decoder_create_internal(GAUD_Track * track,
    const GAUD_Decoder_Vtable * vtable, void * codec_private,
    GAUD_Decoder ** out_decoder);

/** @brief The codec state given to gaud_decoder_create_internal(). */
GAUD_API void * gaud_decoder_private(const GAUD_Decoder * decoder);

/** @brief Tell the library which frame the decoder is now at. */
GAUD_API void gaud_decoder_set_position(
    GAUD_Decoder * decoder, uint64_t frame);

/** @brief The allocator the decoder's document was created with. */
GAUD_API const GAUD_Allocator * gaud_decoder_allocator(
    const GAUD_Decoder * decoder);

/** @brief What a codec's encoder must do. */
typedef struct {
  /** Append @p buffer's frames. Already checked against the parameters. */
  GAUD_Result (*write)(GAUD_Encoder * encoder, const GAUD_Buffer * buffer);
  /** Patch the header, flush, and stop. Required. */
  GAUD_Result (*finish)(GAUD_Encoder * encoder);
  /** Release the codec's own encoder state. May be NULL. */
  void (*close)(GAUD_Encoder * encoder);
} GAUD_Encoder_Vtable;

/**
 * @brief Build an encoder around a codec's vtable.
 *
 * @param stream Borrowed; must outlive the encoder.
 * @param params Copied.
 * @param allocator NULL for the default.
 * @param vtable Borrowed; a file-scope constant.
 * @param codec_private Reachable with gaud_encoder_private().
 * @param out_encoder Receives the encoder. Written only on success.
 */
GAUD_API GAUD_Result gaud_encoder_create_internal(GAUD_Stream * stream,
    const GAUD_Encode_Params * params, const GAUD_Allocator * allocator,
    const GAUD_Encoder_Vtable * vtable, void * codec_private,
    GAUD_Encoder ** out_encoder);

/** @brief The codec state given to gaud_encoder_create_internal(). */
GAUD_API void * gaud_encoder_private(const GAUD_Encoder * encoder);

/** @brief The stream an encoder writes to. */
GAUD_API GAUD_Stream * gaud_encoder_stream(const GAUD_Encoder * encoder);

/** @brief The parameters an encoder was created with. */
GAUD_API const GAUD_Encode_Params * gaud_encoder_params(
    const GAUD_Encoder * encoder);

/** @brief Tell the library how many frames have been written. */
GAUD_API void gaud_encoder_add_frames(GAUD_Encoder * encoder, uint64_t frames);

/** @brief The allocator an encoder was created with. */
GAUD_API const GAUD_Allocator * gaud_encoder_allocator(
    const GAUD_Encoder * encoder);

/**
 * @brief The allocator a stream was created with.
 *
 * A codec allocates its own state from the same place the stream came from,
 * so that a caller who supplied an allocator gets one used throughout rather
 * than the default appearing halfway down.
 */
GAUD_API const GAUD_Allocator * gaud_stream_allocator(
    const GAUD_Stream * stream);

/**
 * @brief How many bytes one frame occupies in @p format with @p channels.
 *
 * Overflow-checked, and 0 for a non-PCM format where the question has no
 * answer. Spelled once because every codec needs it and a codec computing it
 * by hand is a codec that can get it wrong for 24-bit.
 */
GAUD_API size_t gaud_frame_size(GAUD_Sample_Format format, uint32_t channels);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_CODEC_SDK_H
