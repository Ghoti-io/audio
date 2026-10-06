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
 * The quantiser and its two nested loops: how coarsely to quantise, and
 * which bands to quantise more finely than that.
 *
 * **The inner loop** answers "how coarse can the step be and still fit in
 * the bits". Bits fall as the global gain rises, so it is a bisection.
 *
 * **The outer loop** answers "which bands are too noisy for what the ear
 * would forgive". A band whose quantisation noise exceeds the threshold the
 * psychoacoustic model allows it is amplified - its scalefactor goes up
 * one, which narrows that band's step - and the inner loop runs again to
 * pay for it. It stops when every band is masked, when no band can be
 * amplified further, or when it has stopped getting better.
 *
 * Every number here is an integer. Noise and energy are sums of squares of
 * the lines' Q28 values scaled down by 2^6 first (::MP3E_ENERGY_SHIFT), so
 * that a band of a hundred lines at full scale is 2^53 and cannot wrap.
 */

#include "mp3enc_internal.h"

/** A bit count that nothing can meet. */
#define BITS_INF 0x7FFFFFFFu

/** The largest magnitude the table and the escape bits can carry. */
#define QUANT_MAX 8206

/** How many outer-loop passes before giving up. */
#define OUTER_LIMIT 40u

/* ------------------------------------------------------------ one value */

int32_t gaud_mp3e_quantize_one(int32_t value, int e4) {
  uint64_t magnitude = value < 0 ? (uint64_t)(-(int64_t)value) : (uint64_t)value;
  if (magnitude == 0) {
    return 0;
  }
  int whole = e4 >> 2;
  unsigned rest = (unsigned)(e4 & 3);
  uint64_t v = (magnitude * (uint64_t)gaud_mp3enc_gain_inverse[rest]) >> MP3_Q;
  const uint64_t cap = gaud_mp3enc_quant_threshold[QUANT_MAX] * 2u;
  if (whole >= 0) {
    v = whole >= 63 ? 0 : v >> whole;
  }
  else if (-whole >= 40 || v > (cap >> -whole)) {
    v = cap;
  }
  else {
    v <<= -whole;
  }
  /* The number of thresholds at or below v. */
  unsigned lo = 0;
  unsigned hi = QUANT_MAX + 1u;
  while (lo < hi) {
    unsigned mid = (lo + hi) >> 1;
    if (gaud_mp3enc_quant_threshold[mid] <= v) {
      lo = mid + 1u;
    }
    else {
      hi = mid;
    }
  }
  int32_t q = (int32_t)lo;
  return value < 0 ? -q : q;
}

int64_t gaud_mp3e_dequantize_one(int32_t quantized, int e4) {
  if (quantized == 0) {
    return 0;
  }
  int32_t magnitude = quantized < 0 ? -quantized : quantized;
  if (magnitude > QUANT_MAX) {
    magnitude = QUANT_MAX;
  }
  uint32_t packed = gaud_mp3_pow43[magnitude];
  int64_t mantissa = packed & 0xFFFFFFu;
  int exponent = (int)(packed >> 24);
  int whole = e4 >> 2;
  unsigned rest = (unsigned)(e4 & 3);
  int64_t scaled = mantissa * gaud_mp3_gain_frac[rest];
  int shift = 23 - exponent - whole;
  int64_t result;
  if (shift >= 0) {
    result = shift > 62 ? 0 : scaled >> shift;
  }
  else {
    result = -shift > 24 ? INT64_MAX / 2 : scaled << -shift;
  }
  return quantized < 0 ? -result : result;
}

/* ------------------------------------------------------------ the state */

typedef struct {
  const MP3E_Quant_Input * in;
  const MP3E_Layout * layout;
  unsigned row;
  MP3_Version version;
  unsigned runs;
  int32_t xs[MP3E_LINES];       ///< The spectrum in bitstream order.
  int sf[MP3E_MAX_BANDS];       ///< Scalefactor per run.
  int sf_cap[MP3E_MAX_BANDS];   ///< The most each may be.
  uint64_t energy[MP3E_MAX_BANDS];
  bool preflag;
  bool scale;
} Work;

static int pretab_for(const Work * w, unsigned run) {
  if (!w->preflag || w->layout->is_short) {
    return 0;
  }
  return gaud_mp3_pretab[w->layout->band[run].sfb];
}

/** The step of run @p run, in quarter octaves of amplitude. */
static int step_of(const Work * w, unsigned run, unsigned global_gain) {
  return (int)global_gain - 210 - (w->scale ? 4 : 2) * (w->sf[run] + pretab_for(w, run));
}

/** Quantise every run at @p global_gain into @p g. */
static void quantize_all(const Work * w, unsigned global_gain, MP3E_Granule * g) {
  for (unsigned r = 0; r < w->runs; ++r) {
    const MP3E_Band * band = &w->layout->band[r];
    int e4 = step_of(w, r, global_gain);
    for (unsigned i = band->start; i < (unsigned)band->start + band->width; ++i) {
      g->is[i] = gaud_mp3e_quantize_one(w->xs[i], e4);
    }
  }
  for (unsigned i = w->layout->band[w->runs - 1u].start
           + w->layout->band[w->runs - 1u].width;
      i < MP3E_LINES; ++i) {
    g->is[i] = 0;
  }
}

/** Fill the side information and plan the coding; the bits, or BITS_INF. */
static uint32_t finish(const Work * w, unsigned global_gain, MP3E_Granule * g) {
  MP3_Granule * side = &g->side;
  memset(side, 0, sizeof(*side));
  side->global_gain = global_gain;
  side->block_type = w->in->block_type;
  side->window_switching = w->in->block_type != 0u;
  side->preflag = w->preflag;
  side->scalefac_scale = w->scale;
  memset(g->sf_long, 0, sizeof(g->sf_long));
  memset(g->sf_short, 0, sizeof(g->sf_short));
  memset(g->scfsi_reused, 0, sizeof(g->scfsi_reused));
  for (unsigned r = 0; r < w->runs; ++r) {
    const MP3E_Band * band = &w->layout->band[r];
    if (w->layout->is_short) {
      if (band->sfb < 12u) {
        g->sf_short[band->sfb][band->window] = (uint8_t)w->sf[r];
      }
    }
    else if (band->sfb < 21u) {
      g->sf_long[band->sfb] = (uint8_t)w->sf[r];
    }
  }
  if (!gaud_mp3e_scalefactor_plan(g, w->version)) {
    return BITS_INF;
  }
  gaud_mp3e_huffman_plan(g, w->layout, w->row);
  side->part2_3_length = g->part2_bits + g->part3_bits;
  return side->part2_3_length;
}

/* The finest gain whose bits fit. */
static unsigned fit_gain(const Work * w, uint32_t budget, MP3E_Granule * g,
    bool * out_fits) {
  unsigned lo = 0;
  unsigned hi = 255;
  /* Bisect for the smallest gain whose bits are within the budget. Bits
   * fall as the gain rises, so "fits" is monotone. */
  while (lo < hi) {
    unsigned mid = (lo + hi) >> 1;
    quantize_all(w, mid, g);
    uint32_t bits = finish(w, mid, g);
    if (bits <= budget) {
      hi = mid;
    }
    else {
      lo = mid + 1u;
    }
  }
  quantize_all(w, lo, g);
  uint32_t bits = finish(w, lo, g);
  *out_fits = bits <= budget;
  return lo;
}

/** Noise of run @p r with what @p g quantised it to. */
static uint64_t run_noise(const Work * w, unsigned r, unsigned global_gain,
    const MP3E_Granule * g) {
  const MP3E_Band * band = &w->layout->band[r];
  int e4 = step_of(w, r, global_gain);
  uint64_t noise = 0;
  for (unsigned i = band->start; i < (unsigned)band->start + band->width; ++i) {
    int64_t error = (int64_t)w->xs[i] - gaud_mp3e_dequantize_one(g->is[i], e4);
    int64_t scaled = error / (1 << MP3E_ENERGY_SHIFT);
    noise += (uint64_t)(scaled * scaled);
  }
  return noise;
}

/** How loud the bands are: the noise the quantiser must stay under. */
static void measure_energy(Work * w) {
  for (unsigned r = 0; r < w->runs; ++r) {
    const MP3E_Band * band = &w->layout->band[r];
    uint64_t e = 0;
    for (unsigned i = band->start; i < (unsigned)band->start + band->width; ++i) {
      int64_t scaled = (int64_t)w->xs[i] / (1 << MP3E_ENERGY_SHIFT);
      e += (uint64_t)(scaled * scaled);
    }
    w->energy[r] = e;
  }
}

/** Scalefactor caps by band, from what the field widths can carry. */
static void set_caps(Work * w) {
  for (unsigned r = 0; r < w->runs; ++r) {
    const MP3E_Band * band = &w->layout->band[r];
    int cap = 0;
    if (w->layout->is_short) {
      if (band->sfb < 12u) {
        cap = w->version == MP3_MPEG1 ? (band->sfb < 6u ? 15 : 7) : 15;
      }
    }
    else if (band->sfb < 21u) {
      cap = w->version == MP3_MPEG1 ? (band->sfb < 11u ? 15 : 7) : 15;
    }
    w->sf_cap[r] = cap;
  }
  if (w->version != MP3_MPEG1) {
    /* 13818-3's partitions: the first two are four bits wide, the last
     * two three. */
    const uint8_t * counts
        = gaud_mp3_lsf_nsfb[0][w->layout->is_short ? 1u : 0u];
    unsigned index = 0;
    for (unsigned p = 0; p < 4u; ++p) {
      for (unsigned n = 0; n < counts[p]; ++n, ++index) {
        if (index < w->runs) {
          w->sf_cap[index] = p < 2u ? 15 : 7;
        }
      }
    }
  }
}

/** How much worse @p a is than @p b: more bands over, or more over by. */
typedef struct {
  unsigned over;
  uint64_t excess; ///< Sum of octaves over, Q8.
} Score;

static bool better(const Score * a, const Score * b) {
  if (a->over != b->over) {
    return a->over < b->over;
  }
  return a->excess < b->excess;
}

unsigned gaud_mp3e_quantize_granule(const MP3E_Quant_Input * input,
    const MP3E_Layout * layout, unsigned row, MP3_Version version,
    MP3E_Granule * out) {
  Work w;
  memset(&w, 0, sizeof(w));
  w.in = input;
  w.layout = layout;
  w.row = row;
  w.version = version;
  w.runs = layout->count;
  for (unsigned i = 0; i < MP3E_LINES; ++i) {
    w.xs[i] = input->spectrum[layout->order[i]];
  }
  measure_energy(&w);
  set_caps(&w);

  MP3E_Granule * trial = out;
  MP3E_Granule best;
  Score best_score = {UINT32_MAX, UINT64_MAX};
  bool have_best = false;
  uint32_t budget = input->target_bits;
  unsigned stale = 0;

  for (unsigned pass = 0; pass < OUTER_LIMIT; ++pass) {
    bool fits = false;
    unsigned gain = fit_gain(&w, budget, trial, &fits);

    Score score = {0, 0};
    int32_t worst = INT32_MIN;
    int32_t excess_q8[MP3E_MAX_BANDS];
    for (unsigned r = 0; r < w.runs; ++r) {
      uint64_t noise = run_noise(&w, r, gain, trial);
      uint64_t allowed = input->allowed[r];
      excess_q8[r] = INT32_MIN;
      if (noise > allowed) {
        ++score.over;
        int32_t over = gaud_mp3e_log2_q8(noise) - gaud_mp3e_log2_q8(allowed ? allowed : 1u);
        excess_q8[r] = over;
        score.excess += (uint64_t)(over > 0 ? over : 0);
        if (over > worst) {
          worst = over;
        }
      }
    }
    /* A granule that did not fit its budget at the coarsest step is worse
     * than any that did, however quiet. */
    if (!fits) {
      score.over += MP3E_MAX_BANDS;
    }
    if (!have_best || better(&score, &best_score)) {
      best = *trial;
      best_score = score;
      have_best = true;
      stale = 0;
    }
    else if (++stale >= 4u) {
      break;
    }
    if (score.over == 0 || !fits) {
      break;
    }

    /* Amplify the worst bands: those within half an octave of the worst. */
    bool changed = false;
    for (unsigned r = 0; r < w.runs; ++r) {
      if (excess_q8[r] != INT32_MIN && excess_q8[r] + 128 >= worst
          && w.sf[r] < w.sf_cap[r]) {
        ++w.sf[r];
        changed = true;
      }
    }
    if (!changed) {
      break;
    }
  }
  *out = best;
  return best_score.over >= MP3E_MAX_BANDS ? best_score.over - MP3E_MAX_BANDS
                                           : best_score.over;
}
