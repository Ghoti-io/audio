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
 * The buffer's layout, shared with ops.c. Never installed.
 */

#ifndef GHOTI_IO_GAUD_SRC_CORE_BUFFER_INTERNAL_H
#define GHOTI_IO_GAUD_SRC_CORE_BUFFER_INTERNAL_H

#include <ghoti.io/audio/buffer.h>
#include <ghoti.io/audio/macros.h>
#include <stddef.h>
#include <stdint.h>

/** @brief The block behind ::GAUD_Buffer. */
struct GAUD_Buffer {
  const GAUD_Allocator * allocator; ///< NULL means the default.
  GAUD_Sample_Format format;        ///< How each sample is stored.
  GAUD_Channel_Layout layout;       ///< Which speakers, and how many.
  GAUD_Sample_Layout sample_layout; ///< Interleaved or planar.
  size_t capacity;                  ///< Frames it can hold.
  size_t frames;                    ///< Frames that mean anything.
  size_t bytes;                     ///< Size of @p data.
  unsigned char * data;             ///< The samples; NULL when capacity is 0.
};

/**
 * @brief Bytes needed for @p frames frames, or 0 on overflow.
 *
 * Shared so that the buffer and anything sizing one agree; the DSD and
 * opaque cases are the ones a caller would get wrong.
 */
size_t gaud_buffer_bytes_for(
    GAUD_Sample_Format format, uint32_t channels, size_t frames);

#endif // GHOTI_IO_GAUD_SRC_CORE_BUFFER_INTERNAL_H
