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
 * Finding pages without reading the ones in between: the scan, the list of
 * logical streams, the last granule position, and the bisection.
 *
 * Separate from ogg.c because the two halves answer different questions.
 * ogg.c walks a stream forwards and knows about packets; this walks a file
 * by byte offset and knows only about pages. Nothing here touches the
 * reader's packet assembly, and that is deliberate - a bisection that left
 * a reader half way through a packet would be a very quiet bug.
 *
 * **Why a page is identified by its checksum.** Ogg's designers intended
 * the format to be bisected and said so, and the mechanism they provided
 * is the page checksum: `OggS` in a body is not improbable, it is expected,
 * because a page body is compressed audio and four particular bytes turn up
 * about once in every four gigabytes of it by chance and rather more often
 * than that in practice. A scan that accepted the magic would find pages
 * that are not pages, read a nonsense segment table, and land the caller
 * somewhere arbitrary. So every candidate here is read in full and believed
 * only if the CRC over it agrees, which costs a read per false positive and
 * leaves a one-in-four-billion residue.
 *
 * **Why this is not in flac_ogg.c, where the need first arose.** Phase 4
 * wrote FLAC's Ogg seek as a linear decode from the start of the audio and
 * left a comment saying the bisection belonged here, with Vorbis and Opus,
 * where it could be written once and scored against three codecs' worth of
 * fixtures rather than one. That is what this is.
 */

#include "../../codec/shared/bytes.h"
#include "ogg.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/**
 * How much is read at a time while looking for the magic.
 *
 * Bigger than a page so that the common case - the next page starts within
 * a few bytes of where the scan began - is one read, and small enough that
 * a backwards scan from the end of a file does not pull in a megabyte to
 * answer a question about its last 300 bytes.
 */
#define SCAN_WINDOW 65536u

/**
 * Read a candidate page at @p offset and decide whether it is one.
 *
 * @param scratch At least ::OGG_MAX_PAGE bytes.
 * @return ::GAUD_OK if a page is there; ::GAUD_ERR_FORMAT if not, which is
 *   an ordinary answer and not a complaint about the file.
 */
static GAUD_Result page_at(GAUD_Stream * stream, unsigned char * scratch,
    uint64_t offset, OGG_Page_Info * out) {
  if (gaud_stream_seek(stream, (int64_t)offset, GAUD_SEEK_SET) != GAUD_OK) {
    return GAUD_ERR_FORMAT;
  }
  if (gaud_stream_read(stream, scratch, OGG_HEADER_FIXED) != OGG_HEADER_FIXED) {
    return GAUD_ERR_FORMAT;
  }
  if (memcmp(scratch, OGG_MAGIC, 4) != 0 || scratch[4] != 0) {
    return GAUD_ERR_FORMAT;
  }
  uint32_t segments = scratch[26];
  size_t header_size = OGG_HEADER_FIXED + segments;
  if (segments
      && gaud_stream_read(stream, scratch + OGG_HEADER_FIXED, segments)
          != segments) {
    return GAUD_ERR_FORMAT;
  }
  size_t body = 0;
  for (uint32_t i = 0; i < segments; ++i) {
    body += scratch[OGG_HEADER_FIXED + i];
  }
  if (body && gaud_stream_read(stream, scratch + header_size, body) != body) {
    return GAUD_ERR_FORMAT;
  }

  /* The field is zeroed for the computation and not put back, because this
   * buffer is scratch and the bytes are re-read if they are wanted again.
   * ogg.c's reader does put them back, because its buffer is the page it
   * then hands out. */
  uint32_t stated = gaud_rd_u32le(scratch + 22);
  memset(scratch + 22, 0, 4);
  if (gaud_ogg_crc32(scratch, header_size + body) != stated) {
    return GAUD_ERR_FORMAT;
  }

  out->offset = offset;
  out->next = offset + header_size + body;
  out->granule = gaud_rd_u64le(scratch + 6);
  out->serial = gaud_rd_u32le(scratch + 14);
  out->flags = scratch[5];
  out->segments = segments;
  out->body = (uint32_t)body;
  return GAUD_OK;
}

GAUD_Result gaud_ogg_find_page(GAUD_Stream * stream,
    const GAUD_Allocator * allocator, uint64_t from, uint64_t limit,
    OGG_Page_Info * out) {
  if (!gaud_stream_seekable(stream)) {
    return GAUD_ERR_UNSUPPORTED;
  }
  unsigned char * scratch = gcu_allocator_malloc(allocator, OGG_MAX_PAGE);
  unsigned char * window = gcu_allocator_malloc(allocator, SCAN_WINDOW);
  if (!scratch || !window) {
    gcu_allocator_free(allocator, scratch);
    gcu_allocator_free(allocator, window);
    return GAUD_ERR_OOM;
  }

  GAUD_Result result = GAUD_ERR_FORMAT;
  uint64_t at = from;
  while (at < limit) {
    if (gaud_stream_seek(stream, (int64_t)at, GAUD_SEEK_SET) != GAUD_OK) {
      break;
    }
    size_t got = gaud_stream_read(stream, window, SCAN_WINDOW);
    if (got < 4) {
      break;
    }
    /* The last three bytes of a window may be the first three of a magic,
     * so the next window starts three bytes before this one ended. Without
     * that overlap a page is invisible about once per 16,384 pages, which
     * is exactly often enough to be reported as "seeking sometimes fails
     * on large files". */
    size_t searchable = got - 3u;
    bool stop = false;
    for (size_t i = 0; i < searchable; ++i) {
      if (window[i] != 'O' || memcmp(window + i, OGG_MAGIC, 4) != 0) {
        continue;
      }
      uint64_t candidate = at + i;
      if (candidate >= limit) {
        stop = true; /* Past the ceiling: no page may begin here. */
        break;
      }
      if (page_at(stream, scratch, candidate, out) == GAUD_OK) {
        result = GAUD_OK;
        stop = true;
        break;
      }
      /* A false magic. The scan continues from the byte after it rather
       * than from four bytes on, because overlapping magics are possible
       * in principle and skipping them would be a reason for a page to be
       * missed that no test would ever produce. */
    }
    if (stop) {
      break;
    }
    at += searchable;
  }

  gcu_allocator_free(allocator, scratch);
  gcu_allocator_free(allocator, window);
  if (result == GAUD_OK) {
    (void)gaud_stream_seek(stream, (int64_t)out->next, GAUD_SEEK_SET);
  }
  return result;
}

GAUD_Result gaud_ogg_scan_logical(GAUD_Stream * stream,
    const GAUD_Allocator * allocator, OGG_Logical * out, size_t capacity,
    size_t * out_count) {
  *out_count = 0;
  uint64_t size = 0;
  if (gaud_stream_size(stream, &size) != GAUD_OK) {
    return GAUD_ERR_UNSUPPORTED;
  }

  uint64_t at = 0;
  for (;;) {
    OGG_Page_Info page;
    /* A BOS page is at the head of the file by RFC 3533's own requirement,
     * so the scan does not range over the whole file looking for one: it
     * walks page to page and stops at the first page that is not a
     * beginning. A file that violated that would have its later streams
     * missed, which is the right failure - a reader that went hunting
     * would find the BOS pages of a *chained* stream further on and report
     * them as though they were multiplexed alongside the first. */
    if (gaud_ogg_find_page(stream, allocator, at, size, &page) != GAUD_OK) {
      break;
    }
    if ((page.flags & OGG_FLAG_BOS) == 0) {
      break;
    }
    if (*out_count < capacity) {
      OGG_Logical * entry = &out[*out_count];
      entry->serial = page.serial;
      entry->offset = page.offset;
      entry->head_size = 0;
      memset(entry->head, 0, sizeof(entry->head));
      size_t want = page.body < OGG_HEAD_KEPT ? page.body : OGG_HEAD_KEPT;
      if (want) {
        uint64_t body_at = page.offset + OGG_HEADER_FIXED + page.segments;
        if (gaud_stream_seek(stream, (int64_t)body_at, GAUD_SEEK_SET)
            == GAUD_OK) {
          entry->head_size = gaud_stream_read(stream, entry->head, want);
        }
      }
      ++*out_count;
    }
    at = page.next;
  }
  return *out_count ? GAUD_OK : GAUD_ERR_FORMAT;
}

GAUD_Result gaud_ogg_last_granule(GAUD_Stream * stream,
    const GAUD_Allocator * allocator, uint32_t serial,
    uint64_t * out_granule) {
  uint64_t size = 0;
  if (gaud_stream_size(stream, &size) != GAUD_OK) {
    return GAUD_ERR_UNSUPPORTED;
  }

  /*
   * Widening windows from the end, rather than one pass over the file.
   *
   * The last page of the file need not be the last page of this logical
   * stream: a multiplexed file's streams do not have to end together, and
   * the stream asked about may finish long before the file does. So a
   * window that finds nothing is widened and the search repeated, and only
   * a window covering the whole file may answer "nowhere".
   *
   * Each pass scans forward to the end of the file rather than to the
   * start of the previous window, because what is wanted is the *last*
   * granule position and a page found early in a pass may be followed by
   * a later one. That makes a pass that fails cost its whole span; the
   * first span is 64 KiB and the first pass succeeds for every file any
   * encoder writes, so the repetition is the unusual path and not the
   * normal one.
   */
  for (uint64_t span = SCAN_WINDOW;; span *= 2u) {
    uint64_t begin = size > span ? size - span : 0;
    uint64_t at = begin;
    bool any = false;
    uint64_t best = OGG_NO_GRANULE;
    for (;;) {
      OGG_Page_Info page;
      if (gaud_ogg_find_page(stream, allocator, at, size, &page) != GAUD_OK) {
        break;
      }
      if (page.serial == serial && page.granule != OGG_NO_GRANULE) {
        best = page.granule;
        any = true;
      }
      at = page.next;
    }
    if (any) {
      *out_granule = best;
      return GAUD_OK;
    }
    if (begin == 0) {
      return GAUD_ERR_FORMAT;
    }
    if (span > size) {
      span = size; /* The next doubling covers the file; do not overflow. */
    }
  }
}

/**
 * The first page of @p serial beginning in [@p from, @p limit) that states
 * a granule position.
 *
 * A page with no granule position is no use to a bisection - it says
 * nothing about where it is in the stream - and neither is another
 * stream's page, so both are skipped rather than ending the search.
 */
static GAUD_Result granule_page(GAUD_Stream * stream,
    const GAUD_Allocator * allocator, uint32_t serial, uint64_t from,
    uint64_t limit, OGG_Page_Info * out) {
  uint64_t at = from;
  while (at < limit) {
    if (gaud_ogg_find_page(stream, allocator, at, limit, out) != GAUD_OK) {
      return GAUD_ERR_FORMAT;
    }
    if (out->serial == serial && out->granule != OGG_NO_GRANULE) {
      return GAUD_OK;
    }
    at = out->next > at ? out->next : at + 1u;
  }
  return GAUD_ERR_FORMAT;
}

GAUD_Result gaud_ogg_bisect(OGG_Reader * reader, uint64_t target,
    uint64_t floor, uint64_t * out_offset, uint64_t * out_granule) {
  uint64_t size = 0;
  if (gaud_stream_size(reader->stream, &size) != GAUD_OK) {
    return GAUD_ERR_UNSUPPORTED;
  }
  if (!reader->have_serial) {
    return GAUD_ERR_FORMAT;
  }

  /* The answer when nothing better is found: start at the first audio page
   * with nothing behind it. This is the whole of the "wrong near the ends"
   * problem that the phase-4 comment named - a bisection whose bounds
   * never move must still return a usable place to start, and the only
   * place that is always usable is the beginning. */
  uint64_t best = floor;
  uint64_t best_granule = 0;

  uint64_t begin = floor;
  uint64_t end = size;
  while (begin < end) {
    /*
     * **The guess is the lower bound once the window is small**, and that
     * is not an optimisation - it is the far end of the "wrong near the
     * ends" problem.
     *
     * A midpoint guess is always strictly above `begin`, and the scan from
     * it looks *forward*. So a page beginning at exactly `begin` is never
     * probed, however small the window gets, and the answer for a target
     * inside the last page of a file is the page before it. The symptom is
     * a seek to the end of a file landing one page short - which decodes
     * the right samples, just not all of them, and so is invisible to any
     * test that does not assert the count.
     *
     * Below a couple of pages the search becomes a forward walk, which is
     * what a bisection's last few steps are worth anyway: the window is
     * then bounded by 2 * OGG_MAX_PAGE, so the walk reads at most about
     * 130 KiB however large the file is.
     */
    uint64_t guess = end - begin > 2u * (uint64_t)OGG_MAX_PAGE
        ? begin + (end - begin) / 2u
        : begin;
    OGG_Page_Info page;
    if (granule_page(reader->stream, reader->allocator, reader->serial, guess,
            end, &page)
        != GAUD_OK) {
      /* No page of this stream states a position anywhere above the guess
       * and below the ceiling, so the ceiling comes down to the guess. */
      end = guess;
      continue;
    }
    if (page.granule > target) {
      if (page.offset <= begin) {
        break; /* Everything from here on is too far; the answer is best. */
      }
      end = page.offset;
    }
    else {
      if (page.next <= begin) {
        break; /* No progress possible; stop rather than spin. */
      }
      begin = page.next;
      best = page.next;
      best_granule = page.granule;
    }
  }

  *out_offset = best;
  *out_granule = best_granule;
  return GAUD_OK;
}
