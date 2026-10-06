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
  const uint64_t * threshold = gaud_mp3enc_quant_threshold;
  if (v < threshold[0]) {
    return 0;
  }
  /* A guess from logarithms - v^(3/4), rounded with the standard's offset -
   * and then the table decides: the guess is within a few of the answer,
   * so a walk of a few thresholds finds the exact one, where a search of
   * the whole table would take thirteen. */
  int32_t log2_v = gaud_mp3e_log2_q8(v) - MP3_Q * 256;
  uint32_t y = gaud_mp3e_exp2_q8(log2_v * 3 / 4);
  uint64_t guess = ((uint64_t)y + 26568u) >> 16;
  unsigned q = guess > QUANT_MAX ? QUANT_MAX : (unsigned)guess;
  /* q is the count of thresholds at or below v. */
  while (q > 0 && threshold[q - 1u] > v) {
    --q;
  }
  while (q < QUANT_MAX + 1u && threshold[q] <= v) {
    ++q;
  }
  int32_t result = (int32_t)(q > QUANT_MAX ? QUANT_MAX : q);
  return value < 0 ? -result : result;
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
  unsigned gain_floor; ///< The finest step that does not clamp the loudest line.
  MP3E_Granule scratch; ///< A second granule to try a step in.
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

/** Part 2 and an estimate of part 3 at @p gain, with @p g quantised. */
static uint32_t estimate_bits(const Work * w, unsigned gain, MP3E_Granule * g) {
  quantize_all(w, gain, g);
  memset(g->sf_long, 0, sizeof(g->sf_long));
  memset(g->sf_short, 0, sizeof(g->sf_short));
  memset(g->scfsi_reused, 0, sizeof(g->scfsi_reused));
  g->side.block_type = w->in->block_type;
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
  return g->part2_bits + gaud_mp3e_huffman_estimate(g->is);
}

/* The finest gain whose bits fit: bisect on the estimate, then let the
 * exact count - which is never more - claim a step or two back. */
static unsigned fit_gain(Work * w, uint32_t budget, MP3E_Granule * g,
    bool * out_fits) {
  unsigned lo = 0;
  unsigned hi = 255;
  while (lo < hi) {
    unsigned mid = (lo + hi) >> 1;
    if (estimate_bits(w, mid, g) <= budget) {
      hi = mid;
    }
    else {
      lo = mid + 1u;
    }
  }
  /* The exact plan at the estimate's answer, then finer while it fits. */
  quantize_all(w, lo, g);
  uint32_t bits = finish(w, lo, g);
  bool fits = bits <= budget;
  for (unsigned step = 0; fits && lo > 0 && step < 4u; ++step) {
    MP3E_Granule * probe = &w->scratch;
    quantize_all(w, lo - 1u, probe);
    uint32_t finer = finish(w, lo - 1u, probe);
    if (finer > budget) {
      break;
    }
    --lo;
    *g = *probe;
    bits = finer;
  }
  *out_fits = fits;
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

/** Bands louder than allowed when every run is quantised at @p gain. */
static unsigned masked_over(const Work * w, unsigned gain, MP3E_Granule * g) {
  quantize_all(w, gain, g);
  unsigned over = 0;
  for (unsigned r = 0; r < w->runs; ++r) {
    if (run_noise(w, r, gain, g) > w->in->allowed[r]) {
      ++over;
    }
  }
  return over;
}

/** The largest gain at which no band is over its threshold. The caller has
 *  shown the finest useful gain is masked. */
static unsigned coarsest_masked(const Work * w, MP3E_Granule * g) {
  unsigned low = w->gain_floor;
  unsigned high = 255;
  while (low < high) {
    unsigned mid = (low + high + 1u) >> 1;
    if (masked_over(w, mid, g) == 0u) {
      low = mid;
    }
    else {
      high = mid - 1u;
    }
  }
  return low;
}

/**
 * Each band's noise against its threshold, in octaves of energy (Q8): how
 * far over it is, or - negative - how far under. Bands whose signal is not
 * above their threshold at all need no bits and are left out, marked
 * INT32_MIN, because their margin is not something coding can change.
 *
 * @return how many bands are over the thresholds as given.
 */
static unsigned measure_excess(const Work * w, unsigned gain,
    const MP3E_Granule * g, int32_t * excess) {
  unsigned over = 0;
  for (unsigned r = 0; r < w->runs; ++r) {
    uint64_t noise = run_noise(w, r, gain, g);
    uint64_t allowed = w->in->allowed[r];
    if (noise > allowed) {
      ++over;
    }
    if (w->energy[r] <= allowed) {
      excess[r] = INT32_MIN;
      continue;
    }
    excess[r] = gaud_mp3e_log2_q8(noise ? noise : 1u)
        - gaud_mp3e_log2_q8(allowed ? allowed : 1u);
  }
  return over;
}

/**
 * Move each band's scalefactor toward making every band's margin the same.
 *
 * A band more than half an octave over the weighted median is amplified, a
 * band more than half an octave under it - and amplified before - is let
 * go. The deadband and the one-step limit are what keep this from
 * oscillating: the global gain is chosen afresh after every move, which
 * shifts every margin together.
 *
 * @return whether anything changed.
 */
static bool equalise(Work * w, const int32_t * excess) {
  int32_t value[MP3E_MAX_BANDS];
  unsigned weight[MP3E_MAX_BANDS];
  unsigned count = 0;
  unsigned total = 0;
  for (unsigned r = 0; r < w->runs; ++r) {
    if (excess[r] == INT32_MIN) {
      continue;
    }
    unsigned width = w->layout->band[r].width;
    /* Insertion sort by value; there are at most 39. */
    unsigned at = count++;
    while (at > 0 && value[at - 1u] > excess[r]) {
      value[at] = value[at - 1u];
      weight[at] = weight[at - 1u];
      --at;
    }
    value[at] = excess[r];
    weight[at] = width;
    total += width;
  }
  if (count == 0) {
    return false;
  }
  unsigned running = 0;
  int32_t median = value[count - 1u];
  for (unsigned i = 0; i < count; ++i) {
    running += weight[i];
    if (running * 2u >= total) {
      median = value[i];
      break;
    }
  }
  bool changed = false;
  for (unsigned r = 0; r < w->runs; ++r) {
    if (excess[r] == INT32_MIN) {
      continue;
    }
    if (excess[r] > median + 128 && w->sf[r] < w->sf_cap[r]) {
      ++w->sf[r];
      changed = true;
    }
    else if (excess[r] < median - 128 && w->sf[r] > 0) {
      --w->sf[r];
      changed = true;
    }
  }
  return changed;
}

/** The most the shaping loop may run before it settles for what it has. */
#define SHAPE_PASSES 8u

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
  /* Below some step the loudest line is clamped to the largest value a
   * granule can carry and its noise is not small: that is not a step to
   * search, however fine it sounds. */
  int32_t loudest = 0;
  for (unsigned i = 0; i < MP3E_LINES; ++i) {
    int32_t magnitude = w.xs[i] < 0 ? -w.xs[i] : w.xs[i];
    loudest = magnitude > loudest ? magnitude : loudest;
  }
  w.gain_floor = 0;
  while (w.gain_floor < 255u
      && gaud_mp3e_quantize_one(loudest, (int)w.gain_floor - 210) >= QUANT_MAX) {
    ++w.gain_floor;
  }

  MP3E_Granule * trial = out;
  uint32_t budget = input->target_bits;
  MP3E_Granule best;
  bool have_best = false;
  unsigned best_over = 0;
  int32_t best_worst = INT32_MAX;
  uint32_t best_bits = UINT32_MAX;

  /* The loop has two aims. With bits to use, it takes the finest step the
   * budget affords and evens out the margins, so the surplus is spread as an
   * equal noise-to-mask ratio instead of as noise that is the same size in
   * every band. Without, it takes the coarsest step that masks every band
   * and evens out the margins at that, which is the cheapest way to mask
   * them. Either way each pass moves the scalefactors toward equal margins
   * and chooses the step again. */
  bool masks = masked_over(&w, w.gain_floor, trial) == 0u;
  for (unsigned pass = 0; pass < SHAPE_PASSES; ++pass) {
    unsigned gain;
    bool fits = true;
    uint32_t bits;
    if (input->spend || !masks) {
      gain = fit_gain(&w, budget, trial, &fits);
      bits = trial->side.part2_3_length;
    }
    else {
      gain = coarsest_masked(&w, trial);
      quantize_all(&w, gain, trial);
      bits = finish(&w, gain, trial);
      fits = bits <= budget;
    }
    int32_t excess[MP3E_MAX_BANDS];
    unsigned over = measure_excess(&w, gain, trial, excess);
    int32_t worst = INT32_MIN;
    for (unsigned r = 0; r < w.runs; ++r) {
      if (excess[r] > worst) {
        worst = excess[r];
      }
    }
    if (worst == INT32_MIN) {
      worst = 0;
    }
    if (!fits) {
      if (!have_best) {
        best = *trial;
        best_over = over;
        have_best = true;
      }
      break;
    }
    bool better_state = !have_best
        || ((input->spend || !masks) ? worst < best_worst
                                      : bits < best_bits);
    if (better_state) {
      best = *trial;
      best_over = over;
      best_worst = worst;
      best_bits = bits;
      have_best = true;
    }
    if (!equalise(&w, excess)) {
      break;
    }
  }
  *out = best;
  return best_over;
}

/**
 * What a spectrum would cost to code with noise held to the thresholds: the
 * perceptual entropy, sum of width * log2(1 + sqrt(energy / allowed)) over
 * the bands that need any, in bits.
 */
uint32_t gaud_mp3e_estimate_bits(const MP3E_Layout * layout,
    const int32_t * spectrum, const uint64_t * allowed) {
  uint64_t total_q8 = 0;
  for (unsigned r = 0; r < layout->count; ++r) {
    const MP3E_Band * band = &layout->band[r];
    uint64_t energy = 0;
    for (unsigned i = band->start; i < (unsigned)band->start + band->width; ++i) {
      int64_t scaled = (int64_t)spectrum[layout->order[i]] / (1 << MP3E_ENERGY_SHIFT);
      energy += (uint64_t)(scaled * scaled);
    }
    uint64_t limit = allowed[r] ? allowed[r] : 1u;
    if (energy <= limit) {
      continue;
    }
    /* width * log2(1 + sqrt(energy / limit)), the perceptual entropy. */
    int32_t diff = gaud_mp3e_log2_q8(energy) - gaud_mp3e_log2_q8(limit);
    uint32_t root = gaud_mp3e_exp2_q8(diff / 2);
    int32_t bits_q8 = gaud_mp3e_log2_q8(65536u + (uint64_t)root) - 16 * 256;
    total_q8 += (uint64_t)band->width * (uint64_t)bits_q8;
  }
  return (uint32_t)(total_q8 >> 8);
}
