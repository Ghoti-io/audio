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
 * One SILK frame, from the range decoder to 48 kHz samples. RFC 6716
 * section 4.2. Never installed.
 *
 * Everything below this file does one stage; this is the order they
 * go in, and the state that has to survive between frames when the
 * stream changes shape under it.
 *
 * **Three kinds of change reset part of the state and not the rest.**
 * A change of internal rate restarts a channel's synthesis, because
 * its history is in samples at the old rate. A channel count that
 * grows restarts the second channel, which has never been decoded. And
 * the side channel restarts when it returns after a frame without it,
 * because while it was absent its history was not being kept - the
 * encoder was not coding it, so there was nothing to keep. What is
 * deliberately *not* reset on any of these is the bitstream's own
 * memory, the last pitch lag and signal type, because the symbols
 * after them are decoded against it and those have to line up with an
 * encoder that never reset anything it was not asked to.
 */

#include "opus_silk.h"
#include "opus_silk_math.h"
#include <string.h>

/** The side channel's gain index after a reset. */
#define SILK_RESET_GAIN_INDEX 10

bool gaud_silk_configure(SILK_Decoder * decoder, int channels,
    int api_channels, int fs_khz, int duration_ms) {
  int previous_internal = decoder->channels;
  int previous_api = decoder->api_channels;
  if (channels < 1 || channels > 2 || api_channels < 1 || api_channels > 2) {
    return false;
  }
  // A stream that drops to one channel at the rate it already had keeps
  // the second output channel's resampler running on the mid signal,
  // so that the second output does not click.
  decoder->stereo_to_mono = channels == 1 && previous_internal == 2
      && fs_khz == decoder->channel[0].fs_khz;
  if (channels > previous_internal) {
    memset(&decoder->channel[1], 0, sizeof decoder->channel[1]);
    memset(&decoder->resampler[1], 0, sizeof decoder->resampler[1]);
  }
  if (!gaud_silk_decoder_init(decoder, channels, fs_khz, duration_ms)) {
    return false;
  }
  if (api_channels == 2 && channels == 2
      && (previous_api == 1 || previous_internal == 1)) {
    memset(decoder->pred_prev_q13, 0, sizeof decoder->pred_prev_q13);
    memset(decoder->s_side, 0, sizeof decoder->s_side);
    decoder->resampler[1] = decoder->resampler[0];
  }
  decoder->api_channels = api_channels;
  return true;
}

/** Forget the side channel's synthesis, as the format says to on its return. */
static void reset_side_channel(SILK_Channel * channel) {
  memset(channel->out_buf, 0, sizeof channel->out_buf);
  memset(channel->lpc_state_q14, 0, sizeof channel->lpc_state_q14);
  channel->prev_gain_index = SILK_RESET_GAIN_INDEX;
  channel->first_after_reset = true;
  channel->plc_signal_type = SILK_SIGNAL_INACTIVE;
  channel->lag_prev = 100;
}

/** Push a frame onto the end of the channel's history of output. */
static void push_output(SILK_Channel * channel, const int16_t * frame) {
  int keep = channel->ltp_mem_length - channel->frame_length;
  memmove(channel->out_buf, channel->out_buf + channel->frame_length,
      (size_t)keep * sizeof *channel->out_buf);
  memcpy(channel->out_buf + keep, frame,
      (size_t)channel->frame_length * sizeof *channel->out_buf);
}

/**
 * @brief Everything after the frames are decoded: stereo, then 48 kHz.
 *
 * @param decoder The decoder.
 * @param samples Each channel's frame, at offset two.
 * @param pred_q13 The stereo weights to unmix with.
 * @param out Receives the interleaved output.
 * @param out_samples Receives how many per channel.
 */
static void finish_frame(SILK_Decoder * decoder, int16_t (*samples)[2 + SILK_MAX_FRAME_LENGTH],
    const int32_t * pred_q13, int16_t * out, int * out_samples) {
  int internal = decoder->channels;
  int api = decoder->api_channels;
  int fs_khz = decoder->channel[0].fs_khz;
  int length = decoder->channel[0].frame_length;
  int16_t resampled[960];

  if (api == 2 && internal == 2) {
    gaud_silk_stereo_ms_to_lr(
        decoder, samples[0], samples[1], pred_q13, fs_khz, length);
  } else {
    // Just the one-frame delay the stereo path has.
    samples[0][0] = decoder->s_mid[0];
    samples[0][1] = decoder->s_mid[1];
    decoder->s_mid[0] = samples[0][length];
    decoder->s_mid[1] = samples[0][length + 1];
  }

  int per_channel = length * 48 / fs_khz;
  *out_samples = per_channel;
  int coded = api < internal ? api : internal;
  for (int c = 0; c < coded; ++c) {
    gaud_silk_resample(
        &decoder->resampler[c], resampled, samples[c] + 1, length);
    for (int i = 0; i < per_channel; ++i) {
      out[api * i + c] = resampled[i];
    }
  }
  if (api == 2 && internal == 1) {
    if (decoder->stereo_to_mono) {
      gaud_silk_resample(
          &decoder->resampler[1], resampled, samples[0] + 1, length);
      for (int i = 0; i < per_channel; ++i) {
        out[2 * i + 1] = resampled[i];
      }
    } else {
      for (int i = 0; i < per_channel; ++i) {
        out[2 * i + 1] = out[2 * i];
      }
    }
  }
  decoder->stereo_to_mono = false;
}

void gaud_silk_decode_frame(SILK_Decoder * decoder, OPUS_Range * range,
    int16_t * out, int * out_samples) {
  int frame = decoder->channel[0].frames_decoded;
  int internal = decoder->channels;
  int length = decoder->channel[0].frame_length;
  // The two samples in front of each channel hold the previous frame's
  // last ones, and the filter below reads one of them.
  int16_t samples[2][2 + SILK_MAX_FRAME_LENGTH];
  SILK_Parameters parameters;

  if (frame == 0) {
    gaud_silk_decode_header(decoder, range);
    gaud_silk_skip_redundancy(decoder, range);
  }
  bool was_mid_only = decoder->prev_mid_only;
  bool mid_only = gaud_silk_parse_frame(decoder, range, frame);
  if (internal == 2 && !mid_only && was_mid_only) {
    reset_side_channel(&decoder->channel[1]);
  }

  for (int c = 0; c < internal; ++c) {
    SILK_Channel * channel = &decoder->channel[c];
    if (c > 0 && mid_only) {
      memset(samples[c] + 2, 0, (size_t)length * sizeof(int16_t));
      continue;
    }
    gaud_silk_decode_parameters(channel, &parameters, channel->coding);
    gaud_silk_decode_core(channel, &parameters, samples[c] + 2);
    // Concealment's memory of this frame, and the comfort noise's.
    gaud_silk_plc_update(channel, &parameters);
    channel->loss_cnt = 0;
    gaud_silk_plc_glue(channel, samples[c] + 2, length);
    gaud_silk_cng(channel, &parameters, samples[c] + 2, length);
  }
  finish_frame(decoder, samples, decoder->stereo_pred_q13, out, out_samples);
}

/** One channel's frame, extrapolated from what came before it. */
static void conceal_channel(SILK_Channel * channel, int16_t * frame, int length) {
  gaud_silk_plc_conceal(channel, frame);
  push_output(channel, frame);
  gaud_silk_plc_glue(channel, frame, length);
  gaud_silk_cng(channel, NULL, frame, length);
}

void gaud_silk_decode_lost(
    SILK_Decoder * decoder, int16_t * out, int * out_samples) {
  int internal = decoder->channels;
  int length = decoder->channel[0].frame_length;
  int16_t samples[2][2 + SILK_MAX_FRAME_LENGTH];

  // A lost frame carries no symbols, so the stereo weights and the
  // side channel's presence are what the previous frame said.
  bool was_mid_only = decoder->prev_mid_only;
  if (internal == 2 && was_mid_only) {
    reset_side_channel(&decoder->channel[1]);
  }
  bool has_side = !was_mid_only;
  for (int c = 0; c < internal; ++c) {
    SILK_Channel * channel = &decoder->channel[c];
    if (c == 0 || has_side) {
      conceal_channel(channel, samples[c] + 2, length);
    } else {
      memset(samples[c] + 2, 0, (size_t)length * sizeof(int16_t));
    }
    ++channel->frames_decoded;
  }
  int32_t pred_q13[2] = {decoder->pred_prev_q13[0], decoder->pred_prev_q13[1]};
  finish_frame(decoder, samples, pred_q13, out, out_samples);
  // On a loss the gain clamp is removed, so that the energy does not
  // bounce back if the loss comes while it is falling.
  for (int c = 0; c < internal; ++c) {
    decoder->channel[c].prev_gain_index = SILK_RESET_GAIN_INDEX;
  }
}

void gaud_silk_decode_fec(SILK_Decoder * decoder, OPUS_Range * range,
    int16_t * out, int * out_samples) {
  int frame = decoder->channel[0].frames_decoded;
  int internal = decoder->channels;
  int length = decoder->channel[0].frame_length;
  int16_t samples[2][2 + SILK_MAX_FRAME_LENGTH];
  SILK_Parameters parameters;
  int32_t pred_q13[2] = {0, 0};
  bool mid_only = false;

  if (frame == 0) {
    gaud_silk_decode_header(decoder, range);
  }
  if (internal == 2) {
    if (decoder->channel[0].lbrr[frame]) {
      gaud_silk_decode_stereo_pred(range, pred_q13);
      // As in the regular frames, the flag is only sent when the side
      // channel has nothing of its own here to say it is present.
      mid_only = !decoder->channel[1].lbrr[frame]
          && gaud_silk_decode_mid_only(range);
    }
    else {
      pred_q13[0] = decoder->pred_prev_q13[0];
      pred_q13[1] = decoder->pred_prev_q13[1];
    }
  }
  bool was_mid_only = decoder->prev_mid_only;
  if (internal == 2 && !mid_only && was_mid_only) {
    reset_side_channel(&decoder->channel[1]);
  }
  // A side channel that was absent is still concealed if the copy has
  // nothing for it, but only a copy of its own brings it back.
  bool has_side = !was_mid_only
      || (internal == 2 && decoder->channel[1].lbrr[frame]);
  for (int c = 0; c < internal; ++c) {
    SILK_Channel * channel = &decoder->channel[c];
    if (c == 0 || has_side) {
      if (channel->lbrr[frame]) {
        SILK_Coding coding = frame > 0 && channel->lbrr[frame - 1]
            ? SILK_CODE_CONDITIONAL
            : SILK_CODE_INDEPENDENT;
        channel->coding = coding;
        gaud_silk_decode_indices(channel, range, frame, true, coding);
        gaud_silk_decode_pulses(channel, range);
        gaud_silk_decode_parameters(channel, &parameters, coding);
        gaud_silk_decode_core(channel, &parameters, samples[c] + 2);
        gaud_silk_plc_update(channel, &parameters);
        channel->loss_cnt = 0;
        gaud_silk_plc_glue(channel, samples[c] + 2, length);
        gaud_silk_cng(channel, &parameters, samples[c] + 2, length);
      }
      else {
        conceal_channel(channel, samples[c] + 2, length);
      }
    }
    else {
      memset(samples[c] + 2, 0, (size_t)length * sizeof(int16_t));
    }
    ++channel->frames_decoded;
  }
  finish_frame(decoder, samples, pred_q13, out, out_samples);
  decoder->prev_mid_only = mid_only;
}
