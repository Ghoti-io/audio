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
 * Vorbis I: what the headers state, and what a document concludes from
 * them. Never installed.
 *
 * Three things about Vorbis are different from every format already in
 * this library, and all three are visible here.
 *
 * **Vorbis has no frames.** It has packets, and a packet's decoded length
 * depends on the packet *before* it: a block of N samples overlaps the
 * previous block by half, so the samples a packet contributes are
 * (previous_block + this_block) / 4 and the first packet of a stream
 * contributes nothing at all. There is no field anywhere that says how
 * long the stream is. The only answer is the last page's granule
 * position, which is why gaud_ogg_last_granule() had to exist before this
 * file could.
 *
 * **The bit packing runs the other way.** FLAC and MPEG read bits from the
 * most significant end of each byte; Vorbis reads from the least
 * significant end, and a multi-bit field's first bit read is its *lowest*.
 * A reader that got this backwards reads the identification header
 * perfectly - it is byte-aligned apart from two nibbles - and then reads
 * the codebooks as noise.
 *
 * **Almost everything is in the setup header.** The codebooks, the floor
 * curves, the residue layout, the channel coupling and the block modes are
 * all defined per stream rather than by the specification, so there are no
 * tables in this codec to extract from a document the way phase 5's were
 * (planning/audio.md section 11.20). What a standard would have fixed, a
 * Vorbis stream states.
 *
 * This file is the identification half: the three headers, the tags, and
 * the length. See planning/audio.md section 11.18 for why that is a commit
 * of its own.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_VORBIS_VORBIS_INTERNAL_H
#define GHOTI_IO_GAUD_SRC_CODEC_VORBIS_VORBIS_INTERNAL_H

#include "../../container/ogg/ogg.h"
#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The common part of every Vorbis header packet: a type byte, then this. */
#define VORBIS_SIGNATURE "vorbis"

/** Bytes in that common part, type byte included. */
#define VORBIS_HEAD_SIZE 7u

/** Header packet types. Odd, so that no header can be mistaken for audio:
 * an audio packet's first bit is zero and a header's is one. */
enum {
  VORBIS_PACKET_IDENTIFICATION = 1,
  VORBIS_PACKET_COMMENT = 3,
  VORBIS_PACKET_SETUP = 5
};

/** Bytes of the identification header, signature included. */
#define VORBIS_IDENTIFICATION_SIZE 30u

/** The smallest block size the format allows. */
#define VORBIS_MIN_BLOCKSIZE 64u

/** The largest block size the format allows. */
#define VORBIS_MAX_BLOCKSIZE 8192u

/** @brief What the identification header states, and nothing more. */
typedef struct {
  uint32_t version;      ///< Must be zero; anything else is another format.
  uint32_t channels;     ///< At least one.
  uint32_t sample_rate;  ///< At least one; no list of legal values.
  int32_t bitrate_maximum; ///< Advisory. Zero or negative means unset.
  int32_t bitrate_nominal; ///< Advisory.
  int32_t bitrate_minimum; ///< Advisory.
  uint32_t blocksize_short; ///< A power of two in [64, 8192].
  uint32_t blocksize_long;  ///< The same, and not smaller than the short.
} VORBIS_Info;

/** @brief What one Vorbis document keeps. */
typedef struct {
  VORBIS_Info info;                 ///< From the identification header.
  const GAUD_Allocator * allocator; ///< Everything here comes from it.
  /**
   * The logical stream the Vorbis is in.
   *
   * Kept for the same reason Ogg FLAC keeps it: an Ogg file may
   * multiplex, and a decoder that let the reader choose a serial afresh
   * could follow a different stream from the one the document describes.
   */
  uint32_t serial;
  uint64_t audio_offset; ///< Where the first page after the headers begins.
  uint64_t frames;       ///< The last granule position, which is the length.
  bool frames_known;     ///< Whether that could be established at all.
} VORBIS_File;

/**
 * @brief Read an identification header out of @p data.
 *
 * @return ::GAUD_OK; ::GAUD_ERR_FORMAT if it is not one; ::GAUD_ERR_CORRUPT
 *   if it is one and says something impossible.
 */
GAUD_Result gaud_vorbis_parse_identification(
    const unsigned char * data, size_t size, VORBIS_Info * out);

/** @brief Whether @p data begins a Vorbis header packet of @p type. */
bool gaud_vorbis_is_header(
    const unsigned char * data, size_t size, unsigned type);

/** @brief ::GAUD_Codec::open. */
GAUD_Result gaud_vorbis_open(const GAUD_Codec * codec, GAUD_Stream * stream,
    const GAUD_Limits * limits, GAUD_Diagnostics * diagnostics,
    GAUD_Doc ** out_doc);

/** @brief ::GAUD_Codec::close. */
void gaud_vorbis_close(const GAUD_Codec * codec, GAUD_Doc * doc);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_VORBIS_VORBIS_INTERNAL_H
