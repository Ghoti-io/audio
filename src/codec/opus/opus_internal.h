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
 * Opus: what its headers state, and what one packet is. Never installed.
 *
 * Four things about Opus have no analogue in anything else here, and all
 * four are visible before a single sample is decoded.
 *
 * **The sample rate is always 48,000 and the file's own rate is advice.**
 * RFC 6716 lets a decoder output at 8, 12, 16, 24 or 48 kHz whatever the
 * encoder was given, the Ogg mapping counts granule positions at 48 kHz,
 * and `OpusHead` records the original rate as a field explicitly marked
 * informational. So a track's sample rate here is 48,000 for every Opus
 * file, and the number in the header is reported nowhere.
 *
 * **The length needs the pre-skip subtracted.** Every Opus stream begins
 * with samples the encoder's own filters needed and the recording does
 * not contain - a number of them stated in `OpusHead` - and the granule
 * positions count those. So the length is the last page's position minus
 * the pre-skip, and a reader that forgot would report every file as
 * several milliseconds too long. This is the gapless arithmetic MPEG
 * audio needed a Xing tag for, except that here it is in the format and
 * is not optional.
 *
 * **A packet may hold several frames.** One byte of table of contents
 * says which of 32 configurations the packet uses and how many frames
 * follow, in one of four framings; a 60 ms packet is three 20 ms frames.
 * Counting packets is therefore not counting anything.
 *
 * **The mode changes within a stream.** A packet is SILK, CELT, or both
 * at once, chosen by the configuration, and may differ from the packet
 * before it. That is the point of the design and is why
 * ::GAUD_CODING_OPUS is one value rather than three.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_INTERNAL_H
#define GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_INTERNAL_H

#include "../../container/ogg/ogg.h"
#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The identification header's magic. */
#define OPUS_HEAD_MAGIC "OpusHead"

/** The comment header's magic. */
#define OPUS_TAGS_MAGIC "OpusTags"

/** Bytes of either magic. */
#define OPUS_MAGIC_SIZE 8u

/** The smallest `OpusHead` the format allows: mapping family 0. */
#define OPUS_HEAD_MIN 19u

/** Every Opus decoder outputs at this rate, whatever the encoder saw. */
#define OPUS_RATE 48000u

/** The most channels a mapping family may describe. */
#define OPUS_MAX_CHANNELS 255u

/** @brief Which of the three modes a packet's configuration selects. */
typedef enum {
  OPUS_MODE_SILK = 0, ///< Speech, 8 to 16 kHz of bandwidth.
  OPUS_MODE_HYBRID,   ///< SILK below 8 kHz and CELT above it.
  OPUS_MODE_CELT      ///< Music, and the only mode below 10 ms.
} OPUS_Mode;

/** @brief What one packet's table of contents byte says. */
typedef struct {
  OPUS_Mode mode;      ///< Which coder.
  unsigned bandwidth;  ///< 0 narrow, 1 medium, 2 wide, 3 super-wide, 4 full.
  uint32_t frame_size; ///< Samples per frame at 48 kHz.
  uint32_t frames;     ///< How many frames the packet holds.
  bool stereo;         ///< Whether the packet is coded as two channels.
  unsigned code;       ///< The framing, 0 to 3.
} OPUS_Toc;

/** @brief What `OpusHead` states. */
typedef struct {
  unsigned version;      ///< Major must be 0; the minor is ignorable.
  uint32_t channels;     ///< At least one.
  uint32_t pre_skip;     ///< Samples at 48 kHz to drop from the front.
  uint32_t input_rate;   ///< Informational; the output is always 48 kHz.
  int32_t output_gain;   ///< In Q7.8 decibels, to be applied by a player.
  unsigned mapping_family; ///< 0, 1 or 255.
  uint32_t streams;      ///< How many Opus streams are multiplexed.
  uint32_t coupled;      ///< How many of those are stereo pairs.
  unsigned char mapping[OPUS_MAX_CHANNELS]; ///< Which stream feeds each.
} OPUS_Head;

/** @brief What one Opus document keeps. */
typedef struct {
  OPUS_Head head;                   ///< From `OpusHead`.
  const GAUD_Allocator * allocator; ///< Everything here comes from it.
  uint32_t serial;                  ///< The logical stream being followed.
  uint64_t audio_offset;            ///< The page the headers finished on.
  uint64_t frames;                  ///< The length, pre-skip subtracted.
  bool frames_known;                ///< Whether that could be established.
} OPUS_File;

/** The most frames one packet may carry: 48 of 2.5 ms is 120 ms. */
#define OPUS_MAX_FRAMES 48u

/** The most bytes one frame may be, so a gateway can repack a stream. */
#define OPUS_MAX_FRAME_BYTES 1275u

/** @brief One packet split into the frames it carries. */
typedef struct {
  OPUS_Toc toc;                            ///< What the first byte says.
  const unsigned char * frame[OPUS_MAX_FRAMES]; ///< Into the packet.
  uint32_t length[OPUS_MAX_FRAMES];        ///< Bytes of each.
  uint32_t count;                          ///< How many are in use.
  size_t payload_offset;                   ///< Where the first one starts.
} OPUS_Packet;

/**
 * @brief Split one packet into its frames.
 *
 * Stricter than ::gaud_opus_parse_toc, which answers only how long the
 * packet is; this enforces every one of section 3.2's numbered framing
 * requirements and so refuses packets whose duration is still readable.
 *
 * @param data The packet.
 * @param size Its length.
 * @param self_delimited Whether the last frame states its own length,
 *   which is how the multistream mapping packs all but its last stream.
 * @param out Receives the frames; the pointers are into @p data.
 * @return ::GAUD_OK, or ::GAUD_ERR_CORRUPT.
 */
GAUD_Result gaud_opus_parse_packet(const unsigned char * data, size_t size,
    bool self_delimited, OPUS_Packet * out);

/**
 * @brief Read an `OpusHead` out of @p data.
 *
 * @return ::GAUD_OK; ::GAUD_ERR_FORMAT if it is not one; ::GAUD_ERR_CORRUPT
 *   if it is one and says something impossible; ::GAUD_ERR_UNSUPPORTED for
 *   a major version this does not know.
 */
GAUD_Result gaud_opus_parse_head(
    const unsigned char * data, size_t size, OPUS_Head * out);

/**
 * @brief Read one packet's table of contents and frame lengths.
 *
 * @param data The packet.
 * @param size Its length.
 * @param out Receives what the byte says, and how many frames follow.
 * @return ::GAUD_OK, or ::GAUD_ERR_CORRUPT for a packet whose framing
 *   does not fit in the bytes it has.
 */
GAUD_Result gaud_opus_parse_toc(
    const unsigned char * data, size_t size, OPUS_Toc * out);

/** @brief ::GAUD_Codec::open. */
GAUD_Result gaud_opus_open(const GAUD_Codec * codec, GAUD_Stream * stream,
    const GAUD_Limits * limits, GAUD_Diagnostics * diagnostics,
    GAUD_Doc ** out_doc);

/** @brief ::GAUD_Codec::close. */
void gaud_opus_close(const GAUD_Codec * codec, GAUD_Doc * doc);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_INTERNAL_H
