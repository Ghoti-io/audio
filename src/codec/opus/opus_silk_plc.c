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
 * Concealing a SILK frame that did not arrive, and the comfort noise
 * that goes with it. RFC 6716 section 4.4. Never installed.
 *
 * Like CELT's, this is informative in the RFC and exact in the
 * reference, and it runs on streams that lose nothing: the transition
 * from CELT to SILK conceals a frame that was never lost to hide the
 * seam, and every good frame updates the state a later loss would use.
 * So `gaud_silk_plc_update` and `gaud_silk_cng` are called on every frame
 * of every stream.
 *
 * **A lost voiced frame is the last pitch pulse, repeated.** The
 * long-term predictor's history is whitened with the last good filter,
 * the pitch lag and the strongest set of long-term taps from the last
 * good frame are held, and a noise generator drawing from the last good
 * excitation stands in for the part of the signal the pitch does not
 * explain. Every subframe the pitch gain, the noise gain and the lag
 * itself drift: gain down, lag up by one percent.
 *
 * **The first good frame after a loss is faded in, never cut in.**
 * Concealment is rarely as loud as the signal that follows it, so the
 * good frame's start is scaled by a ramp up to its true level if it is
 * louder than what concealment produced, over the frame's own length.
 */

#include "opus_silk.h"
#include "opus_silk_math.h"
#include "opus_silk_tables.h"
#include <string.h>

/** The bandwidth expansion applied to the last filter, 0.99 in Q16. */
#define SILK_PLC_BWE_COEF_Q16 64881

/** The long-term gain below which taps are scaled up: 0.7 in Q14. */
#define SILK_PLC_PITCH_GAIN_MIN_Q14 11469

/** And above which they are scaled down: 0.95 in Q14. */
#define SILK_PLC_PITCH_GAIN_MAX_Q14 15565

/** The longest lag concealment lets the pitch drift to, in ms. */
#define SILK_PLC_MAX_PITCH_LAG_MS 18

/** The pitch's upward drift per subframe: 0.01 in Q16. */
#define SILK_PLC_PITCH_DRIFT_Q16 655

/** The LPC gain at or above which noise is reduced: 2^3. */
#define SILK_PLC_LOG2_INV_GAIN_HIGH 3

/** And below which it stops being reduced: 2^8. */
#define SILK_PLC_LOG2_INV_GAIN_LOW 8

/** The comfort noise's excitation buffer mask. */
#define SILK_CNG_BUF_MASK_MAX 255

/** How fast the comfort noise's gain moves: 0.25^(1/4) in Q16. */
#define SILK_CNG_GAIN_SMTH_Q16 4634

/** How fast its spectrum does: 0.25 in Q16. */
#define SILK_CNG_NLSF_SMTH_Q16 16348

/** The noise generators' multiplier. */
#define SILK_PLC_RAND_MULTIPLIER 196314165

/** And their increment. */
#define SILK_PLC_RAND_INCREMENT 907633515

/** The harmonic gain after the first and second lost frames, Q15. */
static const int16_t kHarmAttenuation[2] = {32440, 31130};

/** The noise gain after each, for voiced frames. */
static const int16_t kRandAttenuationVoiced[2] = {31130, 26214};

/** And for unvoiced ones. */
static const int16_t kRandAttenuationUnvoiced[2] = {32440, 29491};

static int32_t rand_next(int32_t seed) {
  return (int32_t)((uint32_t)SILK_PLC_RAND_INCREMENT
      + (uint32_t)seed * (uint32_t)SILK_PLC_RAND_MULTIPLIER);
}

static int32_t mul_wrap(int32_t a, int32_t b) {
  return (int32_t)((uint32_t)a * (uint32_t)b);
}

/**
 * @brief Sum of squares of an int16 vector, shifted to fit in an int32.
 *
 * @param energy Receives the shifted sum.
 * @param shift Receives how far it was shifted.
 * @param x The vector.
 * @param length Its length.
 */
static void sum_sqr_shift(
    int32_t * energy, int * shift, const int16_t * x, int length) {
  int32_t nrg = 0;
  int shft = 0;
  int i = 0;
  --length;
  for (; i < length; i += 2) {
    nrg = (int32_t)((uint32_t)nrg + (uint32_t)gaud_silk_smulbb(x[i], x[i])
        + (uint32_t)gaud_silk_smulbb(x[i + 1], x[i + 1]));
    if (nrg < 0) {
      nrg = (int32_t)((uint32_t)nrg >> 2);
      shft = 2;
      break;
    }
  }
  for (; i < length; i += 2) {
    int32_t tmp = gaud_silk_smulbb(x[i], x[i]);
    tmp = (int32_t)((uint32_t)tmp + (uint32_t)gaud_silk_smulbb(x[i + 1], x[i + 1]));
    nrg = (int32_t)((uint32_t)nrg + ((uint32_t)tmp >> shft));
    if (nrg < 0) {
      nrg = (int32_t)((uint32_t)nrg >> 2);
      shft += 2;
    }
  }
  if (i == length) {
    // One sample left over.
    int32_t tmp = gaud_silk_smulbb(x[i], x[i]);
    nrg = (int32_t)((uint32_t)nrg + ((uint32_t)tmp >> shft));
  }
  // At least one extra leading zero, so a caller can add two of these.
  if (nrg & (int32_t)0xC0000000) {
    nrg = (int32_t)((uint32_t)nrg >> 2);
    shft += 2;
  }
  *shift = shft;
  *energy = nrg;
}

void gaud_silk_bwexpander(int16_t * ar, int d, int32_t chirp_q16) {
  int32_t chirp_minus_one = chirp_q16 - 65536;
  // A plain rounding shift of the product, not the biased wide multiply:
  // the bias lets the filter become unstable.
  for (int i = 0; i < d - 1; ++i) {
    ar[i] = (int16_t)gaud_silk_rshift_round(mul_wrap(chirp_q16, ar[i]), 16);
    chirp_q16 += gaud_silk_rshift_round(mul_wrap(chirp_q16, chirp_minus_one), 16);
  }
  ar[d - 1] =
      (int16_t)gaud_silk_rshift_round(mul_wrap(chirp_q16, ar[d - 1]), 16);
}

static void plc_reset(SILK_Channel * channel) {
  channel->plc.pitch_l_q8 = (int32_t)((uint32_t)channel->frame_length << 7);
  channel->plc.prev_gain_q16[0] = 1 << 16;
  channel->plc.prev_gain_q16[1] = 1 << 16;
  channel->plc.subfr_length = 20;
  channel->plc.nb_subfr = 2;
}

/** A rate change restarts concealment's memory, whichever call notices. */
static void plc_check_rate(SILK_Channel * channel) {
  if (channel->fs_khz != channel->plc.fs_khz) {
    plc_reset(channel);
    channel->plc.fs_khz = channel->fs_khz;
  }
}

void gaud_silk_plc_update(
    SILK_Channel * channel, const SILK_Parameters * parameters) {
  SILK_Plc * plc = &channel->plc;
  int32_t ltp_gain_q14 = 0;
  plc_check_rate(channel);

  channel->plc_signal_type = channel->indices.signal_type;
  if (channel->indices.signal_type == SILK_SIGNAL_VOICED) {
    // The parameters of the last subframe that holds a pitch pulse.
    for (int j = 0; j * channel->subfr_length
         < parameters->pitch[channel->nb_subfr - 1]; ++j) {
      if (j == channel->nb_subfr) {
        break;
      }
      int32_t tap_sum = 0;
      for (int i = 0; i < SILK_LTP_ORDER; ++i) {
        tap_sum += parameters->ltp_q14
            [(channel->nb_subfr - 1 - j) * SILK_LTP_ORDER + i];
      }
      if (tap_sum > ltp_gain_q14) {
        ltp_gain_q14 = tap_sum;
        memcpy(plc->ltp_coef_q14,
            &parameters->ltp_q14[(channel->nb_subfr - 1 - j) * SILK_LTP_ORDER],
            SILK_LTP_ORDER * sizeof(int16_t));
        plc->pitch_l_q8 = (int32_t)(
            (uint32_t)parameters->pitch[channel->nb_subfr - 1 - j] << 8);
      }
    }
    memset(plc->ltp_coef_q14, 0, sizeof plc->ltp_coef_q14);
    plc->ltp_coef_q14[SILK_LTP_ORDER / 2] = (int16_t)ltp_gain_q14;

    // Limit the long-term coefficients.
    if (ltp_gain_q14 < SILK_PLC_PITCH_GAIN_MIN_Q14) {
      int32_t tmp = SILK_PLC_PITCH_GAIN_MIN_Q14 << 10;
      int32_t scale_q10 = tmp / (ltp_gain_q14 > 1 ? ltp_gain_q14 : 1);
      for (int i = 0; i < SILK_LTP_ORDER; ++i) {
        plc->ltp_coef_q14[i] = (int16_t)(
            gaud_silk_smulbb(plc->ltp_coef_q14[i], scale_q10) >> 10);
      }
    } else if (ltp_gain_q14 > SILK_PLC_PITCH_GAIN_MAX_Q14) {
      int32_t tmp = SILK_PLC_PITCH_GAIN_MAX_Q14 << 14;
      int32_t scale_q14 = tmp / (ltp_gain_q14 > 1 ? ltp_gain_q14 : 1);
      for (int i = 0; i < SILK_LTP_ORDER; ++i) {
        plc->ltp_coef_q14[i] = (int16_t)(
            gaud_silk_smulbb(plc->ltp_coef_q14[i], scale_q14) >> 14);
      }
    }
  } else {
    plc->pitch_l_q8 = (int32_t)((uint32_t)(channel->fs_khz * 18) << 8);
    memset(plc->ltp_coef_q14, 0, sizeof plc->ltp_coef_q14);
  }

  memcpy(plc->prev_lpc_q12, parameters->lpc_q12[1],
      (size_t)channel->lpc_order * sizeof(int16_t));
  plc->prev_ltp_scale_q14 = parameters->ltp_scale_q14;
  plc->prev_gain_q16[0] = parameters->gains_q16[channel->nb_subfr - 2];
  plc->prev_gain_q16[1] = parameters->gains_q16[channel->nb_subfr - 1];
  plc->subfr_length = channel->subfr_length;
  plc->nb_subfr = channel->nb_subfr;
}

void gaud_silk_plc_conceal(SILK_Channel * channel, int16_t * frame) {
  SILK_Plc * plc = &channel->plc;
  int16_t exc_buf[2 * SILK_MAX_SUBFRAME_LENGTH];
  int16_t a_q12[SILK_MAX_LPC_ORDER];
  int16_t s_ltp[SILK_MAX_FRAME_LENGTH];
  int32_t s_ltp_q14[2 * SILK_MAX_FRAME_LENGTH];
  int32_t prev_gain_q10[2];
  int energy1;
  int energy2;
  int shift1;
  int shift2;
  const int32_t * rand_ptr;
  int order = channel->lpc_order;
  int ltp_mem = channel->ltp_mem_length;

  plc_check_rate(channel);
  prev_gain_q10[0] = plc->prev_gain_q16[0] >> 6;
  prev_gain_q10[1] = plc->prev_gain_q16[1] >> 6;

  if (channel->first_after_reset) {
    memset(plc->prev_lpc_q12, 0, sizeof plc->prev_lpc_q12);
  }

  // The noise component: scale the last two subframes' excitation,
  // and use the quieter of the two as the generator's buffer.
  int16_t * exc_ptr = exc_buf;
  for (int k = 0; k < 2; ++k) {
    for (int i = 0; i < plc->subfr_length; ++i) {
      exc_ptr[i] = (int16_t)gaud_silk_sat16(gaud_silk_smulww(
          channel->exc_q14[i + (k + plc->nb_subfr - 2) * plc->subfr_length],
          prev_gain_q10[k]) >> 8);
    }
    exc_ptr += plc->subfr_length;
  }
  int32_t e1;
  int32_t e2;
  sum_sqr_shift(&e1, &shift1, exc_buf, plc->subfr_length);
  sum_sqr_shift(&e2, &shift2, exc_buf + plc->subfr_length, plc->subfr_length);
  energy1 = e1;
  energy2 = e2;
  if ((energy1 >> shift2) < (energy2 >> shift1)) {
    int start = (plc->nb_subfr - 1) * plc->subfr_length - SILK_PLC_RAND_BUF_SIZE;
    rand_ptr = &channel->exc_q14[start > 0 ? start : 0];
  } else {
    int start = plc->nb_subfr * plc->subfr_length - SILK_PLC_RAND_BUF_SIZE;
    rand_ptr = &channel->exc_q14[start > 0 ? start : 0];
  }

  int16_t * b_q14 = plc->ltp_coef_q14;
  int16_t rand_scale_q14 = plc->rand_scale_q14;
  int attenuation = channel->loss_cnt < 1 ? channel->loss_cnt : 1;
  int32_t harm_gain_q15 = kHarmAttenuation[attenuation];
  int32_t rand_gain_q15 = channel->plc_signal_type == SILK_SIGNAL_VOICED
      ? kRandAttenuationVoiced[attenuation]
      : kRandAttenuationUnvoiced[attenuation];

  gaud_silk_bwexpander(plc->prev_lpc_q12, order, SILK_PLC_BWE_COEF_Q16);
  memcpy(a_q12, plc->prev_lpc_q12, (size_t)order * sizeof(int16_t));

  // The first lost frame sets the noise level; later ones inherit it.
  if (channel->loss_cnt == 0) {
    rand_scale_q14 = 1 << 14;
    if (channel->plc_signal_type == SILK_SIGNAL_VOICED) {
      // Less noise where the pitch explains more of the signal.
      for (int i = 0; i < SILK_LTP_ORDER; ++i) {
        rand_scale_q14 = (int16_t)(rand_scale_q14 - b_q14[i]);
      }
      rand_scale_q14 = rand_scale_q14 > 3277 ? rand_scale_q14 : 3277;
      rand_scale_q14 = (int16_t)(
          gaud_silk_smulbb(rand_scale_q14, plc->prev_ltp_scale_q14) >> 14);
    } else {
      // And less for unvoiced frames whose filter amplifies a lot.
      int32_t inv_gain_q30 = gaud_silk_lpc_inverse_gain(plc->prev_lpc_q12, order);
      int32_t down_scale_q30 = (1 << 30) >> SILK_PLC_LOG2_INV_GAIN_HIGH;
      down_scale_q30 = down_scale_q30 < inv_gain_q30 ? down_scale_q30 : inv_gain_q30;
      int32_t floor_q30 = (1 << 30) >> SILK_PLC_LOG2_INV_GAIN_LOW;
      down_scale_q30 = down_scale_q30 > floor_q30 ? down_scale_q30 : floor_q30;
      down_scale_q30 = gaud_silk_shl32(down_scale_q30, SILK_PLC_LOG2_INV_GAIN_HIGH);
      rand_gain_q15 =
          gaud_silk_smulwb(down_scale_q30, rand_gain_q15) >> 14;
    }
  }

  int32_t rand_seed = plc->rand_seed;
  int lag = gaud_silk_rshift_round(plc->pitch_l_q8, 8);
  int buf_at = ltp_mem;

  // Rewhiten the long-term state, and scale it.
  int idx = ltp_mem - lag - order - SILK_LTP_ORDER / 2;
  gaud_silk_lpc_analysis_filter(
      &s_ltp[idx], &channel->out_buf[idx], a_q12, ltp_mem - idx, order);
  int32_t inv_gain_q30 = gaud_silk_inverse32_varq(plc->prev_gain_q16[1], 46);
  inv_gain_q30 = inv_gain_q30 < (INT32_MAX >> 1) ? inv_gain_q30 : INT32_MAX >> 1;
  for (int i = idx + order; i < ltp_mem; ++i) {
    s_ltp_q14[i] = gaud_silk_smulwb(inv_gain_q30, s_ltp[i]);
  }

  // Long-term synthesis.
  for (int k = 0; k < channel->nb_subfr; ++k) {
    const int32_t * pred = &s_ltp_q14[buf_at - lag + SILK_LTP_ORDER / 2];
    for (int i = 0; i < channel->subfr_length; ++i) {
      // Two, because the wide multiply rounds towards minus infinity.
      int32_t predicted_q12 = 2;
      predicted_q12 = gaud_silk_smlawb(predicted_q12, pred[0], b_q14[0]);
      predicted_q12 = gaud_silk_smlawb(predicted_q12, pred[-1], b_q14[1]);
      predicted_q12 = gaud_silk_smlawb(predicted_q12, pred[-2], b_q14[2]);
      predicted_q12 = gaud_silk_smlawb(predicted_q12, pred[-3], b_q14[3]);
      predicted_q12 = gaud_silk_smlawb(predicted_q12, pred[-4], b_q14[4]);
      ++pred;
      rand_seed = rand_next(rand_seed);
      idx = (rand_seed >> 25) & (SILK_PLC_RAND_BUF_SIZE - 1);
      s_ltp_q14[buf_at] = gaud_silk_shl32(
          gaud_silk_smlawb(predicted_q12, rand_ptr[idx], rand_scale_q14), 2);
      ++buf_at;
    }
    // Gradually reduce the long-term gain and the noise.
    for (int j = 0; j < SILK_LTP_ORDER; ++j) {
      b_q14[j] = (int16_t)(gaud_silk_smulbb(harm_gain_q15, b_q14[j]) >> 15);
    }
    rand_scale_q14 =
        (int16_t)(gaud_silk_smulbb(rand_scale_q14, rand_gain_q15) >> 15);
    // And let the pitch drift up slowly, to a ceiling.
    plc->pitch_l_q8 = gaud_silk_smlawb(
        plc->pitch_l_q8, plc->pitch_l_q8, SILK_PLC_PITCH_DRIFT_Q16);
    int32_t ceiling = (int32_t)(
        (uint32_t)(SILK_PLC_MAX_PITCH_LAG_MS * channel->fs_khz) << 8);
    plc->pitch_l_q8 = plc->pitch_l_q8 < ceiling ? plc->pitch_l_q8 : ceiling;
    lag = gaud_silk_rshift_round(plc->pitch_l_q8, 8);
  }

  // Short-term synthesis.
  int32_t * lpc_ptr = &s_ltp_q14[ltp_mem - SILK_MAX_LPC_ORDER];
  memcpy(lpc_ptr, channel->lpc_state_q14, SILK_MAX_LPC_ORDER * sizeof(int32_t));
  for (int i = 0; i < channel->frame_length; ++i) {
    int32_t predicted_q10 = order >> 1;
    for (int j = 0; j < order; ++j) {
      predicted_q10 = gaud_silk_smlawb(
          predicted_q10, lpc_ptr[SILK_MAX_LPC_ORDER + i - j - 1], a_q12[j]);
    }
    lpc_ptr[SILK_MAX_LPC_ORDER + i] = gaud_silk_add32(
        lpc_ptr[SILK_MAX_LPC_ORDER + i], gaud_silk_shl32(predicted_q10, 4));
    frame[i] = (int16_t)gaud_silk_sat16(gaud_silk_sat16(gaud_silk_rshift_round(
        gaud_silk_smulww(
            lpc_ptr[SILK_MAX_LPC_ORDER + i], prev_gain_q10[1]),
        8)));
  }
  memcpy(channel->lpc_state_q14, &lpc_ptr[channel->frame_length],
      SILK_MAX_LPC_ORDER * sizeof(int32_t));

  plc->rand_seed = rand_seed;
  plc->rand_scale_q14 = rand_scale_q14;
  ++channel->loss_cnt;
  channel->lag_prev = lag;
}

void gaud_silk_plc_glue(SILK_Channel * channel, int16_t * frame, int length) {
  SILK_Plc * plc = &channel->plc;
  if (channel->loss_cnt) {
    // Remember how loud the concealment was.
    sum_sqr_shift(&plc->conc_energy, &plc->conc_energy_shift, frame, length);
    plc->last_frame_lost = true;
    return;
  }
  if (plc->last_frame_lost) {
    int32_t energy;
    int energy_shift;
    sum_sqr_shift(&energy, &energy_shift, frame, length);
    // Normalise the two energies to the same scale.
    if (energy_shift > plc->conc_energy_shift) {
      plc->conc_energy >>= energy_shift - plc->conc_energy_shift;
    } else if (energy_shift < plc->conc_energy_shift) {
      energy >>= plc->conc_energy_shift - energy_shift;
    }
    // Fade in the energy difference.
    if (energy > plc->conc_energy) {
      int lz = gaud_silk_clz32((uint32_t)plc->conc_energy) - 1;
      plc->conc_energy = gaud_silk_shl32(plc->conc_energy, (unsigned)lz);
      int drop = 24 - lz;
      energy >>= drop > 0 ? drop : 0;
      int32_t frac_q24 = plc->conc_energy / (energy > 1 ? energy : 1);
      int32_t gain_q16 =
          gaud_silk_shl32(gaud_silk_sqrt_approx(frac_q24), 4);
      int32_t slope_q16 = (int32_t)(((1 << 16) - gain_q16) / length);
      // Four times as steep, so the onset after a pause is not missed.
      slope_q16 = gaud_silk_shl32(slope_q16, 2);
      for (int i = 0; i < length; ++i) {
        frame[i] = (int16_t)gaud_silk_smulwb(gain_q16, frame[i]);
        gain_q16 += slope_q16;
        if (gain_q16 > 1 << 16) {
          break;
        }
      }
    }
  }
  plc->last_frame_lost = false;
}

static void cng_reset(SILK_Channel * channel) {
  int32_t step_q15 = INT16_MAX / (channel->lpc_order + 1);
  int32_t acc_q15 = 0;
  for (int i = 0; i < channel->lpc_order; ++i) {
    acc_q15 += step_q15;
    channel->cng.smth_nlsf_q15[i] = (int16_t)acc_q15;
  }
  channel->cng.smth_gain_q16 = 0;
  channel->cng.rand_seed = 3176576;
}

void gaud_silk_cng(SILK_Channel * channel, const SILK_Parameters * parameters,
    int16_t * frame, int length) {
  SILK_Cng * cng = &channel->cng;
  int order = channel->lpc_order;
  int32_t cng_sig_q10[SILK_MAX_FRAME_LENGTH + SILK_MAX_LPC_ORDER];

  if (channel->fs_khz != cng->fs_khz) {
    cng_reset(channel);
    cng->fs_khz = channel->fs_khz;
  }
  if (channel->loss_cnt == 0
      && channel->plc_signal_type == SILK_SIGNAL_INACTIVE) {
    // Update the estimate from a good inactive frame.
    for (int i = 0; i < order; ++i) {
      cng->smth_nlsf_q15[i] = (int16_t)(cng->smth_nlsf_q15[i]
          + gaud_silk_smulwb(
              channel->prev_nlsf_q15[i] - cng->smth_nlsf_q15[i],
              SILK_CNG_NLSF_SMTH_Q16));
    }
    int32_t max_gain_q16 = 0;
    int subframe = 0;
    for (int i = 0; i < channel->nb_subfr; ++i) {
      if (parameters->gains_q16[i] > max_gain_q16) {
        max_gain_q16 = parameters->gains_q16[i];
        subframe = i;
      }
    }
    memmove(&cng->exc_buf_q14[channel->subfr_length], cng->exc_buf_q14,
        (size_t)(channel->nb_subfr - 1) * (size_t)channel->subfr_length
            * sizeof(int32_t));
    memcpy(cng->exc_buf_q14, &channel->exc_q14[subframe * channel->subfr_length],
        (size_t)channel->subfr_length * sizeof(int32_t));
    for (int i = 0; i < channel->nb_subfr; ++i) {
      cng->smth_gain_q16 += gaud_silk_smulwb(
          parameters->gains_q16[i] - cng->smth_gain_q16, SILK_CNG_GAIN_SMTH_Q16);
    }
  }

  // Add noise when a packet was lost.
  if (channel->loss_cnt) {
    int16_t a_q12[SILK_MAX_LPC_ORDER];
    int mask = SILK_CNG_BUF_MASK_MAX;
    int32_t seed = cng->rand_seed;
    while (mask > length) {
      mask >>= 1;
    }
    for (int i = 0; i < length; ++i) {
      seed = rand_next(seed);
      int idx = (seed >> 24) & mask;
      cng_sig_q10[SILK_MAX_LPC_ORDER + i] = (int16_t)gaud_silk_sat16(
          gaud_silk_smulww(cng->exc_buf_q14[idx], cng->smth_gain_q16 >> 4));
    }
    cng->rand_seed = seed;

    gaud_silk_nlsf_to_lpc(a_q12, cng->smth_nlsf_q15, order);
    memcpy(cng_sig_q10, cng->synth_state, SILK_MAX_LPC_ORDER * sizeof(int32_t));
    for (int i = 0; i < length; ++i) {
      int32_t sum_q6 = order >> 1;
      for (int j = 0; j < order; ++j) {
        sum_q6 = gaud_silk_smlawb(
            sum_q6, cng_sig_q10[SILK_MAX_LPC_ORDER + i - j - 1], a_q12[j]);
      }
      cng_sig_q10[SILK_MAX_LPC_ORDER + i] = gaud_silk_add32(
          cng_sig_q10[SILK_MAX_LPC_ORDER + i], gaud_silk_shl32(sum_q6, 4));
      frame[i] = (int16_t)gaud_silk_sat16(
          gaud_silk_add32(frame[i], gaud_silk_rshift_round(sum_q6, 6)));
    }
    memcpy(cng->synth_state, &cng_sig_q10[length],
        SILK_MAX_LPC_ORDER * sizeof(int32_t));
  } else {
    memset(cng->synth_state, 0, (size_t)order * sizeof(int32_t));
  }
}
