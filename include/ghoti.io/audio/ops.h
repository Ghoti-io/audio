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
 * @file ops.h
 *
 * The conversions this library will do, and only when asked.
 *
 * `image`'s rule is that colour is described and carried, never silently
 * converted, and that the conversion is one named call. The audio analogues
 * of colour are the sample format, the sample rate, the channel layout and
 * the loudness. Loading and saving touch none of them. This header is where
 * a caller says otherwise.
 *
 * ### These are floating point and promise nothing about being identical
 *
 * planning/audio.md 11.1 promises that the *decoders* produce byte-identical
 * PCM on every platform, gated across architectures. **That promise does not
 * extend here.** Resampling and loudness are float, and two machines may
 * differ in the last bit. The two numerical contracts live in one library and
 * this paragraph is the boundary between them.
 *
 * ### Non-PCM buffers are refused
 *
 * Every entry point below begins by asking gaud_sample_format_is_pcm(), and
 * answers ::GAUD_ERR_UNSUPPORTED when it is false. A DSD buffer has no
 * per-sample amplitude and an opaque buffer is coded bytes; neither has an
 * arithmetic meaning, and the alternative to one guard per entry point is two
 * cases quietly missing from every switch.
 */

#ifndef GHOTI_IO_GAUD_OPS_H
#define GHOTI_IO_GAUD_OPS_H

#include <ghoti.io/audio/allocator.h>
#include <ghoti.io/audio/buffer.h>
#include <ghoti.io/audio/core.h>
#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What to do about the error when narrowing a sample.
 *
 * Going from 24 bits to 16, or from float to any integer, throws information
 * away. Rounding alone correlates the error with the signal, which is heard
 * as distortion on quiet passages rather than as noise. Dither decorrelates
 * it by adding a small random amount before rounding, trading a slightly
 * higher noise floor for an error that does not track the music.
 *
 * **There is no right default, which is why this is a parameter.** Rounding
 * is right for an intermediate step that will be narrowed again later, and
 * dither is right for the last one. A library that picked one silently would
 * have an opinion it never told anyone - which is the same objection this
 * suite makes to an image library that quantises without saying so.
 */
typedef enum {
  /** Round to nearest, ties away from zero. No noise added. */
  GAUD_DITHER_NONE = 0,
  /**
   * Rectangular probability density: one uniform random LSB. Cheapest, and
   * leaves a little noise modulation.
   */
  GAUD_DITHER_RECTANGULAR,
  /**
   * Triangular probability density: the sum of two uniform LSBs. The usual
   * choice, and the one that actually eliminates noise modulation rather
   * than reducing it. 3 dB more noise than rectangular, in exchange.
   */
  GAUD_DITHER_TRIANGULAR,
  GAUD_DITHER_COUNT
} GAUD_Dither;

/**
 * @brief How a conversion should behave.
 */
typedef struct {
  /** What to do about the error when narrowing. Ignored when widening, where
   *  nothing is lost and dither would only add noise. */
  GAUD_Dither dither;
  /**
   * The dither generator's seed.
   *
   * Explicit, and not taken from the clock, because a conversion that cannot
   * be repeated cannot be tested: a golden-file gate over a dithered
   * conversion needs the same noise every run. A caller wanting different
   * noise per run passes a different seed and has said so.
   */
  uint64_t dither_seed;
  /**
   * Whether a float sample outside -1.0..+1.0 is clamped rather than
   * wrapping when it becomes an integer.
   *
   * True is almost always right. False exists so that a test can show what
   * the clamp is doing, and because wrapping is at least a defined answer
   * rather than the C undefined behaviour an unchecked cast would be.
   */
  bool clip;
} GAUD_Convert_Options;

/** @brief Triangular dither, seed 0, clipping on. */
GAUD_API void gaud_convert_options_default(GAUD_Convert_Options * options);

/**
 * @brief Convert a buffer to another sample format.
 *
 * Allocates the result; the input is untouched. The channel layout, the
 * sample layout and the frame count carry over unchanged - this changes how
 * a sample is stored and nothing else.
 *
 * Integer formats are scaled by their full-scale value, so s16 -32768 and
 * f32 -1.0 are the same sample. The asymmetry of two's complement means the
 * positive extreme is not exactly 1.0, and converting f32 +1.0 to s16 gives
 * +32767 rather than wrapping to -32768 when clipping is on.
 *
 * @param allocator NULL for the default.
 * @param source The buffer to convert.
 * @param format What to convert it to.
 * @param options NULL for gaud_convert_options_default().
 * @param out_buffer Receives the result. Written only on success.
 * @return ::GAUD_OK; ::GAUD_ERR_UNSUPPORTED when either format is not PCM;
 *   ::GAUD_ERR_INVALID; ::GAUD_ERR_OOM.
 */
GAUD_API GAUD_Result gaud_ops_convert_format(const GAUD_Allocator * allocator,
    const GAUD_Buffer * source, GAUD_Sample_Format format,
    const GAUD_Convert_Options * options, GAUD_Buffer ** out_buffer);

/**
 * @brief Convert between interleaved and planar.
 *
 * The sample format is unchanged; this moves samples about. Converting a
 * buffer to the layout it already has is a copy rather than an error, so a
 * caller normalising an input does not have to check first.
 */
GAUD_API GAUD_Result gaud_ops_convert_sample_layout(
    const GAUD_Allocator * allocator, const GAUD_Buffer * source,
    GAUD_Sample_Layout sample_layout, GAUD_Buffer ** out_buffer);

/**
 * @brief The largest absolute sample in a buffer, as a fraction of full
 *   scale.
 *
 * Not a conversion, but it belongs with them: it is what a caller uses to
 * decide whether a narrowing conversion is going to clip, and it is what a
 * test uses to tell a decode that is 6 dB down from a decode that is right.
 * planning/audio.md 12 names measuring the absolute level, rather than only
 * a difference, as the thing that catches a uniformly quiet decoder.
 *
 * @param buffer The buffer.
 * @param out_peak Receives 0.0 for silence, 1.0 for full scale, and more than
 *   1.0 for a float buffer that is over.
 */
GAUD_API GAUD_Result gaud_ops_peak(
    const GAUD_Buffer * buffer, double * out_peak);

/**
 * @brief The root-mean-square level of a buffer, as a fraction of full scale.
 *
 * Per the whole buffer, across every channel. A silent buffer is 0.0.
 */
GAUD_API GAUD_Result gaud_ops_rms(const GAUD_Buffer * buffer, double * out_rms);

/**
 * @brief The mean sample value, as a fraction of full scale.
 *
 * Should be about zero for any real recording. A test measures it because a
 * sign error or a bias in a decoder moves it and leaves the RMS almost
 * unchanged - so an RMS comparison alone would pass.
 */
GAUD_API GAUD_Result gaud_ops_dc_offset(
    const GAUD_Buffer * buffer, double * out_dc);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_OPS_H
