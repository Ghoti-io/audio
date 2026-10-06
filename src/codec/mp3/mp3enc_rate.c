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
 * How big each frame is: the constant, average and variable bit rate
 * policies, and the padding that makes a frame length an integer.
 *
 * **A frame has a whole number of bytes and the arithmetic does not.** At
 * 44.1 kHz a 128 kbit/s frame is 417.96 bytes, and the format's answer is a
 * one-byte padding bit set on the frames that need it so that the stream
 * averages the exact rate. The rule is an accumulator of the fractional
 * remainder, below the sampling frequency, to which each frame adds its
 * bit rate times 144 (72 for the lower frequencies) before dividing: the
 * quotient is the frame's length, and its excess over the length without
 * padding is the padding bit. Because the accumulator carries over, the
 * rule holds when the bit rate changes from frame to frame, which is the
 * only reason a variable-rate stream can be written by the same code.
 *
 * **The policies differ in one decision**, made after a frame's granules have
 * been coded as coarsely as masking allows and so say how many bits they
 * need: constant rate takes the rate it was given whatever they need;
 * variable takes the smallest rate that holds them; average takes the
 * smallest that holds them scaled by a factor that steers the running
 * total toward the target.
 */

#include "mp3enc_internal.h"

void gaud_mp3e_rate_init(MP3E_Rate * rate, GAUD_Rate_Control mode,
    MP3_Version version, uint32_t sample_rate, unsigned min_index,
    unsigned max_index, unsigned fixed_index, uint32_t target_bps) {
  memset(rate, 0, sizeof(*rate));
  rate->mode = mode;
  rate->version = version;
  rate->sample_rate = sample_rate;
  rate->min_index = min_index;
  rate->max_index = max_index;
  rate->fixed_index = fixed_index;
  rate->target_bps = target_bps;
}

/** The numerator of a frame's length: bytes are this over the frequency. */
static uint64_t per_frame(const MP3E_Rate * rate, unsigned index) {
  uint64_t slots = rate->version == MP3_MPEG1 ? 144u : 72u;
  return slots * 1000u * gaud_mp3e_bitrate_kbps(rate->version, index);
}

uint32_t gaud_mp3e_rate_size(const MP3E_Rate * rate, unsigned index, bool * padding) {
  uint64_t k = per_frame(rate, index);
  uint64_t whole = (rate->pad_accumulator + k) / rate->sample_rate;
  if (padding) {
    *padding = whole > k / rate->sample_rate;
  }
  return (uint32_t)whole;
}

void gaud_mp3e_rate_commit(MP3E_Rate * rate, unsigned index) {
  uint64_t k = per_frame(rate, index);
  uint32_t size = gaud_mp3e_rate_size(rate, index, NULL);
  rate->pad_accumulator = (rate->pad_accumulator + k) % rate->sample_rate;
  rate->actual_bytes += size;
  if (rate->mode == GAUD_RATE_ABR) {
    uint64_t slots = rate->version == MP3_MPEG1 ? 144u : 72u;
    rate->target_accumulator += slots * rate->target_bps;
  }
}

unsigned gaud_mp3e_rate_choose(const MP3E_Rate * rate, uint64_t need_bits,
    uint64_t reservoir_bytes, uint32_t head_bytes) {
  if (rate->mode == GAUD_RATE_CBR || rate->mode == GAUD_RATE_DEFAULT) {
    return rate->fixed_index;
  }
  uint64_t scaled = need_bits;
  if (rate->mode == GAUD_RATE_ABR) {
    /* Steer toward the target: the bytes it says there should be by now
     * against the bytes there are, in frames, as a factor on what this
     * frame asks for. A stream that is behind asks for more, one that is
     * ahead for less, over about eight frames. */
    uint64_t slots = rate->version == MP3_MPEG1 ? 144u : 72u;
    int64_t target_total = (int64_t)((rate->target_accumulator + slots * rate->target_bps) / rate->sample_rate);
    int64_t error = target_total - (int64_t)rate->actual_bytes;
    int64_t frame = (int64_t)(slots * rate->target_bps / rate->sample_rate);
    if (frame < 1) {
      frame = 1;
    }
    int64_t factor = 65536 + error * 65536 / (8 * frame);
    if (factor < 26214) {
      factor = 26214; /* 0.4 */
    }
    if (factor > 131072) {
      factor = 131072; /* 2 */
    }
    scaled = need_bits * (uint64_t)factor / 65536u;
  }
  for (unsigned index = rate->min_index; index <= rate->max_index; ++index) {
    uint32_t size = gaud_mp3e_rate_size(rate, index, NULL);
    if (size < head_bytes) {
      continue;
    }
    uint64_t area_bits = 8u * ((uint64_t)(size - head_bytes) + reservoir_bytes * 9u / 10u);
    if (area_bits >= scaled) {
      return index;
    }
  }
  return rate->max_index;
}
