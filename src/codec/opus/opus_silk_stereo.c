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
 * Mid and side back to left and right. RFC 6716 section 4.2.9. Never
 * installed.
 *
 * The side channel is coded as what is left of it once a prediction
 * from the mid channel is subtracted, so this adds the prediction back
 * before forming the sum and difference. The prediction uses a
 * low-passed mid (a three-tap filter, which is also why every output
 * sample here is one behind the decoder's) and the mid itself, with
 * weights that glide from the previous frame's to this frame's over the
 * first eight milliseconds rather than jumping.
 */

#include "opus_silk.h"
#include "opus_silk_math.h"

/** Milliseconds over which the weights move to their new values. */
#define SILK_STEREO_INTERP_MS 8

void gaud_silk_stereo_ms_to_lr(SILK_Decoder * decoder, int16_t * mid,
    int16_t * side, const int32_t * pred_q13, int fs_khz, int length) {
  // Two samples of history in front, and the new tail saved for next time.
  mid[0] = decoder->s_mid[0];
  mid[1] = decoder->s_mid[1];
  side[0] = decoder->s_side[0];
  side[1] = decoder->s_side[1];
  decoder->s_mid[0] = mid[length];
  decoder->s_mid[1] = mid[length + 1];
  decoder->s_side[0] = side[length];
  decoder->s_side[1] = side[length + 1];

  int interp = SILK_STEREO_INTERP_MS * fs_khz;
  int32_t pred0 = decoder->pred_prev_q13[0];
  int32_t pred1 = decoder->pred_prev_q13[1];
  int32_t denom_q16 = (1 << 16) / interp;
  int32_t delta0 = gaud_silk_rshift_round(
      gaud_silk_smulbb(pred_q13[0] - decoder->pred_prev_q13[0], denom_q16),
      16);
  int32_t delta1 = gaud_silk_rshift_round(
      gaud_silk_smulbb(pred_q13[1] - decoder->pred_prev_q13[1], denom_q16),
      16);
  for (int n = 0; n < length; ++n) {
    if (n < interp) {
      pred0 += delta0;
      pred1 += delta1;
    } else if (n == interp) {
      pred0 = pred_q13[0];
      pred1 = pred_q13[1];
    }
    int32_t sum = (mid[n] + mid[n + 2] + 2 * mid[n + 1]) * 512;
    sum = gaud_silk_smlawb(
        (int32_t)((uint32_t)side[n + 1] << 8), sum, pred0);
    sum = gaud_silk_smlawb(
        sum, (int32_t)((uint32_t)mid[n + 1] << 11), pred1);
    side[n + 1] = (int16_t)gaud_silk_sat16(gaud_silk_rshift_round(sum, 8));
  }
  decoder->pred_prev_q13[0] = pred_q13[0];
  decoder->pred_prev_q13[1] = pred_q13[1];

  for (int n = 0; n < length; ++n) {
    int32_t sum = mid[n + 1] + (int32_t)side[n + 1];
    int32_t diff = mid[n + 1] - (int32_t)side[n + 1];
    mid[n + 1] = (int16_t)gaud_silk_sat16(sum);
    side[n + 1] = (int16_t)gaud_silk_sat16(diff);
  }
}
