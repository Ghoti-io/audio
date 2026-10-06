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
 * One CELT frame, start to finish. RFC 6716 section 4.3 in the order
 * the bits arrive. Never installed.
 *
 * Every other file in this directory decodes one stage; this is the
 * one that says what order they go in, and the order is the whole
 * point. The range decoder is a single stream of symbols whose meaning
 * depends on everything read before, so a stage taken out of turn does
 * not produce a slightly wrong frame - it produces noise, and so does
 * every frame after it.
 *
 * **Bits are reserved before they are spent, and the reservations are
 * not symmetric.** The anti-collapse bit is set aside before the
 * allocator runs and read after the bands; the allocator's own
 * reservations for intensity and dual stereo are handed back if the
 * decision turns out not to be coded. Section 4.3.3's warning that the
 * allocation "MUST be recovered exactly" is about this.
 *
 * **Several decisions are skipped when the budget cannot afford them**,
 * and the default taken instead is part of the format: no transient
 * flag means not transient, no spreading symbol means
 * ::CELT_SPREAD_NORMAL, no trim means 5. A decoder that reads them
 * anyway desynchronises on exactly the packets that are too small to
 * waste bits on.
 *
 * **The state carried between frames is most of the decoder.** The
 * energy envelope and two frames of history for anti-collapse, the
 * overlap for the transform, a thousand samples of synthesis for the
 * post-filter, the de-emphasis memory, and the range decoder's own
 * final state - which is what RFC 6716 section 6 compares to decide
 * whether a decoder is conformant at all.
 */

#include "opus_celt.h"
#include "opus_celt_math.h"
#include <string.h>

/** Samples of synthesis history, RFC 6716's `DECODE_BUFFER_SIZE`. */
#define CELT_DECODE_BUFFER 2048

/** How far back the post-filter may reach, RFC 6716's `MAX_PERIOD`. */
#define CELT_MAX_PERIOD 1024

/** The quietest the envelope is ever reset to: -28 dB in Q10. */
#define CELT_ENERGY_FLOOR (-(28 << CELT_DB_SHIFT))

void gaud_celt_decoder_init(CELT_Decoder * decoder, uint32_t channels,
    int downsample) {
  memset(decoder, 0, sizeof *decoder);
  decoder->channels = channels;
  decoder->downsample = downsample;
  decoder->end = CELT_BANDS;
  // Everything else starts at zero, but the two history envelopes
  // start at the floor: a first frame has no history, and treating
  // that as "silent before" is what keeps anti-collapse from firing
  // on it.
  for (uint32_t i = 0; i < 2u * CELT_BANDS; ++i) {
    decoder->old_log_e[i] = CELT_ENERGY_FLOOR;
    decoder->old_log_e2[i] = CELT_ENERGY_FLOOR;
  }
}

GAUD_Result gaud_celt_decode_frame(CELT_Decoder * decoder, OPUS_Range * range,
    uint32_t bytes, uint32_t stream_channels, unsigned lm, int16_t * pcm,
    CELT_Scratch * scratch) {
  CELT_Mode mode;
  uint32_t cc = decoder->channels;
  uint32_t c = stream_channels;
  uint32_t start = decoder->start;
  uint32_t end = decoder->end;
  uint32_t effective_end = end;
  int32_t n;
  int32_t total_bits;
  int32_t tell;
  int32_t bits;
  int32_t balance = 0;
  int32_t anti_collapse_reserve;
  bool anti_collapse_on = false;
  bool silence = false;
  bool transient = false;
  bool intra = false;
  bool dual_stereo = false;
  uint32_t intensity = 0;
  uint32_t coded_bands;
  unsigned spread = CELT_SPREAD_NORMAL;
  int alloc_trim = 5;
  int postfilter_pitch = 0;
  int16_t postfilter_gain = 0;
  unsigned postfilter_tapset = 0;
  int32_t * synthesis[2];
  int32_t * overlap[2];
  const int32_t * deemph_in[2];

  if (!gaud_celt_mode_init(&mode, lm)) {
    return GAUD_ERR_CORRUPT;
  }
  n = (int32_t)mode.size;

  // Bands outside [start, effective_end) are never written by the
  // shape coder, so they have to be cleared rather than left holding
  // the previous frame's spectrum.
  memset(scratch->shape, 0, (size_t)(c * (uint32_t)n) * sizeof *scratch->shape);

  total_bits = (int32_t)(bytes * 8u);
  tell = (int32_t)gaud_opus_tell(range);
  if (tell >= total_bits) {
    silence = true;
  } else if (tell == 1) {
    silence = gaud_opus_dec_bit_logp(range, 15) != 0;
  }
  if (silence) {
    // Charge the decoder for every remaining bit, so that nothing
    // below thinks it can afford anything.
    tell = total_bits;
    range->total_bits += (uint32_t)(tell - (int32_t)gaud_opus_tell(range));
  }

  if (start == 0u && tell + 16 <= total_bits) {
    if (gaud_opus_dec_bit_logp(range, 1)) {
      // The octave is 0 to 5, then 4+octave raw bits of fine pitch;
      // together they reach 15 to 1022, which is the range section
      // 4.3.7.1 states.
      uint32_t octave = gaud_opus_dec_uint(range, 6u);
      int fine = (int)gaud_opus_dec_bits(range, 4u + octave);
      int quantised_gain;
      postfilter_pitch = (int)(16u << octave) + fine - 1;
      quantised_gain = (int)gaud_opus_dec_bits(range, 3u);
      if ((int32_t)gaud_opus_tell(range) + 2 <= total_bits) {
        postfilter_tapset = (unsigned)gaud_opus_dec_icdf(
            range, gaud_opus_tapset_icdf, 2u);
      }
      postfilter_gain = (int16_t)(3072 * (quantised_gain + 1));
    }
    tell = (int32_t)gaud_opus_tell(range);
  }

  if (lm > 0u && tell + 3 <= total_bits) {
    transient = gaud_opus_dec_bit_logp(range, 3) != 0;
    tell = (int32_t)gaud_opus_tell(range);
  }
  intra = tell + 3 <= total_bits && gaud_opus_dec_bit_logp(range, 3) != 0;

  if (c == 1u) {
    // A stream that was stereo a frame ago still has two envelopes;
    // the louder of the two is the history a mono frame predicts from.
    for (uint32_t i = 0; i < CELT_BANDS; ++i) {
      decoder->old_band_e[i] = (int16_t)gaud_celt_max32(
          decoder->old_band_e[i], decoder->old_band_e[CELT_BANDS + i]);
    }
  }

  gaud_celt_decode_coarse_energy(
      range, &mode, decoder->old_band_e, start, end, intra, c);
  gaud_celt_tf_decode(range, &mode, start, end, transient, scratch->tf_res);

  tell = (int32_t)gaud_opus_tell(range);
  if (tell + 4 <= total_bits) {
    spread = (unsigned)gaud_opus_dec_icdf(range, gaud_opus_spread_icdf, 5u);
  }

  gaud_celt_init_caps(&mode, scratch->cap, c);
  total_bits = gaud_celt_decode_boosts(range, &mode, start, end, c,
      scratch->cap, (int32_t)(bytes * 8u) << CELT_BITRES, scratch->offsets);

  if ((int32_t)gaud_opus_tell_frac(range) + (6 << CELT_BITRES) <= total_bits) {
    alloc_trim = gaud_opus_dec_icdf(range, gaud_opus_trim_icdf, 7u);
  }

  bits = ((int32_t)(bytes * 8u) << CELT_BITRES)
      - (int32_t)gaud_opus_tell_frac(range) - 1;
  // One bit set aside now and read after the bands, because whether
  // the frame needs anti-collapse is not known until they are decoded.
  anti_collapse_reserve = transient && lm >= 2u
          && bits >= (int32_t)((lm + 2u) << CELT_BITRES)
      ? (1 << CELT_BITRES)
      : 0;
  bits -= anti_collapse_reserve;
  coded_bands = gaud_celt_compute_allocation(range, &mode, start, end,
      scratch->offsets, scratch->cap, alloc_trim, &intensity, &dual_stereo,
      bits, &balance, scratch->pulses, scratch->fine, scratch->priority, c);

  gaud_celt_decode_fine_energy(
      range, &mode, decoder->old_band_e, scratch->fine, start, end, c);

  gaud_celt_quant_all_bands(range, &mode, start, end, scratch->shape,
      c == 2u ? scratch->shape + n : NULL, scratch->collapse, scratch->pulses,
      transient, spread, dual_stereo, intensity, scratch->tf_res,
      ((int32_t)(bytes * 8u) << CELT_BITRES) - anti_collapse_reserve, balance,
      coded_bands, &decoder->rng, scratch->bands);

  if (anti_collapse_reserve > 0) {
    anti_collapse_on = gaud_opus_dec_bits(range, 1u) != 0u;
  }
  gaud_celt_decode_final_energy(range, &mode, decoder->old_band_e,
      scratch->fine, scratch->priority,
      (int32_t)(bytes * 8u) - (int32_t)gaud_opus_tell(range), start, end, c);

  if (anti_collapse_on) {
    gaud_celt_anti_collapse(&mode, scratch->shape, scratch->collapse, c,
        (uint32_t)n, start, end, decoder->old_band_e, decoder->old_log_e,
        decoder->old_log_e2, scratch->pulses, decoder->rng);
  }

  gaud_celt_log2_amp(scratch->amplitude, decoder->old_band_e, start, end, c);
  if (silence) {
    for (uint32_t i = 0; i < c * CELT_BANDS; ++i) {
      scratch->amplitude[i] = 0;
      decoder->old_band_e[i] = CELT_ENERGY_FLOOR;
    }
  }

  gaud_celt_denormalise_bands(&mode, scratch->shape, scratch->spectrum,
      scratch->amplitude, effective_end, c);

  // The synthesis buffer slides left by one frame before the new one
  // is written into the gap at its end.
  for (uint32_t channel = 0; channel < cc; ++channel) {
    memmove(decoder->decode_mem[channel],
        decoder->decode_mem[channel] + n,
        (size_t)(CELT_DECODE_BUFFER - n) * sizeof decoder->decode_mem[0][0]);
  }

  for (uint32_t channel = 0; channel < c; ++channel) {
    int32_t * spectrum = scratch->spectrum + channel * (uint32_t)n;
    int32_t bound = (int32_t)((uint32_t)mode.edges[effective_end] << lm);
    if (decoder->downsample != 1) {
      int32_t limit = n / decoder->downsample;
      bound = bound < limit ? bound : limit;
    }
    for (int32_t i = 0; i < (int32_t)((uint32_t)mode.edges[start] << lm); ++i) {
      spectrum[i] = 0;
    }
    for (int32_t i = bound; i < n; ++i) {
      spectrum[i] = 0;
    }
  }
  if (cc == 2u && c == 1u) {
    // A mono frame in a stereo stream feeds both sides.
    memcpy(scratch->spectrum + n, scratch->spectrum,
        (size_t)n * sizeof *scratch->spectrum);
  }
  if (cc == 1u && c == 2u) {
    for (int32_t i = 0; i < n; ++i) {
      scratch->spectrum[i] = gaud_celt_add32(
                                 scratch->spectrum[i], scratch->spectrum[n + i])
          >> 1;
    }
  }

  for (uint32_t channel = 0; channel < cc; ++channel) {
    synthesis[channel] = decoder->decode_mem[channel] + CELT_DECODE_BUFFER - n;
    overlap[channel] = decoder->decode_mem[channel] + CELT_DECODE_BUFFER;
  }

  // The inverse transform, one short MDCT per time block for a
  // transient frame and one long one otherwise.
  for (uint32_t channel = 0; channel < cc; ++channel) {
    int32_t blocks = transient ? (int32_t)(1u << lm) : 1;
    int32_t block_size = transient ? (int32_t)CELT_SHORT_MDCT : n;
    int shift = transient ? (int)CELT_MAX_LM : (int)(CELT_MAX_LM - (int)lm);
    int32_t * work = scratch->synthesis;
    memset(work, 0, CELT_OVERLAP * sizeof *work);
    for (int32_t b = 0; b < blocks; ++b) {
      gaud_celt_imdct(
          scratch->spectrum + channel * (uint32_t)n + (uint32_t)b,
          work + b * block_size, gaud_opus_window120, CELT_OVERLAP, shift,
          (uint32_t)blocks);
    }
    for (int32_t j = 0; j < (int32_t)CELT_OVERLAP; ++j) {
      synthesis[channel][j] = gaud_celt_add32(work[j], overlap[channel][j]);
    }
    for (int32_t j = (int32_t)CELT_OVERLAP; j < n; ++j) {
      synthesis[channel][j] = work[j];
    }
    for (int32_t j = 0; j < (int32_t)CELT_OVERLAP; ++j) {
      overlap[channel][j] = work[n + j];
    }
  }

  for (uint32_t channel = 0; channel < cc; ++channel) {
    if (decoder->postfilter_period < CELT_COMB_MIN_PERIOD) {
      decoder->postfilter_period = CELT_COMB_MIN_PERIOD;
    }
    if (decoder->postfilter_period_old < CELT_COMB_MIN_PERIOD) {
      decoder->postfilter_period_old = CELT_COMB_MIN_PERIOD;
    }
    gaud_celt_comb_filter(synthesis[channel], synthesis[channel],
        decoder->postfilter_period_old, decoder->postfilter_period,
        (int)CELT_SHORT_MDCT, decoder->postfilter_gain_old,
        decoder->postfilter_gain, decoder->postfilter_tapset_old,
        decoder->postfilter_tapset, gaud_opus_window120, CELT_OVERLAP);
    if (lm != 0u) {
      // The rest of the frame fades from this frame's parameters to
      // the next one's, which were decoded at the top.
      gaud_celt_comb_filter(synthesis[channel] + CELT_SHORT_MDCT,
          synthesis[channel] + CELT_SHORT_MDCT, decoder->postfilter_period,
          postfilter_pitch, n - (int32_t)CELT_SHORT_MDCT,
          decoder->postfilter_gain, postfilter_gain,
          decoder->postfilter_tapset, postfilter_tapset, gaud_opus_window120,
          CELT_OVERLAP);
    }
  }
  decoder->postfilter_period_old = decoder->postfilter_period;
  decoder->postfilter_gain_old = decoder->postfilter_gain;
  decoder->postfilter_tapset_old = decoder->postfilter_tapset;
  decoder->postfilter_period = postfilter_pitch;
  decoder->postfilter_gain = postfilter_gain;
  decoder->postfilter_tapset = postfilter_tapset;
  if (lm != 0u) {
    decoder->postfilter_period_old = decoder->postfilter_period;
    decoder->postfilter_gain_old = decoder->postfilter_gain;
    decoder->postfilter_tapset_old = decoder->postfilter_tapset;
  }

  if (c == 1u) {
    memcpy(decoder->old_band_e + CELT_BANDS, decoder->old_band_e,
        CELT_BANDS * sizeof decoder->old_band_e[0]);
  }
  if (!transient) {
    memcpy(decoder->old_log_e2, decoder->old_log_e,
        sizeof decoder->old_log_e2);
    memcpy(decoder->old_log_e, decoder->old_band_e, sizeof decoder->old_log_e);
    for (uint32_t i = 0; i < 2u * CELT_BANDS; ++i) {
      // The background estimate creeps up by a thousandth of a decibel
      // per 2.5 ms and is pulled straight down by anything quieter.
      int32_t crept = decoder->background_log_e[i] + (int32_t)(1u << lm);
      decoder->background_log_e[i] = (int16_t)gaud_celt_min32(
          crept, decoder->old_band_e[i]);
    }
  } else {
    for (uint32_t i = 0; i < 2u * CELT_BANDS; ++i) {
      decoder->old_log_e[i] = (int16_t)gaud_celt_min32(
          decoder->old_log_e[i], decoder->old_band_e[i]);
    }
  }
  // Both channels' bands outside the coded range are reset, in case
  // the range moves between frames.
  for (uint32_t channel = 0; channel < 2u; ++channel) {
    for (uint32_t i = 0; i < CELT_BANDS; ++i) {
      if (i >= start && i < end) {
        continue;
      }
      decoder->old_band_e[channel * CELT_BANDS + i] = 0;
      decoder->old_log_e[channel * CELT_BANDS + i] = CELT_ENERGY_FLOOR;
      decoder->old_log_e2[channel * CELT_BANDS + i] = CELT_ENERGY_FLOOR;
    }
  }
  decoder->rng = range->rng;

  for (uint32_t channel = 0; channel < cc; ++channel) {
    deemph_in[channel] = synthesis[channel];
  }
  gaud_celt_deemphasis(
      deemph_in, pcm, n, cc, decoder->downsample, decoder->preemph_memory);
  decoder->loss_count = 0;

  if ((int32_t)gaud_opus_tell(range) > (int32_t)(bytes * 8u)) {
    return GAUD_ERR_CORRUPT;
  }
  return GAUD_OK;
}
