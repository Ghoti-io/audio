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
 * The range decoder itself. opus_range.h says why it is shaped this way.
 */

#include "opus_range.h"

unsigned gaud_opus_ilog(uint32_t value) {
  unsigned bits = 0;
  while (value) {
    ++bits;
    value >>= 1;
  }
  return bits;
}

/** The smaller of two unsigned values. */
static uint32_t mini(uint32_t a, uint32_t b) {
  return a < b ? a : b;
}

/**
 * One byte from the front, or zero past the end.
 *
 * Section 4.1.2.1: once the frame's bytes are gone the decoder "MUST
 * continue to use zero", which is a decoding rule and not a fallback -
 * the encoder relies on it to terminate the stream short.
 */
static uint32_t read_byte(OPUS_Range * range) {
  return range->offset < range->size ? range->buf[range->offset++] : 0u;
}

/** One byte from the back, or zero once those are gone too. */
static uint32_t read_byte_from_end(OPUS_Range * range) {
  if (range->end_offset >= range->size) {
    return 0u;
  }
  ++range->end_offset;
  return range->buf[range->size - range->end_offset];
}

/**
 * Section 4.1.2.1: pull bytes in until `rng` exceeds 2**23.
 *
 * The byte read last is kept whole in ::OPUS_Range::rem even though only
 * its lowest bit is still owed, because the next iteration wants that
 * bit as the *high* bit of its symbol and shifting the whole byte is
 * cheaper than masking it.
 */
static void normalize(OPUS_Range * range) {
  while (range->rng <= OPUS_CODE_BOT) {
    uint32_t sym;
    range->total_bits += OPUS_SYM_BITS;
    range->rng <<= OPUS_SYM_BITS;
    sym = range->rem;
    range->rem = read_byte(range);
    sym = ((sym << OPUS_SYM_BITS) | range->rem)
        >> (OPUS_SYM_BITS - OPUS_CODE_EXTRA);
    range->val = ((range->val << OPUS_SYM_BITS)
                     + (OPUS_SYM_MAX & ~sym))
        & (OPUS_CODE_TOP - 1u);
  }
}

void gaud_opus_range_init(
    OPUS_Range * range, const unsigned char * data, size_t size) {
  range->buf = data;
  range->size = (uint32_t)size;
  range->offset = 0;
  range->end_offset = 0;
  range->end_window = 0;
  range->end_bits = 0;
  range->ext = 0;
  range->error = false;
  /*
   * Section 4.1.6: nine, not zero. Two bits more than the range coder
   * has actually buffered, so that the count is an upper bound and so
   * that the encoder has somewhere to put its terminating bit. The
   * consequence a reader trips over first is that a decoder which has
   * read nothing reports one bit used.
   */
  range->total_bits = OPUS_CODE_BITS + 1u
      - ((OPUS_CODE_BITS - OPUS_CODE_EXTRA) / OPUS_SYM_BITS) * OPUS_SYM_BITS;
  range->rng = 1u << OPUS_CODE_EXTRA;
  range->rem = read_byte(range);
  range->val = range->rng - 1u
      - (range->rem >> (OPUS_SYM_BITS - OPUS_CODE_EXTRA));
  normalize(range);
}

uint32_t gaud_opus_decode(OPUS_Range * range, uint32_t ft) {
  range->ext = range->rng / ft;
  uint32_t s = range->val / range->ext;
  return ft - mini(s + 1u, ft);
}

uint32_t gaud_opus_decode_bin(OPUS_Range * range, unsigned ftb) {
  range->ext = range->rng >> ftb;
  uint32_t s = range->val / range->ext;
  return (1u << ftb) - mini(s + 1u, 1u << ftb);
}

void gaud_opus_dec_update(
    OPUS_Range * range, uint32_t fl, uint32_t fh, uint32_t ft) {
  uint32_t s = range->ext * (ft - fh);
  range->val -= s;
  /*
   * The special case is on the *first* symbol rather than the last, so
   * every truncation error in the division above lands on symbol zero.
   * Section 4.1.2 explains the choice: contexts are written with the
   * likeliest symbol first, so the one that is cheap to code slightly
   * more often than its probability says is the one coded most.
   */
  range->rng = fl > 0 ? range->ext * (fh - fl) : range->rng - s;
  normalize(range);
}

int gaud_opus_dec_bit_logp(OPUS_Range * range, unsigned logp) {
  uint32_t r = range->rng;
  uint32_t d = range->val;
  uint32_t s = r >> logp;
  int ret = d < s;
  if (!ret) {
    range->val = d - s;
  }
  range->rng = ret ? s : r - s;
  normalize(range);
  return ret;
}

int gaud_opus_dec_icdf(
    OPUS_Range * range, const unsigned char * icdf, unsigned ftb) {
  uint32_t s = range->rng;
  uint32_t d = range->val;
  uint32_t r = s >> ftb;
  uint32_t t;
  int ret = -1;
  /*
   * The table's terminating zero is what stops this: `icdf[k]` is
   * `(1 << ftb) - fh[k]`, so the last entry makes `s` zero and `d < 0`
   * is false for every unsigned `d`. A table without that zero runs off
   * the end, which is why every table in this codec is generated rather
   * than typed.
   */
  do {
    t = s;
    s = r * icdf[++ret];
  } while (d < s);
  range->val = d - s;
  range->rng = t - s;
  normalize(range);
  return ret;
}

uint32_t gaud_opus_dec_bits(OPUS_Range * range, unsigned bits) {
  uint32_t window = range->end_window;
  unsigned available = range->end_bits;
  if (available < bits) {
    do {
      window |= read_byte_from_end(range) << available;
      available += OPUS_SYM_BITS;
    } while (available <= OPUS_CODE_BITS - OPUS_SYM_BITS);
  }
  uint32_t ret = window & ((1u << bits) - 1u);
  window >>= bits;
  available -= bits;
  range->end_window = window;
  range->end_bits = available;
  range->total_bits += bits;
  return ret;
}

uint32_t gaud_opus_dec_uint(OPUS_Range * range, uint32_t ft) {
  /*
   * Section 4.1.5 states this takes `ft` of at least two; one value is
   * zero bits and no caller in the format asks for it.
   */
  --ft;
  unsigned ftb = gaud_opus_ilog(ft);
  if (ftb > OPUS_UINT_BITS) {
    ftb -= OPUS_UINT_BITS;
    uint32_t top = (ft >> ftb) + 1u;
    uint32_t s = gaud_opus_decode(range, top);
    gaud_opus_dec_update(range, s, s + 1u, top);
    uint32_t t = (s << ftb) | gaud_opus_dec_bits(range, ftb);
    if (t <= ft) {
      return t;
    }
    /*
     * The one error the specification names. Splitting the value
     * between a coded symbol and raw bits means the two halves can
     * disagree, and the raw half carries no redundancy to catch it
     * with - so a corrupt frame can produce a value outside the range
     * that was asked for. Saturating is what section 4.1.5 suggests.
     */
    range->error = true;
    return ft;
  }
  ++ft;
  uint32_t s = gaud_opus_decode(range, ft);
  gaud_opus_dec_update(range, s, s + 1u, ft);
  return s;
}

uint32_t gaud_opus_tell(const OPUS_Range * range) {
  return range->total_bits - gaud_opus_ilog(range->rng);
}

uint32_t gaud_opus_tell_frac(const OPUS_Range * range) {
  uint32_t nbits = range->total_bits << OPUS_BITRES;
  uint32_t l = gaud_opus_ilog(range->rng);
  uint32_t r = range->rng >> (l - 16u);
  /*
   * Three squarings, each adding one bit to `l`. Squaring a Q15 value
   * in [1,2) and testing whether it reached 2 is the base-two logarithm
   * one bit at a time; the shift afterwards puts it back in range.
   */
  for (unsigned i = OPUS_BITRES; i-- > 0;) {
    r = (r * r) >> 15;
    uint32_t b = r >> 16;
    l = (l << 1) | b;
    r >>= b;
  }
  return nbits - l;
}
