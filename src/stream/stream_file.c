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
 * Streams over files.
 */

#include "stream_internal.h"
#include <errno.h>
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

static size_t file_read(GAUD_Stream * stream, void * out, size_t count) {
  size_t n = fread(out, 1, count, stream->file);
  stream->position += n;
  /* feof is only set after a read that hit the end, so a read that exactly
   * consumed the last byte leaves it clear. Comparing against the length
   * makes this agree with the memory stream, which matters because a codec
   * looping "while not eof" must behave the same on both. */
  if (n < count) {
    stream->eof = true;
  }
  else {
    uint64_t size = 0;
    if (stream->vtable->size && stream->vtable->size(stream, &size) == GAUD_OK) {
      stream->eof = stream->position >= size;
    }
  }
  return n;
}

static GAUD_Result file_write(
    GAUD_Stream * stream, const void * bytes, size_t count) {
  if (fwrite(bytes, 1, count, stream->file) != count) {
    return GAUD_ERR_IO;
  }
  stream->position += count;
  if (stream->position > stream->owned_length) {
    stream->owned_length = (size_t)stream->position; /* high-water mark */
  }
  return GAUD_OK;
}

static GAUD_Result file_length(const GAUD_Stream * stream, uint64_t * out) {
  long here = ftell(stream->file);
  if (here < 0 || fseek(stream->file, 0, SEEK_END) != 0) {
    return GAUD_ERR_IO;
  }
  long end = ftell(stream->file);
  if (end < 0 || fseek(stream->file, here, SEEK_SET) != 0) {
    return GAUD_ERR_IO;
  }
  *out = (uint64_t)end;
  return GAUD_OK;
}

static GAUD_Result file_seek(
    GAUD_Stream * stream, int64_t offset, GAUD_Seek_Origin origin) {
  uint64_t length = 0;
  GAUD_Result result = file_length(stream, &length);
  if (result != GAUD_OK) {
    return result;
  }
  /* Bounds-check against the file's length first, so an out-of-range seek is
   * refused identically to the memory stream's rather than being whatever
   * fseek happens to permit - seeking past the end of a file is legal in C
   * and produces a hole on the next write. */
  result = gaud_stream_seek_within(stream, offset, origin, length);
  if (result != GAUD_OK) {
    return result;
  }
  if (fseek(stream->file, (long)stream->position, SEEK_SET) != 0) {
    return GAUD_ERR_IO;
  }
  return GAUD_OK;
}

static GAUD_Result file_size(const GAUD_Stream * stream, uint64_t * out_size) {
  return file_length(stream, out_size);
}

static void file_destroy(GAUD_Stream * stream) {
  if (stream->file) {
    fclose(stream->file);
  }
  gcu_allocator_free(stream->allocator, stream);
}

static const GAUD_Stream_Vtable file_read_vtable = {
    .read = file_read,
    .write = NULL,
    .seek = file_seek,
    .size = file_size,
    .destroy = file_destroy,
};

static const GAUD_Stream_Vtable file_write_vtable = {
    .read = file_read,
    .write = file_write,
    .seek = file_seek,
    .size = file_size,
    .destroy = file_destroy,
};

static GAUD_Result file_open(const GAUD_Allocator * allocator,
    const char * path, const char * mode, bool writable,
    GAUD_Stream ** out_stream) {
  if (!path || !out_stream) {
    return GAUD_ERR_INVALID;
  }
  FILE * file = fopen(path, mode);
  if (!file) {
    return GAUD_ERR_IO;
  }
  GAUD_Stream * stream = gcu_allocator_malloc(allocator, sizeof(GAUD_Stream));
  if (!stream) {
    fclose(file);
    return GAUD_ERR_OOM;
  }
  *stream = (GAUD_Stream){
      .vtable = writable ? &file_write_vtable : &file_read_vtable,
      .allocator = allocator,
      .seekable = true,
      .writable = writable,
      .eof = false,
      .file = file,
  };
  *out_stream = stream;
  return GAUD_OK;
}

GAUD_Result gaud_stream_create_file_with_allocator(
    const GAUD_Allocator * allocator, const char * path,
    GAUD_Stream ** out_stream) {
  /* "rb" and not "r": on Windows a text-mode read translates CRLF, which
   * would corrupt every audio file it touched. */
  return file_open(allocator, path, "rb", false, out_stream);
}

GAUD_Result gaud_stream_create_file(
    const char * path, GAUD_Stream ** out_stream) {
  return gaud_stream_create_file_with_allocator(NULL, path, out_stream);
}

GAUD_Result gaud_stream_create_file_writer(const GAUD_Allocator * allocator,
    const char * path, GAUD_Stream ** out_stream) {
  /* "w+b": written, truncated, and readable, because finishing a RIFF file
   * seeks back to patch a length it did not know when it wrote the header. */
  return file_open(allocator, path, "w+b", true, out_stream);
}
