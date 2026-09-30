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
 * A read-only stream over bytes the caller owns.
 */

#include "stream_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

static size_t memory_read(GAUD_Stream * stream, void * out, size_t count) {
  size_t remaining = stream->length - (size_t)stream->position;
  size_t n = count < remaining ? count : remaining;
  if (n) {
    memcpy(out, stream->bytes + stream->position, n);
    stream->position += n;
  }
  /* Set on reaching the end, not on passing it: a read that consumed the
   * last byte has reached the end, and a caller looping "while not eof"
   * should not need one more short read to discover that. */
  stream->eof = stream->position >= stream->length;
  return n;
}

GAUD_Result gaud_stream_seek_within(
    GAUD_Stream * stream, int64_t offset, GAUD_Seek_Origin origin,
    uint64_t length) {
  /* Computed in int64_t against a length held as a size_t, so a negative
   * offset past the start is caught rather than wrapping into an enormous
   * unsigned position. */
  int64_t base;
  switch (origin) {
  case GAUD_SEEK_SET: base = 0; break;
  case GAUD_SEEK_CUR: base = (int64_t)stream->position; break;
  case GAUD_SEEK_END: base = (int64_t)length; break;
  default: return GAUD_ERR_INVALID;
  }
  if (offset > 0 && base > INT64_MAX - offset) {
    return GAUD_ERR_INVALID;
  }
  int64_t target = base + offset;
  if (target < 0 || (uint64_t)target > length) {
    return GAUD_ERR_INVALID;
  }
  stream->position = (uint64_t)target;
  /* Seeking away from the end clears it. Without this a stream read to the
   * end and rewound still reports eof, and the second of two passes over a
   * seekable stream ends immediately. */
  stream->eof = stream->position >= length;
  return GAUD_OK;
}

static GAUD_Result memory_seek(
    GAUD_Stream * stream, int64_t offset, GAUD_Seek_Origin origin) {
  return gaud_stream_seek_within(stream, offset, origin, stream->length);
}

static GAUD_Result memory_size(
    const GAUD_Stream * stream, uint64_t * out_size) {
  *out_size = (uint64_t)stream->length;
  return GAUD_OK;
}

static void memory_destroy(GAUD_Stream * stream) {
  /* The bytes are the caller's; only the handle is ours. */
  gcu_allocator_free(stream->allocator, stream);
}

static const GAUD_Stream_Vtable memory_vtable = {
    .read = memory_read,
    .write = NULL,
    .seek = memory_seek,
    .size = memory_size,
    .destroy = memory_destroy,
};

GAUD_Result gaud_stream_create_memory_with_allocator(
    const GAUD_Allocator * allocator, const void * bytes, size_t length,
    GAUD_Stream ** out_stream) {
  if (!out_stream || (!bytes && length)) {
    return GAUD_ERR_INVALID;
  }
  GAUD_Stream * stream = gcu_allocator_malloc(allocator, sizeof(GAUD_Stream));
  if (!stream) {
    return GAUD_ERR_OOM;
  }
  *stream = (GAUD_Stream){
      .vtable = &memory_vtable,
      .allocator = allocator,
      .seekable = true,
      .writable = false,
      .eof = length == 0,
      .position = 0,
      .bytes = (const unsigned char *)bytes,
      .length = length,
  };
  *out_stream = stream;
  return GAUD_OK;
}

GAUD_Result gaud_stream_create_memory(
    const void * bytes, size_t length, GAUD_Stream ** out_stream) {
  return gaud_stream_create_memory_with_allocator(
      NULL, bytes, length, out_stream);
}
