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
 * The accessors every stream shares, over the vtable each one fills in.
 */

#include "stream_internal.h"
/* For gaud_stream_allocator's GAUD_API declaration; see buffer.c. */
#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/cutil/allocator.h>

size_t gaud_stream_read(GAUD_Stream * stream, void * out, size_t count) {
  if (!stream || !out || !count || !stream->vtable->read) {
    return 0;
  }
  return stream->vtable->read(stream, out, count);
}

GAUD_Result gaud_stream_write(
    GAUD_Stream * stream, const void * bytes, size_t count) {
  if (!stream || (!bytes && count)) {
    return GAUD_ERR_INVALID;
  }
  if (!stream->writable || !stream->vtable->write) {
    return GAUD_ERR_INVALID;
  }
  if (count == 0) {
    return GAUD_OK;
  }
  return stream->vtable->write(stream, bytes, count);
}

GAUD_Result gaud_stream_seek(
    GAUD_Stream * stream, int64_t offset, GAUD_Seek_Origin origin) {
  if (!stream) {
    return GAUD_ERR_INVALID;
  }
  if (!stream->seekable || !stream->vtable->seek) {
    return GAUD_ERR_UNSUPPORTED;
  }
  return stream->vtable->seek(stream, offset, origin);
}

uint64_t gaud_stream_tell(const GAUD_Stream * stream) {
  return stream ? stream->position : (uint64_t)-1;
}

GAUD_Result gaud_stream_size(const GAUD_Stream * stream, uint64_t * out_size) {
  if (!stream || !out_size) {
    return GAUD_ERR_INVALID;
  }
  if (!stream->vtable->size) {
    return GAUD_ERR_UNSUPPORTED;
  }
  return stream->vtable->size(stream, out_size);
}

bool gaud_stream_eof(const GAUD_Stream * stream) {
  return stream ? stream->eof : true;
}

bool gaud_stream_seekable(const GAUD_Stream * stream) {
  return stream ? stream->seekable : false;
}

bool gaud_stream_writable(const GAUD_Stream * stream) {
  return stream ? stream->writable : false;
}

void gaud_stream_destroy(GAUD_Stream * stream) {
  if (stream) {
    stream->vtable->destroy(stream);
  }
}

const GAUD_Allocator * gaud_stream_allocator(const GAUD_Stream * stream) {
  return stream ? stream->allocator : NULL;
}
