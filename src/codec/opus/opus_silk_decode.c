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
 * The order a packet's SILK frames come in. RFC 6716 4.2. Never installed.
 *
 * Section 4.2.2's Table 3 is the whole of this file: the header flags
 * for the packet, then every redundancy frame, then every regular
 * frame, with the stereo parameters interleaved before each frame
 * rather than gathered with the header. Nothing here reads a sample
 * value, and that is deliberate - see opus_silk.h.
 *
 * **The hard part is deciding, for each frame, which of the three
 * conditional-coding cases applies**, because that changes how many
 * symbols the frame contains. Three facts conspire:
 *
 *   - a frame that is first in its packet has nothing to be
 *     conditional on, so it is independent;
 *   - a side channel whose previous frame was skipped is independent
 *     of the parameters but *not* of the samples, because the samples
 *     are a known zero - so it takes the middle case, which drops the
 *     LTP scaling factor it would otherwise code;
 *   - whether the previous frame was skipped is a fact about the
 *     previous packet when the frame is the first in this one, which
 *     is why ::SILK_Decoder carries it.
 *
 * The redundancy frames use a simpler rule of their own: a redundancy
 * frame is conditional exactly when the previous frame of the same
 * channel also carried redundancy.
 */

#include "opus_silk.h"

void gaud_silk_parse_packet(SILK_Decoder * decoder, OPUS_Range * range) {
  int frames = decoder->channel[0].frames_per_packet;
  bool stereo = decoder->channels == 2;

  gaud_silk_decode_header(decoder, range);

  // Section 4.2.5. A receiver that lost nothing still decodes every
  // symbol of these, because they sit in front of the frames it wants
  // in the same arithmetic-coded stream. Only the result is discarded.
  for (int frame = 0; frame < frames; ++frame) {
    for (int c = 0; c < decoder->channels; ++c) {
      SILK_Channel * channel = &decoder->channel[c];
      if (!channel->lbrr[frame]) {
        continue;
      }
      if (stereo && c == 0) {
        int32_t pred[2];
        gaud_silk_decode_stereo_pred(range, pred);
        // The mid-only flag is present only when the side channel has
        // nothing of its own here to make it redundant.
        if (!decoder->channel[1].lbrr[frame]) {
          (void)gaud_silk_decode_mid_only(range);
        }
      }
      SILK_Coding coding = frame > 0 && channel->lbrr[frame - 1]
          ? SILK_CODE_CONDITIONAL
          : SILK_CODE_INDEPENDENT;
      gaud_silk_decode_indices(channel, range, frame, true, coding);
      gaud_silk_decode_pulses(channel, range);
    }
  }

  for (int frame = 0; frame < frames; ++frame) {
    bool mid_only = false;
    if (stereo) {
      gaud_silk_decode_stereo_pred(range, decoder->stereo_pred_q13);
      // A side channel with voice activity is always coded, so the
      // flag is only sent when it might be absent.
      mid_only = !decoder->channel[1].vad[frame]
          && gaud_silk_decode_mid_only(range);
    }
    for (int c = 0; c < decoder->channels; ++c) {
      if (c > 0 && mid_only) {
        continue;
      }
      SILK_Coding coding;
      if (frame == 0) {
        coding = SILK_CODE_INDEPENDENT;
      } else if (c > 0 && decoder->prev_mid_only) {
        coding = SILK_CODE_INDEPENDENT_NO_SCALE;
      } else {
        coding = SILK_CODE_CONDITIONAL;
      }
      gaud_silk_decode_indices(
          &decoder->channel[c], range, frame, false, coding);
      gaud_silk_decode_pulses(&decoder->channel[c], range);
    }
    decoder->prev_mid_only = mid_only;
    decoder->channel[0].frames_decoded = frame + 1;
    decoder->channel[1].frames_decoded = frame + 1;
  }
  decoder->mid_only = decoder->prev_mid_only;
}
