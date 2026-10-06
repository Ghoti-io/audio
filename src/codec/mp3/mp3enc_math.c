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
 * The integer arithmetic the encoder is built on: a square root, a
 * logarithm and a power of two, and the bit writer.
 *
 * Nothing here touches floating point, because the encoder's output has to
 * be the same bytes on every architecture and "the same libm" is not
 * something a big-endian target can promise. The logarithm and the power
 * are a leading-bit count and a 256-entry table each; both tables are
 * generated, in `mp3enc_tables.c`.
 */

#include "mp3enc_internal.h"

uint32_t gaud_mp3e_isqrt(uint64_t value) {
  uint64_t result = 0;
  uint64_t bit = (uint64_t)1 << 62;
  while (bit > value) {
    bit >>= 2;
  }
  while (bit) {
    if (value >= result + bit) {
      value -= result + bit;
      result = (result >> 1) + bit;
    }
    else {
      result >>= 1;
    }
    bit >>= 2;
  }
  return (uint32_t)result;
}

int32_t gaud_mp3e_log2_q8(uint64_t value) {
  if (value == 0) {
    return 0;
  }
  int msb = 63 - __builtin_clzll(value);
  /* The eight bits after the leading one, which select the table row. */
  uint64_t mantissa = msb >= 8 ? (value >> (msb - 8)) & 0xFFu
                               : (value << (8 - msb)) & 0xFFu;
  return msb * 256 + (int32_t)gaud_mp3enc_log2_frac[mantissa];
}

uint32_t gaud_mp3e_exp2_q8(int32_t exponent) {
  /* Floor division, so the fraction is always 0 to 255. */
  int32_t whole = exponent >> 8;
  uint32_t fraction = (uint32_t)(exponent & 0xFF);
  uint32_t base = gaud_mp3enc_exp2_frac[fraction];
  if (whole >= 15) {
    return UINT32_MAX >> 1; /* Saturate: the caller is far outside range. */
  }
  if (whole >= 0) {
    return base << whole;
  }
  if (whole <= -32) {
    return 0;
  }
  return base >> -whole;
}

void gaud_mp3e_bits_init(
    MP3E_Bits * bits, unsigned char * data, size_t capacity) {
  bits->data = data;
  bits->capacity = capacity;
  bits->bits = 0;
  bits->overflow = false;
}

void gaud_mp3e_bits_put(MP3E_Bits * bits, uint32_t value, unsigned count) {
  for (unsigned i = count; i > 0; --i) {
    size_t at = bits->bits++;
    if ((at >> 3) >= bits->capacity) {
      bits->overflow = true;
      continue;
    }
    if ((value >> (i - 1u)) & 1u) {
      bits->data[at >> 3] = (unsigned char)(bits->data[at >> 3]
          | (0x80u >> (at & 7u)));
    }
  }
}
