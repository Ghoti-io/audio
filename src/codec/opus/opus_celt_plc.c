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
 * Concealing a lost CELT frame. RFC 6716 section 4.4. Never installed.
 *
 * Section 4.4 is informative: it says what a decoder *may* do about a
 * packet that did not arrive. What follows is what the reference does,
 * and it matters for more than lost packets, because the transition
 * between modes conceals a frame that was never lost to smooth over
 * the seam. So the decoder of a stream that loses nothing still calls
 * this, and its output depends on it exactly.
 *
 * **There are two concealments, and a count of frames lost in a row
 * picks between them.** For the first four the decoder repeats the last
 * pitch period it can find, shaped by a short-term predictor fitted to
 * the recent past and faded; from the fifth on, or whenever the band
 * range does not start at zero, it falls back to noise with the
 * spectral envelope the decoder has been tracking in the background,
 * because a pitch that has been repeated for 100 ms is no longer
 * speech but a buzz.
 *
 * Every function before the last is a piece of RFC 6716's pitch
 * analysis, which has an encoder half that this does not need: only the
 * downsampling and the search are here.
 */

#include "opus_celt.h"
#include "opus_celt_math.h"
#include "opus_tables.h"
#include <stdlib.h>
#include <string.h>

/** The longest period concealment looks back over, and its history. */
#define CELT_PLC_MAX_PERIOD 1024

/** Where in the history the concealment pitch search begins. */
#define CELT_PLC_SEARCH_OFFSET 720

/** The decoder's headroom: its samples carry twelve extra bits. */
#define CELT_SIG_SHIFT 12

/** Round a signal sample to 16 bits, without overflowing doing it. */
static int16_t round16(int32_t value, unsigned shift) {
  return (int16_t)(int32_t)(((uint32_t)value + ((1u << shift) >> 1))
      >> shift);
}

/** The reference's `frac_div32`: a 32-bit quotient from a 16-bit reciprocal. */
static int32_t frac_div32(int32_t a, int32_t b) {
  int shift = gaud_celt_ilog2(b) - 29;
  a = gaud_celt_vshr32(a, shift);
  b = gaud_celt_vshr32(b, shift);
  int16_t reciprocal = round16(gaud_celt_rcp(round16(b, 16)), 3);
  int32_t result = gaud_celt_shl32(gaud_celt_mult16_32_q15(reciprocal, a), 2);
  int32_t rem = gaud_celt_sub32(a, gaud_celt_mult32_32_q31(result, b));
  return gaud_celt_add32(result,
      gaud_celt_shl32(gaud_celt_mult16_32_q15(reciprocal, rem), 2));
}

/** A running sum that wraps rather than overflowing. */
static int32_t mac(int32_t sum, int32_t a, int32_t b) {
  return (int32_t)((uint32_t)sum + (uint32_t)(a * b));
}

/**
 * @brief Autocorrelation of up to 1024 samples, scaled to fit.
 *
 * @param x The signal.
 * @param ac Receives @p lag + 1 values.
 * @param window An overlap-sample window to taper both ends, or NULL.
 * @param overlap How long the taper is.
 * @param lag The highest lag.
 * @param n Samples.
 */
static void autocorr(const int16_t * x, int32_t * ac, const int16_t * window,
    int overlap, int lag, int n) {
  int16_t xx[CELT_PLC_MAX_PERIOD];
  memcpy(xx, x, (size_t)n * sizeof *xx);
  for (int i = 0; i < overlap; ++i) {
    xx[i] = (int16_t)gaud_celt_mult16_16_q15(x[i], window[i]);
    xx[n - i - 1] = (int16_t)gaud_celt_mult16_16_q15(x[n - i - 1], window[i]);
  }
  int32_t ac0 = 0;
  for (int i = 0; i < n; ++i) {
    ac0 = gaud_celt_add32(ac0, gaud_celt_mult16_16(xx[i], xx[i]) >> 9);
  }
  ac0 = gaud_celt_add32(ac0, 1 + n);
  // Scale so the correlation sums cannot overflow; the rounding of
  // the divide is towards zero, as in the reference.
  int shift = gaud_celt_ilog2(ac0) - 30 + 10;
  shift = (shift + 1) / 2;
  for (int i = 0; i < n; ++i) {
    xx[i] = (int16_t)gaud_celt_vshr32(xx[i], shift);
  }
  while (lag >= 0) {
    int32_t d = 0;
    for (int i = lag; i < n; ++i) {
      d = mac(d, xx[i], xx[i - lag]);
    }
    ac[lag] = d;
    --lag;
  }
  ac[0] = gaud_celt_add32(ac[0], 10);
}

/**
 * @brief Levinson-Durbin: predictor coefficients from autocorrelation.
 *
 * @param lpc Receives @p order Q12 coefficients.
 * @param ac The autocorrelation, @p order + 1 values.
 * @param order At most ::CELT_LPC_ORDER.
 */
static void lpc_from_autocorr(
    int16_t * lpc, const int32_t * ac, int order) {
  int32_t wide[CELT_LPC_ORDER];
  int32_t error = ac[0];
  for (int i = 0; i < order; ++i) {
    wide[i] = 0;
  }
  if (ac[0] != 0) {
    for (int i = 0; i < order; ++i) {
      // This iteration's reflection coefficient.
      int32_t rr = 0;
      for (int j = 0; j < i; ++j) {
        rr = gaud_celt_add32(rr, gaud_celt_mult32_32_q31(wide[j], ac[i - j]));
      }
      rr = gaud_celt_add32(rr, ac[i + 1] >> 3);
      int32_t r = -frac_div32(gaud_celt_shl32(rr, 3), error);
      wide[i] = r >> 3;
      for (int j = 0; j < ((i + 1) >> 1); ++j) {
        int32_t first = wide[j];
        int32_t second = wide[i - 1 - j];
        wide[j] = gaud_celt_add32(first, gaud_celt_mult32_32_q31(r, second));
        wide[i - 1 - j] =
            gaud_celt_add32(second, gaud_celt_mult32_32_q31(r, first));
      }
      error = gaud_celt_sub32(error,
          gaud_celt_mult32_32_q31(gaud_celt_mult32_32_q31(r, r), error));
      // Bail out once the filter has 30 dB of gain.
      if (error < (ac[0] >> 10)) {
        break;
      }
    }
  }
  for (int i = 0; i < order; ++i) {
    lpc[i] = round16(wide[i], 16);
  }
}

/** FIR filter, in place if @p x and @p y are the same. */
static void fir(const int16_t * x, const int16_t * num, int16_t * y, int n,
    int order, int16_t * memory) {
  for (int i = 0; i < n; ++i) {
    int32_t sum = gaud_celt_shl32(x[i], CELT_SIG_SHIFT);
    for (int j = 0; j < order; ++j) {
      sum = mac(sum, num[j], memory[j]);
    }
    for (int j = order - 1; j >= 1; --j) {
      memory[j] = memory[j - 1];
    }
    memory[0] = x[i];
    y[i] = round16(sum, CELT_SIG_SHIFT);
  }
}

/** All-pole filter, in place if @p x and @p y are the same. */
static void iir(const int32_t * x, const int16_t * den, int32_t * y, int n,
    int order, int16_t * memory) {
  for (int i = 0; i < n; ++i) {
    int32_t sum = x[i];
    for (int j = 0; j < order; ++j) {
      sum = (int32_t)((uint32_t)sum - (uint32_t)(den[j] * memory[j]));
    }
    for (int j = order - 1; j >= 1; --j) {
      memory[j] = memory[j - 1];
    }
    memory[0] = round16(sum, CELT_SIG_SHIFT);
    y[i] = sum;
  }
}

/** The two best pitch candidates by normalised correlation. */
static void find_best_pitch(const int32_t * xcorr, const int16_t * y, int len,
    int max_pitch, int * best_pitch, int yshift, int32_t maxcorr) {
  int32_t syy = 1;
  int16_t best_num[2] = {-1, -1};
  int32_t best_den[2] = {0, 0};
  int xshift = gaud_celt_ilog2(maxcorr) - 14;
  best_pitch[0] = 0;
  best_pitch[1] = 1;
  for (int j = 0; j < len; ++j) {
    syy = mac(syy, y[j], y[j]);
  }
  for (int i = 0; i < max_pitch; ++i) {
    if (xcorr[i] > 0) {
      int16_t xcorr16 = (int16_t)gaud_celt_vshr32(xcorr[i], xshift);
      int16_t num = (int16_t)gaud_celt_mult16_16_q15(xcorr16, xcorr16);
      if (gaud_celt_mult16_32_q15(num, best_den[1])
          > gaud_celt_mult16_32_q15(best_num[1], syy)) {
        if (gaud_celt_mult16_32_q15(num, best_den[0])
            > gaud_celt_mult16_32_q15(best_num[0], syy)) {
          best_num[1] = best_num[0];
          best_den[1] = best_den[0];
          best_pitch[1] = best_pitch[0];
          best_num[0] = num;
          best_den[0] = syy;
          best_pitch[0] = i;
        } else {
          best_num[1] = num;
          best_den[1] = syy;
          best_pitch[1] = i;
        }
      }
    }
    syy = gaud_celt_add32(syy,
        (gaud_celt_mult16_16(y[i + len], y[i + len]) >> yshift)
            - (gaud_celt_mult16_16(y[i], y[i]) >> yshift));
    syy = syy > 1 ? syy : 1;
  }
}

/** Downsample the decoder's history by two and whiten it for the search. */
static void pitch_downsample(const int32_t * const * x, int16_t * x_lp,
    int len, uint32_t channels) {
  int32_t ac[5];
  int16_t lpc[4];
  int16_t memory[4] = {0, 0, 0, 0};
  int16_t tmp = CELT_Q15ONE;
  int half = len >> 1;
  for (int c = 0; c < (int)channels; ++c) {
    const int32_t * s = x[c];
    for (int i = 1; i < half; ++i) {
      int32_t value = (gaud_celt_add32(
                           gaud_celt_add32(s[2 * i - 1], s[2 * i + 1]) >> 1,
                           s[2 * i])
                          >> 1)
          >> (CELT_SIG_SHIFT + 3);
      x_lp[i] = (int16_t)(c == 0 ? value : gaud_celt_add32(x_lp[i], value));
    }
    int32_t first = (gaud_celt_add32(s[1] >> 1, s[0]) >> 1)
        >> (CELT_SIG_SHIFT + 3);
    x_lp[0] = (int16_t)(c == 0 ? first : gaud_celt_add32(x_lp[0], first));
  }
  autocorr(x_lp, ac, NULL, 0, 4, half);
  // A noise floor at -40 dB, and a lag window.
  ac[0] = gaud_celt_add32(ac[0], ac[0] >> 13);
  for (int i = 1; i <= 4; ++i) {
    ac[i] = gaud_celt_sub32(
        ac[i], gaud_celt_mult16_32_q15(2 * i * i, ac[i]));
  }
  lpc_from_autocorr(lpc, ac, 4);
  for (int i = 0; i < 4; ++i) {
    tmp = (int16_t)gaud_celt_mult16_16_q15(29491, tmp);
    lpc[i] = (int16_t)gaud_celt_mult16_16_q15(lpc[i], tmp);
  }
  fir(x_lp, lpc, x_lp, half, 4, memory);
  memory[0] = 0;
  lpc[0] = 3277;
  fir(x_lp, lpc, x_lp, half, 1, memory);
}

static int16_t maxabs16(const int16_t * x, int len) {
  int32_t best = 0;
  for (int i = 0; i < len; ++i) {
    int32_t value = x[i] < 0 ? -(int32_t)x[i] : x[i];
    best = value > best ? value : best;
  }
  return (int16_t)best;
}

/** Find the pitch period, in the downsampled signal's own samples. */
static void pitch_search(const int16_t * x_lp, const int16_t * y, int len,
    int max_pitch, int * pitch) {
  int lag = len + max_pitch;
  int16_t x_lp4[CELT_PLC_MAX_PERIOD / 2];
  int16_t y_lp4[CELT_PLC_MAX_PERIOD];
  int32_t xcorr[CELT_PLC_MAX_PERIOD / 2];
  int best_pitch[2] = {0, 0};
  int32_t maxcorr = 1;
  int offset;

  for (int j = 0; j < len >> 2; ++j) {
    x_lp4[j] = x_lp[2 * j];
  }
  for (int j = 0; j < lag >> 2; ++j) {
    y_lp4[j] = y[2 * j];
  }
  int top = maxabs16(x_lp4, len >> 2);
  int other = maxabs16(y_lp4, lag >> 2);
  top = other > top ? other : top;
  int shift = gaud_celt_ilog2(top > 1 ? top : 1) - 11;
  if (shift > 0) {
    for (int j = 0; j < len >> 2; ++j) {
      x_lp4[j] = (int16_t)(x_lp4[j] >> shift);
    }
    for (int j = 0; j < lag >> 2; ++j) {
      y_lp4[j] = (int16_t)(y_lp4[j] >> shift);
    }
    // Twice the shift, for a multiply-accumulate.
    shift *= 2;
  } else {
    shift = 0;
  }

  // Coarse search at a quarter of the rate.
  for (int i = 0; i < (max_pitch >> 2); ++i) {
    int32_t sum = 0;
    for (int j = 0; j < (len >> 2); ++j) {
      sum = mac(sum, x_lp4[j], y_lp4[i + j]);
    }
    xcorr[i] = sum > -1 ? sum : -1;
    maxcorr = sum > maxcorr ? sum : maxcorr;
  }
  find_best_pitch(
      xcorr, y_lp4, len >> 2, max_pitch >> 2, best_pitch, 0, maxcorr);

  // Finer search at half the rate, near the coarse candidates only.
  maxcorr = 1;
  for (int i = 0; i < (max_pitch >> 1); ++i) {
    int32_t sum = 0;
    xcorr[i] = 0;
    if (abs(i - 2 * best_pitch[0]) > 2 && abs(i - 2 * best_pitch[1]) > 2) {
      continue;
    }
    for (int j = 0; j < (len >> 1); ++j) {
      sum = gaud_celt_add32(
          sum, gaud_celt_mult16_16(x_lp[j], y[i + j]) >> shift);
    }
    xcorr[i] = sum > -1 ? sum : -1;
    maxcorr = sum > maxcorr ? sum : maxcorr;
  }
  find_best_pitch(
      xcorr, y, len >> 1, max_pitch >> 1, best_pitch, shift, maxcorr);

  // Pseudo-interpolation between neighbouring lags.
  if (best_pitch[0] > 0 && best_pitch[0] < (max_pitch >> 1) - 1) {
    int32_t a = xcorr[best_pitch[0] - 1];
    int32_t b = xcorr[best_pitch[0]];
    int32_t c = xcorr[best_pitch[0] + 1];
    if (gaud_celt_sub32(c, a) > gaud_celt_mult16_32_q15(22938, gaud_celt_sub32(b, a))) {
      offset = 1;
    } else if (gaud_celt_sub32(a, c)
        > gaud_celt_mult16_32_q15(22938, gaud_celt_sub32(b, c))) {
      offset = -1;
    } else {
      offset = 0;
    }
  } else {
    offset = 0;
  }
  *pitch = 2 * best_pitch[0] - offset;
}

/** Noise with the background envelope: the fallback after four lost frames. */
static void conceal_with_noise(CELT_Decoder * st, const CELT_Mode * mode,
    uint32_t n, unsigned lm, CELT_Scratch * scratch, int32_t ** out_syn,
    int32_t ** overlap_mem) {
  uint32_t channels = st->channels;
  uint32_t eff_end = st->end < CELT_BANDS ? st->end : CELT_BANDS;
  uint32_t seed = st->rng;
  int16_t * x = scratch->shape;
  int32_t * freq = scratch->spectrum;

  if (st->loss_count >= 5) {
    gaud_celt_log2_amp(scratch->amplitude, st->background_log_e, st->start,
        st->end, channels);
  } else {
    // Energy decay: fast on the first lost frame, slow after.
    int16_t decay = st->loss_count == 0 ? 1536 : 512;
    for (uint32_t c = 0; c < channels; ++c) {
      for (uint32_t i = st->start; i < st->end; ++i) {
        st->old_band_e[c * CELT_BANDS + i] =
            (int16_t)(st->old_band_e[c * CELT_BANDS + i] - decay);
      }
    }
    gaud_celt_log2_amp(
        scratch->amplitude, st->old_band_e, st->start, st->end, channels);
  }
  for (uint32_t c = 0; c < channels; ++c) {
    for (uint32_t i = 0; i < ((uint32_t)mode->edges[st->start] << lm); ++i) {
      x[c * n + i] = 0;
    }
    for (uint32_t i = st->start; i < CELT_BANDS; ++i) {
      uint32_t boffs = n * c + ((uint32_t)mode->edges[i] << lm);
      uint32_t blen =
          ((uint32_t)(mode->edges[i + 1] - mode->edges[i])) << lm;
      for (uint32_t j = 0; j < blen; ++j) {
        seed = gaud_celt_lcg_rand(seed);
        x[boffs + j] = (int16_t)((int32_t)seed >> 20);
      }
      gaud_celt_renormalise_vector(x + boffs, blen, CELT_Q15ONE);
    }
    for (uint32_t i = ((uint32_t)mode->edges[st->end] << lm); i < n; ++i) {
      x[c * n + i] = 0;
    }
  }
  st->rng = seed;

  gaud_celt_denormalise_bands(
      mode, x, freq, scratch->amplitude, CELT_BANDS, channels);
  for (uint32_t c = 0; c < channels; ++c) {
    for (uint32_t i = 0; i < ((uint32_t)mode->edges[st->start] << lm); ++i) {
      freq[c * n + i] = 0;
    }
    uint32_t bound = (uint32_t)mode->edges[eff_end] << lm;
    if (st->downsample != 1) {
      uint32_t limit = n / (uint32_t)st->downsample;
      bound = bound < limit ? bound : limit;
    }
    for (uint32_t i = bound; i < n; ++i) {
      freq[c * n + i] = 0;
    }
  }
  for (uint32_t c = 0; c < channels; ++c) {
    int32_t * work = scratch->synthesis;
    memset(work, 0, CELT_OVERLAP * sizeof *work);
    gaud_celt_imdct(freq + c * n, work, gaud_opus_window120, CELT_OVERLAP,
        (int)(CELT_MAX_LM - lm), 1);
    for (uint32_t j = 0; j < CELT_OVERLAP; ++j) {
      out_syn[c][j] = gaud_celt_add32(work[j], overlap_mem[c][j]);
    }
    for (uint32_t j = CELT_OVERLAP; j < n; ++j) {
      out_syn[c][j] = work[j];
    }
    for (uint32_t j = 0; j < CELT_OVERLAP; ++j) {
      overlap_mem[c][j] = work[n + j];
    }
  }
}

/** Repeat the last pitch period, shaped and faded, for one channel. */
static void conceal_with_pitch(CELT_Decoder * st, uint32_t c, uint32_t n,
    int pitch_index, int16_t fade, int32_t * out_mem) {
  enum { kOverlap = CELT_OVERLAP, kMax = CELT_PLC_MAX_PERIOD };
  int16_t exc[kMax];
  int32_t e[kMax + 2 * kOverlap];
  int32_t ac[CELT_LPC_ORDER + 1];
  int16_t memory[CELT_LPC_ORDER];
  int16_t * lpc = st->lpc + c * CELT_LPC_ORDER;
  int32_t len = (int32_t)n + kOverlap;
  int16_t decay = 1;
  int32_t s1 = 0;
  int offset = kMax - pitch_index;

  memset(memory, 0, sizeof memory);
  for (int i = 0; i < kMax; ++i) {
    exc[i] = round16(out_mem[i], CELT_SIG_SHIFT);
  }
  if (st->loss_count == 0) {
    autocorr(exc, ac, gaud_opus_window120, kOverlap, CELT_LPC_ORDER, kMax);
    // A noise floor at -40 dB, and a lag window.
    ac[0] = gaud_celt_add32(ac[0], ac[0] >> 13);
    for (int i = 1; i <= CELT_LPC_ORDER; ++i) {
      ac[i] = gaud_celt_sub32(
          ac[i], gaud_celt_mult16_32_q15(2 * i * i, ac[i]));
    }
    lpc_from_autocorr(lpc, ac, CELT_LPC_ORDER);
  }
  for (int i = 0; i < CELT_LPC_ORDER; ++i) {
    memory[i] = round16(out_mem[kMax - 1 - i], CELT_SIG_SHIFT);
  }
  fir(exc, lpc, exc, kMax, CELT_LPC_ORDER, memory);

  // Is the waveform decaying, and how fast?
  {
    int32_t e1 = 1;
    int32_t e2 = 1;
    int period = pitch_index <= kMax / 2 ? pitch_index : kMax / 2;
    for (int i = 0; i < period; ++i) {
      e1 = gaud_celt_add32(e1,
          gaud_celt_mult16_16(exc[kMax - period + i], exc[kMax - period + i])
              >> 8);
      e2 = gaud_celt_add32(e2,
          gaud_celt_mult16_16(
              exc[kMax - 2 * period + i], exc[kMax - 2 * period + i])
              >> 8);
    }
    if (e1 > e2) {
      e1 = e2;
    }
    decay = (int16_t)gaud_celt_sqrt(frac_div32(e1 >> 1, e2));
  }

  // Copy the excitation, taking the decay into account.
  for (int32_t i = 0; i < len + kOverlap; ++i) {
    if (offset + i >= kMax) {
      offset -= pitch_index;
      decay = (int16_t)gaud_celt_mult16_16_q15(decay, decay);
    }
    e[i] = gaud_celt_shl32(
        gaud_celt_mult16_16_q15(decay, exc[offset + i]), CELT_SIG_SHIFT);
    int16_t tmp = round16(out_mem[offset + i], CELT_SIG_SHIFT);
    s1 = gaud_celt_add32(s1, gaud_celt_mult16_16(tmp, tmp) >> 8);
  }
  for (int i = 0; i < CELT_LPC_ORDER; ++i) {
    memory[i] = round16(out_mem[kMax - 1 - i], CELT_SIG_SHIFT);
  }
  for (int32_t i = 0; i < len + kOverlap; ++i) {
    e[i] = gaud_celt_mult16_32_q15(fade, e[i]);
  }
  iir(e, lpc, e, len + kOverlap, CELT_LPC_ORDER, memory);

  {
    int32_t s2 = 0;
    for (int32_t i = 0; i < len + kOverlap; ++i) {
      int16_t tmp = round16(e[i], CELT_SIG_SHIFT);
      s2 = gaud_celt_add32(s2, gaud_celt_mult16_16(tmp, tmp) >> 8);
    }
    // This checks for an "explosion" in the synthesis.
    if (!(s1 > (s2 >> 2))) {
      for (int32_t i = 0; i < len + kOverlap; ++i) {
        e[i] = 0;
      }
    } else if (s1 < s2) {
      int16_t ratio = (int16_t)gaud_celt_sqrt(
          frac_div32(gaud_celt_add32(s1 >> 1, 1), gaud_celt_add32(s2, 1)));
      for (int32_t i = 0; i < len + kOverlap; ++i) {
        e[i] = gaud_celt_mult16_32_q15(ratio, e[i]);
      }
    }
  }

  // Apply the post-filter to the previous frame's MDCT overlap.
  gaud_celt_comb_filter(out_mem + kMax, out_mem + kMax, st->postfilter_period,
      st->postfilter_period, kOverlap, st->postfilter_gain,
      st->postfilter_gain, st->postfilter_tapset, st->postfilter_tapset, NULL,
      0);
  for (int i = 0; i < kMax + kOverlap - (int)n; ++i) {
    out_mem[i] = out_mem[(int)n + i];
  }

  // Apply TDAC to the concealed audio so it blends with both neighbours.
  for (int i = 0; i < kOverlap / 2; ++i) {
    int32_t tmp = gaud_celt_add32(
        gaud_celt_mult16_32_q15(
            gaud_opus_window120[i], e[(int)n + kOverlap - 1 - i]),
        gaud_celt_mult16_32_q15(
            gaud_opus_window120[kOverlap - i - 1], e[(int)n + i]));
    out_mem[kMax + i] = gaud_celt_mult16_32_q15(
        gaud_opus_window120[kOverlap - i - 1], tmp);
    out_mem[kMax + kOverlap - i - 1] =
        gaud_celt_mult16_32_q15(gaud_opus_window120[i], tmp);
  }
  for (int i = 0; i < (int)n; ++i) {
    out_mem[kMax - (int)n + i] = e[i];
  }

  // Pre-filter the overlap for the next frame, whose post-filter undoes it.
  gaud_celt_comb_filter(e, out_mem + kMax, st->postfilter_period,
      st->postfilter_period, kOverlap, (int16_t)-st->postfilter_gain,
      (int16_t)-st->postfilter_gain, st->postfilter_tapset,
      st->postfilter_tapset, NULL, 0);
  for (int i = 0; i < kOverlap; ++i) {
    out_mem[kMax + i] = e[i];
  }
}

void gaud_celt_decode_lost(CELT_Decoder * st, int16_t * pcm, uint32_t n,
    unsigned lm, CELT_Scratch * scratch) {
  CELT_Mode mode;
  uint32_t channels = st->channels;
  int32_t * decode_mem[2];
  int32_t * out_mem[2];
  int32_t * overlap_mem[2];
  int32_t * out_syn[2];
  const int32_t * history[2];

  (void)gaud_celt_mode_init(&mode, lm);
  for (uint32_t c = 0; c < channels; ++c) {
    decode_mem[c] = st->decode_mem[c];
    out_mem[c] = decode_mem[c] + CELT_DECODE_HISTORY - CELT_PLC_MAX_PERIOD;
    overlap_mem[c] = decode_mem[c] + CELT_DECODE_HISTORY;
    out_syn[c] = out_mem[c] + CELT_PLC_MAX_PERIOD - n;
    history[c] = decode_mem[c];
  }

  if (st->loss_count >= 5 || st->start != 0) {
    conceal_with_noise(st, &mode, n, lm, scratch, out_syn, overlap_mem);
  } else {
    int pitch_index;
    int16_t fade = CELT_Q15ONE;
    if (st->loss_count == 0) {
      int16_t pitch_buf[CELT_DECODE_HISTORY >> 1];
      int offset = CELT_PLC_SEARCH_OFFSET;
      pitch_downsample(history, pitch_buf, CELT_DECODE_HISTORY, channels);
      // The longest period considered is 100 samples, which is 480 Hz.
      pitch_search(pitch_buf + (offset >> 1), pitch_buf,
          CELT_DECODE_HISTORY - offset, offset - 100, &pitch_index);
      pitch_index = offset - pitch_index;
      st->last_pitch_index = pitch_index;
    } else {
      pitch_index = st->last_pitch_index;
      fade = 26214;
    }
    for (uint32_t c = 0; c < channels; ++c) {
      conceal_with_pitch(st, c, n, pitch_index, fade, out_mem[c]);
    }
  }
  gaud_celt_deemphasis((const int32_t * const *)out_syn, pcm, (int)n,
      channels, st->downsample, st->preemph_memory);
  ++st->loss_count;
}
