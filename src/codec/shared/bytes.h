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
 * Reading and writing fixed-width integers in a stated byte order.
 *
 * Shared between WAV, which is little-endian, and AIFF, which is big. Having
 * both in one place is the point: a library with one codec per byte order
 * grows two copies of this and they drift, and the one that is wrong is the
 * one nobody tests on a machine of the other endianness.
 *
 * Every function here is byte-at-a-time and so is correct on any host
 * whatever its own order. Nothing casts a pointer to a wider type, which
 * would be both an alignment fault waiting to happen and a strict-aliasing
 * violation no sanitizer would report.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_SHARED_BYTES_H
#define GHOTI_IO_GAUD_SRC_CODEC_SHARED_BYTES_H

#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline uint16_t gaud_rd_u16le(const unsigned char * p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t gaud_rd_u32le(const unsigned char * p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
      | ((uint32_t)p[3] << 24);
}

static inline uint64_t gaud_rd_u64le(const unsigned char * p) {
  return (uint64_t)gaud_rd_u32le(p)
      | ((uint64_t)gaud_rd_u32le(p + 4) << 32);
}

static inline uint16_t gaud_rd_u16be(const unsigned char * p) {
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline uint32_t gaud_rd_u32be(const unsigned char * p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
      | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void gaud_wr_u16le(unsigned char * p, uint16_t v) {
  p[0] = (unsigned char)(v & 0xFFu);
  p[1] = (unsigned char)((v >> 8) & 0xFFu);
}

static inline void gaud_wr_u32le(unsigned char * p, uint32_t v) {
  p[0] = (unsigned char)(v & 0xFFu);
  p[1] = (unsigned char)((v >> 8) & 0xFFu);
  p[2] = (unsigned char)((v >> 16) & 0xFFu);
  p[3] = (unsigned char)((v >> 24) & 0xFFu);
}

static inline void gaud_wr_u16be(unsigned char * p, uint16_t v) {
  p[0] = (unsigned char)((v >> 8) & 0xFFu);
  p[1] = (unsigned char)(v & 0xFFu);
}

static inline void gaud_wr_u32be(unsigned char * p, uint32_t v) {
  p[0] = (unsigned char)((v >> 24) & 0xFFu);
  p[1] = (unsigned char)((v >> 16) & 0xFFu);
  p[2] = (unsigned char)((v >> 8) & 0xFFu);
  p[3] = (unsigned char)(v & 0xFFu);
}

/**
 * @brief Whether this host stores integers least-significant byte first.
 *
 * Decided at run time from the bytes of a known value rather than from a
 * macro, because the macros that answer this differ between compilers and
 * the wrong one silently answers for the compiler rather than the target.
 * A constant-folding compiler removes the branch anyway.
 */
static inline bool gaud_host_is_little_endian(void) {
  const uint16_t one = 1;
  unsigned char bytes[sizeof(one)];
  memcpy(bytes, &one, sizeof(one));
  return bytes[0] == 1;
}

/**
 * @brief Swap every @p width-byte group in @p data, in place.
 *
 * What a codec calls when the file's byte order is not the host's. Samples
 * are in host order once decoded, which is the whole reason WAV and AIFF can
 * produce the same buffer; this is where that happens.
 */
static inline void gaud_swap_samples(
    unsigned char * data, size_t count, size_t width) {
  if (width < 2) {
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    unsigned char * p = data + i * width;
    for (size_t a = 0, b = width - 1; a < b; ++a, --b) {
      unsigned char t = p[a];
      p[a] = p[b];
      p[b] = t;
    }
  }
}

#endif // GHOTI_IO_GAUD_SRC_CODEC_SHARED_BYTES_H
