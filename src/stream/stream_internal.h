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

/**
 * @brief A stream's behaviour.
 *
 * A vtable so that the file stream phase 1 adds is a second implementation
 * rather than a flag every function has to branch on.
 */
typedef struct {
  /** Read up to @p count bytes; returns how many. */
  size_t (*read)(GAUD_Stream * stream, void * out, size_t count);
  /** Move the read position. Only called on a seekable stream. */
  GAUD_Result (*seek)(
      GAUD_Stream * stream, int64_t offset, GAUD_Seek_Origin origin);
  /** Total length, or GAUD_ERR_UNSUPPORTED where it is not knowable. */
  GAUD_Result (*size)(const GAUD_Stream * stream, uint64_t * out_size);
  /** Release whatever the stream owns, including the handle itself. */
  void (*destroy)(GAUD_Stream * stream);
} GAUD_Stream_Vtable;

/** @brief The stream handle behind ::GAUD_Stream. */
struct GAUD_Stream {
  const GAUD_Stream_Vtable * vtable; ///< What kind of stream this is.
  const GAUD_Allocator * allocator;  ///< NULL means the default.
  bool seekable;                     ///< Whether seek() may be called.
  bool eof;                          ///< Whether the end has been reached.
  uint64_t position;                 ///< The read position, in bytes.

  const unsigned char * bytes; ///< Memory stream: borrowed, never freed.
  size_t length;               ///< Memory stream: how many bytes.
};

#endif // GHOTI_IO_GAUD_SRC_STREAM_STREAM_INTERNAL_H
