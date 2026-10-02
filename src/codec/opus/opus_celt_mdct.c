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
 * CELT's inverse MDCT, and the fixed-point FFT underneath it.
 * Never installed.
 *
 * **An inverse MDCT of N points is a complex inverse FFT of N/4 points
 * with a rotation either side.** That is the whole structure here: a
 * pre-rotation that folds the real spectrum into a complex sequence,
 * an inverse FFT, a post-rotation, and then the windowing and
 * time-domain-alias-cancellation that make consecutive frames add up to
 * the original signal.
 *
 * **The FFT is mixed-radix and its factorisation is a table, not a
 * computation.** 480 points factor as 4 x 4 x 3 x 5 x 5, and which
 * order those radices are applied in is part of the format because it
 * decides where every rounding happens. The factors, the input
 * permutation and the twiddles all come out of RFC 6716 Appendix A.
 *
 * **The twiddles are the fixed-point cosine's output, not a rounded
 * real one.** `floor(0.5 + 32767*cos(phase))` - which is what kiss_fft's
 * own macro computes - disagrees with the shipped table by up to two
 * for a third of its entries. They are ::gaud_celt_cos_norm applied to
 * the phase, and tools/tables/gen_opus_tables.py checks all 960 of them
 * that way.
 *
 * **Only the inverse transform is here.** The forward FFT's butterflies
 * differ from the inverse's by more than a sign - `kf_bfly2` halves its
 * inputs where `ki_bfly2` does not - so leaving the encoder out leaves
 * out real code rather than a negation.
 */

#include "opus_celt.h"
#include "opus_celt_math.h"
#include <string.h>

/** The largest inverse FFT a frame needs: 1920/4. */
#define CELT_FFT_MAX 480

/** Radices the factorisation may use, as pairs; RFC 6716's MAXFACTORS. */
#define CELT_MAX_FACTORS 8

/** @brief One complex value of the transform, Q0 with 32-bit parts. */
typedef struct {
  int32_t r; ///< Real.
  int32_t i; ///< Imaginary.
} CELT_Complex;

/**
 * @brief Multiply a 32-bit value by a Q15 one, RFC 6716's `S_MUL`.
 *
 * @param value The 32-bit factor.
 * @param twiddle The Q15 factor.
 * @return Their product, back in the first factor's scale.
 */
static inline int32_t s_mul(int32_t value, int32_t twiddle) {
  return gaud_celt_shl32(
             gaud_celt_mult16_16(twiddle, value >> 16), 1)
      + (((int32_t)(int16_t)twiddle * (int32_t)(uint16_t)(uint32_t)value)
          >> 15);
}

/**
 * @brief The conjugate complex multiply the inverse butterflies use.
 *
 * RFC 6716's `C_MULC`: `a * conj(b)`.
 *
 * @param out Receives the product.
 * @param a The transform value.
 * @param twiddle_r The twiddle's real part, Q15.
 * @param twiddle_i Its imaginary part, Q15.
 */
static inline void mul_conj(CELT_Complex * out, CELT_Complex a,
    int32_t twiddle_r, int32_t twiddle_i) {
  int32_t real = s_mul(a.r, twiddle_r) + s_mul(a.i, twiddle_i);
  int32_t imaginary = s_mul(a.i, twiddle_r) - s_mul(a.r, twiddle_i);
  out->r = real;
  out->i = imaginary;
}

/**
 * @brief One scalar of a complex array read as a flat run.
 *
 * @param a The array.
 * @param index Twice the element index, plus one for the imaginary part.
 * @return That scalar.
 */
static inline int32_t flat_get(const CELT_Complex * a, int32_t index) {
  return (index & 1) ? a[index >> 1].i : a[index >> 1].r;
}

/**
 * @brief Write one scalar of a complex array read as a flat run.
 *
 * @param a The array.
 * @param index Twice the element index, plus one for the imaginary part.
 * @param value What to store.
 */
static inline void flat_set(CELT_Complex * a, int32_t index, int32_t value) {
  if (index & 1) {
    a[index >> 1].i = value;
  } else {
    a[index >> 1].r = value;
  }
}

/** @brief Everything one inverse FFT size needs. */
typedef struct {
  int nfft;                 ///< Points.
  int shift;                ///< Twiddle stride as a shift; -1 for none.
  const int16_t * factors;  ///< Sixteen entries: radix, remainder, ...
  const int16_t * bitrev;   ///< The input permutation.
} CELT_FftState;

/**
 * @brief The configuration for one of the four frame sizes.
 *
 * @param level 0 for the longest frame, 3 for the shortest.
 * @param out Receives it.
 */
static void fft_state(int level, CELT_FftState * out) {
  static const int16_t * const kBitrev[4] = {
    gaud_opus_fft_bitrev480, gaud_opus_fft_bitrev240,
    gaud_opus_fft_bitrev120, gaud_opus_fft_bitrev60
  };
  out->nfft = gaud_opus_fft_nfft[level];
  out->shift = gaud_opus_fft_shift[level];
  out->factors = gaud_opus_fft_factors + level * 16;
  out->bitrev = kBitrev[level];
}

/** The real part of the twiddle at @p index. */
#define TWIDDLE_R(index) ((int32_t)gaud_opus_fft_twiddles48000_960[2 * (index)])

/** The imaginary part of the twiddle at @p index. */
#define TWIDDLE_I(index) \
  ((int32_t)gaud_opus_fft_twiddles48000_960[2 * (index) + 1])

/**
 * @brief Radix-2 inverse butterfly.
 *
 * @param out The transform, in place.
 * @param stride Twiddle stride.
 * @param m Points per sub-transform.
 * @param n How many sub-transforms.
 * @param mm Stride between them.
 */
static void bfly2(CELT_Complex * out, size_t stride, int m, int n, int mm) {
  for (int i = 0; i < n; ++i) {
    CELT_Complex * first = out + (size_t)i * (size_t)mm;
    CELT_Complex * second = first + m;
    size_t twiddle = 0;
    for (int j = 0; j < m; ++j) {
      CELT_Complex t;
      mul_conj(&t, *second, TWIDDLE_R(twiddle), TWIDDLE_I(twiddle));
      twiddle += stride;
      second->r = first->r - t.r;
      second->i = first->i - t.i;
      first->r += t.r;
      first->i += t.i;
      ++second;
      ++first;
    }
  }
}

/**
 * @brief Radix-4 inverse butterfly.
 *
 * @param out The transform, in place.
 * @param stride Twiddle stride.
 * @param m Points per sub-transform.
 * @param n How many sub-transforms.
 * @param mm Stride between them.
 */
static void bfly4(CELT_Complex * out, size_t stride, int m, int n, int mm) {
  const size_t m2 = 2u * (size_t)m;
  const size_t m3 = 3u * (size_t)m;
  for (int i = 0; i < n; ++i) {
    CELT_Complex * f = out + (size_t)i * (size_t)mm;
    size_t t1 = 0;
    size_t t2 = 0;
    size_t t3 = 0;
    for (int j = 0; j < m; ++j) {
      CELT_Complex s0;
      CELT_Complex s1;
      CELT_Complex s2;
      CELT_Complex s3;
      CELT_Complex s4;
      CELT_Complex s5;
      mul_conj(&s0, f[m], TWIDDLE_R(t1), TWIDDLE_I(t1));
      mul_conj(&s1, f[m2], TWIDDLE_R(t2), TWIDDLE_I(t2));
      mul_conj(&s2, f[m3], TWIDDLE_R(t3), TWIDDLE_I(t3));
      s5.r = f->r - s1.r;
      s5.i = f->i - s1.i;
      f->r += s1.r;
      f->i += s1.i;
      s3.r = s0.r + s2.r;
      s3.i = s0.i + s2.i;
      s4.r = s0.r - s2.r;
      s4.i = s0.i - s2.i;
      f[m2].r = f->r - s3.r;
      f[m2].i = f->i - s3.i;
      t1 += stride;
      t2 += stride * 2u;
      t3 += stride * 3u;
      f->r += s3.r;
      f->i += s3.i;
      f[m].r = s5.r - s4.i;
      f[m].i = s5.i + s4.r;
      f[m3].r = s5.r + s4.i;
      f[m3].i = s5.i - s4.r;
      ++f;
    }
  }
}

/**
 * @brief Radix-3 inverse butterfly.
 *
 * @param out The transform, in place.
 * @param stride Twiddle stride.
 * @param m Points per sub-transform.
 * @param n How many sub-transforms.
 * @param mm Stride between them.
 */
static void bfly3(CELT_Complex * out, size_t stride, int m, int n, int mm) {
  const size_t m2 = 2u * (size_t)m;
  // The cube root of unity, taken from the same table rather than
  // written down: it is exactly the twiddle a third of the way round.
  int32_t epi3_i = TWIDDLE_I(stride * (size_t)m);
  for (int i = 0; i < n; ++i) {
    CELT_Complex * f = out + (size_t)i * (size_t)mm;
    size_t t1 = 0;
    size_t t2 = 0;
    for (int k = 0; k < m; ++k) {
      CELT_Complex s0;
      CELT_Complex s1;
      CELT_Complex s2;
      CELT_Complex s3;
      mul_conj(&s1, f[m], TWIDDLE_R(t1), TWIDDLE_I(t1));
      mul_conj(&s2, f[m2], TWIDDLE_R(t2), TWIDDLE_I(t2));
      s3.r = s1.r + s2.r;
      s3.i = s1.i + s2.i;
      s0.r = s1.r - s2.r;
      s0.i = s1.i - s2.i;
      t1 += stride;
      t2 += stride * 2u;
      f[m].r = f->r - (s3.r >> 1);
      f[m].i = f->i - (s3.i >> 1);
      s0.r = s_mul(s0.r, -epi3_i);
      s0.i = s_mul(s0.i, -epi3_i);
      f->r += s3.r;
      f->i += s3.i;
      f[m2].r = f[m].r + s0.i;
      f[m2].i = f[m].i - s0.r;
      f[m].r -= s0.i;
      f[m].i += s0.r;
      ++f;
    }
  }
}

/**
 * @brief Radix-5 inverse butterfly.
 *
 * @param out The transform, in place.
 * @param stride Twiddle stride.
 * @param m Points per sub-transform.
 * @param n How many sub-transforms.
 * @param mm Stride between them.
 */
static void bfly5(CELT_Complex * out, size_t stride, int m, int n, int mm) {
  int32_t ya_r = TWIDDLE_R(stride * (size_t)m);
  int32_t ya_i = TWIDDLE_I(stride * (size_t)m);
  int32_t yb_r = TWIDDLE_R(stride * 2u * (size_t)m);
  int32_t yb_i = TWIDDLE_I(stride * 2u * (size_t)m);
  for (int i = 0; i < n; ++i) {
    CELT_Complex * f0 = out + (size_t)i * (size_t)mm;
    CELT_Complex * f1 = f0 + m;
    CELT_Complex * f2 = f0 + 2 * m;
    CELT_Complex * f3 = f0 + 3 * m;
    CELT_Complex * f4 = f0 + 4 * m;
    for (int u = 0; u < m; ++u) {
      CELT_Complex s[13];
      s[0] = *f0;
      mul_conj(&s[1], *f1, TWIDDLE_R((size_t)u * stride),
          TWIDDLE_I((size_t)u * stride));
      mul_conj(&s[2], *f2, TWIDDLE_R(2u * (size_t)u * stride),
          TWIDDLE_I(2u * (size_t)u * stride));
      mul_conj(&s[3], *f3, TWIDDLE_R(3u * (size_t)u * stride),
          TWIDDLE_I(3u * (size_t)u * stride));
      mul_conj(&s[4], *f4, TWIDDLE_R(4u * (size_t)u * stride),
          TWIDDLE_I(4u * (size_t)u * stride));
      s[7].r = s[1].r + s[4].r;
      s[7].i = s[1].i + s[4].i;
      s[10].r = s[1].r - s[4].r;
      s[10].i = s[1].i - s[4].i;
      s[8].r = s[2].r + s[3].r;
      s[8].i = s[2].i + s[3].i;
      s[9].r = s[2].r - s[3].r;
      s[9].i = s[2].i - s[3].i;
      f0->r += s[7].r + s[8].r;
      f0->i += s[7].i + s[8].i;
      s[5].r = s[0].r + s_mul(s[7].r, ya_r) + s_mul(s[8].r, yb_r);
      s[5].i = s[0].i + s_mul(s[7].i, ya_r) + s_mul(s[8].i, yb_r);
      s[6].r = -s_mul(s[10].i, ya_i) - s_mul(s[9].i, yb_i);
      s[6].i = s_mul(s[10].r, ya_i) + s_mul(s[9].r, yb_i);
      f1->r = s[5].r - s[6].r;
      f1->i = s[5].i - s[6].i;
      f4->r = s[5].r + s[6].r;
      f4->i = s[5].i + s[6].i;
      s[11].r = s[0].r + s_mul(s[7].r, yb_r) + s_mul(s[8].r, ya_r);
      s[11].i = s[0].i + s_mul(s[7].i, yb_r) + s_mul(s[8].i, ya_r);
      s[12].r = s_mul(s[10].i, yb_i) - s_mul(s[9].i, ya_i);
      s[12].i = -s_mul(s[10].r, yb_i) + s_mul(s[9].r, ya_i);
      f2->r = s[11].r + s[12].r;
      f2->i = s[11].i + s[12].i;
      f3->r = s[11].r - s[12].r;
      f3->i = s[11].i - s[12].i;
      ++f0;
      ++f1;
      ++f2;
      ++f3;
      ++f4;
    }
  }
}

/**
 * @brief The inverse FFT, RFC 6716's `opus_ifft`.
 *
 * Mixed radix, driven entirely by the factor table: a permutation of
 * the input followed by one butterfly pass per factor, outermost last.
 *
 * @param state Which size.
 * @param in The spectrum; must not alias @p out.
 * @param out Receives the result.
 */
static void inverse_fft(const CELT_FftState * state, const CELT_Complex * in,
    CELT_Complex * out) {
  int fstride[CELT_MAX_FACTORS + 1];
  int shift = state->shift > 0 ? state->shift : 0;
  int levels = 0;
  int m;
  for (int i = 0; i < state->nfft; ++i) {
    out[state->bitrev[i]] = in[i];
  }
  fstride[0] = 1;
  do {
    int radix = state->factors[2 * levels];
    m = state->factors[2 * levels + 1];
    fstride[levels + 1] = fstride[levels] * radix;
    ++levels;
  } while (m != 1);
  m = state->factors[2 * levels - 1];
  for (int i = levels - 1; i >= 0; --i) {
    int next = i != 0 ? state->factors[2 * i - 1] : 1;
    size_t stride = (size_t)fstride[i] << shift;
    switch (state->factors[2 * i]) {
      case 2: bfly2(out, stride, m, fstride[i], next); break;
      case 4: bfly4(out, stride, m, fstride[i], next); break;
      case 3: bfly3(out, stride, m, fstride[i], next); break;
      case 5: bfly5(out, stride, m, fstride[i], next); break;
      default: break;
    }
    m = next;
  }
}

void gaud_celt_imdct(const int32_t * in, int32_t * out, const int16_t * window,
    uint32_t overlap, int shift, uint32_t stride) {
  CELT_Complex scratch[CELT_FFT_MAX];
  CELT_Complex rotated[CELT_FFT_MAX];
  CELT_FftState state;
  int32_t n = 1920 >> shift;
  int32_t n2 = n >> 1;
  int32_t n4 = n >> 2;
  // sin(x) is close enough to x at this size that one multiply
  // substitutes for the whole extra rotation.
  int32_t sine = (int32_t)((25736 + n2) / n);
  int32_t half_overlap = (int32_t)overlap / 2;
  fft_state(shift, &state);

  // Pre-rotate: fold the real spectrum into n/4 complex points.
  for (int32_t i = 0; i < n4; ++i) {
    int32_t low = in[(size_t)(2 * i) * stride];
    int32_t high = in[(size_t)(n2 - 1 - 2 * i) * stride];
    int32_t a = (int32_t)gaud_opus_mdct_twiddles960[(size_t)i << shift];
    int32_t b = (int32_t)gaud_opus_mdct_twiddles960[(size_t)(n4 - i) << shift];
    int32_t yr = -s_mul(high, a) + s_mul(low, b);
    int32_t yi = -s_mul(high, b) - s_mul(low, a);
    rotated[i].r = yr - s_mul(yi, sine);
    rotated[i].i = yi + s_mul(yr, sine);
  }

  inverse_fft(&state, rotated, scratch);

  // Post-rotate, by the same angles the other way.
  for (int32_t i = 0; i < n4; ++i) {
    int32_t re = scratch[i].r;
    int32_t im = scratch[i].i;
    int32_t a = (int32_t)gaud_opus_mdct_twiddles960[(size_t)i << shift];
    int32_t b = (int32_t)gaud_opus_mdct_twiddles960[(size_t)(n4 - i) << shift];
    int32_t yr = s_mul(re, a) - s_mul(im, b);
    int32_t yi = s_mul(im, a) + s_mul(re, b);
    scratch[i].r = yr - s_mul(yi, sine);
    scratch[i].i = yi + s_mul(yr, sine);
  }

  // De-shuffle into the order the window expects. The reference walks
  // one pointer up through the array and one down, treating it as a
  // flat run of scalars; ::flat_get and ::flat_set say the same thing
  // without casting a struct to its first member's type, which is
  // undefined behaviour and which the aliasing gate here would catch.
  for (int32_t i = 0; i < n4; ++i) {
    flat_set(rotated, 2 * i, -flat_get(scratch, 2 * i));
    flat_set(rotated, 2 * i + 1, flat_get(scratch, n2 - 1 - 2 * i));
  }

  {
    // The output overlaps the previous frame's by `overlap` samples, so
    // it starts before the pointer the caller gave.
    int32_t * base = out - ((n2 - (int32_t)overlap) >> 1);
    // Mirror both halves: the first adds into what the previous frame
    // left there, which is the time-domain alias cancellation; the
    // second writes this frame's own tail for the next one to add to.
    for (int32_t i = 0; i < n4 - half_overlap; ++i) {
      base[n2 - 1 - i] = flat_get(rotated, n4 - 1 - i);
    }
    for (int32_t i = n4 - half_overlap; i < n4; ++i) {
      int32_t value = flat_get(rotated, n4 - 1 - i);
      int32_t at = i - (n4 - half_overlap);
      base[n4 - half_overlap + at] += -s_mul(value, window[at]);
      base[n2 - 1 - i] += s_mul(value, window[(int32_t)overlap - 1 - at]);
    }
    for (int32_t i = 0; i < n4 - half_overlap; ++i) {
      base[n2 + i] = flat_get(rotated, n4 + i);
    }
    for (int32_t i = n4 - half_overlap; i < n4; ++i) {
      int32_t value = flat_get(rotated, n4 + i);
      int32_t at = i - (n4 - half_overlap);
      base[n - 1 - (n4 - half_overlap) - at] = s_mul(value, window[at]);
      base[n2 + i] = s_mul(value, window[(int32_t)overlap - 1 - at]);
    }
  }
}
