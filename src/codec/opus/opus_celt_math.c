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
 * CELT's fixed-point arithmetic. See opus_celt_math.h for why these are
 * not simply the library functions of the same names.
 *
 * Each follows the same shape: shift the argument into the one range
 * the polynomial was fitted over, evaluate it by Horner's method in 16
 * bits, and shift the result back by whatever the first shift was. The
 * coefficients are RFC 6716 Appendix A's and are not derived here,
 * because a coefficient that is a better fit is still the wrong answer.
 */

#include "opus_celt_math.h"

unsigned gaud_celt_isqrt32(uint32_t value) {
  unsigned bit;
  unsigned root = 0;
  int shift;
  // RFC 6716's version shifts by (ilog(0) - 1) >> 1 here, which is a
  // negative shift count; it is never called with zero and so never
  // reaches it. Answering zero keeps the undefined case out of the
  // build without changing any input the reference defines.
  if (value == 0u) {
    return 0u;
  }
  shift = ((int)gaud_opus_ilog(value) - 1) >> 1;
  bit = 1u << shift;
  do {
    uint32_t trial = (((uint32_t)root << 1) + bit) << shift;
    if (trial <= value) {
      root += bit;
      value -= trial;
    }
    bit >>= 1;
    --shift;
  } while (shift >= 0);
  return root;
}

int32_t gaud_celt_rcp(int32_t x) {
  int i = gaud_celt_ilog2(x);
  // n is Q15 over [0,1): the argument with its exponent taken out.
  int16_t n = (int16_t)(gaud_celt_vshr32(x, i - 15) - 32768);
  // A linear first guess, r = 1.88235 - 0.94118 * n, in Q14.
  int16_t r = gaud_celt_add16(30840, gaud_celt_mult16_16_q15(-15420, n));
  // Two Newton iterations of r -= r * ((r * n) - 1).
  r = gaud_celt_sub16(r,
      gaud_celt_mult16_16_q15(r,
          gaud_celt_add16(
              gaud_celt_mult16_16_q15(r, n), gaud_celt_add16(r, -32768))));
  // The second subtracts an extra one, which both keeps the result from
  // overflowing and offsets the truncation the rest of it accumulates.
  r = gaud_celt_sub16(r,
      gaud_celt_add16(1,
          gaud_celt_mult16_16_q15(r,
              gaud_celt_add16(gaud_celt_mult16_16_q15(r, n),
                  gaud_celt_add16(r, -32768)))));
  return gaud_celt_vshr32((int32_t)r, i - 16);
}

int32_t gaud_celt_div(int32_t a, int32_t b) {
  return gaud_celt_mult32_32_q31(a, gaud_celt_rcp(b));
}

int16_t gaud_celt_rsqrt_norm(int32_t x) {
  int16_t n;
  int16_t r;
  int16_t r2;
  int16_t y;
  // n runs over [-16384,32767], which is [-0.5,1) in Q15.
  n = (int16_t)(x - 32768);
  // The minimax quadratic r = 1.4378 + n * (-0.8234 + n * 0.40964), in
  // Q14, which is good to about three places on its own.
  r = gaud_celt_add16(23557,
      gaud_celt_mult16_16_q15(n, gaud_celt_add16(-13490,
          gaud_celt_mult16_16_q15(n, 6713))));
  // y is x*r*r-1 in Q15, reached from n and r rather than from x so
  // that the intermediate products stay inside 32 bits. It runs over
  // [-1564,1594].
  r2 = (int16_t)gaud_celt_mult16_16_q15(r, r);
  y = (int16_t)((uint16_t)(uint32_t)(int32_t)gaud_celt_sub16(
                    gaud_celt_add16(gaud_celt_mult16_16_q15(r2, n), r2), 16384)
      << 1);
  // One second-order Householder step, r += r*y*(y*0.375-0.5).
  return gaud_celt_add16(r,
      gaud_celt_mult16_16_q15(r,
          gaud_celt_mult16_16_q15(y,
              gaud_celt_sub16(gaud_celt_mult16_16_q15(y, 12288), 16384))));
}

int32_t gaud_celt_sqrt(int32_t x) {
  static const int16_t c[5] = {23175, 11561, -3011, 1699, -664};
  int k;
  int16_t n;
  int32_t root;
  if (x == 0) {
    return 0;
  }
  // Halving the exponent is what makes this a square root; the
  // polynomial only ever sees a mantissa.
  k = (gaud_celt_ilog2(x) >> 1) - 7;
  x = gaud_celt_vshr32(x, 2 * k);
  n = (int16_t)(x - 32768);
  root = gaud_celt_add16(c[0],
      gaud_celt_mult16_16_q15(n, gaud_celt_add16(c[1],
          gaud_celt_mult16_16_q15(n, gaud_celt_add16(c[2],
              gaud_celt_mult16_16_q15(n, gaud_celt_add16(c[3],
                  gaud_celt_mult16_16_q15(n, c[4]))))))));
  return gaud_celt_vshr32(root, 7 - k);
}

/**
 * @brief Cosine over the first quarter turn, which is all of it.
 *
 * @param x In Q15, from 0 to 32767.
 * @return Its cosine in Q15.
 */
static int16_t cos_pi_2(int16_t x) {
  int32_t x2 = gaud_celt_mult16_16_p15(x, x);
  int32_t p = gaud_celt_sub16(32767, x2)
      + gaud_celt_mult16_16_p15(x2,
            -7651 + gaud_celt_mult16_16_p15(x2,
                        8277 + gaud_celt_mult16_16_p15(-626, x2)));
  return gaud_celt_add16(1, p < 32766 ? p : 32766);
}

int16_t gaud_celt_cos_norm(int32_t x) {
  // Four quarter turns is the period, so only the low 17 bits matter,
  // and the second half is the first half reflected.
  x = x & 0x0001FFFF;
  if (x > 65536) {
    x = 131072 - x;
  }
  if ((x & 0x00007FFF) != 0) {
    if (x < 32768) {
      return cos_pi_2((int16_t)x);
    }
    return (int16_t)-cos_pi_2((int16_t)(65536 - x));
  }
  // On the quarter turns themselves the answer is exact, and saying so
  // is not an optimisation: the polynomial does not land on 32767 or 0.
  if ((x & 0x0000FFFF) != 0) {
    return 0;
  }
  if ((x & 0x0001FFFF) != 0) {
    return -32767;
  }
  return 32767;
}

int32_t gaud_celt_exp2(int16_t x) {
  int16_t frac;
  // The integer part becomes the final shift and the fraction goes
  // through the polynomial, which is the identity 2^(i+f) = 2^i * 2^f.
  int integer = x >> 10;
  if (integer > 14) {
    return 0x7F000000;
  }
  if (integer < -15) {
    return 0;
  }
  frac = (int16_t)((uint16_t)(uint32_t)(int32_t)gaud_celt_sub16(
                       x, (int16_t)((uint16_t)(uint32_t)integer << 10))
      << 4);
  // Fitted to K0 = 1, K1 = log(2), K2 = 3-4*log(2), K3 = 3*log(2)-2.
  frac = gaud_celt_add16(16383,
      gaud_celt_mult16_16_q15(frac, gaud_celt_add16(22804,
          gaud_celt_mult16_16_q15(frac, gaud_celt_add16(14819,
              gaud_celt_mult16_16_q15(10204, frac))))));
  return gaud_celt_vshr32((int32_t)frac, -integer - 2);
}

int16_t gaud_celt_bitexact_cos(int16_t x) {
  int16_t x2 = (int16_t)((4096 + (int32_t)x * (int32_t)x) >> 13);
  int32_t p = gaud_celt_sub16(32767, x2)
      + gaud_celt_frac_mul16(x2,
            -7651 + gaud_celt_frac_mul16(x2,
                        8277 + gaud_celt_frac_mul16(-626, x2)));
  return (int16_t)(1 + p);
}

int32_t gaud_celt_bitexact_log2tan(int32_t isin, int32_t icos) {
  int lc = (int)gaud_opus_ilog((uint32_t)icos);
  int ls = (int)gaud_opus_ilog((uint32_t)isin);
  icos = (int32_t)((uint32_t)icos << (15 - lc));
  isin = (int32_t)((uint32_t)isin << (15 - ls));
  return (ls - lc) * (1 << 11)
      + gaud_celt_frac_mul16(isin, gaud_celt_frac_mul16(isin, -2597) + 7932)
      - gaud_celt_frac_mul16(icos, gaud_celt_frac_mul16(icos, -2597) + 7932);
}
