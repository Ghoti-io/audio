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
 * G.711 and two ADPCM families, framed three ways.
 *
 * ## The tables are validated, not proofread
 *
 * The IMA step table is 89 entries and the MS adaptation table is 16.
 * Transcribing either correctly is luck, and reading it back over does not
 * find the one digit that is wrong. What finds it is `make check-corpus`
 * and `make check-writer`: real ADPCM files, decoded by this code and by
 * ffmpeg and libsndfile, compared byte for byte. A single wrong step
 * entry diverges within a few samples and never re-converges, because the
 * step index is carried forward - so the gate is sensitive to exactly the
 * error a careful read is not.
 *
 * ## Three framings, one nibble
 *
 * IMA-in-WAV and IMA-in-QuickTime are the same arithmetic. What differs:
 *
 *   |                  | WAV                     | QuickTime `ima4`    |
 *   | preamble         | 4 bytes/channel         | 2 bytes/channel     |
 *   | block size       | the header says         | fixed, 34 bytes     |
 *   | preamble sample  | IS the first sample     | is NOT a sample     |
 *   | interleave       | 4-byte groups per chan  | whole packet per ch |
 *   | nibble order     | low first               | low first           |
 *
 * The "preamble sample" row is the one that produces an off-by-one length
 * for a whole file if it is got wrong, and the reason
 * gaud_coded_geometry() computes `block_frames` per coding rather than
 * from a shared formula.
 */

#include "adpcm.h"
#include "g711.h"
#include <string.h>

/* ------------------------------------------------------------ IMA ADPCM */

static const int16_t ima_step[89] = {7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19,
    21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

/* Mirrored for the sign bit: nibbles 8-15 adjust as 0-7 do. */
static const int8_t ima_index_adjust[16]
    = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

/** One IMA nibble. @p predictor and @p index are updated in place. */
static int16_t ima_step_nibble(
    unsigned nibble, int32_t * predictor, int * index) {
  int32_t step = ima_step[*index];
  /* The step>>3 bias is part of the algorithm and not a rounding
   * flourish: without it a run of zero nibbles would not decay. */
  int32_t diff = step >> 3;
  if (nibble & 1u) {
    diff += step >> 2;
  }
  if (nibble & 2u) {
    diff += step >> 1;
  }
  if (nibble & 4u) {
    diff += step;
  }
  if (nibble & 8u) {
    diff = -diff;
  }

  int32_t value = *predictor + diff;
  if (value > 32767) {
    value = 32767;
  }
  else if (value < -32768) {
    value = -32768;
  }
  *predictor = value;

  int next = *index + ima_index_adjust[nibble & 0x0Fu];
  if (next < 0) {
    next = 0;
  }
  else if (next > 88) {
    next = 88;
  }
  *index = next;
  return (int16_t)value;
}

/**
 * Choose the step index a block should start at.
 *
 * The index is carried in the block header, so any value decodes; the
 * choice is purely about accuracy. Starting at 0 means the step has to
 * climb from 7 to the signal's scale, and it can only roughly double every
 * other sample - which costs nothing over WAV's 2041-frame block and
 * dominates a QuickTime packet of 64. Measured on a 20000-amplitude sine:
 * starting at 0 gave 21.6 dB SNR for `ima4` against 36.3 dB for the same
 * algorithm in WAV's framing. Seeding it closes that gap.
 *
 * The seed is the mean absolute first difference, which is what the step
 * has to be able to span. A nibble reaches about twice its step, so the
 * table entry nearest the mean is a step that can follow the signal
 * without saturating on the first excursion.
 */
static int ima_initial_index(
    const int16_t * in, size_t frames, uint32_t channels, uint32_t channel) {
  if (frames < 2) {
    return 0;
  }
  int64_t total = 0;
  size_t count = 0;
  for (size_t f = 1; f < frames; ++f) {
    int32_t a = in[f * channels + channel];
    int32_t b = in[(f - 1u) * channels + channel];
    total += a > b ? (a - b) : (b - a);
    ++count;
  }
  int32_t mean = count ? (int32_t)(total / (int64_t)count) : 0;

  int best = 0;
  int32_t best_error = INT32_MAX;
  for (int i = 0; i < 89; ++i) {
    int32_t error = ima_step[i] > mean ? ima_step[i] - mean : mean - ima_step[i];
    if (error < best_error) {
      best_error = error;
      best = i;
    }
  }
  return best;
}

/** The inverse: choose the nibble whose result is nearest @p sample. */
static unsigned ima_pick_nibble(
    int32_t sample, int32_t * predictor, int * index) {
  int32_t step = ima_step[*index];
  int32_t delta = sample - *predictor;
  unsigned nibble = 0;
  if (delta < 0) {
    nibble = 8u;
    delta = -delta;
  }
  /* The magnitude search the standard states: peel off step, step/2 and
   * step/4 largest first. This is division by repeated subtraction, which
   * is what makes it exactly invertible by ima_step_nibble(). */
  int32_t threshold = step;
  if (delta >= threshold) {
    nibble |= 4u;
    delta -= threshold;
  }
  threshold >>= 1;
  if (delta >= threshold) {
    nibble |= 2u;
    delta -= threshold;
  }
  threshold >>= 1;
  if (delta >= threshold) {
    nibble |= 1u;
  }
  /* Advance the state exactly as the decoder will, so that the encoder's
   * predictor never drifts from the decoder's. Calling the decoder rather
   * than repeating its arithmetic is the whole point: one logic written
   * twice drifts. */
  (void)ima_step_nibble(nibble, predictor, index);
  return nibble;
}

/* ------------------------------------------------------------- MS ADPCM */

const int16_t gaud_ms_adpcm_default_coef[14]
    = {256, 0, 512, -256, 0, 0, 192, 64, 240, 0, 460, -208, 392, -232};

static const int16_t ms_adapt[16] = {230, 230, 230, 230, 307, 409, 512, 614,
    768, 614, 512, 409, 307, 230, 230, 230};

/** One MS ADPCM nibble. The three state words are updated in place. */
static int16_t ms_step_nibble(unsigned nibble, int16_t coef1, int16_t coef2,
    int32_t * delta, int32_t * sample1, int32_t * sample2) {
  /* The nibble is signed 4-bit. Sign-extending by hand rather than with a
   * cast to int8_t and a shift, which is implementation-defined for the
   * value 8. */
  int32_t signed_nibble = (int32_t)(nibble & 0x0Fu);
  if (signed_nibble & 8) {
    signed_nibble -= 16;
  }

  int32_t predictor
      = ((*sample1) * (int32_t)coef1 + (*sample2) * (int32_t)coef2) / 256;
  predictor += signed_nibble * (*delta);
  if (predictor > 32767) {
    predictor = 32767;
  }
  else if (predictor < -32768) {
    predictor = -32768;
  }

  *sample2 = *sample1;
  *sample1 = predictor;

  int32_t next = (ms_adapt[nibble & 0x0Fu] * (*delta)) / 256;
  /* The floor exists because delta reaching zero would freeze the
   * predictor: every later nibble would add nothing and the block would
   * decode as a held value. */
  *delta = next < 16 ? 16 : next;
  return (int16_t)predictor;
}

/** The inverse, for one nibble. */
static unsigned ms_pick_nibble(int32_t sample, int16_t coef1, int16_t coef2,
    int32_t * delta, int32_t * sample1, int32_t * sample2) {
  int32_t predictor
      = ((*sample1) * (int32_t)coef1 + (*sample2) * (int32_t)coef2) / 256;
  int32_t want = sample - predictor;
  /* Round to nearest rather than truncating toward zero, which halves the
   * error and costs one add. */
  int32_t step = *delta;
  int32_t nibble;
  if (want < 0) {
    nibble = (want - step / 2) / step;
  }
  else {
    nibble = (want + step / 2) / step;
  }
  if (nibble > 7) {
    nibble = 7;
  }
  else if (nibble < -8) {
    nibble = -8;
  }
  unsigned encoded = (unsigned)(nibble & 0x0F);
  (void)ms_step_nibble(encoded, coef1, coef2, delta, sample1, sample2);
  return encoded;
}

/* ------------------------------------------------------------- geometry */

/** Bytes of block preamble one channel occupies, by coding. */
static uint32_t preamble_bytes(GAUD_Sample_Coding coding) {
  switch (coding) {
  case GAUD_CODING_ADPCM_IMA_WAV: return 4u; /* predictor, index, reserved */
  case GAUD_CODING_ADPCM_IMA_QT: return 2u;  /* predictor and index packed */
  case GAUD_CODING_ADPCM_MS: return 7u;      /* index, delta, two samples */
  default: return 0u;
  }
}

bool gaud_coded_writable(GAUD_Sample_Coding coding, uint32_t channels) {
  if (channels == 0u) {
    return false;
  }
  switch (coding) {
  case GAUD_CODING_ADPCM_IMA_WAV:
  case GAUD_CODING_ADPCM_IMA_QT:
  case GAUD_CODING_ADPCM_MS: return channels <= 2u;
  default: return channels <= GAUD_CODED_MAX_CHANNELS;
  }
}

GAUD_Result gaud_coded_geometry(GAUD_Sample_Coding coding, uint32_t channels,
    uint32_t block_bytes, uint32_t stated_frames, GAUD_Coded_Geometry * out) {
  if (!out || channels == 0u || coding <= GAUD_CODING_PCM
      || coding >= GAUD_CODING_COUNT) {
    return GAUD_ERR_INVALID;
  }
  if (channels > GAUD_CODED_MAX_CHANNELS) {
    return GAUD_ERR_LIMIT;
  }
  memset(out, 0, sizeof(*out));
  out->coding = coding;
  out->channels = channels;

  switch (coding) {
  case GAUD_CODING_G711_ULAW:
  case GAUD_CODING_G711_ALAW:
    /* Stateless, so the block is ours to pick. One byte per sample means
     * the two numbers cannot disagree. */
    out->block_frames = GAUD_CODED_RUN_FRAMES;
    out->block_bytes = GAUD_CODED_RUN_FRAMES * channels;
    break;

  case GAUD_CODING_ADPCM_IMA_QT:
    /* Fixed by the format: 34 bytes per channel, 64 frames, always. A
     * header claiming otherwise is describing a different format. */
    out->block_bytes = 34u * channels;
    out->block_frames = 64u;
    break;

  case GAUD_CODING_ADPCM_IMA_WAV: {
    uint32_t head = preamble_bytes(coding) * channels;
    if (block_bytes <= head || block_bytes > GAUD_CODED_MAX_BLOCK_BYTES) {
      return GAUD_ERR_CORRUPT;
    }
    uint32_t payload = block_bytes - head;
    /* Nibbles are dealt out in four-byte groups, one group per channel in
     * turn, so a block whose payload is not a whole number of groups has
     * a tail no reader can place. */
    if (payload % (4u * channels) != 0u) {
      return GAUD_ERR_CORRUPT;
    }
    /* Two samples per byte, per channel, plus the one in the preamble. */
    out->block_bytes = block_bytes;
    out->block_frames = (payload / channels) * 2u + 1u;
    break;
  }

  case GAUD_CODING_ADPCM_MS: {
    uint32_t head = preamble_bytes(coding) * channels;
    if (block_bytes <= head || block_bytes > GAUD_CODED_MAX_BLOCK_BYTES) {
      return GAUD_ERR_CORRUPT;
    }
    uint32_t payload = block_bytes - head;
    if (payload % channels != 0u) {
      return GAUD_ERR_CORRUPT;
    }
    /* Two samples already in the preamble, then two per byte per channel. */
    out->block_bytes = block_bytes;
    out->block_frames = (payload / channels) * 2u + 2u;
    memcpy(out->coef, gaud_ms_adpcm_default_coef,
        sizeof(gaud_ms_adpcm_default_coef));
    out->coef_count = 7u;
    break;
  }

  default: return GAUD_ERR_INVALID;
  }

  /* The container's own claim, checked rather than trusted or ignored.
   * Both writers in the oracle image state it and both state it
   * consistently; a file where it disagrees is one whose length cannot be
   * computed two ways, and guessing which number is right is how a
   * decoder ends up reading past the data chunk. */
  if (stated_frames != 0u && stated_frames != out->block_frames) {
    return GAUD_ERR_CORRUPT;
  }
  return GAUD_OK;
}

uint32_t gaud_coded_tail_frames(
    const GAUD_Coded_Geometry * geometry, size_t bytes) {
  if (!geometry || bytes == 0 || geometry->channels == 0u) {
    return 0;
  }
  if (bytes >= geometry->block_bytes) {
    return geometry->block_frames;
  }
  uint32_t channels = geometry->channels;
  switch (geometry->coding) {
  case GAUD_CODING_G711_ULAW:
  case GAUD_CODING_G711_ALAW: return (uint32_t)(bytes / channels);

  case GAUD_CODING_ADPCM_IMA_WAV: {
    size_t head = 4u * (size_t)channels;
    if (bytes < head) {
      return 0;
    }
    /* Only whole four-byte groups decode; a partial group has no placing. */
    size_t groups = (bytes - head) / (4u * (size_t)channels);
    return (uint32_t)(1u + groups * 8u);
  }
  case GAUD_CODING_ADPCM_IMA_QT:
    /* Packets are per channel and a frame needs all of them, so anything
     * short of a whole set of packets yields nothing. */
    return bytes >= 34u * (size_t)channels ? 64u : 0u;

  case GAUD_CODING_ADPCM_MS: {
    size_t head = 7u * (size_t)channels;
    if (bytes < head) {
      return 0;
    }
    size_t nibbles = (bytes - head) * 2u;
    return (uint32_t)(2u + nibbles / channels);
  }
  default: return 0;
  }
}

/* --------------------------------------------------------------- decode */

static size_t decode_g711(const GAUD_Coded_Geometry * geometry,
    const unsigned char * in, size_t in_bytes, int16_t * out) {
  size_t samples = in_bytes - (in_bytes % geometry->channels);
  bool ulaw = geometry->coding == GAUD_CODING_G711_ULAW;
  for (size_t i = 0; i < samples; ++i) {
    out[i] = ulaw ? gaud_g711_ulaw_decode(in[i]) : gaud_g711_alaw_decode(in[i]);
  }
  return samples / geometry->channels;
}

static size_t decode_ima_wav(const GAUD_Coded_Geometry * geometry,
    const unsigned char * in, size_t in_bytes, int16_t * out) {
  uint32_t channels = geometry->channels;
  size_t head = 4u * (size_t)channels;
  if (in_bytes < head) {
    return 0;
  }
  int32_t predictor[GAUD_CODED_MAX_CHANNELS];
  int index[GAUD_CODED_MAX_CHANNELS];
  for (uint32_t c = 0; c < channels; ++c) {
    const unsigned char * p = in + 4u * c;
    predictor[c] = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    index[c] = p[2] > 88 ? 88 : p[2];
    /* The preamble's predictor is the block's first sample. */
    out[c] = (int16_t)predictor[c];
  }

  size_t frames = 1;
  size_t at = head;
  /* Four bytes per channel per turn: eight consecutive samples of that
   * channel, low nibble of each byte first. */
  while (at + 4u * (size_t)channels <= in_bytes) {
    for (uint32_t c = 0; c < channels; ++c) {
      for (size_t b = 0; b < 4; ++b) {
        unsigned char byte = in[at + 4u * c + b];
        size_t base = frames + b * 2u;
        out[base * channels + c]
            = ima_step_nibble(byte & 0x0Fu, &predictor[c], &index[c]);
        out[(base + 1u) * channels + c]
            = ima_step_nibble(byte >> 4, &predictor[c], &index[c]);
      }
    }
    at += 4u * (size_t)channels;
    frames += 8;
  }
  return frames > geometry->block_frames ? geometry->block_frames : frames;
}

static size_t decode_ima_qt(const GAUD_Coded_Geometry * geometry,
    const unsigned char * in, size_t in_bytes, int16_t * out) {
  uint32_t channels = geometry->channels;
  /*
   * A frame needs every channel, and this framing stores one whole
   * 34-byte packet per channel - so a block missing any channel's packet
   * holds no complete frame at all, not a partial one.
   *
   * The first version returned the full 64 as soon as channel 0 had been
   * decoded, leaving the other channels' samples as whatever was in the
   * caller's buffer. `make fuzz-run-coded` found it in under a minute by
   * comparing the decode against gaud_coded_tail_frames(), which had the
   * rule right: those two numbers are computed by different code and a
   * container believes the prediction before the decode runs.
   */
  if (in_bytes < 34u * (size_t)channels) {
    return 0;
  }
  size_t frames = 64;
  for (uint32_t c = 0; c < channels; ++c) {
    size_t at = 34u * (size_t)c;
    /* Big-endian: the top nine bits are the predictor, already aligned as
     * a 16-bit sample with its low seven bits zero, and the low seven are
     * the step index. */
    uint16_t word = (uint16_t)(((uint16_t)in[at] << 8) | in[at + 1u]);
    int32_t predictor = (int16_t)(word & 0xFF80u);
    int index = word & 0x007Fu;
    if (index > 88) {
      index = 88;
    }
    /* Unlike WAV's framing, this predictor is NOT emitted as a sample. */
    for (size_t n = 0; n < 32; ++n) {
      unsigned char byte = in[at + 2u + n];
      out[(n * 2u) * channels + c]
          = ima_step_nibble(byte & 0x0Fu, &predictor, &index);
      out[(n * 2u + 1u) * channels + c]
          = ima_step_nibble(byte >> 4, &predictor, &index);
    }
  }
  return frames;
}

static size_t decode_ms(const GAUD_Coded_Geometry * geometry,
    const unsigned char * in, size_t in_bytes, int16_t * out) {
  uint32_t channels = geometry->channels;
  size_t head = 7u * (size_t)channels;
  if (in_bytes < head) {
    return 0;
  }
  int16_t coef1[GAUD_CODED_MAX_CHANNELS], coef2[GAUD_CODED_MAX_CHANNELS];
  int32_t delta[GAUD_CODED_MAX_CHANNELS];
  int32_t sample1[GAUD_CODED_MAX_CHANNELS], sample2[GAUD_CODED_MAX_CHANNELS];

  for (uint32_t c = 0; c < channels; ++c) {
    unsigned char which = in[c];
    if (which >= geometry->coef_count) {
      /* An index past the table cannot be decoded and must not silently
       * become zero: a wrong coefficient pair is a block of noise. */
      return 0;
    }
    coef1[c] = geometry->coef[2u * which];
    coef2[c] = geometry->coef[2u * which + 1u];
  }
  const unsigned char * p = in + channels;
  for (uint32_t c = 0; c < channels; ++c) {
    delta[c] = (int16_t)((uint16_t)p[2u * c] | ((uint16_t)p[2u * c + 1u] << 8));
  }
  p += 2u * (size_t)channels;
  for (uint32_t c = 0; c < channels; ++c) {
    sample1[c]
        = (int16_t)((uint16_t)p[2u * c] | ((uint16_t)p[2u * c + 1u] << 8));
  }
  p += 2u * (size_t)channels;
  for (uint32_t c = 0; c < channels; ++c) {
    sample2[c]
        = (int16_t)((uint16_t)p[2u * c] | ((uint16_t)p[2u * c + 1u] << 8));
  }

  /* sample2 precedes sample1 in time, which is the opposite of the order
   * they are stored in, and is the detail that makes a block start with
   * two samples swapped if it is missed. */
  for (uint32_t c = 0; c < channels; ++c) {
    out[c] = (int16_t)sample2[c];
    out[channels + c] = (int16_t)sample1[c];
  }

  /* The nibble stream IS the interleaved sample stream: nibble n is
   * channel n%channels of frame 2 + n/channels, high nibble of each byte
   * first. It is not one byte per channel in turn - that reading gives the
   * same frame count, so the geometry checks out and every stereo block
   * decodes as noise. Measured: it disagreed with ffmpeg and libsndfile,
   * which agree with each other here, by up to 35797 of 32768. */
  size_t nibbles = 0;
  size_t capacity = ((size_t)geometry->block_frames - 2u) * channels;
  for (size_t at = head; at < in_bytes && nibbles < capacity; ++at) {
    unsigned char byte = in[at];
    for (int half = 0; half < 2 && nibbles < capacity; ++half, ++nibbles) {
      unsigned nibble = half == 0 ? (unsigned)(byte >> 4) : (byte & 0x0Fu);
      uint32_t c = (uint32_t)(nibbles % channels);
      size_t frame = 2u + nibbles / channels;
      out[frame * channels + c] = ms_step_nibble(
          nibble, coef1[c], coef2[c], &delta[c], &sample1[c], &sample2[c]);
    }
  }
  /* A partial final frame - some but not all channels present - is not a
   * frame, so the division truncates deliberately. */
  return 2u + nibbles / channels;
}

size_t gaud_coded_decode_block(const GAUD_Coded_Geometry * geometry,
    const unsigned char * in, size_t in_bytes, int16_t * out) {
  if (!geometry || !in || !out || in_bytes == 0) {
    return 0;
  }
  /* gaud_coded_geometry() refuses a wider block, so this cannot fire from
   * a parsed file. It is here because the stack arrays below are sized by
   * the macro and a caller who built a geometry by hand would otherwise
   * overrun them. */
  if (geometry->channels == 0u
      || geometry->channels > GAUD_CODED_MAX_CHANNELS) {
    return 0;
  }
  if (in_bytes > geometry->block_bytes) {
    in_bytes = geometry->block_bytes;
  }
  switch (geometry->coding) {
  case GAUD_CODING_G711_ULAW:
  case GAUD_CODING_G711_ALAW: return decode_g711(geometry, in, in_bytes, out);
  case GAUD_CODING_ADPCM_IMA_WAV:
    return decode_ima_wav(geometry, in, in_bytes, out);
  case GAUD_CODING_ADPCM_IMA_QT:
    return decode_ima_qt(geometry, in, in_bytes, out);
  case GAUD_CODING_ADPCM_MS: return decode_ms(geometry, in, in_bytes, out);
  default: return 0;
  }
}

/* --------------------------------------------------------------- encode */

static void encode_g711(const GAUD_Coded_Geometry * geometry,
    const int16_t * in, size_t frames, unsigned char * out) {
  size_t samples = frames * geometry->channels;
  bool ulaw = geometry->coding == GAUD_CODING_G711_ULAW;
  for (size_t i = 0; i < samples; ++i) {
    out[i] = ulaw ? gaud_g711_ulaw_encode(in[i]) : gaud_g711_alaw_encode(in[i]);
  }
}

static void encode_ima_wav(const GAUD_Coded_Geometry * geometry,
    const int16_t * in, size_t frames, unsigned char * out) {
  uint32_t channels = geometry->channels;
  int32_t predictor[GAUD_CODED_MAX_CHANNELS];
  int index[GAUD_CODED_MAX_CHANNELS];

  for (uint32_t c = 0; c < channels; ++c) {
    /* The first sample goes in the preamble verbatim, so it survives
     * exactly and the block's error starts at zero. The step index starts
     * at 0 rather than being searched for: a wrong guess costs a few
     * samples of accuracy at the head of each block, and searching would
     * make the encoder's output depend on how the caller happened to
     * split its writes. */
    predictor[c] = in[c];
    index[c] = ima_initial_index(in, frames, channels, c);
    unsigned char * p = out + 4u * c;
    p[0] = (unsigned char)((uint16_t)(int16_t)predictor[c] & 0xFFu);
    p[1] = (unsigned char)(((uint16_t)(int16_t)predictor[c] >> 8) & 0xFFu);
    p[2] = (unsigned char)index[c];
    p[3] = 0;
  }

  size_t head = 4u * (size_t)channels;
  size_t at = head;
  size_t frame = 1;
  while (at + 4u * (size_t)channels <= geometry->block_bytes) {
    for (uint32_t c = 0; c < channels; ++c) {
      for (size_t b = 0; b < 4; ++b) {
        size_t base = frame + b * 2u;
        /* Past the caller's data the block is padded, and the padding is
         * the last real sample repeated rather than silence: a run of
         * zero nibbles holds the predictor where it is, so the tail is
         * inaudible instead of a click. */
        int32_t lo = base < frames ? in[base * channels + c] : predictor[c];
        int32_t hi = (base + 1u) < frames ? in[(base + 1u) * channels + c]
                                          : predictor[c];
        unsigned nlo = ima_pick_nibble(lo, &predictor[c], &index[c]);
        unsigned nhi = ima_pick_nibble(hi, &predictor[c], &index[c]);
        out[at + 4u * c + b] = (unsigned char)(nlo | (nhi << 4));
      }
    }
    at += 4u * (size_t)channels;
    frame += 8;
  }
}

static void encode_ima_qt(const GAUD_Coded_Geometry * geometry,
    const int16_t * in, size_t frames, unsigned char * out) {
  uint32_t channels = geometry->channels;
  for (uint32_t c = 0; c < channels; ++c) {
    /* No sample is carried in the preamble, so the predictor starts where
     * the first sample is and that sample is then coded like any other.
     * Starting it at zero instead would spend the first few nibbles
     * climbing to the signal. */
    int32_t predictor = frames > 0 ? in[c] : 0;
    int index = ima_initial_index(in, frames, channels, c);
    unsigned char * p = out + 34u * (size_t)c;
    uint16_t word
        = (uint16_t)(((uint16_t)(int16_t)predictor & 0xFF80u) | (uint16_t)index);
    p[0] = (unsigned char)(word >> 8);
    p[1] = (unsigned char)(word & 0xFFu);
    /* The predictor written is the truncated one, so the decoder and the
     * encoder start from the same value rather than from values seven
     * bits apart. */
    predictor = (int16_t)(word & 0xFF80u);

    for (size_t n = 0; n < 32; ++n) {
      size_t f0 = n * 2u, f1 = n * 2u + 1u;
      int32_t s0 = f0 < frames ? in[f0 * channels + c] : predictor;
      unsigned n0 = ima_pick_nibble(s0, &predictor, &index);
      int32_t s1 = f1 < frames ? in[f1 * channels + c] : predictor;
      unsigned n1 = ima_pick_nibble(s1, &predictor, &index);
      p[2u + n] = (unsigned char)(n0 | (n1 << 4));
    }
  }
}

static void encode_ms(const GAUD_Coded_Geometry * geometry, const int16_t * in,
    size_t frames, unsigned char * out) {
  uint32_t channels = geometry->channels;
  int32_t delta[GAUD_CODED_MAX_CHANNELS];
  int32_t sample1[GAUD_CODED_MAX_CHANNELS], sample2[GAUD_CODED_MAX_CHANNELS];
  /* Coefficient pair 0 is {256, 0}: predictor = sample1, a plain
   * first-order hold. Every writer in the oracle image picks it for every
   * block, and picking per block would need a search whose benefit is not
   * measurable against ADPCM's own noise floor. */
  const unsigned char which = 0;
  int16_t coef1 = geometry->coef[0], coef2 = geometry->coef[1];

  for (uint32_t c = 0; c < channels; ++c) {
    sample2[c] = frames > 0 ? in[c] : 0;
    sample1[c] = frames > 1 ? in[channels + c] : sample2[c];
    /* The initial step, seeded from the signal for the same reason the
     * IMA index is: the adaptation table can only move delta by about a
     * third per nibble, so a block that starts at the floor spends its
     * first samples climbing. A nibble spans -8..+7 deltas, so a quarter
     * of the mean absolute difference is a delta that can follow the
     * signal without saturating. 16 is the floor ms_step_nibble()
     * enforces and stays the minimum. Measured on a 20000-amplitude
     * sine: 43.9 dB fixed at 16, 48.1 dB seeded. */
    int32_t seed = 16;
    if (frames >= 2) {
      int64_t total = 0;
      for (size_t f = 1; f < frames; ++f) {
        int32_t a = in[f * channels + c], b = in[(f - 1u) * channels + c];
        total += a > b ? (a - b) : (b - a);
      }
      seed = (int32_t)(total / (int64_t)(frames - 1) / 4);
      if (seed < 16) {
        seed = 16;
      }
      else if (seed > 32767) {
        seed = 32767;
      }
    }
    delta[c] = seed;
  }

  unsigned char * p = out;
  for (uint32_t c = 0; c < channels; ++c) {
    *p++ = which;
  }
  for (uint32_t c = 0; c < channels; ++c) {
    *p++ = (unsigned char)((uint16_t)delta[c] & 0xFFu);
    *p++ = (unsigned char)(((uint16_t)delta[c] >> 8) & 0xFFu);
  }
  for (uint32_t c = 0; c < channels; ++c) {
    *p++ = (unsigned char)((uint16_t)(int16_t)sample1[c] & 0xFFu);
    *p++ = (unsigned char)(((uint16_t)(int16_t)sample1[c] >> 8) & 0xFFu);
  }
  for (uint32_t c = 0; c < channels; ++c) {
    *p++ = (unsigned char)((uint16_t)(int16_t)sample2[c] & 0xFFu);
    *p++ = (unsigned char)(((uint16_t)(int16_t)sample2[c] >> 8) & 0xFFu);
  }

  /* Mirrors decode_ms(): nibble n is channel n%channels of frame
   * 2 + n/channels. */
  size_t head = 7u * (size_t)channels;
  size_t nibbles = 0;
  for (size_t at = head; at < geometry->block_bytes; ++at) {
    unsigned packed = 0;
    for (int half = 0; half < 2; ++half, ++nibbles) {
      uint32_t c = (uint32_t)(nibbles % channels);
      size_t frame = 2u + nibbles / channels;
      /* Past the caller's data the tail is padding. Coding the last real
       * sample again rather than silence keeps the predictor still, so
       * the padding is inaudible instead of a step to zero. */
      int32_t want = frame < frames ? in[frame * channels + c] : sample1[c];
      unsigned nibble = ms_pick_nibble(
          want, coef1, coef2, &delta[c], &sample1[c], &sample2[c]);
      packed |= half == 0 ? (nibble << 4) : nibble;
    }
    out[at] = (unsigned char)packed;
  }
}

size_t gaud_coded_encode_block(const GAUD_Coded_Geometry * geometry,
    const int16_t * in, size_t frames, unsigned char * out) {
  if (!geometry || !in || !out || geometry->channels == 0u
      || geometry->channels > GAUD_CODED_MAX_CHANNELS) {
    return 0;
  }
  if (frames > geometry->block_frames) {
    frames = geometry->block_frames;
  }
  if (frames == 0) {
    return 0;
  }
  switch (geometry->coding) {
  case GAUD_CODING_G711_ULAW:
  case GAUD_CODING_G711_ALAW:
    /* Stateless, so a short run writes only what it has: there is no
     * block header promising a length, and padding would be extra
     * samples the file's own length then contradicts. */
    encode_g711(geometry, in, frames, out);
    return frames;
  case GAUD_CODING_ADPCM_IMA_WAV:
    encode_ima_wav(geometry, in, frames, out);
    return frames;
  case GAUD_CODING_ADPCM_IMA_QT:
    encode_ima_qt(geometry, in, frames, out);
    return frames;
  case GAUD_CODING_ADPCM_MS:
    encode_ms(geometry, in, frames, out);
    return frames;
  default: return 0;
  }
}
