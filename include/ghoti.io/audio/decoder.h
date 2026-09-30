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
 * @file decoder.h
 *
 * Pulling samples out of a track, and pushing them into a file.
 *
 * ### Why this is a pull and not a load
 *
 * `image` can hand back a whole picture because a picture's size is bounded
 * by dimensions its header states. Audio has no such bound that is any use:
 * an hour of 96 kHz 24-bit stereo is about 2 GB of PCM and a three-minute CD
 * track is 31 MB. There is no `gaud_track_decode_all()` that is safe to call
 * on an arbitrary file, and a ::GAUD_Limits field capping it only moves the
 * failure to a different line.
 *
 * So gaud_doc_load() parses the container and decodes nothing, and samples
 * come from here a block at a time, into a buffer the caller sized.
 *
 * ### Why seeking reports where it landed
 *
 * Audio codecs carry state across frames. MP3's bit reservoir spans them;
 * every MDCT codec needs the previous frame's overlap. A seek is therefore
 * "position, then decode some frames of pre-roll and throw them away", and
 * how many is the codec's business - so the frame a decoder can actually
 * resume at is not always the frame that was asked for.
 *
 * A seek that returned only OK or an error would be a sentinel meaning two
 * things, and every caller would then re-derive the position by a method the
 * library already knew. gaud_decoder_seek() writes where it landed.
 */

#ifndef GHOTI_IO_GAUD_DECODER_H
#define GHOTI_IO_GAUD_DECODER_H

#include <ghoti.io/audio/buffer.h>
#include <ghoti.io/audio/coding.h>
#include <ghoti.io/audio/core.h>
#include <ghoti.io/audio/doc.h>
#include <ghoti.io/audio/macros.h>
#include <ghoti.io/audio/stream.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Reads samples from one track. */
typedef struct GAUD_Decoder GAUD_Decoder;

/** @brief Writes samples into one stream. */
typedef struct GAUD_Encoder GAUD_Encoder;

/**
 * @brief Open a decoder on a track.
 *
 * The track's document, and the stream behind it, must outlive the decoder.
 *
 * @param track The track to read.
 * @param out_decoder Receives the decoder. Written only on success.
 * @return ::GAUD_OK, ::GAUD_ERR_INVALID, ::GAUD_ERR_OOM, or
 *   ::GAUD_ERR_UNSUPPORTED when the codec that produced the document cannot
 *   decode - which is the honest answer for a codec registered by a build
 *   that only knows how to identify the format.
 */
GAUD_API GAUD_Result gaud_decoder_create(
    GAUD_Track * track, GAUD_Decoder ** out_decoder);

/** @brief Close a decoder. Safe on NULL. Does not touch the document. */
GAUD_API void gaud_decoder_destroy(GAUD_Decoder * decoder);

/**
 * @brief Fill a buffer with the next frames.
 *
 * The buffer's format, channel count and sample layout must match the
 * track's; this call converts nothing, which is the same rule load and save
 * follow. gaud_decoder_buffer_create() makes one that matches.
 *
 * **A short read is not an error.** It means the track ended. The buffer's
 * frame count is set to what was written, so a caller reads until it gets
 * zero.
 *
 * @param decoder The decoder.
 * @param buffer Filled, and its frame count set.
 * @return ::GAUD_OK - including at the end of the track, with zero frames -
 *   or ::GAUD_ERR_INVALID, ::GAUD_ERR_IO, ::GAUD_ERR_CORRUPT.
 */
GAUD_API GAUD_Result gaud_decoder_read(
    GAUD_Decoder * decoder, GAUD_Buffer * buffer);

/**
 * @brief A buffer shaped for this decoder.
 *
 * Saves a caller assembling the format, layout and sample layout by hand and
 * getting one of them wrong, which gaud_decoder_read() would then refuse.
 */
GAUD_API GAUD_Result gaud_decoder_buffer_create(const GAUD_Decoder * decoder,
    const GAUD_Allocator * allocator, size_t frames, GAUD_Buffer ** out_buffer);

/**
 * @brief Move to a frame, and say which frame that turned out to be.
 *
 * @param decoder The decoder.
 * @param frame The frame wanted, counted from the start of the track.
 * @param out_landed Receives the frame the next read will actually return.
 *   For a format with no inter-frame state - every format in phase 1 - this
 *   equals @p frame. It is not optional, because a caller that ignored it
 *   would be assuming something only some codecs make true.
 * @return ::GAUD_OK, ::GAUD_ERR_INVALID for a frame past the end,
 *   or ::GAUD_ERR_UNSUPPORTED on a stream that cannot seek.
 */
GAUD_API GAUD_Result gaud_decoder_seek(
    GAUD_Decoder * decoder, uint64_t frame, uint64_t * out_landed);

/** @brief The frame the next read will return. */
GAUD_API uint64_t gaud_decoder_tell(const GAUD_Decoder * decoder);

/** @brief The track being decoded. */
GAUD_API GAUD_Track * gaud_decoder_track(const GAUD_Decoder * decoder);

/**
 * @brief What a file being written should look like.
 *
 * Zero-initialising this is not valid: a sample rate of zero is not a
 * default, it is a file nothing can play. gaud_encode_params_default() fills
 * in something playable which the caller then edits.
 */
typedef struct GAUD_Encode_Params {
  /**
   * What the samples handed to gaud_encoder_write() are.
   *
   * When @p coding is not ::GAUD_CODING_PCM this must be the format that
   * coding decodes to - gaud_sample_coding_format() says which, and it is
   * ::GAUD_SAMPLE_S16 for every coding that exists today. Anything else is
   * ::GAUD_ERR_INVALID at gaud_encoder_create(), rather than a silent
   * conversion the caller did not ask for.
   */
  GAUD_Sample_Format format;
  /**
   * How the file codes them. Zero is ::GAUD_CODING_PCM, so a params struct
   * filled in before this field existed still means what it meant.
   *
   * This is separate from @p format because they answer different
   * questions: a µ-law file's samples are ::GAUD_SAMPLE_S16 in every buffer
   * and one companded byte on disk. See coding.h.
   */
  GAUD_Sample_Coding coding;
  /** Frames per second. */
  uint32_t sample_rate;
  /** Which speakers, and how many channels. */
  GAUD_Channel_Layout layout;
  /**
   * Caps applied while writing, chiefly on how large the file may become.
   * NULL is the defaults.
   */
  const GAUD_Limits * limits;
} GAUD_Encode_Params;

/** @brief 44.1 kHz stereo signed 16-bit, which every format here can write. */
GAUD_API void gaud_encode_params_default(GAUD_Encode_Params * params);

/**
 * @brief Open an encoder that writes @p codec_name into @p stream.
 *
 * The stream must be writable, and for the formats in phase 1 it must also be
 * **seekable**: a RIFF or IFF header states the length of data that has not
 * been written yet, so the header is patched by gaud_encoder_finish(). A
 * non-seekable sink gets ::GAUD_ERR_UNSUPPORTED here rather than a file with
 * a wrong length in it.
 *
 * @param codec_name Which format to write, as registered.
 * @param registry NULL for the default.
 * @param stream Where the bytes go. Must outlive the encoder.
 * @param params Not optional; see ::GAUD_Encode_Params.
 * @param out_encoder Receives the encoder. Written only on success.
 */
GAUD_API GAUD_Result gaud_encoder_create(const char * codec_name,
    struct GAUD_Registry * registry, GAUD_Stream * stream,
    const GAUD_Encode_Params * params, GAUD_Encoder ** out_encoder);

/**
 * @brief Write a buffer's frames.
 *
 * The buffer must match the parameters the encoder was created with. Writing
 * converts nothing, so a caller with samples in another format calls
 * gaud_ops_convert_format() first and decides about dither there rather than
 * having this make the choice quietly.
 */
GAUD_API GAUD_Result gaud_encoder_write(
    GAUD_Encoder * encoder, const GAUD_Buffer * buffer);

/**
 * @brief Finish the file: patch the header, flush, and stop.
 *
 * **Must be called, and its result must be checked.** This is where a RIFF
 * length is written, so an encoder that is merely destroyed leaves a file
 * whose header describes a size it does not have. Destroying without
 * finishing is allowed and means "abandon this file".
 */
GAUD_API GAUD_Result gaud_encoder_finish(GAUD_Encoder * encoder);

/** @brief Free an encoder. Safe on NULL. Does not finish it. */
GAUD_API void gaud_encoder_destroy(GAUD_Encoder * encoder);

/** @brief How many frames have been written so far. */
GAUD_API uint64_t gaud_encoder_frames_written(const GAUD_Encoder * encoder);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_DECODER_H
