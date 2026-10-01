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
 * Ogg pages and packets, as RFC 3533 specifies them. Never installed.
 *
 * **Under `src/container/` and not `src/codec/flac/`, which is the whole
 * reason that directory exists** (planning/audio.md section 1). Ogg carries
 * FLAC here, and Vorbis and Opus in phase 6, and the page layer is
 * identical for all three: the codec decides what a packet means and this
 * decides where one ends.
 *
 * The three things about Ogg that a first implementation gets wrong, all
 * of them visible in the structures below:
 *
 * **A packet is not a page.** One page may hold many packets and one
 * packet may span many pages, and which it is depends on a table of
 * lacing values at the head of each page: a run of 255s continues and
 * anything below 255 ends. A reader that treated a page as a packet works
 * perfectly on the small files anyone writes by hand.
 *
 * **The checksum is computed over the page with its own checksum field
 * zeroed**, so the field has to be cleared, the CRC taken, and the field
 * written - and on the reading side the four bytes have to be put back
 * afterwards if the buffer is used again.
 *
 * **The granule position is the codec's, not the container's.** Ogg
 * carries a 64-bit number per page and attaches no meaning to it beyond
 * "monotonic". For FLAC it is the sample number of the last sample
 * finished on the page, and `-1` means no packet finished there.
 */

#ifndef GHOTI_IO_GAUD_SRC_CONTAINER_OGG_OGG_H
#define GHOTI_IO_GAUD_SRC_CONTAINER_OGG_OGG_H

#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The four bytes a page begins with. */
#define OGG_MAGIC "OggS"

/** Bytes in a page header before the segment table. */
#define OGG_HEADER_FIXED 27u

/** The most segments a page's table can hold, and so the most it can carry. */
#define OGG_MAX_SEGMENTS 255u

/** The most body bytes one page can hold: 255 segments of 255 bytes. */
#define OGG_MAX_BODY (255u * 255u)

/** Page flags, from the header's type byte. */
enum {
  OGG_FLAG_CONTINUED = 0x01u, ///< The first packet continues the last page's.
  OGG_FLAG_BOS = 0x02u,       ///< Beginning of stream.
  OGG_FLAG_EOS = 0x04u        ///< End of stream.
};

/** @brief Ogg's CRC-32: polynomial 0x04C11DB7, unreflected, no final xor. */
uint32_t gaud_ogg_crc32(const unsigned char * data, size_t size);

/** @brief The same, continued from @p crc, for a page in two buffers. */
uint32_t gaud_ogg_crc32_update(
    uint32_t crc, const unsigned char * data, size_t size);

/**
 * @brief Reassembles packets out of one logical stream's pages.
 *
 * Follows a single serial number. An Ogg file may interleave several
 * logical streams - that is what a video file is - and a reader that
 * ignored the serial would splice two streams' packets together.
 */
typedef struct {
  GAUD_Stream * stream;             ///< Borrowed.
  const GAUD_Allocator * allocator; ///< Borrowed.
  uint32_t serial;                  ///< The logical stream being followed.
  bool have_serial;                 ///< Whether @p serial has been chosen.

  unsigned char * page;    ///< The current page, header and body. Owned.
  size_t page_capacity;    ///< Room in @p page.
  size_t body_at;          ///< Where the body starts within @p page.
  size_t body_size;        ///< How many body bytes the page holds.
  unsigned char table[OGG_MAX_SEGMENTS]; ///< The lacing values.
  uint32_t segments;       ///< How many of them.
  uint32_t next_segment;   ///< The next lacing value to consume.
  size_t next_body;        ///< The next body byte to consume.
  uint64_t granule;        ///< The current page's granule position.
  uint32_t flags;          ///< The current page's type byte.
  bool page_live;          ///< Whether a page has been read at all.
  bool eos;                ///< Whether the last page read ended the stream.

  unsigned char * packet;  ///< The packet being assembled. Owned.
  size_t packet_size;      ///< Bytes of it so far.
  size_t packet_capacity;  ///< Room in @p packet.
} OGG_Reader;

/** @brief Start a reader over @p stream at its current position. */
void gaud_ogg_reader_init(OGG_Reader * reader, GAUD_Stream * stream,
    const GAUD_Allocator * allocator);

/** @brief Release everything the reader owns. */
void gaud_ogg_reader_free(OGG_Reader * reader);

/**
 * @brief Read the next complete packet of the followed stream.
 *
 * @param reader The reader.
 * @param out_data Receives a pointer into the reader's own buffer, valid
 *   until the next call.
 * @param out_size Receives its length.
 * @param out_granule May be NULL. The granule position of the page the
 *   packet finished on, or `UINT64_MAX` where that page stated none.
 * @param out_flags May be NULL. The flags of the page it finished on.
 * @return ::GAUD_OK; ::GAUD_ERR_FORMAT at a clean end of stream, which is
 *   how a caller learns there are no more packets; ::GAUD_ERR_CORRUPT for
 *   a page whose checksum or framing is wrong.
 */
GAUD_Result gaud_ogg_reader_packet(OGG_Reader * reader,
    const unsigned char ** out_data, size_t * out_size,
    uint64_t * out_granule, uint32_t * out_flags);

/**
 * @brief Rewind to @p offset and start reading pages again from there.
 *
 * The serial being followed is kept, so a seek does not re-choose the
 * logical stream. Any half-assembled packet is discarded.
 */
GAUD_Result gaud_ogg_reader_seek(OGG_Reader * reader, uint64_t offset);

/** @brief Builds pages around packets. */
typedef struct {
  GAUD_Stream * stream;             ///< Borrowed.
  uint32_t serial;                  ///< This logical stream's number.
  uint32_t sequence;                ///< The next page's sequence number.
  unsigned char body[OGG_MAX_BODY]; ///< Bytes not yet in a page.
  size_t body_size;                 ///< How many of them.
  unsigned char table[OGG_MAX_SEGMENTS]; ///< The lacing values so far.
  uint32_t segments;                ///< How many of them.
  uint64_t granule;                 ///< To stamp on the next page.
  bool started;                     ///< Whether the BOS page has gone out.
  bool continued;                   ///< Whether the next page continues one.
} OGG_Writer;

/** @brief Start a writer for logical stream @p serial over @p stream. */
void gaud_ogg_writer_init(
    OGG_Writer * writer, GAUD_Stream * stream, uint32_t serial);

/**
 * @brief Add one packet, and page it out.
 *
 * @param writer The writer.
 * @param data The packet's bytes.
 * @param size How many of them. Zero is a legal packet.
 * @param granule The granule position for the page this packet completes.
 * @param flush Whether to emit the page immediately rather than letting
 *   the next packet share it. FLAC's mapping flushes every header packet
 *   and every audio frame, which keeps one packet to a page and makes the
 *   granule position exact without any bookkeeping.
 * @param eos Whether this is the last packet of the stream.
 */
GAUD_Result gaud_ogg_writer_packet(OGG_Writer * writer,
    const unsigned char * data, size_t size, uint64_t granule, bool flush,
    bool eos);

/** @brief Emit whatever is buffered, even if it is nothing. */
GAUD_Result gaud_ogg_writer_flush(OGG_Writer * writer, bool eos);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CONTAINER_OGG_OGG_H
