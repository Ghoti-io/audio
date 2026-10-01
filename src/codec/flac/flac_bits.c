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
 * The bit reader FLAC frames are decoded through.
 *
 * Most-significant bit first, over a buffer, with one error state. Three
 * decisions in it are worth stating because each one is a place a bit
 * reader usually goes wrong:
 *
 * **The cache is right-aligned and refills only while it holds 56 bits or
 * fewer.** A refill shifts left by eight and ORs in a byte, so refilling a
 * cache that already held 57 bits or more would shift its top byte out and
 * lose it silently. The bound is on the cache *before* the shift, which is
 * why the loop condition is `bits <= 56` and not `bits < 64` - the cache
 * itself does reach exactly 64.
 *
 * **Overrun latches rather than being returned.** A subframe is a few
 * hundred reads and checking each one would bury the decoder; instead a
 * read past the end yields zero, sets the flag, and the frame decoder asks
 * once. The flag is sticky, so no later read can clear it, and the
 * arithmetic downstream of it operates on zeros - which is safe, because
 * nothing in this reader indexes memory with a value it read.
 *
 * **A zero-width read is legal.** An escaped Rice partition whose raw
 * width is zero means every residual in it is zero, and spelling that as
 * an ordinary `read(br, 0)` keeps it out of the caller as a special case.
 * `1u << 32` is undefined, so the mask is built in a way that never shifts
 * by the width of its own type.
 */

#include "flac_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

void gaud_flac_bits_init(
    FLAC_Bits * br, const unsigned char * data, size_t size) {
  br->data = data;
  br->size = data ? size : 0;
  br->pos = 0;
  br->cache = 0;
  br->bits = 0;
  br->overrun = false;
}

/** Pull bytes in until the cache holds at least @p want bits, or the end. */
static void refill(FLAC_Bits * br, unsigned want) {
  while (br->bits < want && br->bits <= 56u && br->pos < br->size) {
    br->cache = (br->cache << 8) | (uint64_t)br->data[br->pos++];
    br->bits += 8u;
  }
}

/** The low @p n bits of a 64-bit value, without ever shifting by 64. */
static uint64_t low_mask(unsigned n) {
  if (n == 0) {
    return 0;
  }
  if (n >= 64u) {
    return UINT64_MAX;
  }
  return ((uint64_t)1 << n) - 1u;
}

uint64_t gaud_flac_bits_read64(FLAC_Bits * br, unsigned n) {
  if (n == 0) {
    return 0;
  }
  if (n > 64u) {
    br->overrun = true;
    return 0;
  }
  /* Wider than the cache can hold at once, so take it in two halves. The
   * only field this reaches is a 64-bit cuesheet offset. */
  if (n > 56u) {
    unsigned high = n - 32u;
    uint64_t top = gaud_flac_bits_read64(br, high);
    uint64_t bottom = gaud_flac_bits_read64(br, 32u);
    return (top << 32) | bottom;
  }
  refill(br, n);
  if (br->bits < n) {
    /* Everything that was left is consumed, so a caller that keeps asking
     * gets zeros rather than the same trailing bits over and over. */
    br->bits = 0;
    br->cache = 0;
    br->overrun = true;
    return 0;
  }
  br->bits -= n;
  uint64_t value = (br->cache >> br->bits) & low_mask(n);
  br->cache &= low_mask(br->bits);
  return value;
}

uint32_t gaud_flac_bits_read(FLAC_Bits * br, unsigned n) {
  if (n > 32u) {
    br->overrun = true;
    return 0;
  }
  return (uint32_t)gaud_flac_bits_read64(br, n);
}

int32_t gaud_flac_bits_read_signed(FLAC_Bits * br, unsigned n) {
  if (n == 0) {
    return 0;
  }
  if (n > 32u) {
    br->overrun = true;
    return 0;
  }
  return (int32_t)gaud_flac_bits_read_signed64(br, n);
}

int64_t gaud_flac_bits_read_signed64(FLAC_Bits * br, unsigned n) {
  if (n == 0) {
    return 0;
  }
  if (n > 64u) {
    br->overrun = true;
    return 0;
  }
  uint64_t raw = gaud_flac_bits_read64(br, n);
  if (n == 64u) {
    /* The whole width: there is no wider type to borrow, so the two halves
     * are assembled by hand. */
    return (raw & ((uint64_t)1 << 63))
        ? -(int64_t)(UINT64_MAX - raw) - 1
        : (int64_t)raw;
  }
  uint64_t sign = (uint64_t)1 << (n - 1u);
  if ((raw & sign) == 0) {
    return (int64_t)raw;
  }
  /* The magnitude is computed in unsigned arithmetic and negated once. A
   * plain `(int64_t)raw - (1 << n)` is signed overflow at the widest n and
   * a bare cast of a value with its top bit set is implementation-defined;
   * this is neither. Subtracting one before the cast and one after keeps
   * the most negative value in range, which is the value a decoder is
   * least likely to have a fixture for. */
  uint64_t magnitude = ((uint64_t)1 << n) - raw;
  return -(int64_t)(magnitude - 1u) - 1;
}

uint32_t gaud_flac_bits_read_unary(FLAC_Bits * br) {
  uint32_t zeros = 0;
  for (;;) {
    /* A run this long is 32 MiB of zero bits, which no legal frame holds -
     * and the bound is load-bearing rather than defensive. It is what makes
     * a Rice quotient provably under 2^28, so that `quotient << parameter`
     * with a parameter of up to 30 stays under 2^58 and the predictor sum
     * added to it cannot overflow the int64 it lands in. Without it the
     * reconstruction has a signed-overflow path a fuzzer reaches. */
    if (zeros > (1u << 28)) {
      br->overrun = true;
      return zeros;
    }
    /* Refill as far as the cache allows rather than to the one bit this
     * needs. A Rice quotient of forty is ordinary in a quiet passage, and
     * a reader that pulled one byte per iteration would spend the
     * decoder's time in this loop. */
    refill(br, 56u);
    if (br->bits == 0) {
      br->overrun = true;
      return zeros;
    }
    /* Look at the whole cache at once and count the run in it, rather than
     * a bit at a time: a Rice quotient of forty is ordinary in quiet music
     * and a per-bit loop spends the decoder's time there. */
    uint64_t window = br->cache & low_mask(br->bits);
    if (window == 0) {
      zeros += br->bits;
      br->bits = 0;
      br->cache = 0;
      continue;
    }
    unsigned leading = 0;
    while (((window >> (br->bits - 1u - leading)) & 1u) == 0) {
      ++leading;
    }
    zeros += leading;
    /* The terminating one bit is consumed too. */
    br->bits -= leading + 1u;
    br->cache &= low_mask(br->bits);
    return zeros;
  }
}

void gaud_flac_bits_skip(FLAC_Bits * br, uint64_t n) {
  while (n >= 32u) {
    (void)gaud_flac_bits_read(br, 32u);
    if (br->overrun) {
      return;
    }
    n -= 32u;
  }
  if (n) {
    (void)gaud_flac_bits_read(br, (unsigned)n);
  }
}

void gaud_flac_bits_align(FLAC_Bits * br) {
  unsigned extra = br->bits % 8u;
  if (extra) {
    (void)gaud_flac_bits_read(br, extra);
  }
}

uint64_t gaud_flac_bits_consumed(const FLAC_Bits * br) {
  return (uint64_t)br->pos * 8u - br->bits;
}

uint64_t gaud_flac_bits_left(const FLAC_Bits * br) {
  if (br->overrun) {
    return 0;
  }
  return ((uint64_t)(br->size - br->pos) * 8u) + br->bits;
}

bool gaud_flac_bits_read_coded_number(FLAC_Bits * br, uint64_t * out) {
  uint32_t first = gaud_flac_bits_read(br, 8u);
  if (br->overrun) {
    return false;
  }
  if ((first & 0x80u) == 0) {
    *out = first;
    return true;
  }
  /* Count the leading ones to get the length, as UTF-8 does. Two through
   * seven bytes; 0b10xxxxxx is a stray continuation byte and 0b11111111 is
   * longer than the encoding goes. */
  unsigned extra = 0;
  uint32_t mask = 0x40u;
  while (extra < 6u && (first & mask)) {
    ++extra;
    mask >>= 1;
  }
  if (extra == 0 || (first & mask)) {
    return false;
  }
  uint64_t value = first & (mask - 1u);
  for (unsigned i = 0; i < extra; ++i) {
    uint32_t next = gaud_flac_bits_read(br, 8u);
    if (br->overrun || (next & 0xC0u) != 0x80u) {
      return false;
    }
    value = (value << 6) | (next & 0x3Fu);
  }
  *out = value;
  return true;
}

/* ----------------------------------------------------------- bit writer */

/*
 * The mirror of the reader, and it exists for one reason: a FLAC frame is
 * not byte-aligned anywhere between its header and its footer, so an
 * encoder that assembled bytes would have to do the shifting by hand at
 * every field and would get it wrong at exactly the fields the reader is
 * already careful about.
 *
 * Failure is latched rather than returned, as it is on the reading side. An
 * encoder writes a few thousand fields per frame and a status check at each
 * one would bury the arithmetic; the caller asks once, at the end of the
 * frame, and a frame whose buffer could not grow is never emitted.
 */

void gaud_flac_bitw_init(
    FLAC_Bit_Writer * bw, const GAUD_Allocator * allocator) {
  bw->data = NULL;
  bw->size = 0;
  bw->capacity = 0;
  bw->cache = 0;
  bw->bits = 0;
  bw->allocator = allocator;
  bw->failed = false;
}

void gaud_flac_bitw_free(FLAC_Bit_Writer * bw) {
  gcu_allocator_free(bw->allocator, bw->data);
  bw->data = NULL;
  bw->size = 0;
  bw->capacity = 0;
}

void gaud_flac_bitw_reset(FLAC_Bit_Writer * bw) {
  bw->size = 0;
  bw->cache = 0;
  bw->bits = 0;
  bw->failed = false;
}

/** Append one finished byte, growing the buffer when it must. */
static void push_byte(FLAC_Bit_Writer * bw, unsigned char byte) {
  if (bw->failed) {
    return;
  }
  if (bw->size == bw->capacity) {
    size_t want = bw->capacity ? bw->capacity * 2u : 4096u;
    unsigned char * grown = gcu_allocator_malloc(bw->allocator, want);
    if (!grown) {
      bw->failed = true;
      return;
    }
    if (bw->data) {
      memcpy(grown, bw->data, bw->size);
      gcu_allocator_free(bw->allocator, bw->data);
    }
    bw->data = grown;
    bw->capacity = want;
  }
  bw->data[bw->size++] = byte;
}

void gaud_flac_bitw_write(FLAC_Bit_Writer * bw, uint64_t value, unsigned n) {
  if (n == 0) {
    return;
  }
  if (n > 64u) {
    bw->failed = true;
    return;
  }
  /* Mask first. A caller passing a negative number cast to uint64_t has
   * every high bit set, and without this those bits would spill into the
   * field above - which is how a sign-extended residual silently corrupts
   * the field before it. */
  if (n < 64u) {
    value &= ((uint64_t)1 << n) - 1u;
  }
  while (n > 0) {
    unsigned room = 8u - bw->bits;
    unsigned take = n < room ? n : room;
    unsigned shift = n - take;
    uint64_t chunk = (value >> shift) & (((uint64_t)1 << take) - 1u);
    bw->cache = (unsigned char)((bw->cache << take) | (unsigned char)chunk);
    bw->bits += take;
    n -= take;
    if (bw->bits == 8u) {
      push_byte(bw, bw->cache);
      bw->cache = 0;
      bw->bits = 0;
    }
  }
}

void gaud_flac_bitw_write_unary(FLAC_Bit_Writer * bw, uint32_t zeros) {
  while (zeros >= 32u) {
    gaud_flac_bitw_write(bw, 0, 32u);
    zeros -= 32u;
  }
  /* The terminating one bit rides along with the last run of zeros, so a
   * quotient of zero is a single set bit and costs one call. */
  gaud_flac_bitw_write(bw, 1u, zeros + 1u);
}

void gaud_flac_bitw_align(FLAC_Bit_Writer * bw) {
  if (bw->bits) {
    gaud_flac_bitw_write(bw, 0, 8u - bw->bits);
  }
}

void gaud_flac_bitw_write_coded_number(FLAC_Bit_Writer * bw, uint64_t value) {
  if (value < 0x80u) {
    gaud_flac_bitw_write(bw, value, 8u);
    return;
  }
  /* How many continuation bytes the value needs, by the same ladder the
   * reader walks: six data bits each, and the leading byte holds
   * 7 - length of them. */
  static const uint64_t limits[6] = {
      0x800ull, 0x10000ull, 0x200000ull, 0x4000000ull, 0x80000000ull,
      0x1000000000ull};
  unsigned extra = 6u;
  for (unsigned i = 0; i < 6u; ++i) {
    if (value < limits[i]) {
      extra = i + 1u;
      break;
    }
  }
  /* The leading byte: `extra + 1` ones, a zero, then the top data bits -
   * which comes to eight bits for every length, since each continuation
   * byte added costs one of the lead's data bits. */
  uint64_t lead = 0;
  for (unsigned i = 0; i <= extra; ++i) {
    lead = (lead << 1) | 1u;
  }
  lead <<= 1; /* the terminating zero */
  unsigned data_bits = 7u - (extra + 1u);
  lead <<= data_bits;
  lead |= (value >> (6u * extra)) & (((uint64_t)1 << data_bits) - 1u);
  gaud_flac_bitw_write(bw, lead, 8u);
  for (unsigned i = extra; i > 0; --i) {
    uint64_t byte = 0x80u | ((value >> (6u * (i - 1u))) & 0x3Fu);
    gaud_flac_bitw_write(bw, byte, 8u);
  }
}
