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
 * Indices into filters. RFC 6716 sections 4.2.7.4 to 4.2.7.6.
 * Never installed.
 *
 * The parse produced numbers that name things; this turns them into
 * the things. Nothing here reads the bitstream, which is what lets it
 * be checked by value rather than by range state.
 *
 * Three of the steps are more than a table lookup, and each is more
 * than it looks.
 *
 * **The gains are a running sum with a step that doubles.** A gain is
 * coded as a delta from the previous subframe's, and a delta that
 * would carry the index above a threshold counts double - so a frame
 * can cross the whole 64-step range in fewer symbols than the range
 * has steps. The index is then clamped, converted to a logarithm and
 * exponentiated. The clamp is not a safety net: it is reachable, and
 * an encoder relies on it being there.
 *
 * **The line spectral frequencies have to come out sorted and
 * spaced.** An LSF vector that is not strictly increasing is not a
 * filter at all, and one whose values are too close is a filter that
 * rings. The decoder is given a stage-1 vector that is already well
 * spaced and a residual that can ruin it, so section 4.2.7.5.4 runs a
 * repair pass: up to twenty attempts to move the closest pair apart
 * about their midpoint, and then a sort-and-clamp that always
 * succeeds. The twenty attempts are not a heuristic the decoder may
 * vary - the encoder ran the same ones.
 *
 * **Turning the frequencies into a filter can produce an unstable
 * one.** Section 4.2.7.5.6 converts through a cosine and two
 * polynomial recurrences, and the result is then tested for stability
 * and, up to sixteen times, flattened towards the unit circle until it
 * passes. Both loops are bounded by a count rather than by success,
 * and both counts are part of the format.
 */

#include "opus_silk.h"
#include "opus_silk_math.h"
#include "opus_silk_tables.h"
#include <string.h>

/** The scale a gain index is mapped onto, in Q16 per step. */
#define SILK_GAIN_SCALE_Q16 1907825

/** Where that scale starts, in Q7 decibels over two. */
#define SILK_GAIN_OFFSET 2090

/** How many quantization levels a gain index has. */
#define SILK_GAIN_LEVELS 64

/** The value a delta index of zero means. */
#define SILK_GAIN_DELTA_MIN (-4)

/** And the one its largest index means. */
#define SILK_GAIN_DELTA_MAX 36

/** The scale the NLSF-to-LPC conversion works in. */
#define SILK_LPC_QA 16

/** Attempts at spacing the LSFs before falling back to a sort. */
#define SILK_NLSF_REPAIRS 20

/** Attempts at flattening an unstable filter. */
#define SILK_LPC_REPAIRS 16

/** 1/10000 in Q30: the least prediction gain a filter may have. */
#define SILK_MIN_INV_GAIN_Q30 107374

/** 0.99975 in Q24: the largest reflection coefficient allowed. */
#define SILK_RC_LIMIT_QA 16773022

void gaud_silk_gains_dequant(int32_t * gains_q16, const int8_t * indices,
    int8_t * previous, bool conditional, int subframes) {
  for (int k = 0; k < subframes; ++k) {
    if (k == 0 && !conditional) {
      // An absolute gain may still not drop more than sixteen steps,
      // about 21.8 dB, below the last one. The encoder knows that and
      // codes against it.
      *previous = (int8_t)(indices[k] > *previous - 16 ? indices[k]
                                                       : *previous - 16);
    } else {
      int delta = indices[k] + SILK_GAIN_DELTA_MIN;
      // Above this point a delta counts double, so that the top of the
      // range is reachable in the same number of symbols as the
      // middle. The threshold moves with the current index, which is
      // what keeps the doubling from being a discontinuity.
      int threshold = 2 * SILK_GAIN_DELTA_MAX - SILK_GAIN_LEVELS + *previous;
      *previous = (int8_t)(*previous
          + (delta > threshold ? 2 * delta - threshold : delta));
    }
    *previous = (int8_t)gaud_silk_limit(*previous, 0, SILK_GAIN_LEVELS - 1);
    int32_t log_q7 = gaud_silk_smulwb(SILK_GAIN_SCALE_Q16, *previous)
        + SILK_GAIN_OFFSET;
    // 3967 is 31 in Q7: the logarithm is capped so the gain fits.
    gains_q16[k] = gaud_silk_log2lin(log_q7 < 3967 ? log_q7 : 3967);
  }
}

/**
 * @brief Undo the backwards prediction of the LSF residual.
 *
 * Section 4.2.7.5.3. The residual is coded last coefficient first,
 * each predicted from the one above it, so this runs downwards.
 *
 * @param residual_q10 Receives the residual.
 * @param indices The decoded stage-2 indices.
 * @param weights The per-coefficient prediction weights, Q8.
 * @param step_q16 The codebook's quantization step.
 * @param order 10 or 16.
 */
static void nlsf_residual_dequant(int16_t * residual_q10,
    const int8_t * indices, const unsigned char * weights, int32_t step_q16,
    int order) {
  int32_t out_q10 = 0;
  for (int i = order - 1; i >= 0; --i) {
    int32_t predicted = gaud_silk_smulbb(out_q10, (int16_t)weights[i]) >> 8;
    out_q10 = gaud_silk_shl32(indices[i], 10);
    // The quantizer's levels are not evenly spaced: every non-zero
    // level is pulled a tenth of a step towards zero, which is what
    // makes the dead zone around zero wider than one step.
    if (out_q10 > 0) {
      out_q10 -= 102;
    } else if (out_q10 < 0) {
      out_q10 += 102;
    }
    out_q10 = gaud_silk_smlawb(predicted, out_q10, step_q16);
    residual_q10[i] = (int16_t)out_q10;
  }
}

/**
 * @brief How much each coefficient's error matters, from its spacing.
 *
 * Laroia's weighting: a coefficient wedged between two others is
 * harder to hear moved, so the residual it is given is scaled down.
 * The decoder needs this because the encoder quantized the residual
 * after dividing by it.
 *
 * @param weights_q2 Receives one weight per coefficient.
 * @param nlsf_q15 The stage-1 vector.
 * @param order 10 or 16, and even.
 */
static void nlsf_weights(
    int16_t * weights_q2, const int16_t * nlsf_q15, int order) {
  int32_t below;
  int32_t above;
  // The first and last coefficients are spaced against 0 and 1 rather
  // than against a neighbour, which is why they are not in the loop.
  below = nlsf_q15[0] > 1 ? nlsf_q15[0] : 1;
  below = (1 << 17) / below;
  above = nlsf_q15[1] - nlsf_q15[0] > 1 ? nlsf_q15[1] - nlsf_q15[0] : 1;
  above = (1 << 17) / above;
  weights_q2[0] = (int16_t)(below + above > 32767 ? 32767 : below + above);
  for (int k = 1; k < order - 1; k += 2) {
    below = nlsf_q15[k + 1] - nlsf_q15[k] > 1 ? nlsf_q15[k + 1] - nlsf_q15[k]
                                              : 1;
    below = (1 << 17) / below;
    weights_q2[k] = (int16_t)(below + above > 32767 ? 32767 : below + above);
    above = nlsf_q15[k + 2] - nlsf_q15[k + 1] > 1
        ? nlsf_q15[k + 2] - nlsf_q15[k + 1]
        : 1;
    above = (1 << 17) / above;
    weights_q2[k + 1] =
        (int16_t)(below + above > 32767 ? 32767 : below + above);
  }
  below = (1 << 15) - nlsf_q15[order - 1] > 1
      ? (1 << 15) - nlsf_q15[order - 1]
      : 1;
  below = (1 << 17) / below;
  weights_q2[order - 1] =
      (int16_t)(below + above > 32767 ? 32767 : below + above);
}

/** Sort ascending, in place; the fallback path's last step. */
static void insertion_sort(int16_t * values, int count) {
  for (int i = 1; i < count; ++i) {
    int16_t value = values[i];
    int j = i - 1;
    while (j >= 0 && values[j] > value) {
      values[j + 1] = values[j];
      --j;
    }
    values[j + 1] = value;
  }
}

/**
 * @brief Force the LSFs apart until they are a filter. Section 4.2.7.5.4.
 *
 * @param nlsf_q15 Adjusted in place.
 * @param spacing The minimum gaps, including one below the first and
 *   one above the last, so @p order + 1 of them.
 * @param order 10 or 16.
 */
static void nlsf_stabilize(
    int16_t * nlsf_q15, const int16_t * spacing, int order) {
  for (int attempt = 0; attempt < SILK_NLSF_REPAIRS; ++attempt) {
    // The worst gap, counting the one below the first coefficient and
    // the one above the last.
    int32_t worst = nlsf_q15[0] - spacing[0];
    int where = 0;
    for (int i = 1; i <= order - 1; ++i) {
      int32_t gap = nlsf_q15[i] - (nlsf_q15[i - 1] + spacing[i]);
      if (gap < worst) {
        worst = gap;
        where = i;
      }
    }
    int32_t top = (1 << 15) - (nlsf_q15[order - 1] + spacing[order]);
    if (top < worst) {
      worst = top;
      where = order;
    }
    if (worst >= 0) {
      return;
    }
    if (where == 0) {
      nlsf_q15[0] = spacing[0];
    } else if (where == order) {
      nlsf_q15[order - 1] = (int16_t)((1 << 15) - spacing[order]);
    } else {
      // Move the pair apart about their midpoint, but not so far that
      // everything below or above them stops fitting. Those two
      // limits are the sum of the gaps on either side.
      int32_t lowest = 0;
      for (int k = 0; k < where; ++k) {
        lowest += spacing[k];
      }
      lowest += spacing[where] >> 1;
      int32_t highest = 1 << 15;
      for (int k = order; k > where; --k) {
        highest -= spacing[k];
      }
      highest -= spacing[where] >> 1;
      int32_t centre = gaud_silk_limit(
          gaud_silk_rshift_round(
              (int32_t)nlsf_q15[where - 1] + (int32_t)nlsf_q15[where], 1),
          lowest, highest);
      nlsf_q15[where - 1] = (int16_t)(centre - (spacing[where] >> 1));
      nlsf_q15[where] = (int16_t)(nlsf_q15[where - 1] + spacing[where]);
    }
  }
  // Twenty attempts can fail - moving one pair apart can close
  // another - so the fallback sorts and then walks the whole vector
  // once, which always succeeds and sounds worse.
  //
  // **The forward pass saturates at 32767, as RFC 8251 section 7
  // requires.** RFC 6716's reference wrote `NLSF[i-1] + spacing[i]` into
  // an int16 unchecked: when the vector had been pushed up against
  // 32767 the sum wrapped, the last coefficient came out at -32766, and
  // `silk_NLSF2A` then indexed its 129-entry cosine table at -128. That
  // is reachable from a legal bitstream - it needs a stage-2 index of
  // five or more, which means the extension symbol, and it happened in
  // 1,706 of 256,000 synthetic vectors - and this decoder removed the
  // wrap with a ceiling of its own before RFC 8251 stated the fix. The
  // fix is a saturating add, which is what this is now.
  insertion_sort(nlsf_q15, order);
  if (nlsf_q15[0] < spacing[0]) {
    nlsf_q15[0] = spacing[0];
  }
  for (int i = 1; i < order; ++i) {
    int32_t least = gaud_silk_sat16(nlsf_q15[i - 1] + spacing[i]);
    if (nlsf_q15[i] < least) {
      nlsf_q15[i] = (int16_t)least;
    }
  }
  int32_t ceiling = (1 << 15) - spacing[order];
  if (nlsf_q15[order - 1] > ceiling) {
    nlsf_q15[order - 1] = (int16_t)ceiling;
  }
  for (int i = order - 2; i >= 0; --i) {
    int32_t limit = nlsf_q15[i + 1] - spacing[i + 1];
    if (nlsf_q15[i] > limit) {
      nlsf_q15[i] = (int16_t)limit;
    }
  }
}

void gaud_silk_nlsf_decode(
    int16_t * nlsf_q15, const int8_t * indices, bool wideband) {
  int order = wideband ? 16 : 10;
  const unsigned char * codebook = wideband ? gaud_opus_silk_nlsf_cb1_wb_q8
                                            : gaud_opus_silk_nlsf_cb1_nb_mb_q8;
  const unsigned char * select = wideband ? gaud_opus_silk_nlsf_cb2_select_wb
                                          : gaud_opus_silk_nlsf_cb2_select_nb_mb;
  const unsigned char * predictors = wideband
      ? gaud_opus_silk_nlsf_pred_wb_q8
      : gaud_opus_silk_nlsf_pred_nb_mb_q8;
  const int16_t * spacing = wideband ? gaud_opus_silk_nlsf_delta_min_wb_q15
                                     : gaud_opus_silk_nlsf_delta_min_nb_mb_q15;
  // 0.15 and 0.18 in Q16: the wideband codebook is finer because it
  // has more coefficients to place in the same interval.
  int32_t step_q16 = wideband ? 9830 : 11796;
  unsigned char weights_q8[SILK_MAX_LPC_ORDER];
  int16_t residual_q10[SILK_MAX_LPC_ORDER];
  int16_t laroia_q2[SILK_MAX_LPC_ORDER];

  const unsigned char * entry = codebook + (size_t)indices[0] * (size_t)order;
  for (int i = 0; i < order; ++i) {
    nlsf_q15[i] = (int16_t)gaud_silk_shl32(entry[i], 7);
  }
  // The other half of silk_NLSF_unpack: the parse took the
  // distribution out of each select byte and this takes the predictor.
  const unsigned char * row =
      select + (size_t)indices[0] * (size_t)(order / 2);
  for (int i = 0; i < order; i += 2) {
    unsigned packed = *row++;
    weights_q8[i] = predictors[i + (packed & 1u) * (unsigned)(order - 1)];
    weights_q8[i + 1] =
        predictors[i + ((packed >> 4) & 1u) * (unsigned)(order - 1) + 1];
  }
  nlsf_residual_dequant(
      residual_q10, indices + 1, weights_q8, step_q16, order);
  nlsf_weights(laroia_q2, nlsf_q15, order);
  for (int i = 0; i < order; ++i) {
    // The weights are applied as an inverse square root, because the
    // encoder minimised a weighted *squared* error.
    int32_t root_q9 =
        gaud_silk_sqrt_approx(gaud_silk_shl32(laroia_q2[i], 16));
    int32_t value = gaud_silk_add32(
        nlsf_q15[i], gaud_silk_shl32(residual_q10[i], 14) / root_q9);
    nlsf_q15[i] = (int16_t)gaud_silk_limit(value, 0, 32767);
  }
  nlsf_stabilize(nlsf_q15, spacing, order);
}

/**
 * @brief One of the two polynomials whose roots are the LSFs.
 *
 * Each pair of frequencies contributes a quadratic factor, and this
 * convolves them one at a time. The coefficients are symmetric, so
 * only half of them are formed.
 *
 * @param out Receives @p half + 1 coefficients in Q16.
 * @param cosines Twice the cosine of every other frequency, Q16.
 * @param half Half the filter order.
 */
static void nlsf2a_find_poly(
    int32_t * out, const int32_t * cosines, int half) {
  out[0] = gaud_silk_shl32(1, SILK_LPC_QA);
  out[1] = -cosines[0];
  for (int k = 1; k < half; ++k) {
    int32_t cosine = cosines[2 * k];
    out[k + 1] = gaud_silk_shl32(out[k - 1], 1)
        - (int32_t)gaud_silk_rshift_round64(
            (int64_t)cosine * (int64_t)out[k], SILK_LPC_QA);
    for (int n = k; n > 1; --n) {
      out[n] += out[n - 2]
          - (int32_t)gaud_silk_rshift_round64(
              (int64_t)cosine * (int64_t)out[n - 1], SILK_LPC_QA);
    }
    out[1] -= cosine;
  }
}

/** Pull every pole towards the origin by a factor that compounds. */
static void bandwidth_expand(int32_t * coefficients, int order,
    int32_t chirp_q16) {
  int32_t step = chirp_q16 - 65536;
  for (int i = 0; i < order - 1; ++i) {
    coefficients[i] = gaud_silk_smulww(chirp_q16, coefficients[i]);
    chirp_q16 += gaud_silk_rshift_round(chirp_q16 * step, 16);
  }
  coefficients[order - 1] = gaud_silk_smulww(chirp_q16, coefficients[order - 1]);
}

int32_t gaud_silk_lpc_inverse_gain(const int16_t * lpc_q12, int order) {
  int32_t work[2][SILK_MAX_LPC_ORDER];
  int32_t direct = 0;
  int32_t * current = work[order & 1];
  for (int k = 0; k < order; ++k) {
    direct += lpc_q12[k];
    current[k] = gaud_silk_shl32(lpc_q12[k], 24 - 12);
  }
  // A filter whose coefficients sum to one or more has a pole at DC;
  // the full recursion would find that too, but this costs nothing
  // and the case is common enough to be worth short-circuiting.
  if (direct >= 4096) {
    return 0;
  }
  // Step the Levinson recursion backwards, recovering one reflection
  // coefficient at a time. The product of 1 - k^2 over all of them is
  // the inverse prediction gain, and a reflection coefficient at or
  // beyond one means an unstable filter.
  int32_t inverse_gain_q30 = 1 << 30;
  for (int k = order - 1; k > 0; --k) {
    if (current[k] > SILK_RC_LIMIT_QA || current[k] < -SILK_RC_LIMIT_QA) {
      return 0;
    }
    int32_t rc_q31 = -gaud_silk_shl32(current[k], 31 - 24);
    int32_t factor_q30 = ((int32_t)1 << 30) - gaud_silk_smmul(rc_q31, rc_q31);
    int scale = 32
        - gaud_silk_clz32((uint32_t)(factor_q30 > 0 ? factor_q30
                                                    : -factor_q30));
    int32_t inverse = gaud_silk_inverse32_varq(factor_q30, scale + 30);
    inverse_gain_q30 =
        gaud_silk_shl32(gaud_silk_smmul(inverse_gain_q30, factor_q30), 2);
    int32_t * previous = current;
    current = work[k & 1];
    for (int n = 0; n < k; ++n) {
      // RFC 8251 section 6: a bitstream can push these past 32 bits, so
      // the difference saturates and a product that does not fit marks
      // the filter unstable instead of wrapping.
      int32_t product = (int32_t)gaud_silk_rshift_round64(
          (int64_t)previous[k - n - 1] * (int64_t)rc_q31, 31);
      int64_t difference = (int64_t)previous[n] - (int64_t)product;
      int32_t step = difference > INT32_MAX
          ? INT32_MAX
          : (difference < INT32_MIN ? INT32_MIN : (int32_t)difference);
      int64_t scaled = gaud_silk_rshift_round64(
          (int64_t)step * (int64_t)inverse, (unsigned)scale);
      if (scaled > INT32_MAX || scaled < INT32_MIN) {
        return 0;
      }
      current[n] = (int32_t)scaled;
    }
  }
  if (current[0] > SILK_RC_LIMIT_QA || current[0] < -SILK_RC_LIMIT_QA) {
    return 0;
  }
  int32_t rc_q31 = -gaud_silk_shl32(current[0], 31 - 24);
  int32_t factor_q30 = ((int32_t)1 << 30) - gaud_silk_smmul(rc_q31, rc_q31);
  return gaud_silk_shl32(gaud_silk_smmul(inverse_gain_q30, factor_q30), 2);
}

void gaud_silk_nlsf_to_lpc(
    int16_t * lpc_q12, const int16_t * nlsf_q15, int order) {
  const unsigned char * ordering = order == 16
      ? gaud_opus_silk_nlsf_ordering16
      : gaud_opus_silk_nlsf_ordering10;
  // Every entry is written below, because the ordering is a
  // permutation - which a test asserts. The initialiser is here
  // because the compiler cannot see that, and leaving it out would
  // mean reading uninitialised memory if the table were ever wrong.
  int32_t cosines[SILK_MAX_LPC_ORDER] = {0};
  int32_t even[SILK_MAX_LPC_ORDER / 2 + 1];
  int32_t odd[SILK_MAX_LPC_ORDER / 2 + 1];
  int32_t wide[SILK_MAX_LPC_ORDER];

  for (int k = 0; k < order; ++k) {
    // A normalized LSF is between zero and one, so the index is
    // between 0 and 127 and the clamp costs nothing. It is here
    // because the alternative to a clamp is a table read at a
    // negative offset, which is what Appendix A does on a vector its
    // own stabiliser can produce - see nlsf_stabilize.
    int index = gaud_silk_limit(nlsf_q15[k] >> 8, 0, 127);
    int32_t fraction = nlsf_q15[k] - gaud_silk_shl32(index, 8);
    int32_t value = gaud_opus_silk_lsf_cos_q13[index];
    int32_t slope = gaud_opus_silk_lsf_cos_q13[index + 1] - value;
    // The ordering is not cosmetic: it is chosen so that the
    // convolution below accumulates its partial products in an order
    // that keeps them small, and a different order gives a different
    // filter in sixteen-bit arithmetic.
    cosines[ordering[k]] = gaud_silk_rshift_round(
        gaud_silk_shl32(value, 8) + slope * fraction, 20 - SILK_LPC_QA);
  }

  int half = order >> 1;
  nlsf2a_find_poly(even, cosines, half);
  nlsf2a_find_poly(odd, cosines + 1, half);
  for (int k = 0; k < half; ++k) {
    int32_t sum = even[k + 1] + even[k];
    int32_t difference = odd[k + 1] - odd[k];
    wide[k] = -difference - sum;
    wide[order - k - 1] = difference - sum;
  }

  // Bring the coefficients inside sixteen bits. Ten attempts, each
  // flattening by just enough that the largest would fit, and the
  // tenth clips - which is the only place in SILK where the decoder
  // gives up on exactness rather than on quality.
  int attempt;
  for (attempt = 0; attempt < 10; ++attempt) {
    int32_t largest = 0;
    int32_t at = 0;
    for (int k = 0; k < order; ++k) {
      int32_t magnitude = wide[k] > 0 ? wide[k] : -wide[k];
      if (magnitude > largest) {
        largest = magnitude;
        at = k;
      }
    }
    largest = gaud_silk_rshift_round(largest, SILK_LPC_QA + 1 - 12);
    if (largest <= 32767) {
      break;
    }
    // 163838 is where the numerator below would overflow; above it
    // the chirp factor is as small as this can ask for anyway.
    if (largest > 163838) {
      largest = 163838;
    }
    int32_t chirp_q16 = 65470
        - gaud_silk_shl32(largest - 32767, 14)
            / ((largest * (at + 1)) >> 2);
    bandwidth_expand(wide, order, chirp_q16);
  }
  if (attempt == 10) {
    for (int k = 0; k < order; ++k) {
      lpc_q12[k] = (int16_t)gaud_silk_sat16(
          gaud_silk_rshift_round(wide[k], SILK_LPC_QA + 1 - 12));
      wide[k] = gaud_silk_shl32(lpc_q12[k], SILK_LPC_QA + 1 - 12);
    }
  } else {
    for (int k = 0; k < order; ++k) {
      lpc_q12[k] =
          (int16_t)gaud_silk_rshift_round(wide[k], SILK_LPC_QA + 1 - 12);
    }
  }

  // And then make it stable. Each attempt flattens a little harder
  // than the last; sixteen of them reach a chirp factor of about
  // 0.999, which no filter survives being that close to flat.
  for (int i = 0; i < SILK_LPC_REPAIRS; ++i) {
    if (gaud_silk_lpc_inverse_gain(lpc_q12, order) >= SILK_MIN_INV_GAIN_Q30) {
      break;
    }
    bandwidth_expand(wide, order, 65536 - gaud_silk_shl32(2, (unsigned)i));
    for (int k = 0; k < order; ++k) {
      lpc_q12[k] =
          (int16_t)gaud_silk_rshift_round(wide[k], SILK_LPC_QA + 1 - 12);
    }
  }
}

void gaud_silk_decode_pitch(int16_t lag_index, int8_t contour_index,
    int * lags, int fs_khz, int subframes) {
  const int8_t * book;
  int stride;
  if (fs_khz == 8) {
    book = subframes == SILK_MAX_SUBFRAMES ? gaud_opus_silk_cb_lags_stage2
                                           : gaud_opus_silk_cb_lags_stage2_10_ms;
    stride = subframes == SILK_MAX_SUBFRAMES ? 11 : 3;
  } else {
    book = subframes == SILK_MAX_SUBFRAMES ? gaud_opus_silk_cb_lags_stage3
                                           : gaud_opus_silk_cb_lags_stage3_10_ms;
    stride = subframes == SILK_MAX_SUBFRAMES ? 34 : 12;
  }
  // Two and eighteen milliseconds, which is 500 Hz down to about 56.
  int lowest = 2 * fs_khz;
  int highest = 18 * fs_khz;
  int lag = lowest + lag_index;
  for (int k = 0; k < subframes; ++k) {
    lags[k] = gaud_silk_limit(lag + book[k * stride + contour_index], lowest,
        highest);
  }
}

void gaud_silk_decode_parameters(
    SILK_Channel * channel, SILK_Parameters * out, SILK_Coding coding) {
  const SILK_Indices * indices = &channel->indices;
  int order = channel->lpc_order;
  int16_t nlsf_q15[SILK_MAX_LPC_ORDER];

  gaud_silk_gains_dequant(out->gains_q16, indices->gains,
      &channel->prev_gain_index, coding == SILK_CODE_CONDITIONAL,
      channel->nb_subfr);

  gaud_silk_nlsf_decode(nlsf_q15, indices->nlsf, order == SILK_MAX_LPC_ORDER);
  gaud_silk_nlsf_to_lpc(out->lpc_q12[1], nlsf_q15, order);

  // The second half of the frame uses the frequencies just decoded;
  // the first half may be interpolated from the previous frame's, and
  // the factor of four means "do not interpolate at all". A frame
  // that follows a reset has nothing to interpolate from and is given
  // four regardless of what it coded.
  //
  // **That substitution is written back into the indices, not kept
  // local**, because the synthesis reads the same field again: it
  // re-derives the long-term predictor's history halfway through an
  // interpolated frame, and only an interpolated one. Leaving the
  // coded value in place makes a frame after a reset re-whiten when
  // the reference does not, which changes every sample from the
  // third subframe on while the first two still agree.
  if (channel->first_after_reset) {
    channel->indices.nlsf_interp_q2 = 4;
  }
  int factor = indices->nlsf_interp_q2;
  if (factor < 4) {
    int16_t first_q15[SILK_MAX_LPC_ORDER];
    for (int i = 0; i < order; ++i) {
      first_q15[i] = (int16_t)(channel->prev_nlsf_q15[i]
          + ((factor * (nlsf_q15[i] - channel->prev_nlsf_q15[i])) >> 2));
    }
    gaud_silk_nlsf_to_lpc(out->lpc_q12[0], first_q15, order);
  } else {
    memcpy(out->lpc_q12[0], out->lpc_q12[1],
        (size_t)order * sizeof *out->lpc_q12[0]);
  }
  memcpy(channel->prev_nlsf_q15, nlsf_q15, (size_t)order * sizeof *nlsf_q15);

  // After a loss the filters are bandwidth-expanded, so that the first
  // good frame does not ring on a spectrum the listener has just been
  // told was wrong.
  if (channel->loss_cnt) {
    gaud_silk_bwexpander(out->lpc_q12[0], order, SILK_BWE_AFTER_LOSS_Q16);
    gaud_silk_bwexpander(out->lpc_q12[1], order, SILK_BWE_AFTER_LOSS_Q16);
  }

  if (indices->signal_type == SILK_SIGNAL_VOICED) {
    gaud_silk_decode_pitch(indices->lag_index, indices->contour_index,
        out->pitch, channel->fs_khz, channel->nb_subfr);
    const int8_t * book = indices->per_index == 0
        ? gaud_opus_silk_ltp_gain_vq_0
        : (indices->per_index == 1 ? gaud_opus_silk_ltp_gain_vq_1
                                   : gaud_opus_silk_ltp_gain_vq_2);
    for (int k = 0; k < channel->nb_subfr; ++k) {
      const int8_t * taps =
          book + (size_t)indices->ltp[k] * (size_t)SILK_LTP_ORDER;
      for (int i = 0; i < SILK_LTP_ORDER; ++i) {
        out->ltp_q14[k * SILK_LTP_ORDER + i] =
            (int16_t)gaud_silk_shl32(taps[i], 7);
      }
    }
    out->ltp_scale_q14 =
        gaud_opus_silk_ltp_scales_q14[indices->ltp_scale_index];
  } else {
    memset(out->pitch, 0, sizeof out->pitch);
    memset(out->ltp_q14, 0, sizeof out->ltp_q14);
    out->ltp_scale_q14 = 0;
  }
}
