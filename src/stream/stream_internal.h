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
 * Declarations shared between the stream implementations. Never installed.
 */

#ifndef GHOTI_IO_GAUD_SRC_STREAM_STREAM_INTERNAL_H
#define GHOTI_IO_GAUD_SRC_STREAM_STREAM_INTERNAL_H

#include <ghoti.io/audio/allocator.h>
#include <ghoti.io/audio/macros.h>
#include <ghoti.io/audio/stream.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/**
 * @brief A stream's behaviour.
 *
 * A vtable rather than a kind tag, so that adding a stream is adding a file
 * rather than a case to every function.
 */
typedef struct {
  /** Read up to @p count bytes; returns how many. */
  size_t (*read)(GAUD_Stream * stream, void * out, size_t count);
  /** Write @p count bytes at the position. NULL when not writable. */
  GAUD_Result (*write)(
      GAUD_Stream * stream, const void * bytes, size_t count);
  /** Move the position. NULL when the stream cannot seek. */
  GAUD_Result (*seek)(
      GAUD_Stream * stream, int64_t offset, GAUD_Seek_Origin origin);
  /** Total length. NULL when it is not knowable. */
  GAUD_Result (*size)(const GAUD_Stream * stream, uint64_t * out_size);
  /** Release whatever the stream owns, including the handle. */
  void (*destroy)(GAUD_Stream * stream);
} GAUD_Stream_Vtable;

/** @brief The stream handle behind ::GAUD_Stream. */
struct GAUD_Stream {
  const GAUD_Stream_Vtable * vtable; ///< What kind of stream this is.
  const GAUD_Allocator * allocator;  ///< NULL means the default.
  bool seekable;                     ///< Whether seek() may be called.
  bool writable;                     ///< Whether write() may be called.
  bool eof;                          ///< Whether the end has been reached.
  uint64_t position;                 ///< The read/write position, in bytes.

  const unsigned char * bytes; ///< Read-only memory stream: borrowed.
  size_t length;               ///< ...and how many bytes.

  unsigned char * owned; ///< Memory writer: owned, grown as needed.
  size_t owned_length;   ///< ...bytes written.
  size_t owned_capacity; ///< ...bytes allocated.

  FILE * file; ///< File streams.

  GAUD_Stream * source; ///< The unseekable wrapper's target; borrowed.
};

/**
 * @brief The seek arithmetic, shared by every stream that can seek.
 *
 * Kept in one place because the part that is easy to get wrong - a negative
 * offset from the current position, and the overflow check before adding -
 * is the same for all of them, and two copies of it would be two chances to
 * wrap a signed offset into an enormous unsigned position.
 *
 * @param stream The stream whose position is being moved.
 * @param offset How far, signed.
 * @param origin Where from.
 * @param length The stream's length, which is what SEEK_END means and what
 *   bounds the result.
 */
GAUD_Result gaud_stream_seek_within(GAUD_Stream * stream, int64_t offset,
    GAUD_Seek_Origin origin, uint64_t length);

#endif // GHOTI_IO_GAUD_SRC_STREAM_STREAM_INTERNAL_H
