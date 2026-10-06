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
 * Reading and writing Ogg pages.
 *
 * See ogg.h for what a page is and for the three things about it that are
 * easy to get wrong. This file is the mechanics.
 */

#include "ogg.h"
#include "../../codec/shared/bytes.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/*
 * Ogg's CRC-32 is not the one in zlib, zip, PNG or Ethernet, and the
 * difference is not the polynomial - it is the reflection. The usual
 * CRC-32 reflects its input bits, its output and its initial value;
 * Ogg's reflects none of them and applies no final xor. Feeding an
 * ordinary crc32() the same bytes gives a different number for every
 * input, so this cannot borrow `compress`'s.
 *
 * Bit-serial rather than table-driven, for the reason flac_crc.c gives
 * at length: a mistyped table entry is a checksum that is right for
 * every input a test happens to use and wrong for one byte value in 256.
 */
uint32_t gaud_ogg_crc32_update(
    uint32_t crc, const unsigned char * data, size_t size) {
  for (size_t i = 0; i < size; ++i) {
    crc ^= (uint32_t)data[i] << 24;
    for (unsigned bit = 0; bit < 8u; ++bit) {
      crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
    }
  }
  return crc;
}

uint32_t gaud_ogg_crc32(const unsigned char * data, size_t size) {
  return gaud_ogg_crc32_update(0, data, size);
}

/* ---------------------------------------------------------------- reader */

void gaud_ogg_reader_init(OGG_Reader * reader, GAUD_Stream * stream,
    const GAUD_Allocator * allocator) {
  memset(reader, 0, sizeof(*reader));
  reader->stream = stream;
  reader->allocator = allocator;
  reader->granule = UINT64_MAX;
  reader->next_page = gaud_stream_tell(stream);
}

void gaud_ogg_reader_free(OGG_Reader * reader) {
  gcu_allocator_free(reader->allocator, reader->page);
  gcu_allocator_free(reader->allocator, reader->packet);
  reader->page = NULL;
  reader->packet = NULL;
  reader->page_capacity = 0;
  reader->packet_capacity = 0;
}

/** Make sure @p *buffer holds @p want bytes, keeping @p keep of them. */
static bool reserve(const GAUD_Allocator * allocator, unsigned char ** buffer,
    size_t * capacity, size_t want, size_t keep) {
  if (*capacity >= want) {
    return true;
  }
  size_t grown_size = *capacity ? *capacity : 4096u;
  while (grown_size < want) {
    grown_size *= 2u;
  }
  unsigned char * grown = gcu_allocator_malloc(allocator, grown_size);
  if (!grown) {
    return false;
  }
  if (*buffer && keep) {
    memcpy(grown, *buffer, keep);
  }
  gcu_allocator_free(allocator, *buffer);
  *buffer = grown;
  *capacity = grown_size;
  return true;
}

/**
 * Read one page into the reader's buffer and check it.
 *
 * @return ::GAUD_ERR_FORMAT at a clean end of stream.
 */
static GAUD_Result read_page(OGG_Reader * reader) {
  unsigned char header[OGG_HEADER_FIXED];
  uint64_t began_at = reader->next_page;
  if (gaud_stream_tell(reader->stream) != began_at
      && gaud_stream_seek(reader->stream, (int64_t)began_at, GAUD_SEEK_SET)
          != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  size_t got = gaud_stream_read(reader->stream, header, sizeof(header));
  if (got == 0) {
    return GAUD_ERR_FORMAT;
  }
  if (got != sizeof(header)) {
    return GAUD_ERR_CORRUPT;
  }
  if (memcmp(header, OGG_MAGIC, 4) != 0) {
    return GAUD_ERR_CORRUPT;
  }
  if (header[4] != 0) {
    /* Version. Only zero has ever been defined, and a file claiming
     * another is one whose framing this does not know. */
    return GAUD_ERR_UNSUPPORTED;
  }
  uint32_t segments = header[26];

  size_t want = OGG_HEADER_FIXED + segments;
  if (!reserve(reader->allocator, &reader->page, &reader->page_capacity,
          want + OGG_MAX_BODY, 0)) {
    return GAUD_ERR_OOM;
  }
  memcpy(reader->page, header, sizeof(header));
  if (segments
      && gaud_stream_read(reader->stream, reader->page + OGG_HEADER_FIXED,
             segments)
          != segments) {
    return GAUD_ERR_CORRUPT;
  }
  memcpy(reader->table, reader->page + OGG_HEADER_FIXED, segments);

  size_t body = 0;
  for (uint32_t i = 0; i < segments; ++i) {
    body += reader->table[i];
  }
  if (body
      && gaud_stream_read(reader->stream, reader->page + want, body) != body) {
    return GAUD_ERR_CORRUPT;
  }

  /* The checksum covers the whole page with its own field zeroed, so the
   * four bytes are taken out, the CRC computed, and the bytes put back -
   * the page buffer is read again below and must not be left altered. */
  uint32_t stated = gaud_rd_u32le(reader->page + 22);
  memset(reader->page + 22, 0, 4);
  uint32_t actual = gaud_ogg_crc32(reader->page, want + body);
  gaud_wr_u32le(reader->page + 22, stated);
  if (actual != stated) {
    return GAUD_ERR_CORRUPT;
  }

  uint32_t serial = gaud_rd_u32le(reader->page + 14);
  if (!reader->have_serial) {
    reader->serial = serial;
    reader->have_serial = true;
  }

  reader->flags = reader->page[5];
  reader->granule = gaud_rd_u64le(reader->page + 6);
  reader->segments = segments;
  reader->next_segment = 0;
  reader->body_at = want;
  reader->body_size = body;
  reader->next_body = 0;
  reader->page_offset = began_at;
  reader->next_page = began_at + want + body;
  reader->page_live = true;
  reader->eos = (reader->flags & OGG_FLAG_EOS) != 0;
  if (serial == reader->serial && reader->drop_continued) {
    /*
     * This page was landed on by a seek, and its first packet may have
     * begun on a page the reader never read. Throw that fragment away:
     * its segments are consumed here so that packet assembly below starts
     * at a packet boundary.
     *
     * The flag survives a page that does not finish the fragment, because
     * a packet may span three pages and a seek may land in the middle one.
     * A page whose whole table is 255s ends no packet, and the next page's
     * CONTINUED flag is the same fragment still arriving.
     */
    if ((reader->flags & OGG_FLAG_CONTINUED) == 0) {
      reader->drop_continued = false;
    }
    else {
      while (reader->next_segment < reader->segments) {
        unsigned value = reader->table[reader->next_segment++];
        reader->next_body += value;
        if (value < 255u) {
          reader->drop_continued = false;
          break;
        }
      }
    }
  }
  if (serial != reader->serial) {
    /* Another logical stream's page. Skipped rather than refused: an Ogg
     * file is allowed to multiplex, and a FLAC stream inside one is still
     * a FLAC stream. */
    reader->segments = 0;
    reader->body_size = 0;
    reader->eos = false;
  }
  return GAUD_OK;
}

GAUD_Result gaud_ogg_reader_packet(OGG_Reader * reader,
    const unsigned char ** out_data, size_t * out_size,
    uint64_t * out_granule, uint32_t * out_flags) {
  reader->packet_size = 0;
  for (;;) {
    if (!reader->page_live || reader->next_segment >= reader->segments) {
      if (reader->page_live && reader->eos) {
        return GAUD_ERR_FORMAT;
      }
      GAUD_Result result = read_page(reader);
      if (result != GAUD_OK) {
        return result;
      }
      continue;
    }
    /* Consume lacing values until one below 255 ends the packet. */
    bool complete = false;
    size_t take = 0;
    while (reader->next_segment < reader->segments) {
      unsigned value = reader->table[reader->next_segment++];
      take += value;
      if (value < 255u) {
        complete = true;
        break;
      }
    }
    if (reader->next_body + take > reader->body_size) {
      return GAUD_ERR_CORRUPT;
    }
    if (!reserve(reader->allocator, &reader->packet, &reader->packet_capacity,
            reader->packet_size + take + 1u, reader->packet_size)) {
      return GAUD_ERR_OOM;
    }
    memcpy(reader->packet + reader->packet_size,
        reader->page + reader->body_at + reader->next_body, take);
    reader->packet_size += take;
    reader->next_body += take;
    if (!complete) {
      /* The packet runs into the next page. */
      continue;
    }
    *out_data = reader->packet;
    *out_size = reader->packet_size;
    if (out_granule) {
      *out_granule = reader->granule;
    }
    if (out_flags) {
      *out_flags = reader->flags;
    }
    return GAUD_OK;
  }
}

GAUD_Result gaud_ogg_reader_seek(OGG_Reader * reader, uint64_t offset) {
  if (gaud_stream_seek(reader->stream, (int64_t)offset, GAUD_SEEK_SET)
      != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  reader->next_page = offset;
  reader->page_live = false;
  reader->segments = 0;
  reader->next_segment = 0;
  reader->body_size = 0;
  reader->next_body = 0;
  reader->packet_size = 0;
  reader->eos = false;
  reader->drop_continued = true;
  return GAUD_OK;
}

/* ---------------------------------------------------------------- writer */

void gaud_ogg_writer_init(
    OGG_Writer * writer, GAUD_Stream * stream, uint32_t serial) {
  memset(writer, 0, sizeof(*writer));
  writer->stream = stream;
  writer->serial = serial;
}

/**
 * Emit the buffered segments as one page.
 *
 * @param complete Whether the last segment on it ended a packet, which
 *   decides both the granule position and whether the next page is marked
 *   as a continuation. See OGG_Writer::page_complete.
 */
static GAUD_Result emit_page(OGG_Writer * writer, bool eos, bool complete) {
  unsigned char header[OGG_HEADER_FIXED + OGG_MAX_SEGMENTS];
  memcpy(header, OGG_MAGIC, 4);
  header[4] = 0;
  header[5] = (unsigned char)((writer->continued ? OGG_FLAG_CONTINUED : 0)
      | (!writer->started ? OGG_FLAG_BOS : 0) | (eos ? OGG_FLAG_EOS : 0));
  /* A page that does not finish a packet states no granule position, which
   * Ogg spells as all ones. Stamping the packet's eventual position on a
   * page that stops in the middle of it would tell a seeker that samples
   * are available that are not. */
  uint64_t granule = complete ? writer->granule : UINT64_MAX;
  gaud_wr_u32le(header + 6, (uint32_t)granule);
  gaud_wr_u32le(header + 10, (uint32_t)(granule >> 32));
  gaud_wr_u32le(header + 14, writer->serial);
  gaud_wr_u32le(header + 18, writer->sequence);
  memset(header + 22, 0, 4);
  header[26] = (unsigned char)writer->segments;
  memcpy(header + OGG_HEADER_FIXED, writer->table, writer->segments);
  size_t header_size = OGG_HEADER_FIXED + writer->segments;

  /* The checksum is over header and body together with its own field
   * zeroed, and the two live in separate buffers - so the CRC is carried
   * across them rather than joining them into a 64 KiB copy on the stack.
   * The field is filled in afterwards and the header written then. */
  uint32_t crc = gaud_ogg_crc32_update(0, header, header_size);
  crc = gaud_ogg_crc32_update(crc, writer->body, writer->body_size);
  gaud_wr_u32le(header + 22, crc);

  GAUD_Result result = gaud_stream_write(writer->stream, header, header_size);
  if (result == GAUD_OK && writer->body_size) {
    result = gaud_stream_write(writer->stream, writer->body,
        writer->body_size);
  }
  writer->body_size = 0;
  writer->segments = 0;
  writer->page_complete = false;
  ++writer->sequence;
  writer->started = true;
  writer->continued = !complete;
  return result;
}

GAUD_Result gaud_ogg_writer_packet(OGG_Writer * writer,
    const unsigned char * data, size_t size, uint64_t granule, bool flush,
    bool eos) {
  writer->granule = granule;
  size_t at = 0;
  bool ended = false;
  while (!ended) {
    if (writer->segments == OGG_MAX_SEGMENTS) {
      /* No room for another lacing value. Whether this page finished a
       * packet is page_complete's question and not "are we mid-packet": a
       * table that filled exactly on a packet boundary ends a packet, and
       * granule is this packet's only when it does not. */
      GAUD_Result result = emit_page(writer, false, writer->page_complete);
      if (result != GAUD_OK) {
        return result;
      }
    }
    size_t remaining = size - at;
    unsigned value = remaining >= 255u ? 255u : (unsigned)remaining;
    writer->table[writer->segments++] = (unsigned char)value;
    if (value) {
      memcpy(writer->body + writer->body_size, data + at, value);
      writer->body_size += value;
      at += value;
    }
    /* A lacing value below 255 is what ends a packet, so a packet whose
     * length is an exact multiple of 255 ends with an explicit zero.
     * Omitting that zero is the classic Ogg writer defect: the reader
     * then joins the packet to the one after it and both are lost. */
    if (value < 255u) {
      ended = true;
      writer->page_complete = true;
    }
  }
  if (flush || eos) {
    return emit_page(writer, eos, true);
  }
  return GAUD_OK;
}

GAUD_Result gaud_ogg_writer_flush(OGG_Writer * writer, bool eos) {
  if (writer->segments == 0 && !eos) {
    return GAUD_OK;
  }
  /* A page with nothing on it finishes no packet and so states no position
   * - except that an empty end-of-stream page is the last place left to
   * state the stream's length, and a reader learns a length from the last
   * stated position. So that one page keeps the granule. */
  return emit_page(
      writer, eos, writer->segments == 0 || writer->page_complete);
}
