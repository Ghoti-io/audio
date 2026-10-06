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
 * Floor type 0: the spectral envelope as a line spectral pair filter.
 * Vorbis I section 6.2. Never installed.
 *
 * A packet states an amplitude and a vector of angles; the curve is the
 * filter's magnitude response, scaled so that the amplitude is a number of
 * decibels. Every number in it is a transcendental of the stream's: a
 * cosine per coefficient and per Bark band, a square root, and an
 * exponential per band - and the map from a spectral line to its Bark band
 * is two arctangents. Reference decoders use floating point and libm. This
 * decoder promises byte-identical output on every machine, so none of that
 * is available, and **what is here is the same computation in integers,
 * measured against the references rather than argued to be right**: the
 * unit tests compare each function with libm's, and the whole curve is
 * scored against ffmpeg's decoder and libvorbis on streams written for the
 * purpose (tools/oracle/vorbis_synth.py).
 *
 * **Three scales are used and the choice of each is the point.**
 *
 *   - Q30 for anything bounded by a few units: a cosine, a term of the
 *     product, the series of an exponential. A 64-bit product of two of
 *     them does not overflow, which is what the whole file is arranged
 *     around.
 *   - Q32 radians for an angle, because the codebook's own values are
 *     states of a float32 and the cosine's argument has to survive
 *     being reduced modulo two pi.
 *   - A normalised pair, a 32-bit mantissa and an exponent, for the
 *     product of up to a hundred and twenty-eight terms. The product
 *     spans more decades than any fixed scale holds, and a floating
 *     mantissa is what keeps its precision without a wider word.
 *
 * The curve's output is the same pair the inverse decibel table holds for
 * floor 1 - `mantissa * 2^-shift`, mantissa in [2^30, 2^31) - so the
 * multiply that applies it does not care which floor made it.
 */

#include "vorbis_internal.h"
#include <string.h>

/** 2^32 times pi, rounded. */
#define F0_PI_Q32 13493037705LL
/** Twice that: the period of the cosine. */
#define F0_TWO_PI_Q32 26986075409LL
/** Half of it. */
#define F0_HALF_PI_Q32 6746518852LL
/** A quarter of it. */
#define F0_QUARTER_PI_Q32 3373259426LL

/** 2^30. */
#define F0_ONE ((int64_t)1 << 30)
/** pi / 2 in Q30. */
#define F0_HALF_PI_Q30 1686629713LL
/** log2(10) / 20 in Q30: decibels of amplitude to a power of two. */
#define F0_DB_TO_LOG2_Q30 178344657LL
/** ln(2) in Q30. */
#define F0_LN2_Q30 744261118LL

/** The largest amplitude, in decibels, the curve will honour. */
#define F0_DB_LIMIT_Q24 ((int64_t)1 << 35)

/** The most the curve's power of two may be above one. */
#define F0_EXPONENT_MAX 40
/** And the most below: a quieter line is zero. */
#define F0_EXPONENT_MIN (-100)

/* ---------------------------------------------------------- integer maths */

/** floor(sqrt(value)). */
static uint64_t isqrt64(uint64_t value) {
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
  return result;
}

/** The 128-bit product of two 64-bit numbers, as high and low words. */
static void mul64(uint64_t a, uint64_t b, uint64_t * high, uint64_t * low) {
  uint64_t a_low = a & 0xFFFFFFFFu;
  uint64_t a_high = a >> 32;
  uint64_t b_low = b & 0xFFFFFFFFu;
  uint64_t b_high = b >> 32;
  uint64_t ll = a_low * b_low;
  uint64_t lh = a_low * b_high;
  uint64_t hl = a_high * b_low;
  uint64_t hh = a_high * b_high;
  uint64_t middle = (ll >> 32) + (lh & 0xFFFFFFFFu) + (hl & 0xFFFFFFFFu);
  *low = (ll & 0xFFFFFFFFu) | (middle << 32);
  *high = hh + (lh >> 32) + (hl >> 32) + (middle >> 32);
}

/**
 * floor(a * b / c), the product taken in 128 bits.
 *
 * Exact where the result fits 64 bits, which every caller here arranges,
 * and it is what lets a constant such as 0.00074 be spelled as the ratio
 * it is rather than as a rounded fixed-point number.
 */
static uint64_t muldiv(uint64_t a, uint64_t b, uint64_t c) {
  uint64_t high;
  uint64_t low;
  mul64(a, b, &high, &low);
  // Long division of the 128-bit value by c, one bit at a time. This runs
  // when a stream's map is built and not per line, so simple wins.
  uint64_t quotient = 0;
  uint64_t remainder = 0;
  for (int bit = 127; bit >= 0; --bit) {
    uint64_t next = bit >= 64 ? (high >> (bit - 64)) & 1u : (low >> bit) & 1u;
    uint64_t carry = remainder >> 63;
    remainder = (remainder << 1) | next;
    if (carry || remainder >= c) {
      remainder -= c;
      if (bit < 64) {
        quotient |= (uint64_t)1 << bit;
      }
    }
  }
  return quotient;
}

/** cos of 0 to pi/4 in Q30 from its argument in Q30. */
static int64_t cos_series(int64_t u) {
  int64_t u2 = (u * u) >> 30;
  int64_t t = F0_ONE - u2 / 132;
  t = F0_ONE - ((u2 * t) >> 30) / 90;
  t = F0_ONE - ((u2 * t) >> 30) / 56;
  t = F0_ONE - ((u2 * t) >> 30) / 30;
  t = F0_ONE - ((u2 * t) >> 30) / 12;
  return F0_ONE - (((u2 * t) >> 30) >> 1);
}

/** sin of 0 to pi/4 in Q30 from its argument in Q30. */
static int64_t sin_series(int64_t u) {
  int64_t u2 = (u * u) >> 30;
  int64_t t = F0_ONE - u2 / 110;
  t = F0_ONE - ((u2 * t) >> 30) / 72;
  t = F0_ONE - ((u2 * t) >> 30) / 42;
  t = F0_ONE - ((u2 * t) >> 30) / 20;
  t = F0_ONE - ((u2 * t) >> 30) / 6;
  return (u * t) >> 30;
}

int32_t gaud_vorbis_f0_cos(int64_t angle_q32) {
  // The period first, so that any angle a stream can state means
  // something; an enormous one is a garbage stream, and what it means is
  // still the same number on every machine.
  int64_t x = angle_q32 % F0_TWO_PI_Q32;
  if (x < 0) {
    x += F0_TWO_PI_Q32;
  }
  if (x > F0_PI_Q32) {
    x = F0_TWO_PI_Q32 - x;
  }
  int64_t sign = 1;
  if (x > F0_HALF_PI_Q32) {
    x = F0_PI_Q32 - x;
    sign = -1;
  }
  int64_t value;
  if (x > F0_QUARTER_PI_Q32) {
    value = sin_series((F0_HALF_PI_Q32 - x) >> 2);
  }
  else {
    value = cos_series(x >> 2);
  }
  return (int32_t)(sign * value);
}

int32_t gaud_vorbis_f0_atan(int64_t t_q30) {
  if (t_q30 <= 0) {
    return 0;
  }
  if (t_q30 > F0_ONE) {
    // atan(t) = pi/2 - atan(1/t).
    int64_t reciprocal = (int64_t)(((uint64_t)1 << 60) / (uint64_t)t_q30);
    return (int32_t)(F0_HALF_PI_Q30 - gaud_vorbis_f0_atan(reciprocal));
  }
  // Three halvings of the angle bring the argument under 0.1, where the
  // series has converged by its eighth term.
  int64_t t = t_q30;
  for (int step = 0; step < 3; ++step) {
    int64_t t2 = (t * t) >> 30;
    int64_t root = (int64_t)isqrt64((uint64_t)(F0_ONE + t2) << 30);
    t = (int64_t)(((uint64_t)t << 30) / (uint64_t)(F0_ONE + root));
  }
  int64_t t2 = (t * t) >> 30;
  int64_t series = F0_ONE;
  for (int k = 15; k >= 3; k -= 2) {
    // 1 - t^2 (k-2)/k (1 - ...), evaluated from the inside out.
    series = F0_ONE - (((t2 * series) >> 30) * (k - 2)) / k;
  }
  return (int32_t)(8 * ((t * series) >> 30));
}

/* ------------------------------------------------- a floating mantissa */

/** A value `m * 2^e`, with m zero or in [2^31, 2^32). */
typedef struct {
  uint32_t m; ///< The mantissa.
  int e;      ///< The exponent.
} Fl;

static Fl fl_make(uint64_t value, int exponent) {
  Fl out = {0, 0};
  if (value == 0) {
    return out;
  }
  int top = 63;
  while (!((value >> top) & 1u)) {
    --top;
  }
  if (top > 31) {
    int drop = top - 31;
    uint64_t rounded = (value + ((uint64_t)1 << (drop - 1))) >> drop;
    if (rounded >> 32) {
      rounded >>= 1;
      ++drop;
    }
    out.m = (uint32_t)rounded;
    out.e = exponent + drop;
  }
  else {
    out.m = (uint32_t)(value << (31 - top));
    out.e = exponent - (31 - top);
  }
  return out;
}

static Fl fl_mul(Fl a, Fl b) {
  if (a.m == 0 || b.m == 0) {
    Fl zero = {0, 0};
    return zero;
  }
  return fl_make((uint64_t)a.m * b.m, a.e + b.e);
}

static Fl fl_add(Fl a, Fl b) {
  if (a.m == 0) {
    return b;
  }
  if (b.m == 0) {
    return a;
  }
  if (a.e < b.e) {
    Fl swap = a;
    a = b;
    b = swap;
  }
  int gap = a.e - b.e;
  if (gap > 31) {
    return a; // The smaller is below the larger's last bit.
  }
  // Both mantissas widened by 32 bits so the smaller keeps its precision.
  uint64_t big = (uint64_t)a.m << gap;
  uint64_t sum = big + b.m;
  return fl_make(sum, b.e);
}

static Fl fl_sqrt(Fl a) {
  if (a.m == 0) {
    return a;
  }
  uint64_t m = a.m;
  int e = a.e;
  if (e & 1) {
    m <<= 31;
    e -= 31;
  }
  else {
    m <<= 32;
    e -= 32;
  }
  return fl_make(isqrt64(m), e / 2);
}

/** a / b, for b nonzero. */
static Fl fl_div(Fl a, Fl b) {
  if (a.m == 0) {
    return a;
  }
  uint64_t quotient = ((uint64_t)a.m << 31) / b.m;
  return fl_make(quotient, a.e - b.e - 31);
}

/* ----------------------------------------------------------- 2 ^ fraction */

/** 2 to the power of a fraction in [0, 1) given in Q24, as a Q30 number. */
static int64_t exp2_fraction(int64_t f_q24) {
  int64_t u = ((f_q24 << 6) * F0_LN2_Q30) >> 30; // ln2 * f in Q30
  // exp(u) by Horner, 1 + u(1 + u/2(1 + u/3(...))), thirteen terms.
  int64_t t = F0_ONE;
  for (int k = 13; k >= 1; --k) {
    t = F0_ONE + (((u * t) >> 30) / k);
  }
  return t;
}

/* ------------------------------------------------------------- the curve */

/** 13.1 atan(.00074 x) + 2.24 atan(1.85e-8 x^2) + .0001 x, x in Hz as Q16. */
static uint64_t bark_q30(uint64_t x_q16) {
  uint64_t t1 = muldiv(x_q16, 74u * ((uint64_t)1 << 14), 100000u);
  // x^2 in Q32 is up to 2^62; the constant 1.85e-8 in Q30 over Q32.
  uint64_t x2 = x_q16 * x_q16;
  uint64_t t2 = muldiv(x2, 185u, 40000000000u);
  uint64_t first = (uint64_t)gaud_vorbis_f0_atan((int64_t)t1);
  uint64_t second = (uint64_t)gaud_vorbis_f0_atan((int64_t)t2);
  uint64_t linear = muldiv(x_q16, 1u * ((uint64_t)1 << 14), 10000u);
  return muldiv(first, 131u, 10u) + muldiv(second, 224u, 100u) + linear;
}

void gaud_vorbis_f0_bark_map(
    uint32_t rate, uint32_t bark_size, uint32_t lines, uint16_t * map) {
  uint64_t top = bark_q30((uint64_t)rate * 32768u); // rate/2 as Q16
  if (top == 0) {
    top = 1;
  }
  for (uint32_t j = 0; j < lines; ++j) {
    // The line's frequency, rate / 2 / lines * j, in Q16.
    uint64_t x = muldiv((uint64_t)rate * 32768u, j, lines);
    uint64_t value = muldiv(bark_q30(x), bark_size, top);
    if (value >= bark_size) {
      value = bark_size - 1u;
    }
    map[j] = (uint16_t)value;
  }
}

/**
 * A right shift that rounds towards minus infinity, which for a negative
 * value C leaves to the implementation: this library promises the same
 * bytes everywhere, so the result is made by arithmetic on the magnitude.
 */
static int64_t floor_shift(int64_t value, int bits) {
  if (value >= 0) {
    return value >> bits;
  }
  return -(((-value) + (((int64_t)1 << bits) - 1)) >> bits);
}

/** One coefficient's `2 cos`, in Q30. */
static int64_t two_cos(int64_t angle_q32) {
  return 2 * (int64_t)gaud_vorbis_f0_cos(angle_q32);
}

/**
 * The curve's value at one Bark band, as a floor pair.
 *
 * `10 ^ ((amplitude / sqrt(p + q) - offset) / 20)` where p and q are the
 * products of the specification, written here as the magnitudes they are:
 * every factor that is left after the products is squared, so no sign
 * has to be carried.
 */
static void band_gain(const int64_t * lsp, uint32_t order, int64_t w,
    Fl numerator, Fl maxval, int64_t offset_q24, int32_t * mantissa,
    int16_t * shift) {
  Fl p = fl_make(1, -1); // one half
  Fl q = fl_make(1, -1);
  uint32_t j = 1;
  for (; j < order; j += 2) {
    int64_t a = w - lsp[j - 1];
    int64_t b = w - lsp[j];
    q = fl_mul(q, fl_make((uint64_t)(a < 0 ? -a : a), -30));
    p = fl_mul(p, fl_make((uint64_t)(b < 0 ? -b : b), -30));
  }
  if (j == order) {
    // Odd order: one more factor for q, and the pair's own centre term.
    int64_t a = w - lsp[j - 1];
    q = fl_mul(q, fl_make((uint64_t)(a < 0 ? -a : a), -30));
    int64_t four_minus_w2 = ((int64_t)1 << 62) - w * w; // Q60
    p = fl_mul(fl_mul(p, p), fl_make((uint64_t)four_minus_w2, -60));
    q = fl_mul(q, q);
  }
  else {
    int64_t two_minus_w = ((int64_t)2 << 30) - w;
    int64_t two_plus_w = ((int64_t)2 << 30) + w;
    p = fl_mul(fl_mul(p, p), fl_make((uint64_t)two_minus_w, -30));
    q = fl_mul(fl_mul(q, q), fl_make((uint64_t)two_plus_w, -30));
  }
  Fl denominator = fl_mul(maxval, fl_sqrt(fl_add(p, q)));
  int64_t db_q24;
  if (denominator.m == 0) {
    db_q24 = F0_DB_LIMIT_Q24; // A silent denominator: as loud as allowed.
  }
  else {
    Fl x = fl_div(numerator, denominator);
    int shift_bits = x.e + 24;
    if (x.m == 0) {
      db_q24 = 0;
    }
    else if (shift_bits >= 4) {
      db_q24 = F0_DB_LIMIT_Q24;
    }
    else if (shift_bits >= 0) {
      db_q24 = (int64_t)x.m << shift_bits;
    }
    else if (-shift_bits >= 40) {
      db_q24 = 0;
    }
    else {
      db_q24 = ((int64_t)x.m + ((int64_t)1 << (-shift_bits - 1)))
          >> -shift_bits;
    }
    if (db_q24 > F0_DB_LIMIT_Q24) {
      db_q24 = F0_DB_LIMIT_Q24;
    }
  }
  int64_t d = db_q24 - offset_q24;
  int64_t y_q24 = floor_shift(d * F0_DB_TO_LOG2_Q30, 30); // log2 of the amplitude
  int64_t whole = floor_shift(y_q24, 24);
  int64_t fraction = y_q24 - whole * 16777216;
  if (whole < F0_EXPONENT_MIN) {
    *mantissa = 0;
    *shift = 0;
    return;
  }
  if (whole > F0_EXPONENT_MAX) {
    whole = F0_EXPONENT_MAX;
    fraction = 0;
  }
  *mantissa = (int32_t)exp2_fraction(fraction);
  *shift = (int16_t)(30 - whole);
}

GAUD_Result gaud_vorbis_floor0_decode(const VORBIS_Floor0 * floor,
    const VORBIS_Setup * setup, VORBIS_Bits * bits, uint32_t lines,
    const uint16_t * map, int32_t * gain_mantissa, int16_t * gain_shift,
    bool * out_used) {
  *out_used = false;
  uint32_t amplitude = gaud_vorbis_bits_read(bits, floor->amplitude_bits);
  if (bits->past_end || amplitude == 0) {
    return GAUD_OK;
  }
  uint32_t book_number
      = gaud_vorbis_bits_read(bits, gaud_vorbis_ilog(floor->book_count));
  if (bits->past_end) {
    return GAUD_OK;
  }
  if (book_number >= floor->book_count) {
    return GAUD_ERR_CORRUPT;
  }
  uint32_t book_index = floor->books[book_number];
  if (book_index >= setup->codebook_count) {
    return GAUD_ERR_CORRUPT;
  }
  const VORBIS_Codebook * book = &setup->codebooks[book_index];
  if (!book->multiplicands || book->dimensions == 0) {
    return GAUD_ERR_CORRUPT;
  }

  // The coefficients: vectors from the book, each element offset by the
  // last element of the vector before it, until there are enough.
  int64_t lsp[256];
  int64_t vector[256];
  if (book->dimensions > 256u) {
    return GAUD_ERR_UNSUPPORTED; // No floor has an order near that.
  }
  int64_t last = 0;
  uint32_t count = 0;
  while (count < floor->order) {
    uint32_t entry = gaud_vorbis_codebook_decode(book, bits);
    if (entry == UINT32_MAX || bits->past_end) {
      return GAUD_OK; // Out of packet: the channel carries nothing.
    }
    gaud_vorbis_codebook_vector_fine(book, entry, vector);
    // Every element of a vector is offset by the last element of the one
    // before it, and by nothing within itself; the last one kept is the
    // last that was wanted.
    for (uint32_t k = 0; k < book->dimensions && count < floor->order; ++k) {
      lsp[count++] = vector[k] + last;
    }
    last = lsp[count - 1];
  }
  // 2 cos of each, in Q30.
  for (uint32_t i = 0; i < floor->order; ++i) {
    lsp[i] = two_cos(lsp[i]);
  }

  // The amplitude as decibels over the largest amplitude: the numerator
  // here, with the denominator joined to the curve's own.
  uint32_t maxval_bits = floor->amplitude_bits;
  uint64_t maxval_value = maxval_bits >= 32 ? 0xFFFFFFFFu
      : (((uint64_t)1 << maxval_bits) - 1u);
  Fl maxval = fl_make(maxval_value, 0);
  Fl numerator = fl_make((uint64_t)amplitude * floor->amplitude_offset, 0);
  int64_t offset_q24 = (int64_t)floor->amplitude_offset << 24;

  uint32_t j = 0;
  while (j < lines) {
    uint16_t band = map[j];
    int64_t w = two_cos(
        (int64_t)muldiv((uint64_t)F0_PI_Q32, band, floor->bark_map_size));
    int32_t mantissa;
    int16_t shift;
    band_gain(lsp, floor->order, w, numerator, maxval, offset_q24, &mantissa,
        &shift);
    while (j < lines && map[j] == band) {
      gain_mantissa[j] = mantissa;
      gain_shift[j] = shift;
      ++j;
    }
  }
  *out_used = true;
  return GAUD_OK;
}
