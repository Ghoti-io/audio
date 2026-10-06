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
 * Decoding one Opus frame, whichever coder it is. RFC 6716 section 4
 * and, where the prose stops, `opus_decoder.c` of Appendix A. Never
 * installed.
 *
 * The order of operations in ::decode_frame is the order the reference
 * has and it is not arbitrary: a hybrid frame's SILK half is decoded
 * first because its range decoder is the one CELT then continues; a
 * redundancy frame's presence is read *between* the two because it is
 * the last thing in the SILK symbols and the first thing CELT cannot
 * have; and the redundancy frame itself is decoded by a CELT decoder
 * whose state is then either thrown away (SILK to CELT) or kept (CELT
 * to SILK) depending on which way the stream is going.
 */

#include "opus_decoder.h"
#include "opus_celt_math.h"
#include "opus_tables.h"
#include <string.h>

/** Mode numbers with zero meaning "none yet", as the reference has them. */
enum { MODE_NONE = 0, MODE_SILK = 1, MODE_HYBRID = 2, MODE_CELT = 3 };

/** 2.5 ms at 48 kHz. */
#define F2_5 120
/** 5 ms. */
#define F5 240
/** 10 ms. */
#define F10 480
/** 20 ms. */
#define F20 960

/** The band each bandwidth stops at, from the table of contents. */
static const uint32_t kEndBand[5] = {13u, 17u, 17u, 19u, 21u};

/** The SILK rate each narrower bandwidth runs at; hybrid is always 16. */
static const int kSilkKhz[3] = {8, 12, 16};

static unsigned lm_for(uint32_t samples) {
  unsigned lm = 0;
  while (lm < 3u && (120u << lm) < samples) {
    ++lm;
  }
  return lm;
}

GAUD_Result gaud_opus_decoder_create(const GAUD_Allocator * allocator,
    uint32_t channels, OPUS_Decoder ** out) {
  if (channels < 1u || channels > 2u) {
    return GAUD_ERR_INVALID;
  }
  OPUS_Decoder * decoder = gcu_allocator_malloc(allocator, sizeof *decoder);
  if (!decoder) {
    return GAUD_ERR_OOM;
  }
  memset(decoder, 0, sizeof *decoder);
  decoder->channels = channels;
  gaud_opus_decoder_reset(decoder);
  *out = decoder;
  return GAUD_OK;
}

void gaud_opus_decoder_destroy(
    const GAUD_Allocator * allocator, OPUS_Decoder * decoder) {
  if (decoder) {
    gcu_allocator_free(allocator, decoder);
  }
}

void gaud_opus_decoder_reset(OPUS_Decoder * decoder) {
  uint32_t channels = decoder->channels;
  memset(&decoder->silk, 0, sizeof decoder->silk);
  gaud_celt_decoder_init(&decoder->celt, channels, 1);
  decoder->stream_channels = channels;
  decoder->prev_mode = MODE_NONE;
  decoder->mode = MODE_NONE;
  decoder->bandwidth = 0;
  decoder->frame_size = F2_5;
  decoder->prev_redundancy = false;
  decoder->range_final = 0;
  decoder->silk_khz = 16;
  decoder->silk_channels = (int)channels;
}

/** Cross-fade two 2.5 ms stretches over the MDCT window's square. */
static void smooth_fade(const int16_t * in1, const int16_t * in2,
    int16_t * out, uint32_t channels) {
  for (uint32_t c = 0; c < channels; ++c) {
    for (uint32_t i = 0; i < F2_5; ++i) {
      int32_t w = gaud_celt_mult16_16_q15(
          gaud_opus_window120[i], gaud_opus_window120[i]);
      int32_t mixed = gaud_celt_mult16_16(w, in2[i * channels + c])
          + (CELT_Q15ONE - w) * in1[i * channels + c];
      out[i * channels + c] = (int16_t)(mixed >> 15);
    }
  }
}

static int16_t sat16(int32_t value) {
  return (int16_t)(value > 32767 ? 32767 : (value < -32768 ? -32768 : value));
}

static int decode_frame(OPUS_Decoder * st, const unsigned char * data,
    uint32_t len, int16_t * pcm, uint32_t frame_size, GAUD_Result * status);

/** Conceal @p frame_size samples per channel. */
static void conceal(OPUS_Decoder * st, int16_t * pcm, uint32_t frame_size) {
  GAUD_Result ignored = GAUD_OK;
  (void)decode_frame(st, NULL, 0, pcm, frame_size, &ignored);
}

static int decode_frame(OPUS_Decoder * st, const unsigned char * data,
    uint32_t len, int16_t * pcm, uint32_t frame_size, GAUD_Result * status) {
  uint32_t channels = st->channels;
  int16_t pcm_silk[2u * 2880u];
  int16_t pcm_transition[2u * F5];
  int16_t redundant_audio[2u * F5];
  OPUS_Range dec;
  uint32_t audiosize;
  int mode;
  bool transition = false;
  bool redundancy = false;
  bool celt_to_silk = false;
  uint32_t redundancy_bytes = 0;
  uint32_t redundant_rng = 0;
  uint32_t start_band = 0;
  bool have_data = len > 1u && data != NULL;

  *status = GAUD_OK;
  if (frame_size < F2_5) {
    *status = GAUD_ERR_INVALID;
    return 0;
  }
  // A payload of one byte or none is the DTX/loss case; do not
  // conceal more than the last table of contents said.
  if (!have_data) {
    data = NULL;
    len = 0;
    if (frame_size > st->frame_size) {
      frame_size = st->frame_size;
    }
  }
  if (have_data) {
    audiosize = st->frame_size;
    mode = st->mode;
    gaud_opus_range_init(&dec, data, len);
  } else {
    audiosize = frame_size;
    if (st->prev_mode == MODE_NONE) {
      memset(pcm, 0, (size_t)audiosize * channels * sizeof *pcm);
      return (int)audiosize;
    }
    mode = st->prev_mode;
  }

  // Concealing more than 20 ms of CELT or hybrid is done in 20 ms steps.
  if (!have_data && frame_size > F20 && mode != MODE_SILK) {
    uint32_t done = 0;
    do {
      int got = decode_frame(st, NULL, 0, pcm, F20, status);
      if (got != F20) {
        *status = GAUD_ERR_CORRUPT;
        return 0;
      }
      pcm += (size_t)F20 * channels;
      done += F20;
    } while (done < frame_size);
    return (int)frame_size;
  }

  if (have_data && st->prev_mode > MODE_NONE
      && ((mode == MODE_CELT && st->prev_mode != MODE_CELT
              && !st->prev_redundancy)
          || (mode != MODE_CELT && st->prev_mode == MODE_CELT))) {
    transition = true;
    if (mode == MODE_CELT) {
      conceal(st, pcm_transition, audiosize < F5 ? audiosize : F5);
    }
  }
  if (audiosize > frame_size) {
    *status = GAUD_ERR_INVALID;
    return 0;
  }
  frame_size = audiosize;

  if (mode != MODE_CELT) {
    uint32_t decoded = 0;
    int16_t * at = pcm_silk;
    if (st->prev_mode == MODE_CELT) {
      memset(&st->silk.channel, 0, sizeof st->silk.channel);
      memset(st->silk.resampler, 0, sizeof st->silk.resampler);
    }
    int payload_ms = (int)(audiosize * 1000u / 48000u);
    if (payload_ms < 10) {
      payload_ms = 10;
    }
    if (have_data) {
      st->silk_channels = (int)st->stream_channels;
      st->silk_khz = mode == MODE_SILK
          ? kSilkKhz[st->bandwidth < 3u ? st->bandwidth : 2u]
          : 16;
    }
    bool configured = gaud_silk_configure(&st->silk,
        st->silk_channels, (int)channels, st->silk_khz, payload_ms);
    if (!configured && have_data) {
      *status = GAUD_ERR_CORRUPT;
      return 0;
    }
    if (!configured) {
      // Concealment of a length SILK has no frame for is not an error,
      // it is silence.
      memset(pcm_silk, 0, (size_t)frame_size * channels * sizeof *pcm_silk);
    } else {
      do {
        int samples = 0;
        if (have_data) {
          gaud_silk_decode_frame(&st->silk, &dec, at, &samples);
        } else {
          gaud_silk_decode_lost(&st->silk, at, &samples);
        }
        at += (size_t)samples * channels;
        decoded += (uint32_t)samples;
      } while (decoded < frame_size);
    }
  }

  if (have_data && mode != MODE_CELT
      && gaud_opus_tell(&dec) + 17u + (mode == MODE_HYBRID ? 20u : 0u)
          <= 8u * len) {
    // A redundant 0 to 8 kHz CELT frame follows the SILK symbols.
    if (mode == MODE_HYBRID) {
      redundancy = gaud_opus_dec_bit_logp(&dec, 12) != 0;
    } else {
      redundancy = true;
    }
    if (redundancy) {
      celt_to_silk = gaud_opus_dec_bit_logp(&dec, 1) != 0;
      redundancy_bytes = mode == MODE_HYBRID
          ? gaud_opus_dec_uint(&dec, 256) + 2u
          : len - ((gaud_opus_tell(&dec) + 7u) >> 3);
      len -= redundancy_bytes;
      // A sanity check; for a valid packet it never fires, and what
      // happens when it does is not normative.
      if ((int64_t)len * 8 < (int64_t)gaud_opus_tell(&dec)
          || redundancy_bytes > dec.size) {
        len = 0;
        redundancy_bytes = 0;
        redundancy = false;
      }
      dec.size -= redundancy_bytes;
    }
  }
  if (mode != MODE_SILK) {
    // Also true for hybrid: CELT carries only what SILK does not.
  }
  if (mode != MODE_CELT) {
    start_band = 17;
  }

  st->celt.end = kEndBand[st->bandwidth < 5u ? st->bandwidth : 4u];
  if (redundancy) {
    transition = false;
  }
  if (transition && mode != MODE_CELT) {
    conceal(st, pcm_transition, audiosize < F5 ? audiosize : F5);
  }

  // 5 ms redundant frame for CELT to SILK.
  if (redundancy && celt_to_silk) {
    OPUS_Range extra;
    gaud_opus_range_init(&extra, data + len, redundancy_bytes);
    st->celt.start = 0;
    (void)gaud_celt_decode_frame(&st->celt, &extra, redundancy_bytes,
        st->stream_channels, 1, redundant_audio, &st->scratch);
    redundant_rng = st->celt.rng;
  }

  // Must come after any concealment above, which resets the band range.
  st->celt.start = start_band;

  if (mode != MODE_SILK) {
    uint32_t celt_size = frame_size < F20 ? frame_size : F20;
    unsigned lm = lm_for(celt_size);
    // Discard any previous CELT state when the mode changed.
    if (mode != st->prev_mode && st->prev_mode > MODE_NONE
        && !st->prev_redundancy) {
      uint32_t end = st->celt.end;
      gaud_celt_decoder_init(&st->celt, channels, 1);
      st->celt.end = end;
      st->celt.start = start_band;
    }
    // A frame the redundancy check left with one byte or none is
    // concealed like one that never came, as the reference does.
    if (!have_data || len <= 1u) {
      // Only the four frame sizes CELT has can be concealed.
      if ((120u << lm) != celt_size) {
        *status = GAUD_ERR_INVALID;
        return 0;
      }
      gaud_celt_decode_lost(
          &st->celt, pcm, celt_size, lm, &st->scratch);
    } else {
      GAUD_Result result = gaud_celt_decode_frame(&st->celt, &dec, len,
          st->stream_channels, lm, pcm, &st->scratch);
      if (result != GAUD_OK) {
        *status = result;
      }
    }
  } else {
    static const unsigned char silence[2] = {0xFF, 0xFF};
    memset(pcm, 0, (size_t)frame_size * channels * sizeof *pcm);
    // From hybrid to SILK there is no CELT frame to let the high band
    // fade, so one that says "silence" is decoded to do it.
    if (st->prev_mode == MODE_HYBRID
        && !(redundancy && celt_to_silk && st->prev_redundancy)) {
      OPUS_Range quiet;
      gaud_opus_range_init(&quiet, silence, 2);
      st->celt.start = 0;
      (void)gaud_celt_decode_frame(&st->celt, &quiet, 2, st->stream_channels,
          0, pcm, &st->scratch);
    }
  }

  if (mode != MODE_CELT) {
    for (uint32_t i = 0; i < frame_size * channels; ++i) {
      pcm[i] = sat16((int32_t)pcm[i] + pcm_silk[i]);
    }
  }

  // 5 ms redundant frame for SILK to CELT.
  if (redundancy && !celt_to_silk) {
    OPUS_Range extra;
    uint32_t end = st->celt.end;
    gaud_celt_decoder_init(&st->celt, channels, 1);
    st->celt.end = end;
    st->celt.start = 0;
    gaud_opus_range_init(&extra, data + len, redundancy_bytes);
    (void)gaud_celt_decode_frame(&st->celt, &extra, redundancy_bytes,
        st->stream_channels, 1, redundant_audio, &st->scratch);
    redundant_rng = st->celt.rng;
    smooth_fade(pcm + channels * (frame_size - F2_5),
        redundant_audio + channels * F2_5,
        pcm + channels * (frame_size - F2_5), channels);
  }
  if (redundancy && celt_to_silk) {
    for (uint32_t c = 0; c < channels; ++c) {
      for (uint32_t i = 0; i < F2_5; ++i) {
        pcm[channels * i + c] = redundant_audio[channels * i + c];
      }
    }
    smooth_fade(redundant_audio + channels * F2_5, pcm + channels * F2_5,
        pcm + channels * F2_5, channels);
  }
  if (transition) {
    if (audiosize >= F5) {
      for (uint32_t i = 0; i < channels * F2_5; ++i) {
        pcm[i] = pcm_transition[i];
      }
      smooth_fade(pcm_transition + channels * F2_5, pcm + channels * F2_5,
          pcm + channels * F2_5, channels);
    } else {
      // Not enough time for a clean transition, but one is done
      // anyway: it may not keep the amplitude exactly, and it is still
      // better than the step it replaces.
      smooth_fade(pcm_transition, pcm, pcm, channels);
    }
  }

  st->range_final = len <= 1u ? 0u : (dec.rng ^ redundant_rng);
  st->prev_mode = mode;
  st->prev_redundancy = redundancy && !celt_to_silk;
  st->redundancy = !redundancy ? 0 : (celt_to_silk ? 1 : 2);
  return (int)audiosize;
}

GAUD_Result gaud_opus_decode_stream_packet(OPUS_Decoder * st,
    const unsigned char * data, size_t size, bool self_delimited,
    int16_t * pcm, uint32_t capacity, uint32_t * out_samples,
    size_t * consumed) {
  GAUD_Result status = GAUD_OK;
  *out_samples = 0;
  if (consumed) {
    *consumed = size;
  }
  if (size == 0 || data == NULL) {
    return GAUD_ERR_CORRUPT;
  }
  // The table of contents is read into the decoder *before* the packet
  // is validated, as the reference does. A packet that is then refused
  // has still told the decoder how long its frames were, and a loss
  // concealed after it is that long.
  uint8_t toc = data[0];
  if (toc & 0x80u) {
    st->mode = MODE_CELT;
    unsigned wide = (toc >> 5) & 3u;
    st->bandwidth = wide == 0u ? 0u : wide + 1u;
    st->frame_size = (120u << ((toc >> 3) & 3u));
  } else if ((toc & 0x60u) == 0x60u) {
    st->mode = MODE_HYBRID;
    st->bandwidth = (toc & 0x10u) ? 4u : 3u;
    st->frame_size = (toc & 0x08u) ? F20 : F10;
  } else {
    st->mode = MODE_SILK;
    st->bandwidth = (toc >> 5) & 3u;
    unsigned length = (toc >> 3) & 3u;
    st->frame_size = length == 3u ? 2880u : (480u << length);
  }
  st->stream_channels = (toc & 4u) ? 2u : 1u;

  OPUS_Packet packet;
  GAUD_Result result = gaud_opus_parse_packet(data, size, self_delimited, &packet);
  if (result != GAUD_OK) {
    return result;
  }
  if (consumed) {
    *consumed = packet.consumed;
  }
  if (packet.count * packet.toc.frame_size > capacity) {
    return GAUD_ERR_INVALID;
  }
  uint32_t done = 0;
  for (uint32_t i = 0; i < packet.count; ++i) {
    int got = decode_frame(st, packet.frame[i], packet.length[i],
        pcm + (size_t)done * st->channels, capacity - done, &status);
    if (status != GAUD_OK) {
      return status;
    }
    done += (uint32_t)got;
  }
  *out_samples = done;
  return GAUD_OK;
}

GAUD_Result gaud_opus_decode_packet(OPUS_Decoder * st,
    const unsigned char * data, size_t size, int16_t * pcm, uint32_t capacity,
    uint32_t lost_samples, uint32_t * out_samples) {
  *out_samples = 0;
  GAUD_Result status = GAUD_OK;
  if (size == 0 || data == NULL) {
    if (lost_samples > capacity) {
      return GAUD_ERR_INVALID;
    }
    int got = decode_frame(st, NULL, 0, pcm, lost_samples, &status);
    *out_samples = (uint32_t)got;
    return status;
  }
  return gaud_opus_decode_stream_packet(
      st, data, size, false, pcm, capacity, out_samples, NULL);
}

void gaud_opus_apply_gain(int16_t * pcm, size_t count, int32_t gain_q8) {
  if (gain_q8 == 0) {
    return;
  }
  // 6.48814081e-4 in Q25 is 21771: the decibel-to-octave factor, so
  // that the sum below is a base-two logarithm in Q10.
  int32_t log2_q10 = gaud_celt_mult16_16_p15(21771, gain_q8);
  int32_t gain = gaud_celt_exp2((int16_t)log2_q10);
  for (size_t i = 0; i < count; ++i) {
    // MULT16_32_P16: the high half exactly, the low half rounded.
    int32_t x = pcm[i] * (gain >> 16)
        + (int32_t)(((int64_t)pcm[i] * (int64_t)(gain & 0xFFFF) + 32768)
            >> 16);
    pcm[i] = (int16_t)(x > 32767 ? 32767 : (x < -32767 ? -32767 : x));
  }
}
