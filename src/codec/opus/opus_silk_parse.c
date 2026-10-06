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
 * One SILK frame's parameters, read off the bitstream. Never installed.
 *
 * RFC 6716 section 4.2.7, in the order Table 5 gives: the frame type,
 * the subframe gains, the line spectral frequencies, the pitch lag and
 * its filter, and the noise generator's seed. None of it is turned
 * into a filter here; these are indices into the tables, and section
 * 4.2.8 is where they become numbers.
 *
 * **Three parameters are coded against something outside the frame**,
 * and each of the three is a different kind of outside. The first
 * subframe's gain is coded against the previous *subframe's*, which may
 * be in the previous frame. The pitch lag is coded against the previous
 * frame's lag, but only if that frame was voiced and this one is not
 * the first in the packet. The frame type is chosen by the
 * voice-activity flag decoded for this frame in the packet header,
 * which is twenty symbols earlier. Any of the three read wrongly costs
 * a different number of symbols and desynchronises everything after.
 */

#include "opus_silk.h"
#include "opus_silk_tables.h"
#include <string.h>

/** The number of vectors in either stage-1 LSF codebook. */
#define SILK_NLSF_VECTORS 32

/** Symbols in one stage-2 LSF distribution: -4 to 4 inclusive. */
#define SILK_NLSF_STAGE2_SYMBOLS (2 * SILK_NLSF_MAX_AMPLITUDE + 1)

/**
 * @brief Which stage-2 distribution each LSF coefficient uses.
 *
 * This is half of the reference's `silk_NLSF_unpack`: the select table
 * packs two choices into each byte, and only one of them - the
 * distribution - is needed to read the bitstream. The other is a
 * prediction weight, which nothing in the parse touches.
 *
 * @param ec_ix Receives one offset into the stage-2 iCDF table per
 *   coefficient.
 * @param select The codebook's select table.
 * @param order 10 or 16.
 * @param vector Which stage-1 vector was decoded.
 */
static void nlsf_distributions(int16_t * ec_ix, const unsigned char * select,
    int order, int vector) {
  const unsigned char * row = select + (size_t)vector * (size_t)(order / 2);
  for (int i = 0; i < order; i += 2) {
    unsigned entry = *row++;
    ec_ix[i] = (int16_t)(((entry >> 1) & 7u) * SILK_NLSF_STAGE2_SYMBOLS);
    ec_ix[i + 1] = (int16_t)(((entry >> 5) & 7u) * SILK_NLSF_STAGE2_SYMBOLS);
  }
}

/** The pitch lag's low bits: a uniform symbol whose size is the rate. */
static const unsigned char * pitch_lag_low_bits(int fs_khz) {
  if (fs_khz == 8) {
    return gaud_opus_silk_uniform4_icdf;
  }
  return fs_khz == 12 ? gaud_opus_silk_uniform6_icdf
                      : gaud_opus_silk_uniform8_icdf;
}

/** The subframe pitch contour's distribution, by rate and frame length. */
static const unsigned char * pitch_contour(int fs_khz, int nb_subfr) {
  if (fs_khz == 8) {
    return nb_subfr == SILK_MAX_SUBFRAMES
        ? gaud_opus_silk_pitch_contour_nb_icdf
        : gaud_opus_silk_pitch_contour_10_ms_nb_icdf;
  }
  return nb_subfr == SILK_MAX_SUBFRAMES
      ? gaud_opus_silk_pitch_contour_icdf
      : gaud_opus_silk_pitch_contour_10_ms_icdf;
}

bool gaud_silk_decoder_init(
    SILK_Decoder * decoder, int channels, int fs_khz, int duration_ms) {
  int frames;
  int subframes;
  if (channels < 1 || channels > 2) {
    return false;
  }
  if (fs_khz != 8 && fs_khz != 12 && fs_khz != 16) {
    return false;
  }
  switch (duration_ms) {
    case 10: frames = 1; subframes = 2; break;
    case 20: frames = 1; subframes = 4; break;
    case 40: frames = 2; subframes = 4; break;
    case 60: frames = 3; subframes = 4; break;
    default: return false;
  }
  decoder->channels = channels;
  decoder->stereo_pred_q13[0] = 0;
  decoder->stereo_pred_q13[1] = 0;
  decoder->mid_only = false;
  for (int c = 0; c < 2; ++c) {
    SILK_Channel * channel = &decoder->channel[c];
    int subfr_length = 5 * fs_khz;
    int frame_length = subframes * subfr_length;
    // A 10 ms medium-band frame is 120 samples, which is not a multiple
    // of the 16-sample shell block. Section 4.2.7.8 says to code eight
    // blocks and discard the last eight samples - and to parse any
    // pulses an encoder puts there, which is why this rounds up rather
    // than truncating.
    int blocks = (frame_length + SILK_SHELL_BLOCK - 1) / SILK_SHELL_BLOCK;
    // Only the bitstream-carried history survives a reconfiguration,
    // and only while the rate is the same. A rate change leaves the
    // previous lag meaningless, because a lag is in samples. A change
    // of frame length alone leaves all of it meaningful, and the
    // reference keeps it: resetting here as well would drop the
    // synthesis state whenever a stream moved between 10 and 20 ms.
    if (channel->fs_khz != fs_khz) {
      gaud_silk_resampler_init(&decoder->resampler[c], fs_khz);
      channel->prev_signal_type = SILK_SIGNAL_INACTIVE;
      channel->prev_lag_index = 0;
      // The synthesis state is in samples at the old rate and in
      // units of the old gain, so none of it survives a change of
      // either. The gain starts at one rather than zero because the
      // first subframe divides by it.
      channel->prev_gain_index = 0;
      channel->prev_gain_q16 = 1 << 16;
      channel->first_after_reset = true;
      channel->lag_prev = 100;
      channel->plc_signal_type = SILK_SIGNAL_INACTIVE;
      memset(channel->prev_nlsf_q15, 0, sizeof channel->prev_nlsf_q15);
      memset(channel->lpc_state_q14, 0, sizeof channel->lpc_state_q14);
      memset(channel->out_buf, 0, sizeof channel->out_buf);
    }
    channel->fs_khz = fs_khz;
    channel->nb_subfr = subframes;
    channel->lpc_order = fs_khz == 16 ? 16 : 10;
    channel->subfr_length = subfr_length;
    channel->frame_length = frame_length;
    channel->shell_blocks = blocks;
    channel->ltp_mem_length = SILK_LTP_MEM_MS * fs_khz;
    channel->frames_per_packet = frames;
    channel->frames_decoded = 0;
    channel->lbrr_flag = false;
    memset(channel->vad, 0, sizeof(channel->vad));
    memset(channel->lbrr, 0, sizeof(channel->lbrr));
  }
  return true;
}

void gaud_silk_decode_header(SILK_Decoder * decoder, OPUS_Range * range) {
  // Section 4.2.3. Both flags are uniform binary symbols, which is what
  // lets a receiver read them straight out of the first byte without a
  // range decoder - and the reason they are first.
  for (int c = 0; c < decoder->channels; ++c) {
    SILK_Channel * channel = &decoder->channel[c];
    for (int frame = 0; frame < channel->frames_per_packet; ++frame) {
      channel->vad[frame] = gaud_opus_dec_bit_logp(range, 1) != 0;
    }
    channel->lbrr_flag = gaud_opus_dec_bit_logp(range, 1) != 0;
  }
  // Section 4.2.4. A packet of 10 or 20 ms has at most one redundancy
  // frame per channel, so the flag above already says which; a longer
  // one spends a symbol saying which of its two or three it has.
  for (int c = 0; c < decoder->channels; ++c) {
    SILK_Channel * channel = &decoder->channel[c];
    memset(channel->lbrr, 0, sizeof(channel->lbrr));
    if (!channel->lbrr_flag) {
      continue;
    }
    if (channel->frames_per_packet == 1) {
      channel->lbrr[0] = true;
      continue;
    }
    const unsigned char * icdf = channel->frames_per_packet == 2
        ? gaud_opus_silk_lbrr_flags_2_icdf
        : gaud_opus_silk_lbrr_flags_3_icdf;
    // The symbol is the flags packed least significant bit first, and
    // one is added because the all-zero case cannot arise: the
    // per-channel flag above already said there was at least one.
    int packed = gaud_opus_dec_icdf(range, icdf, 8) + 1;
    for (int frame = 0; frame < channel->frames_per_packet; ++frame) {
      channel->lbrr[frame] = ((packed >> frame) & 1) != 0;
    }
  }
}

void gaud_silk_decode_stereo_pred(OPUS_Range * range, int32_t pred_q13[2]) {
  int index[2][3];
  // One symbol carries both weights' high-order index, as a number in
  // base five: the first weight's is the quotient and the second's the
  // remainder. Five is STEREO_QUANT_SUB_STEPS, and the table has
  // sixteen entries, so three times five covers the fifteen gaps.
  int joint = gaud_opus_dec_icdf(
      range, gaud_opus_silk_stereo_pred_joint_icdf, 8);
  index[0][2] = joint / 5;
  index[1][2] = joint - 5 * index[0][2];
  for (int n = 0; n < 2; ++n) {
    index[n][0] = gaud_opus_dec_icdf(range, gaud_opus_silk_uniform3_icdf, 8);
    index[n][1] = gaud_opus_dec_icdf(range, gaud_opus_silk_uniform5_icdf, 8);
  }
  for (int n = 0; n < 2; ++n) {
    index[n][0] += 3 * index[n][2];
    int32_t low = gaud_opus_silk_stereo_pred_quant_q13[index[n][0]];
    // Half a sub-step, in Q13: the gap scaled by 0.5/5 in Q16, which is
    // 6554, and then the odd multiple 2*i+1 puts the result in the
    // middle of the i'th of five equal pieces rather than at its edge.
    int32_t gap = gaud_opus_silk_stereo_pred_quant_q13[index[n][0] + 1] - low;
    int32_t step = (int32_t)(((int64_t)gap * 6554) >> 16);
    pred_q13[n] = low + step * (2 * index[n][1] + 1);
  }
  // The reference leaves the first predictor reduced by the second,
  // because that is the form silk_stereo_MS_to_LR wants.
  pred_q13[0] -= pred_q13[1];
}

bool gaud_silk_decode_mid_only(OPUS_Range * range) {
  return gaud_opus_dec_icdf(
             range, gaud_opus_silk_stereo_only_code_mid_icdf, 8)
      != 0;
}

void gaud_silk_decode_indices(SILK_Channel * channel, OPUS_Range * range,
    int frame, bool lbrr, SILK_Coding coding) {
  SILK_Indices * indices = &channel->indices;
  int16_t ec_ix[SILK_MAX_LPC_ORDER];

  // Section 4.2.7.3. A redundancy frame is only ever sent for speech,
  // so it uses the active distribution whatever the frame's own
  // voice-activity flag said - the flag describes the frame it stands
  // in for, not this one.
  int type;
  if (lbrr || channel->vad[frame]) {
    type = gaud_opus_dec_icdf(range, gaud_opus_silk_type_offset_vad_icdf, 8)
        + 2;
  } else {
    type =
        gaud_opus_dec_icdf(range, gaud_opus_silk_type_offset_no_vad_icdf, 8);
  }
  indices->signal_type = (int8_t)(type >> 1);
  indices->quant_offset_type = (int8_t)(type & 1);

  // Section 4.2.7.4. The first subframe's gain is absolute only when
  // there is no previous subframe to code it against.
  if (coding == SILK_CODE_CONDITIONAL) {
    indices->gains[0] =
        (int8_t)gaud_opus_dec_icdf(range, gaud_opus_silk_delta_gain_icdf, 8);
  } else {
    const unsigned char * icdf =
        gaud_opus_silk_gain_icdf + 8 * indices->signal_type;
    int high = gaud_opus_dec_icdf(range, icdf, 8);
    int low = gaud_opus_dec_icdf(range, gaud_opus_silk_uniform8_icdf, 8);
    indices->gains[0] = (int8_t)((high << 3) + low);
  }
  for (int i = 1; i < channel->nb_subfr; ++i) {
    indices->gains[i] =
        (int8_t)gaud_opus_dec_icdf(range, gaud_opus_silk_delta_gain_icdf, 8);
  }

  // Section 4.2.7.5. The stage-1 index picks a codebook vector and,
  // with it, which distribution each of the residual's coefficients is
  // read under - so the two cannot be read in either order.
  bool wideband = channel->lpc_order == SILK_MAX_LPC_ORDER;
  const unsigned char * cb1_icdf = wideband
      ? gaud_opus_silk_nlsf_cb1_icdf_wb
      : gaud_opus_silk_nlsf_cb1_icdf_nb_mb;
  const unsigned char * cb2_icdf = wideband
      ? gaud_opus_silk_nlsf_cb2_icdf_wb
      : gaud_opus_silk_nlsf_cb2_icdf_nb_mb;
  const unsigned char * select = wideband
      ? gaud_opus_silk_nlsf_cb2_select_wb
      : gaud_opus_silk_nlsf_cb2_select_nb_mb;
  // Voiced frames get their own half of the stage-1 distribution; the
  // two unvoiced types share the other.
  indices->nlsf[0] = (int8_t)gaud_opus_dec_icdf(range,
      cb1_icdf + (indices->signal_type >> 1) * SILK_NLSF_VECTORS, 8);
  nlsf_distributions(
      ec_ix, select, channel->lpc_order, indices->nlsf[0]);
  for (int i = 0; i < channel->lpc_order; ++i) {
    int value = gaud_opus_dec_icdf(range, cb2_icdf + ec_ix[i], 8);
    // The residual saturates at either end of its nine symbols, and an
    // extension symbol then says how much further it goes. Only the
    // two outermost values can be extended, which is what keeps the
    // extension from being ambiguous.
    if (value == 0) {
      value -= gaud_opus_dec_icdf(range, gaud_opus_silk_nlsf_ext_icdf, 8);
    } else if (value == 2 * SILK_NLSF_MAX_AMPLITUDE) {
      value += gaud_opus_dec_icdf(range, gaud_opus_silk_nlsf_ext_icdf, 8);
    }
    indices->nlsf[i + 1] = (int8_t)(value - SILK_NLSF_MAX_AMPLITUDE);
  }

  // Section 4.2.7.5.5. A 10 ms frame has no room to interpolate, and
  // the value 4 is the one that means "use the new coefficients
  // throughout", so it is what a 10 ms frame is given.
  if (channel->nb_subfr == SILK_MAX_SUBFRAMES) {
    indices->nlsf_interp_q2 = (int8_t)gaud_opus_dec_icdf(
        range, gaud_opus_silk_nlsf_interpolation_factor_icdf, 8);
  } else {
    indices->nlsf_interp_q2 = 4;
  }

  if (indices->signal_type == SILK_SIGNAL_VOICED) {
    // Section 4.2.7.6.1. The delta is only offered when there is a
    // previous voiced frame in this packet to be a delta from, and
    // even then the encoder may decline it - which it signals by
    // coding zero, the one delta value that is not a delta.
    bool absolute = true;
    if (coding == SILK_CODE_CONDITIONAL
        && channel->prev_signal_type == SILK_SIGNAL_VOICED) {
      int delta =
          gaud_opus_dec_icdf(range, gaud_opus_silk_pitch_delta_icdf, 8);
      if (delta > 0) {
        indices->lag_index = (int16_t)(channel->prev_lag_index + delta - 9);
        absolute = false;
      }
    }
    if (absolute) {
      // The high part is in units of half a millisecond, which is what
      // multiplying by fs_kHz/2 means; the low part is a uniform
      // symbol whose size is the sample rate in kHz.
      indices->lag_index = (int16_t)(
          gaud_opus_dec_icdf(range, gaud_opus_silk_pitch_lag_icdf, 8)
          * (channel->fs_khz >> 1));
      indices->lag_index = (int16_t)(indices->lag_index
          + gaud_opus_dec_icdf(range, pitch_lag_low_bits(channel->fs_khz), 8));
    }
    channel->prev_lag_index = indices->lag_index;

    indices->contour_index = (int8_t)gaud_opus_dec_icdf(
        range, pitch_contour(channel->fs_khz, channel->nb_subfr), 8);

    // Section 4.2.7.6.2. The periodicity index chooses a codebook, and
    // the codebooks have different sizes, so it also chooses how many
    // bits each of the following indices costs.
    indices->per_index = (int8_t)gaud_opus_dec_icdf(
        range, gaud_opus_silk_ltp_per_index_icdf, 8);
    const unsigned char * ltp_icdf = indices->per_index == 0
        ? gaud_opus_silk_ltp_gain_icdf_0
        : (indices->per_index == 1 ? gaud_opus_silk_ltp_gain_icdf_1
                                   : gaud_opus_silk_ltp_gain_icdf_2);
    for (int k = 0; k < channel->nb_subfr; ++k) {
      indices->ltp[k] = (int8_t)gaud_opus_dec_icdf(range, ltp_icdf, 8);
    }

    // Section 4.2.7.6.3. Only a frame that cannot rely on the previous
    // one's samples says how far to scale the long-term history, since
    // that is the only case where the choice can matter.
    indices->ltp_scale_index = coding == SILK_CODE_INDEPENDENT
        ? (int8_t)gaud_opus_dec_icdf(range, gaud_opus_silk_ltpscale_icdf, 8)
        : 0;
  } else {
    indices->lag_index = 0;
    indices->contour_index = 0;
    indices->per_index = 0;
    memset(indices->ltp, 0, sizeof(indices->ltp));
    indices->ltp_scale_index = 0;
  }
  channel->prev_signal_type = indices->signal_type;

  // Section 4.2.7.7.
  indices->seed =
      (int8_t)gaud_opus_dec_icdf(range, gaud_opus_silk_uniform4_icdf, 8);
}
