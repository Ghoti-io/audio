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
 * Vorbis's bit packing, which runs the other way from everything else
 * here, and the three small integer functions its specification defines.
 *
 * **The first bit read out of a field is its least significant.** FLAC and
 * MPEG read from the most significant end of each byte, so a field's first
 * bit is its highest; Vorbis reads from the least significant end, and a
 * multi-bit field is assembled with the first bit read at the bottom. A
 * reader that got this backwards reads the identification header perfectly
 * - it is byte-aligned apart from two nibbles sharing a byte - and then
 * reads the codebooks as noise, which is why the only test that catches it
 * is one over a field whose two readings are both plausible.
 *
 * **Running off the end of a packet is not an error.** The specification
 * says so explicitly: a truncated packet at the end of a stream is a
 * legitimate end of decode, and a decoder reads zeros past the end and
 * stops when it notices. So a read past the end returns zero and raises a
 * flag rather than failing, and the callers decide - setup, where a
 * truncated packet means the stream is unusable, and audio, where it means
 * this packet is the last and contributes what it has.
 */

#include "vorbis_internal.h"

void gaud_vorbis_bits_init(
    VORBIS_Bits * bits, const unsigned char * data, size_t size) {
  bits->data = data;
  bits->size = size;
  bits->at = 0;
  bits->bit = 0;
  bits->past_end = false;
}

uint32_t gaud_vorbis_bits_read(VORBIS_Bits * bits, unsigned width) {
  uint32_t value = 0;
  for (unsigned i = 0; i < width; ++i) {
    if (bits->at >= bits->size) {
      bits->past_end = true;
      return value;
    }
    uint32_t one = (uint32_t)((bits->data[bits->at] >> bits->bit) & 1u);
    value |= one << i;
    if (++bits->bit == 8u) {
      bits->bit = 0;
      ++bits->at;
    }
  }
  return value;
}

uint64_t gaud_vorbis_bits_used(const VORBIS_Bits * bits) {
  return (uint64_t)bits->at * 8u + bits->bit;
}

unsigned gaud_vorbis_ilog(uint32_t value) {
  /* The specification's `ilog`: how many bits the value occupies, and
   * zero for zero. It is *not* a base-2 logarithm, and the difference is
   * exactly one for every power of two - which is where a reader that
   * used a log gets every ordered codebook and every mode number wrong
   * on some files and right on others. */
  unsigned bits = 0;
  while (value) {
    ++bits;
    value >>= 1;
  }
  return bits;
}

uint32_t gaud_vorbis_lookup1_values(uint32_t entries, uint32_t dimensions) {
  /*
   * The greatest integer r with r to the power of `dimensions` not more
   * than `entries`, which is how many multiplicands a lattice codebook
   * stores: the lattice is r values per axis and the entry number is read
   * as a number in base r.
   *
   * By search rather than by `pow`, and not only because this library has
   * no floating point in a decoder: `floor(pow(entries, 1.0/dim))` is
   * off by one at exact powers for some values on some platforms, which
   * would make a codebook's multiplicand count differ by architecture -
   * and every bit read after it would then be misaligned.
   */
  if (dimensions == 0) {
    return 0;
  }
  uint32_t value = 0;
  for (;;) {
    uint64_t power = 1;
    for (uint32_t i = 0; i < dimensions; ++i) {
      power *= (uint64_t)(value + 1u);
      if (power > entries) {
        break;
      }
    }
    if (power > entries) {
      return value;
    }
    ++value;
    if (value == 0) {
      return UINT32_MAX; /* Unreachable for any real codebook. */
    }
  }
}
