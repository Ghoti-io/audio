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
 * SILK's output resampler, from its own rate to 48 kHz. RFC 6716
 * section 4.2.10. Never installed.
 *
 * The format does not specify this stage, and the reference's is what
 * the conformance vectors were made with, so it is what this is. Every
 * SILK rate reaches 48 kHz by the same two steps: a pair of all-pass
 * chains doubles the rate with a notch above the old Nyquist, and a
 * 12-phase polyphase interpolator reads the doubled signal at the
 * fractional positions the remaining ratio calls for (three for 8 kHz,
 * two for 12, one and a half for 16).
 *
 * **The first millisecond is delayed, by a different amount per rate**,
 * so that the three internal rates all have the same total delay by
 * the time they reach the output: 0 samples at 8 kHz, 4 at 12 kHz and
 * 7 at 16 kHz. The delayed samples are held in the state and played at
 * the start of the next frame.
 */

#include "opus_silk.h"
#include "opus_silk_math.h"
#include "opus_silk_tables.h"
#include <string.h>

/** The output rate, in kHz. */
#define SILK_RESAMPLER_OUT_KHZ 48

/** Milliseconds of input handled per pass, bounding the work buffer. */
#define SILK_RESAMPLER_BATCH_MS 10

/** The most input samples one pass handles. */
#define SILK_RESAMPLER_BATCH_MAX (SILK_RESAMPLER_BATCH_MS * SILK_RESAMPLER_MAX_KHZ)

void gaud_silk_resampler_init(SILK_Resampler * resampler, int fs_in_khz) {
  // Indexed by 8, 12 and 16 kHz: the delay that equalises the total.
  static const int kDelay[3] = {0, 4, 7};
  memset(resampler, 0, sizeof *resampler);
  resampler->fs_in_khz = fs_in_khz;
  resampler->input_delay = kDelay[(fs_in_khz - 8) >> 2];
  // Input steps per output step, in the doubled domain, rounded up so
  // that the interpolator never reads past the end of what it has.
  int32_t fs_in = fs_in_khz * 1000;
  int32_t fs_out = SILK_RESAMPLER_OUT_KHZ * 1000;
  resampler->inv_ratio_q16 = (int32_t)((uint32_t)((fs_in << 15) / fs_out) << 2);
  while (gaud_silk_smulww(resampler->inv_ratio_q16, fs_out) < (fs_in << 1)) {
    ++resampler->inv_ratio_q16;
  }
}

/** Double the rate with two three-section all-pass chains. */
static void up2_hq(int32_t * state, int16_t * out, const int16_t * in,
    int length) {
  const int16_t * even = gaud_opus_silk_resampler_up2_hq_0;
  const int16_t * odd = gaud_opus_silk_resampler_up2_hq_1;
  for (int k = 0; k < length; ++k) {
    // State and arithmetic are in Q10.
    int32_t in32 = (int32_t)((uint32_t)in[k] << 10);
    int32_t y;
    int32_t x;
    int32_t first;
    int32_t second;

    y = in32 - state[0];
    x = gaud_silk_smulwb(y, even[0]);
    first = state[0] + x;
    state[0] = in32 + x;
    y = first - state[1];
    x = gaud_silk_smulwb(y, even[1]);
    second = state[1] + x;
    state[1] = first + x;
    y = second - state[2];
    x = gaud_silk_smlawb(y, y, even[2]);
    first = state[2] + x;
    state[2] = second + x;
    out[2 * k] = (int16_t)gaud_silk_sat16(gaud_silk_rshift_round(first, 10));

    y = in32 - state[3];
    x = gaud_silk_smulwb(y, odd[0]);
    first = state[3] + x;
    state[3] = in32 + x;
    y = first - state[4];
    x = gaud_silk_smulwb(y, odd[1]);
    second = state[4] + x;
    state[4] = first + x;
    y = second - state[5];
    x = gaud_silk_smlawb(y, y, odd[2]);
    first = state[5] + x;
    state[5] = second + x;
    out[2 * k + 1] =
        (int16_t)gaud_silk_sat16(gaud_silk_rshift_round(first, 10));
  }
}

/** Interpolate the doubled signal at the positions the ratio asks for. */
static int16_t * interpolate(int16_t * out, const int16_t * buf,
    int32_t max_index_q16, int32_t increment_q16) {
  const int16_t * table = gaud_opus_silk_resampler_frac_fir_12;
  for (int32_t index = 0; index < max_index_q16; index += increment_q16) {
    int32_t phase = gaud_silk_smulwb(index & 0xFFFF, 12);
    const int16_t * at = buf + (index >> 16);
    const int16_t * near = table + 4 * phase;
    const int16_t * far = table + 4 * (11 - phase);
    int32_t result = gaud_silk_smulbb(at[0], near[0]);
    result = gaud_silk_smlabb(result, at[1], near[1]);
    result = gaud_silk_smlabb(result, at[2], near[2]);
    result = gaud_silk_smlabb(result, at[3], near[3]);
    result = gaud_silk_smlabb(result, at[4], far[3]);
    result = gaud_silk_smlabb(result, at[5], far[2]);
    result = gaud_silk_smlabb(result, at[6], far[1]);
    result = gaud_silk_smlabb(result, at[7], far[0]);
    *out++ = (int16_t)gaud_silk_sat16(gaud_silk_rshift_round(result, 15));
  }
  return out;
}

/** One run of input, in batches no longer than ten milliseconds. */
static void iir_fir(SILK_Resampler * resampler, int16_t * out,
    const int16_t * in, int length) {
  int16_t buf[2 * SILK_RESAMPLER_BATCH_MAX + SILK_RESAMPLER_FIR_ORDER];
  int batch = resampler->fs_in_khz * SILK_RESAMPLER_BATCH_MS;
  int count;

  memcpy(buf, resampler->fir, sizeof resampler->fir);
  for (;;) {
    count = length < batch ? length : batch;
    up2_hq(resampler->iir, buf + SILK_RESAMPLER_FIR_ORDER, in, count);
    // One more bit of shift because the signal was doubled.
    int32_t max_index_q16 = (int32_t)((uint32_t)count << 17);
    out = interpolate(out, buf, max_index_q16, resampler->inv_ratio_q16);
    in += count;
    length -= count;
    if (length <= 0) {
      break;
    }
    memmove(buf, buf + 2 * count, sizeof resampler->fir);
  }
  memcpy(resampler->fir, buf + 2 * count, sizeof resampler->fir);
}

void gaud_silk_resample(SILK_Resampler * resampler, int16_t * out,
    const int16_t * in, int in_len) {
  int khz = resampler->fs_in_khz;
  int delay = resampler->input_delay;
  int fresh = khz - delay;

  // The first millisecond is the delayed one: the tail of the last
  // frame's input, followed by the head of this one's.
  memcpy(resampler->delay_buf + delay, in, (size_t)fresh * sizeof *in);
  iir_fir(resampler, out, resampler->delay_buf, khz);
  iir_fir(resampler, out + SILK_RESAMPLER_OUT_KHZ, in + fresh, in_len - khz);
  memcpy(resampler->delay_buf, in + in_len - delay, (size_t)delay * sizeof *in);
}
