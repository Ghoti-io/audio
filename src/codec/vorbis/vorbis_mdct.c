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
 * The inverse transform, in integers.
 *
 * ## The factorisation, and how it was arrived at
 *
 * The specification defines the inverse transform as a sum over n/2
 * coefficients for each of n outputs, which is 8 million multiply-adds
 * for the largest block and not a thing to do per packet. It factors in
 * two steps, and **both were verified against the specification's own
 * formula before any of this was written** - a prototype agreed to 2e-11
 * at n = 2048, which is floating-point noise:
 *
 *   1. **An inverse MDCT of n points is a DCT-IV of n/2 points** plus a
 *      time-domain aliasing symmetry: the DCT-IV output, read in four
 *      quarters with two of them reversed and three negated, is the
 *      transform's n outputs. No arithmetic, only indexing.
 *   2. **A DCT-IV of M points is a complex transform of M/2 points**
 *      with one rotation applied before it and the same one after. The
 *      rotation is `exp(-i*pi*(k + 1/8)/M)`, and **the 1/8 is the whole
 *      of it**: a first attempt used 1/4, which gives a result wrong by
 *      a few percent - recognisable audio with a buzz on it rather than
 *      noise, which is to say an error a listening test passes.
 *
 * So the work per packet is one n/4-point complex fast Fourier transform
 * and two rotations. The twiddles come from vorbis_tables.c, generated
 * from the formulas in exact arithmetic.
 *
 * ## The fixed point, and where the scale goes
 *
 * This is the only part of the Vorbis decoder whose answer is not exactly
 * the specification's: a floor is integer by design and a residue is a
 * sum of table entries, but a transform is multiplications by irrational
 * numbers and the question is only how much precision to keep.
 *
 * The input is Q#VORBIS_SPECTRUM_Q, which holds ±8 - ample, because a
 * spectral line cannot much exceed the signal's own amplitude. The
 * transform's gain is not one: a single coefficient comes out at its own
 * amplitude, but M coefficients of real audio come out at about
 * sqrt(M) times their root-mean-square, so the state grows through the
 * stages. **The state is therefore halved after every second stage**,
 * which removes 2^(stages/2) - almost exactly the growth - and leaves the
 * output at a scale that depends on the block size. Normalising that to
 * one scale is the caller's last step, and ::VORBIS_TIME_Q is where it
 * goes.
 *
 * Every multiply is 32 by 32 into 64 and shifted back, so a twiddle
 * cannot overflow; every butterfly saturates, so a stream whose spectrum
 * is arithmetically possible and physically absurd is bounded rather than
 * undefined. planning/audio.md section 11.1 is why that matters: signed
 * overflow is undefined rather than modular, so a wrapped value is a
 * value that could differ between compilers, and this library promises
 * the same bytes everywhere.
 */

#include "vorbis_internal.h"
#include "vorbis_tables.h"
#include <string.h>

/** Saturating addition. */
static int32_t add_sat(int32_t a, int32_t b) {
  int32_t sum;
  if (!__builtin_add_overflow(a, b, &sum)) {
    return sum;
  }
  return b < 0 ? INT32_MIN : INT32_MAX;
}

/** Saturating subtraction. */
static int32_t sub_sat(int32_t a, int32_t b) {
  int32_t difference;
  if (!__builtin_sub_overflow(a, b, &difference)) {
    return difference;
  }
  return b < 0 ? INT32_MAX : INT32_MIN;
}

/** @p a times a Q::VORBIS_TABLE_Q factor, rounded and saturated. */
static int32_t mul_q(int32_t a, int32_t factor) {
  int64_t product = (int64_t)a * factor;
  /* Rounded rather than truncated, and the rounding is away from zero on
   * a tie so that it does not depend on the sign representation. */
  int64_t half = (int64_t)1 << (VORBIS_TABLE_Q - 1);
  product = product >= 0 ? product + half : product - half;
  product >>= VORBIS_TABLE_Q;
  if (product > INT32_MAX) {
    return INT32_MAX;
  }
  if (product < INT32_MIN) {
    return INT32_MIN;
  }
  return (int32_t)product;
}

/**
 * How many halvings an @p points-wide transform applies.
 *
 * **The stages are numbered from zero and the halving is on the odd
 * ones**, so five stages halve twice and not three times - which is
 * `stages / 2` and not `(stages + 1) / 2`. The first draft had the
 * second, and it was wrong for exactly the block sizes whose log2 is
 * odd: 128, 512, 2,048 and 8,192 came out a factor of two off while 64,
 * 256, 1,024 and 4,096 were right, because for an even stage count the
 * two expressions agree. Half the block sizes passing is what a test
 * over one block size would have reported as success.
 */
static unsigned halvings(uint32_t points) {
  unsigned stages = 0;
  while ((1u << stages) < points) {
    ++stages;
  }
  /* One per two stages, which is what matches the square-root growth of
   * the transform on real audio. */
  return stages / 2u;
}

/**
 * An in-place complex fast Fourier transform of @p points values.
 *
 * Radix-2, decimation in time, with the bit reversal done first. The
 * twiddles are one shared table read at a stride: a stage of size `s`
 * wants `exp(-2*pi*i*k/s)` for k below s/2, and every such value is in
 * ::gaud_vorbis_fft_cos at stride ::VORBIS_FFT_TURN / s - so eight block
 * sizes and all their stages read one array.
 *
 * @param re Interleaved with @p im as one array of pairs.
 * @param points A power of two, at most ::VORBIS_FFT_TURN.
 */
static void fft(int32_t * values, uint32_t points) {
  /* Bit reversal. The loop carries the reversed index rather than
   * recomputing it, which is the standard trick and is exact. */
  uint32_t j = 0;
  for (uint32_t i = 1; i < points; ++i) {
    uint32_t bit = points >> 1;
    while (j & bit) {
      j ^= bit;
      bit >>= 1;
    }
    j |= bit;
    if (i < j) {
      int32_t re = values[2u * i];
      int32_t im = values[2u * i + 1u];
      values[2u * i] = values[2u * j];
      values[2u * i + 1u] = values[2u * j + 1u];
      values[2u * j] = re;
      values[2u * j + 1u] = im;
    }
  }

  unsigned stage = 0;
  for (uint32_t size = 2; size <= points; size *= 2u) {
    uint32_t half = size / 2u;
    uint32_t stride = VORBIS_FFT_TURN / size;
    /* Halve after every second stage: the transform grows by about
     * sqrt(2) per stage on real audio, so one bit per two stages tracks
     * it. The shift is applied to the *outputs* of the stage rather than
     * its inputs, so the rounding happens once per value. */
    bool halve = (stage & 1u) != 0;
    for (uint32_t start = 0; start < points; start += size) {
      for (uint32_t k = 0; k < half; ++k) {
        int32_t cosine = gaud_vorbis_fft_cos[k * stride];
        int32_t sine = gaud_vorbis_fft_sin[k * stride];
        int32_t * low = &values[2u * (start + k)];
        int32_t * high = &values[2u * (start + k + half)];
        int32_t re = sub_sat(mul_q(high[0], cosine), mul_q(high[1], sine));
        int32_t im = add_sat(mul_q(high[0], sine), mul_q(high[1], cosine));
        int32_t low_re = low[0];
        int32_t low_im = low[1];
        int32_t sum_re = add_sat(low_re, re);
        int32_t sum_im = add_sat(low_im, im);
        int32_t difference_re = sub_sat(low_re, re);
        int32_t difference_im = sub_sat(low_im, im);
        if (halve) {
          sum_re >>= 1;
          sum_im >>= 1;
          difference_re >>= 1;
          difference_im >>= 1;
        }
        low[0] = sum_re;
        low[1] = sum_im;
        high[0] = difference_re;
        high[1] = difference_im;
      }
    }
    ++stage;
  }
}

void gaud_vorbis_imdct(const int32_t * spectrum, uint32_t n,
    int32_t * scratch, int32_t * out) {
  uint32_t m = n / 2u;
  uint32_t points = n / 4u;
  unsigned log2n = 0;
  while ((1u << log2n) < n) {
    ++log2n;
  }
  const int32_t (* rotation)[2]
      = gaud_vorbis_rotations[log2n - VORBIS_LOG2_MIN_BLOCK];

  /*
   * The pre-rotation, and the pairing that makes the DCT-IV complex.
   *
   * The real sequence is read from both ends at once - line 2k and line
   * m-1-2k become the real and imaginary parts of one complex value -
   * which is what halves the transform. A decoder that paired them any
   * other way gets a transform of something else.
   */
  for (uint32_t k = 0; k < points; ++k) {
    int32_t re = spectrum[2u * k];
    int32_t im = spectrum[m - 1u - 2u * k];
    int32_t cosine = rotation[k][0];
    int32_t sine = rotation[k][1];
    scratch[2u * k] = sub_sat(mul_q(re, cosine), mul_q(im, sine));
    scratch[2u * k + 1u] = add_sat(mul_q(re, sine), mul_q(im, cosine));
  }

  fft(scratch, points);

  /*
   * The post-rotation, and the aliasing symmetry.
   *
   * The same rotation table, applied again, gives the DCT-IV's output
   * interleaved from both ends: the real part is line 2k and the
   * negated imaginary part is line m-1-2k. Writing it straight into the
   * four quarters of the output - two of them reversed, three of them
   * negated - is the inverse transform's own symmetry, and is why this
   * step costs no arithmetic beyond the rotation.
   */
  /*
   * The post-rotation, which produces the DCT-IV's output, and then the
   * aliasing symmetry, which is the inverse transform's.
   *
   * The same rotation table applied again gives the DCT-IV interleaved
   * from both ends: line 2k is the real part and line m-1-2k is the
   * negated imaginary part. Those go into the upper half of the scratch,
   * which the transform above has finished with - it used n/2 of the n
   * it was given, and this needs the other n/2.
   */
  int32_t * dct = scratch + m;
  for (uint32_t k = 0; k < points; ++k) {
    int32_t re = scratch[2u * k];
    int32_t im = scratch[2u * k + 1u];
    int32_t cosine = rotation[k][0];
    int32_t sine = rotation[k][1];
    dct[2u * k] = sub_sat(mul_q(re, cosine), mul_q(im, sine));
    dct[m - 1u - 2u * k]
        = sub_sat(0, add_sat(mul_q(re, sine), mul_q(im, cosine)));
  }

  /*
   * Four quarters, two of them read backwards and three negated. This is
   * the whole of step 1 of the factorisation and it costs no arithmetic:
   * written out as four loops rather than as one loop with a branch,
   * because the four ranges overlap in the index and a single pass over
   * them writes some outputs twice - which a first draft did.
   */
  uint32_t quarter = m / 2u;
  for (uint32_t i = 0; i < quarter; ++i) {
    out[i] = dct[quarter + i];
  }
  for (uint32_t i = 0; i < quarter; ++i) {
    out[quarter + i] = sub_sat(0, dct[m - 1u - i]);
  }
  for (uint32_t i = 0; i < quarter; ++i) {
    out[m + i] = sub_sat(0, dct[quarter - 1u - i]);
  }
  for (uint32_t i = 0; i < quarter; ++i) {
    out[m + quarter + i] = sub_sat(0, dct[i]);
  }
}

unsigned gaud_vorbis_imdct_shift(uint32_t n) {
  return halvings(n / 4u);
}
