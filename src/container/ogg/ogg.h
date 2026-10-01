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
 *
 * **A page is found by its checksum and not by its magic.** `OggS` is four
 * bytes of compressed audio away from being a coincidence, and a page body
 * may contain it; a scan that trusted the magic alone would find pages that
 * are not there. Everything below that looks for a page reads the candidate
 * header, reads the body it claims, and believes it only if the CRC agrees
 * - which is what makes bisection safe (gaud_ogg_bisect()).
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

/** The most bytes one page can occupy: fixed header, table, body. */
#define OGG_MAX_PAGE (OGG_HEADER_FIXED + OGG_MAX_SEGMENTS + OGG_MAX_BODY)

/** The granule position a page states when no packet finished on it. */
#define OGG_NO_GRANULE UINT64_MAX

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
  /**
   * Where the current page begins in the stream.
   *
   * Which is what a caller needs to record "start reading again here":
   * the stream's own position after a packet is somewhere inside a page
   * body, and seeking back to that is not a page boundary. Vorbis needs
   * it because its setup header's last page may also carry the first
   * audio packet, so the place to resume is that page and not the byte
   * after the header.
   */
  uint64_t page_offset;
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
  /**
   * Whether a packet continued from before this reader's position must be
   * thrown away.
   *
   * Set by gaud_ogg_reader_seek(), because a seek lands on a page boundary
   * and the first packet finishing there may have *started* on a page the
   * reader never saw. Handing that fragment to a codec as a whole packet
   * is the defect this flag exists to prevent: it is not corrupt, it is
   * the tail of something, and a decoder has no way to tell.
   */
  bool drop_continued;

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

/* ------------------------------------------------- finding pages by hand */

/** What a page scan found, without any of the reader's packet state. */
typedef struct {
  uint64_t offset;  ///< Where the page begins.
  uint64_t next;    ///< The byte just past it, where the next page may be.
  uint64_t granule; ///< Its granule position, or ::OGG_NO_GRANULE.
  uint32_t serial;  ///< Which logical stream it belongs to.
  uint32_t flags;   ///< Its type byte.
  uint32_t segments; ///< How many lacing values its table holds.
  uint32_t body;    ///< How many body bytes it carries.
} OGG_Page_Info;

/**
 * @brief Find the first valid page at or after @p from.
 *
 * Scans for `OggS`, reads what the candidate claims, and accepts it only
 * if its CRC agrees - see the note at the head of this file about why the
 * magic alone is not enough.
 *
 * @param stream Seekable. Left positioned past the page that was found.
 * @param allocator For the scratch buffer.
 * @param from Where to start looking.
 * @param limit One past the last byte a page may begin at.
 * @param out Receives what was found.
 * @return ::GAUD_OK; ::GAUD_ERR_FORMAT if no page begins before @p limit,
 *   which is the ordinary way a scan ends and not an error in the file.
 */
GAUD_Result gaud_ogg_find_page(GAUD_Stream * stream,
    const GAUD_Allocator * allocator, uint64_t from, uint64_t limit,
    OGG_Page_Info * out);

/** The most logical streams gaud_ogg_scan_logical() will report. */
#define OGG_MAX_LOGICAL 16u

/** The bytes of a logical stream's first packet that are kept for it. */
#define OGG_HEAD_KEPT 64u

/**
 * @brief One logical stream, as its beginning-of-stream page describes it.
 *
 * `head` is what identifies the codec: every Ogg mapping puts a signature
 * at the start of its first packet (`\x01vorbis`, `OpusHead`, `\x7FFLAC`),
 * and that packet is required to be alone on the BOS page, so the first
 * bytes of the page body are the signature without any packet assembly.
 */
typedef struct {
  uint32_t serial;                    ///< Its serial number.
  uint64_t offset;                    ///< Where its BOS page begins.
  size_t head_size;                   ///< How much of @p head is filled.
  unsigned char head[OGG_HEAD_KEPT];  ///< The start of its first packet.
} OGG_Logical;

/**
 * @brief List the logical streams an Ogg file begins with.
 *
 * Ogg requires every logical stream's BOS page to precede any other page,
 * so the whole list is at the head of the file and this does not read the
 * body. A file with one stream - which is what every audio-only Ogg file
 * is - reports one, and the caller may then stop thinking about
 * multiplexing. A file with several is a container the caller has to
 * choose from, and choosing is the caller's job because only the codec
 * knows which signature it answers to.
 *
 * @param stream Seekable; its position is not preserved.
 * @param allocator For the scan's scratch buffers.
 * @param out Receives the streams found, in the order their BOS pages
 *   appear, which is the order Ogg requires them to be written in.
 * @param capacity How many @p out holds.
 * @param out_count Receives how many were written; never more than
 *   @p capacity, and a file with more streams than that is reported up to
 *   the capacity rather than refused.
 * @return ::GAUD_OK; ::GAUD_ERR_FORMAT if the file does not begin with a
 *   page at all.
 */
GAUD_Result gaud_ogg_scan_logical(GAUD_Stream * stream,
    const GAUD_Allocator * allocator, OGG_Logical * out, size_t capacity,
    size_t * out_count);

/**
 * @brief The granule position the last page of @p serial states.
 *
 * Which, for the three codecs Ogg carries here, is the stream's length.
 * Vorbis and Opus state their length nowhere else - there is no field for
 * it in either codec's headers - so this is not an optimisation, it is the
 * only way to answer how long the file is.
 *
 * Scans backwards in windows from the end of the file, because the last
 * page of the file need not belong to @p serial and a multiplexed file's
 * streams need not end together.
 *
 * @return ::GAUD_OK; ::GAUD_ERR_FORMAT if no page of @p serial anywhere in
 *   the file states a granule position.
 */
GAUD_Result gaud_ogg_last_granule(GAUD_Stream * stream,
    const GAUD_Allocator * allocator, uint32_t serial, uint64_t * out_granule);

/**
 * @brief Find a page to start decoding at, for a target granule position.
 *
 * Bisects the file, which is what Ogg's monotonic granule positions are
 * for. Returns the offset of a page of the reader's own serial whose
 * granule position is at or before @p target, and as close to it as
 * bisection can get - so the caller decodes forward from there and
 * discards, rather than decoding from the beginning and discarding.
 *
 * **The result is a page whose first packet is whole**, or @p floor. A
 * bisection that returned a page carrying the tail of a packet would hand
 * the caller's reader a fragment, so gaud_ogg_reader_seek() drops a
 * leading continuation and this is safe either way.
 *
 * @param reader A reader whose serial is already chosen; used for its
 *   stream and allocator only, and left untouched.
 * @param target The granule position wanted.
 * @param floor The offset of the first page it is legal to start at - past
 *   the headers. Returned as-is when @p target is at or before the first
 *   audio page, which is the end the search is easiest to get wrong at.
 * @param out_offset Receives the offset to start reading pages from.
 * @param out_granule Receives the granule position of the page before it,
 *   i.e. how many of the codec's units are already behind @p out_offset,
 *   or 0 when the answer is @p floor.
 */
GAUD_Result gaud_ogg_bisect(OGG_Reader * reader, uint64_t target,
    uint64_t floor, uint64_t * out_offset, uint64_t * out_granule);

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
  /**
   * Whether the last lacing value buffered ended a packet.
   *
   * Which is not the same question as "is a packet being written", and
   * conflating the two is a defect that only appears when the segment
   * table fills *exactly* at a packet boundary. A page is then emitted
   * because there is no room for another lacing value, and it did finish a
   * packet - so it must state that packet's granule position, and the page
   * after it must not be marked as a continuation. A writer that decided
   * both from "we are part way through paging a packet" gets both wrong on
   * one page in however many, which is the kind of arithmetic no fixture
   * from a real encoder reaches.
   */
  bool page_complete;
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
