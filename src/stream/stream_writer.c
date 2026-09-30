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
 * A growable in-memory stream that can be written to and read back.
 */

#include "stream_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/* Grow to hold at least `needed` bytes, doubling. */
static GAUD_Result writer_reserve(GAUD_Stream * stream, size_t needed) {
  if (needed <= stream->owned_capacity) {
    return GAUD_OK;
  }
  size_t next = stream->owned_capacity ? stream->owned_capacity : 256;
  while (next < needed) {
    if (next > SIZE_MAX / 2) {
      return GAUD_ERR_OOM;
    }
    next *= 2;
  }
  unsigned char * grown
      = gcu_allocator_realloc(stream->allocator, stream->owned, next);
  if (!grown) {
    return GAUD_ERR_OOM;
  }
  /* Zero the new space. A writer that seeks past the end and writes leaves a
   * gap, and a gap of uninitialised bytes would make the output depend on
   * the heap - which a byte-for-byte round-trip test would catch as a
   * mysterious intermittent rather than as this. */
  memset(grown + stream->owned_capacity, 0, next - stream->owned_capacity);
  stream->owned = grown;
  stream->owned_capacity = next;
  return GAUD_OK;
}

static size_t writer_read(GAUD_Stream * stream, void * out, size_t count) {
  size_t remaining = stream->owned_length - (size_t)stream->position;
  size_t n = count < remaining ? count : remaining;
  if (n) {
    memcpy(out, stream->owned + stream->position, n);
    stream->position += n;
  }
  stream->eof = stream->position >= stream->owned_length;
  return n;
}

static GAUD_Result writer_write(
    GAUD_Stream * stream, const void * bytes, size_t count) {
  if (stream->position > SIZE_MAX - count) {
    return GAUD_ERR_OOM;
  }
  size_t end = (size_t)stream->position + count;
  GAUD_Result result = writer_reserve(stream, end);
  if (result != GAUD_OK) {
    return result;
  }
  memcpy(stream->owned + stream->position, bytes, count);
  stream->position = end;
  /* Overwriting the middle does not shorten the stream: patching a header
   * whose length was unknown at the time is exactly what this supports, and
   * a write that truncated would destroy everything after it. */
  if (end > stream->owned_length) {
    stream->owned_length = end;
  }
  return GAUD_OK;
}

static GAUD_Result writer_seek(
    GAUD_Stream * stream, int64_t offset, GAUD_Seek_Origin origin) {
  return gaud_stream_seek_within(stream, offset, origin, stream->owned_length);
}

static GAUD_Result writer_size(
    const GAUD_Stream * stream, uint64_t * out_size) {
  *out_size = (uint64_t)stream->owned_length;
  return GAUD_OK;
}

static void writer_destroy(GAUD_Stream * stream) {
  gcu_allocator_free(stream->allocator, stream->owned);
  gcu_allocator_free(stream->allocator, stream);
}

static const GAUD_Stream_Vtable writer_vtable = {
    .read = writer_read,
    .write = writer_write,
    .seek = writer_seek,
    .size = writer_size,
    .destroy = writer_destroy,
};

GAUD_Result gaud_stream_create_memory_writer(
    const GAUD_Allocator * allocator, GAUD_Stream ** out_stream) {
  if (!out_stream) {
    return GAUD_ERR_INVALID;
  }
  GAUD_Stream * stream = gcu_allocator_malloc(allocator, sizeof(GAUD_Stream));
  if (!stream) {
    return GAUD_ERR_OOM;
  }
  *stream = (GAUD_Stream){
      .vtable = &writer_vtable,
      .allocator = allocator,
      .seekable = true,
      .writable = true,
      .eof = true,
  };
  *out_stream = stream;
  return GAUD_OK;
}

GAUD_Result gaud_stream_writer_bytes(
    const GAUD_Stream * stream, const void ** out_bytes, size_t * out_length) {
  if (!stream || !out_bytes || !out_length) {
    return GAUD_ERR_INVALID;
  }
  if (stream->vtable != &writer_vtable) {
    return GAUD_ERR_UNSUPPORTED;
  }
  *out_bytes = stream->owned;
  *out_length = stream->owned_length;
  return GAUD_OK;
}

/* ------------------------------------------------------------------------
 * The unseekable wrapper.
 *
 * Exists so the non-seekable path can be reached by a test. "A pipe is an
 * ordinary way to receive audio" describes code that never runs unless
 * something makes it run, and a gate that cannot reach a branch reports
 * success for it.
 * --------------------------------------------------------------------- */

static size_t unseekable_read(GAUD_Stream * stream, void * out, size_t count) {
  size_t n = gaud_stream_read(stream->source, out, count);
  stream->position += n;
  stream->eof = gaud_stream_eof(stream->source);
  return n;
}

static void unseekable_destroy(GAUD_Stream * stream) {
  /* The source is borrowed and stays the caller's. */
  gcu_allocator_free(stream->allocator, stream);
}

static const GAUD_Stream_Vtable unseekable_vtable = {
    .read = unseekable_read,
    .write = NULL,
    /* Both NULL deliberately. A pipe cannot seek and does not know its own
     * length, and a wrapper that passed the real length through would be
     * modelling a stream that exists nowhere. */
    .seek = NULL,
    .size = NULL,
    .destroy = unseekable_destroy,
};

GAUD_Result gaud_stream_create_unseekable(
    GAUD_Stream * source, GAUD_Stream ** out_stream) {
  if (!source || !out_stream) {
    return GAUD_ERR_INVALID;
  }
  GAUD_Stream * stream
      = gcu_allocator_malloc(source->allocator, sizeof(GAUD_Stream));
  if (!stream) {
    return GAUD_ERR_OOM;
  }
  *stream = (GAUD_Stream){
      .vtable = &unseekable_vtable,
      .allocator = source->allocator,
      .seekable = false,
      .writable = false,
      .eof = gaud_stream_eof(source),
      .source = source,
  };
  *out_stream = stream;
  return GAUD_OK;
}
