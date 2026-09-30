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
 * Sample-format conversion, layout conversion, and the three measurements a
 * decode gate needs.
 *
 * Everything routes through a double. That is not the fastest arrangement -
 * s16 to s32 is a shift - but it is one conversion path rather than
 * forty-nine, and phase 1's job is to be right. A specialised path is worth
 * adding when something measures it as the bottleneck, and not before.
 */

#include "../core/buffer_internal.h"
#include <ghoti.io/audio/ops.h>
#include <ghoti.io/cutil/allocator.h>
#include <math.h>
#include <string.h>

void gaud_convert_options_default(GAUD_Convert_Options * options) {
  if (!options) {
    return;
  }
  *options = (GAUD_Convert_Options){
      .dither = GAUD_DITHER_TRIANGULAR,
      .dither_seed = 0,
      .clip = true,
  };
}

/* Full-scale magnitude for an integer format: 2^(bits-1). A sample divided
 * by this is in -1.0..+1.0, with the negative extreme reaching exactly -1.0
 * and the positive one falling one step short - which is two's complement
 * rather than a choice made here. */
static double full_scale(GAUD_Sample_Format format) {
  switch (format) {
  case GAUD_SAMPLE_U8:
  case GAUD_SAMPLE_S8: return 128.0;
  case GAUD_SAMPLE_S16: return 32768.0;
  case GAUD_SAMPLE_S24: return 8388608.0;
  case GAUD_SAMPLE_S32: return 2147483648.0;
  default: return 1.0;
  }
}

/* Read one sample as a double in -1.0..+1.0. */
static double read_sample(const unsigned char * p, GAUD_Sample_Format format) {
  switch (format) {
  case GAUD_SAMPLE_U8:
    /* WAV's 8-bit is unsigned with 128 as silence. */
    return ((double)p[0] - 128.0) / 128.0;
  case GAUD_SAMPLE_S8: return (double)(int8_t)p[0] / 128.0;
  case GAUD_SAMPLE_S16: {
    int16_t v;
    memcpy(&v, p, sizeof(v));
    return (double)v / 32768.0;
  }
  case GAUD_SAMPLE_S24: {
    /* Three packed bytes, host order, sign-extended from bit 23. Assembled
     * rather than memcpy'd into an int32_t: there is no 24-bit integer type
     * and a cast through one would read a fourth byte. */
    uint32_t raw
        = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
    if (raw & 0x800000u) {
      raw |= 0xFF000000u;
    }
    return (double)(int32_t)raw / 8388608.0;
  }
  case GAUD_SAMPLE_S32: {
    int32_t v;
    memcpy(&v, p, sizeof(v));
    return (double)v / 2147483648.0;
  }
  case GAUD_SAMPLE_F32: {
    float v;
    memcpy(&v, p, sizeof(v));
    return (double)v;
  }
  case GAUD_SAMPLE_F64: {
    double v;
    memcpy(&v, p, sizeof(v));
    return v;
  }
  default: return 0.0;
  }
}

/* xorshift64*, so that a dithered conversion is reproducible from its seed.
 * A conversion that cannot be repeated cannot be gated by a golden file. */
static uint64_t next_random(uint64_t * state) {
  uint64_t x = *state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *state = x;
  return x * UINT64_C(0x2545F4914F6CDD1D);
}

/* A uniform double in [-0.5, +0.5). 53 bits, which is a double's mantissa:
 * fewer would quantise the noise and more would be discarded. */
static double uniform_half(uint64_t * state) {
  uint64_t bits = next_random(state) >> 11;
  return (double)bits / 9007199254740992.0 - 0.5;
}

/* Write one sample from a double, narrowing as the format requires. */
static void write_sample(unsigned char * p, GAUD_Sample_Format format,
    double value, const GAUD_Convert_Options * options, uint64_t * state,
    bool narrowing) {
  if (format == GAUD_SAMPLE_F32) {
    float v = (float)value;
    memcpy(p, &v, sizeof(v));
    return;
  }
  if (format == GAUD_SAMPLE_F64) {
    memcpy(p, &value, sizeof(value));
    return;
  }

  double scale = full_scale(format);
  double scaled = value * scale;

  /* Dither is added in the destination's units, before rounding, and only
   * when the conversion actually discards something. Adding it while
   * widening would be adding noise to a step that loses nothing. */
  if (narrowing && options->dither != GAUD_DITHER_NONE) {
    if (options->dither == GAUD_DITHER_RECTANGULAR) {
      scaled += uniform_half(state);
    }
    else {
      /* Triangular: the sum of two independent uniforms. This is the one
       * that removes noise modulation rather than merely reducing it. */
      scaled += uniform_half(state) + uniform_half(state);
    }
  }

  /* Round half away from zero. nearbyint would follow the ambient rounding
   * mode, and this library's output must not depend on it. */
  double rounded = scaled >= 0.0 ? floor(scaled + 0.5) : ceil(scaled - 0.5);

  /* The bound is not optional: converting a double outside an integer's
   * range is undefined behaviour in C, so even the "no clip" path has to
   * bound before the cast. What `clip` selects is the width of the bound. */
  double lo = -scale;
  double hi = scale - 1.0;
  if (rounded < lo) {
    rounded = lo;
  }
  if (rounded > hi) {
    rounded = hi;
  }

  int64_t v = (int64_t)rounded;
  switch (format) {
  case GAUD_SAMPLE_U8: p[0] = (unsigned char)(v + 128); break;
  case GAUD_SAMPLE_S8: p[0] = (unsigned char)(int8_t)v; break;
  case GAUD_SAMPLE_S16: {
    int16_t s = (int16_t)v;
    memcpy(p, &s, sizeof(s));
    break;
  }
  case GAUD_SAMPLE_S24: {
    uint32_t u = (uint32_t)(int32_t)v;
    p[0] = (unsigned char)(u & 0xFFu);
    p[1] = (unsigned char)((u >> 8) & 0xFFu);
    p[2] = (unsigned char)((u >> 16) & 0xFFu);
    break;
  }
  case GAUD_SAMPLE_S32: {
    int32_t s = (int32_t)v;
    memcpy(p, &s, sizeof(s));
    break;
  }
  default: break;
  }
}

GAUD_Result gaud_ops_convert_format(const GAUD_Allocator * allocator,
    const GAUD_Buffer * source, GAUD_Sample_Format format,
    const GAUD_Convert_Options * options, GAUD_Buffer ** out_buffer) {
  if (!source || !out_buffer) {
    return GAUD_ERR_INVALID;
  }
  if (!gaud_sample_format_is_pcm(source->format)
      || !gaud_sample_format_is_pcm(format)) {
    return GAUD_ERR_UNSUPPORTED;
  }
  GAUD_Convert_Options defaults;
  if (!options) {
    gaud_convert_options_default(&defaults);
    options = &defaults;
  }
  if ((unsigned)options->dither >= (unsigned)GAUD_DITHER_COUNT) {
    return GAUD_ERR_INVALID;
  }

  GAUD_Buffer * out = NULL;
  GAUD_Result result = gaud_buffer_create(allocator, format, source->layout,
      source->sample_layout, source->capacity, &out);
  if (result != GAUD_OK) {
    return result;
  }

  /* "Narrowing" is about how many distinct values survive, not byte width.
   * Anything to a float is lossless here; float to integer always discards;
   * integer to integer discards only with fewer destination bits. */
  bool narrowing;
  if (gaud_sample_format_is_float(format)) {
    narrowing = false;
  }
  else if (gaud_sample_format_is_float(source->format)) {
    narrowing = true;
  }
  else {
    narrowing = gaud_sample_format_bits(format)
        < gaud_sample_format_bits(source->format);
  }

  /* Seeded once for the buffer rather than per sample, so the noise is a
   * sequence rather than one value repeated - and reproducible, which is
   * what lets a golden file gate a dithered conversion. */
  uint64_t state = options->dither_seed ? options->dither_seed
                                        : UINT64_C(0x9E3779B97F4A7C15);

  uint32_t channels = source->layout.channels;
  for (size_t frame = 0; frame < source->capacity; ++frame) {
    for (uint32_t ch = 0; ch < channels; ++ch) {
      size_t src_off = gaud_buffer_offset(source, frame, ch);
      size_t dst_off = gaud_buffer_offset(out, frame, ch);
      double value = read_sample(source->data + src_off, source->format);
      write_sample(
          out->data + dst_off, format, value, options, &state, narrowing);
    }
  }
  out->frames = source->frames;
  *out_buffer = out;
  return GAUD_OK;
}

GAUD_Result gaud_ops_convert_sample_layout(const GAUD_Allocator * allocator,
    const GAUD_Buffer * source, GAUD_Sample_Layout sample_layout,
    GAUD_Buffer ** out_buffer) {
  if (!source || !out_buffer) {
    return GAUD_ERR_INVALID;
  }
  if (!gaud_sample_format_is_pcm(source->format)) {
    return GAUD_ERR_UNSUPPORTED;
  }
  if (sample_layout != GAUD_LAYOUT_INTERLEAVED
      && sample_layout != GAUD_LAYOUT_PLANAR) {
    return GAUD_ERR_INVALID;
  }
  GAUD_Buffer * out = NULL;
  GAUD_Result result = gaud_buffer_create(allocator, source->format,
      source->layout, sample_layout, source->capacity, &out);
  if (result != GAUD_OK) {
    return result;
  }
  /* Deliberately not short-circuited when the layouts already match: a copy
   * is the documented behaviour, so a caller normalising an input need not
   * check first. */
  size_t sample = gaud_sample_format_bits(source->format) / 8u;
  for (size_t frame = 0; frame < source->capacity; ++frame) {
    for (uint32_t ch = 0; ch < source->layout.channels; ++ch) {
      memcpy(out->data + gaud_buffer_offset(out, frame, ch),
          source->data + gaud_buffer_offset(source, frame, ch), sample);
    }
  }
  out->frames = source->frames;
  *out_buffer = out;
  return GAUD_OK;
}

/*
 * The three measurements planning/audio.md 12 asks for, because one number
 * cannot see what the others can: a decoder uniformly 6 dB down has a small
 * difference from the reference and a wrong absolute level, and one with a
 * sign or bias error has the right RMS and a moved mean.
 *
 * All three walk `frames` rather than `capacity`: a half-filled buffer's
 * unwritten tail is silence, and including it would dilute every answer by
 * however much of the buffer went unused.
 */
/** @brief Which of the three statistics measure() should return. */
typedef enum { MEASURE_PEAK, MEASURE_RMS, MEASURE_DC } Measure;

static GAUD_Result measure(
    const GAUD_Buffer * buffer, Measure which, double * out) {
  if (!buffer || !out) {
    return GAUD_ERR_INVALID;
  }
  if (!gaud_sample_format_is_pcm(buffer->format)) {
    return GAUD_ERR_UNSUPPORTED;
  }
  double peak = 0.0;
  double sum = 0.0;
  double sum_squares = 0.0;
  size_t count = 0;
  for (size_t frame = 0; frame < buffer->frames; ++frame) {
    for (uint32_t ch = 0; ch < buffer->layout.channels; ++ch) {
      double v = read_sample(
          buffer->data + gaud_buffer_offset(buffer, frame, ch),
          buffer->format);
      double magnitude = fabs(v);
      if (magnitude > peak) {
        peak = magnitude;
      }
      sum += v;
      sum_squares += v * v;
      ++count;
    }
  }
  if (count == 0) {
    *out = 0.0;
    return GAUD_OK;
  }
  switch (which) {
  case MEASURE_PEAK: *out = peak; break;
  case MEASURE_RMS: *out = sqrt(sum_squares / (double)count); break;
  case MEASURE_DC: *out = sum / (double)count; break;
  }
  return GAUD_OK;
}

GAUD_Result gaud_ops_peak(const GAUD_Buffer * buffer, double * out_peak) {
  return measure(buffer, MEASURE_PEAK, out_peak);
}

GAUD_Result gaud_ops_rms(const GAUD_Buffer * buffer, double * out_rms) {
  return measure(buffer, MEASURE_RMS, out_rms);
}

GAUD_Result gaud_ops_dc_offset(const GAUD_Buffer * buffer, double * out_dc) {
  return measure(buffer, MEASURE_DC, out_dc);
}
