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
 * SILK's two synthesis filters. RFC 6716 4.2.8. Never installed.
 *
 * This is where SILK finally makes a sound. The excitation is run
 * through the long-term predictor, which adds back the pitch, and
 * then through the short-term one, which adds back the vocal tract,
 * and the result is scaled by the subframe's gain.
 *
 * Four things here are not obvious from the shape of the code.
 *
 * **The excitation is dithered, and the dither is part of the
 * format.** Each sample's sign is flipped according to a linear
 * congruential generator seeded from the bitstream, and the generator
 * is then advanced by the sample's own magnitude. So the noise is
 * reproducible and the decoder must reproduce it exactly: it is the
 * difference between a quantizer that sounds grainy and one that
 * sounds like speech.
 *
 * **The long-term predictor's history is kept whitened and scaled.**
 * It is not the output signal: it is the output run back through the
 * *current* short-term filter and divided by the current gain. That
 * keeps the predictor's arithmetic in range, and it means the history
 * has to be rebuilt whenever either of those changes - which is what
 * the re-whitening step at the start of a subframe is doing.
 *
 * **A gain change rescales the filter memory rather than being
 * applied to it.** The short-term filter's state is held in units of
 * the gain, so when the gain changes the old state is multiplied by
 * the ratio. Doing it the other way - applying the gain to the output
 * instead - would be a different filter with the same coefficients.
 *
 * **The two biases are deliberate.** Both prediction sums start at a
 * small positive constant before anything is accumulated, because the
 * wide multiply used for every tap rounds towards negative infinity;
 * without the constant, a signal with no bias would acquire one. The
 * constants are 2 for the long-term sum and half the filter order for
 * the short-term one.
 */

#include "opus_silk.h"
#include "opus_silk_math.h"
#include "opus_silk_tables.h"
#include <string.h>

/** How far a non-zero excitation sample is pulled towards zero, Q10. */
#define SILK_QUANT_ADJUST_Q10 80

/** The dither generator's multiplier. */
#define SILK_RAND_MULTIPLIER 196314165

/** And its increment. */
#define SILK_RAND_INCREMENT 907633515

// The inverse of the synthesis filter below, documented in opus_silk.h.
void gaud_silk_lpc_analysis_filter(int16_t * out, const int16_t * in,
    const int16_t * lpc_q12, int length, int order) {
  for (int at = order; at < length; ++at) {
    const int16_t * from = in + at - 1;
    // The reference allows this accumulator to wrap and says why: two
    // wraps can cancel, and only an invalid stream can reach one.
    uint32_t predicted = 0;
    for (int j = 0; j < order; ++j) {
      predicted += (uint32_t)gaud_silk_smulbb(from[-j], lpc_q12[j]);
    }
    int32_t residual_q12 =
        (int32_t)((uint32_t)gaud_silk_shl32(from[1], 12) - predicted);
    out[at] = (int16_t)gaud_silk_sat16(
        gaud_silk_rshift_round(residual_q12, 12));
  }
  memset(out, 0, (size_t)order * sizeof *out);
}

void gaud_silk_decode_core(SILK_Channel * channel,
    const SILK_Parameters * parameters, int16_t * out) {
  const SILK_Indices * indices = &channel->indices;
  int order = channel->lpc_order;
  int subfr_length = channel->subfr_length;
  int32_t excitation_q14[SILK_MAX_FRAME_LENGTH];
  int16_t whitened[SILK_MAX_FRAME_LENGTH];
  int32_t history_q15[2 * SILK_MAX_FRAME_LENGTH];
  int32_t residual_q14[SILK_MAX_SUBFRAME_LENGTH];
  int32_t state_q14[SILK_MAX_SUBFRAME_LENGTH + SILK_MAX_LPC_ORDER];
  int32_t offset_q10 = gaud_opus_silk_quantization_offsets_q10
      [2 * (indices->signal_type >> 1) + indices->quant_offset_type];
  bool interpolated = indices->nlsf_interp_q2 < 4;
  // After a lost voiced frame, a good unvoiced one would stop the pitch
  // dead. The reference bridges it: for the first two subframes the
  // frame is treated as voiced, at the lag concealment ended on, with a
  // single quarter-strength tap, and the second half is decoded as the
  // unvoiced frame it is.
  bool bridge = channel->loss_cnt != 0
      && channel->plc_signal_type == SILK_SIGNAL_VOICED
      && indices->signal_type != SILK_SIGNAL_VOICED;
  int16_t bridge_taps_q14[SILK_LTP_ORDER] = {0, 0, 1 << 12, 0, 0};

  // Section 4.2.7.9. The pulse magnitudes become a signed excitation:
  // pulled towards zero by a fixed amount, pushed away from it by the
  // frame's quantization offset, and then signed by the generator.
  uint32_t seed = (uint32_t)indices->seed;
  for (int i = 0; i < channel->frame_length; ++i) {
    seed = seed * (uint32_t)SILK_RAND_MULTIPLIER
        + (uint32_t)SILK_RAND_INCREMENT;
    int32_t value = gaud_silk_shl32(channel->pulses[i], 14);
    if (value > 0) {
      value -= SILK_QUANT_ADJUST_Q10 << 4;
    } else if (value < 0) {
      value += SILK_QUANT_ADJUST_Q10 << 4;
    }
    value += offset_q10 << 4;
    if ((int32_t)seed < 0) {
      value = (int32_t)(0u - (uint32_t)value);
    }
    excitation_q14[i] = value;
    seed += (uint32_t)channel->pulses[i];
  }

  if (bridge) {
    memset(history_q15, 0, sizeof history_q15);
  }
  memcpy(channel->exc_q14, excitation_q14,
      (size_t)channel->frame_length * sizeof *channel->exc_q14);
  memcpy(state_q14, channel->lpc_state_q14, sizeof channel->lpc_state_q14);
  int history_at = channel->ltp_mem_length;
  int lag = 0;
  for (int k = 0; k < channel->nb_subfr; ++k) {
    // Two sets of coefficients, one per half of the frame; a 10 ms
    // frame has two subframes and uses only the first.
    const int16_t * lpc_q12 = parameters->lpc_q12[k >> 1];
    const int16_t * taps_q14 = parameters->ltp_q14 + k * SILK_LTP_ORDER;
    int pitch = parameters->pitch[k];
    bool voiced = indices->signal_type == SILK_SIGNAL_VOICED;
    if (bridge && k < SILK_MAX_SUBFRAMES / 2) {
      taps_q14 = bridge_taps_q14;
      pitch = channel->lag_prev;
      voiced = true;
    }
    if (k == channel->nb_subfr - 1) {
      channel->lag_prev = pitch;
    }
    int32_t gain_q16 = parameters->gains_q16[k];
    int32_t gain_q10 = gain_q16 >> 6;
    int32_t inverse_gain_q31 = gaud_silk_inverse32_varq(gain_q16, 47);
    int32_t adjust_q16 = 1 << 16;
    if (gain_q16 != channel->prev_gain_q16) {
      adjust_q16 =
          gaud_silk_div32_varq(channel->prev_gain_q16, gain_q16, 16);
      for (int i = 0; i < SILK_MAX_LPC_ORDER; ++i) {
        state_q14[i] = gaud_silk_smulww(adjust_q16, state_q14[i]);
      }
    }
    channel->prev_gain_q16 = gain_q16;

    if (voiced) {
      lag = pitch;
      // The predictor's history has to be rebuilt whenever the
      // short-term filter changes, which is at the start of the frame
      // and again halfway through if the coefficients were
      // interpolated. Everywhere else a gain change is enough to
      // rescale what is already there.
      if (k == 0 || (k == 2 && interpolated)) {
        int start = channel->ltp_mem_length - lag - order - SILK_LTP_ORDER / 2;
        if (k == 2) {
          // The second half of the frame predicts from the first half,
          // which is not in the history buffer yet.
          memcpy(channel->out_buf + channel->ltp_mem_length, out,
              (size_t)(2 * subfr_length) * sizeof *out);
        }
        gaud_silk_lpc_analysis_filter(whitened + start,
            channel->out_buf + start + k * subfr_length, lpc_q12,
            channel->ltp_mem_length - start, order);
        if (k == 0) {
          // Only the first subframe scales the history down, and it is
          // the one place a frame says how much of the previous
          // frame's signal it is willing to depend on.
          inverse_gain_q31 = gaud_silk_shl32(
              gaud_silk_smulwb(inverse_gain_q31, parameters->ltp_scale_q14),
              2);
        }
        for (int i = 0; i < lag + SILK_LTP_ORDER / 2; ++i) {
          history_q15[history_at - i - 1] = gaud_silk_smulwb(
              inverse_gain_q31, whitened[channel->ltp_mem_length - i - 1]);
        }
      } else if (adjust_q16 != 1 << 16) {
        for (int i = 0; i < lag + SILK_LTP_ORDER / 2; ++i) {
          history_q15[history_at - i - 1] =
              gaud_silk_smulww(adjust_q16, history_q15[history_at - i - 1]);
        }
      }
    }

    const int32_t * source;
    if (voiced) {
      const int32_t * at = history_q15 + history_at - lag + SILK_LTP_ORDER / 2;
      for (int i = 0; i < subfr_length; ++i) {
        // Two, because every tap rounds towards negative infinity.
        int32_t predicted_q13 = 2;
        for (int tap = 0; tap < SILK_LTP_ORDER; ++tap) {
          predicted_q13 =
              gaud_silk_smlawb(predicted_q13, at[-tap], taps_q14[tap]);
        }
        ++at;
        residual_q14[i] = gaud_silk_add32(
            excitation_q14[k * subfr_length + i],
            gaud_silk_shl32(predicted_q13, 1));
        history_q15[history_at] = gaud_silk_shl32(residual_q14[i], 1);
        ++history_at;
      }
      source = residual_q14;
    } else {
      source = excitation_q14 + k * subfr_length;
    }

    for (int i = 0; i < subfr_length; ++i) {
      // Half the order, for the same reason the long-term sum starts
      // at two.
      int32_t predicted_q10 = order >> 1;
      for (int j = 0; j < order; ++j) {
        predicted_q10 = gaud_silk_smlawb(predicted_q10,
            state_q14[SILK_MAX_LPC_ORDER + i - 1 - j], lpc_q12[j]);
      }
      state_q14[SILK_MAX_LPC_ORDER + i] =
          gaud_silk_add32(source[i], gaud_silk_shl32(predicted_q10, 4));
      out[k * subfr_length + i] = (int16_t)gaud_silk_sat16(
          gaud_silk_rshift_round(
              gaud_silk_smulww(state_q14[SILK_MAX_LPC_ORDER + i], gain_q10),
              8));
    }
    memcpy(state_q14, state_q14 + subfr_length,
        SILK_MAX_LPC_ORDER * sizeof *state_q14);
  }
  memcpy(channel->lpc_state_q14, state_q14, sizeof channel->lpc_state_q14);

  // The history buffer keeps the last 20 ms of output, which is more
  // than the longest pitch period, so the next frame's predictor can
  // always reach what it needs.
  int keep = channel->ltp_mem_length - channel->frame_length;
  memmove(channel->out_buf, channel->out_buf + channel->frame_length,
      (size_t)keep * sizeof *channel->out_buf);
  memcpy(channel->out_buf + keep, out,
      (size_t)channel->frame_length * sizeof *channel->out_buf);
  channel->first_after_reset = false;
}
