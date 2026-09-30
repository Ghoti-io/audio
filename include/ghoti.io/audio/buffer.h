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
 * @file buffer.h
 *
 * Samples: how they are shaped, what a channel means, and the block a decoder
 * fills.
 *
 * ::GAUD_Buffer is this library's answer to `image`'s `GIMG_Raster`. The difference that matters is
 * that a raster holds a whole picture and a buffer holds a **block** - see
 * decoder.h for why nothing here can hold a whole track.
 */

#ifndef GHOTI_IO_GAUD_BUFFER_H
#define GHOTI_IO_GAUD_BUFFER_H

#include <ghoti.io/audio/allocator.h>
#include <ghoti.io/audio/core.h>
#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief How one sample is stored.
 *
 * Not a bit count. The signedness and the storage are part of the format -
 * WAV's 8-bit is unsigned and AIFF's is signed, and a `bits` field beside two
 * booleans is the arrangement that lets them disagree with each other.
 *
 * Samples are in the **host's** byte order once decoded. A container's byte
 * order is the container's business and does not reach here; that is the whole
 * point of AIFF and WAV producing the same buffer.
 *
 * ::GAUD_SAMPLE_S24 is **three packed bytes**, not a 32-bit value with a spare
 * byte. That is what a file holds, and this library's rule is that loading
 * converts nothing; widening it to ::GAUD_SAMPLE_S32 is
 * gaud_ops_convert_format(). Storing it padded would mean a 24-bit file could
 * not be written back byte-for-byte without the writer guessing what the
 * caller had meant.
 */
typedef enum {
  GAUD_SAMPLE_U8 = 0, ///< Unsigned 8-bit, 128 is silence. WAV's 8-bit.
  GAUD_SAMPLE_S8,     ///< Signed 8-bit, 0 is silence. AIFF's 8-bit.
  GAUD_SAMPLE_S16,    ///< Signed 16-bit.
  GAUD_SAMPLE_S24,    ///< Signed 24-bit, three packed bytes.
  GAUD_SAMPLE_S32,    ///< Signed 32-bit.
  GAUD_SAMPLE_F32,    ///< IEEE-754 single, nominally -1.0 to +1.0.
  GAUD_SAMPLE_F64,    ///< IEEE-754 double, nominally -1.0 to +1.0.

  /**
   * One-bit delta-sigma, eight samples to a byte, most significant first.
   * DSD. Not PCM: it has no per-sample amplitude, and turning it into PCM is
   * a lowpass filter whose choices are audible - so that is
   * gaud_ops_dsd_to_pcm() with the filter supplied, never something a load
   * does quietly.
   */
  GAUD_SAMPLE_DSD1,

  /**
   * Coded bytes that have not been decoded at all: one track's packets, as
   * the container stored them.
   *
   * This is what remux copies. A buffer in this format has no meaningful
   * frame count in the sample sense and `gaud_buffer_frames()` reports the
   * byte length instead, which is why gaud_sample_format_is_pcm() exists and
   * why every entry point in ops.h consults it.
   */
  GAUD_SAMPLE_OPAQUE,

  GAUD_SAMPLE_FORMAT_COUNT
} GAUD_Sample_Format;

/**
 * @brief Whether a format is linear PCM, and therefore whether the operations
 *   in ops.h mean anything for it.
 *
 * ::GAUD_SAMPLE_DSD1 and ::GAUD_SAMPLE_OPAQUE are the two that are not. Every
 * entry point in ops.h guards on this rather than each switch growing two
 * cases it would otherwise get wrong by omission.
 */
GAUD_API bool gaud_sample_format_is_pcm(GAUD_Sample_Format format);

/** @brief Whether a PCM format is floating point. False for non-PCM. */
GAUD_API bool gaud_sample_format_is_float(GAUD_Sample_Format format);

/**
 * @brief Bits in one sample of @p format.
 *
 * 1 for ::GAUD_SAMPLE_DSD1 and 8 for ::GAUD_SAMPLE_OPAQUE, which is the sense
 * in which each is stored. 0 for a value outside the enum.
 */
GAUD_API unsigned int gaud_sample_format_bits(GAUD_Sample_Format format);

/** @brief A short name, for diagnostics: "s16", "f32", "dsd1". */
GAUD_API const char * gaud_sample_format_string(GAUD_Sample_Format format);

/**
 * @brief One speaker position.
 *
 * The bit values are WAV's `dwChannelMask`, deliberately, because that is the
 * one spelling with an established numbering; every other container's order
 * is mapped onto it on the way in.
 */
typedef enum {
  GAUD_CH_FRONT_LEFT = 1u << 0,
  GAUD_CH_FRONT_RIGHT = 1u << 1,
  GAUD_CH_FRONT_CENTER = 1u << 2,
  GAUD_CH_LOW_FREQUENCY = 1u << 3,
  GAUD_CH_BACK_LEFT = 1u << 4,
  GAUD_CH_BACK_RIGHT = 1u << 5,
  GAUD_CH_FRONT_LEFT_OF_CENTER = 1u << 6,
  GAUD_CH_FRONT_RIGHT_OF_CENTER = 1u << 7,
  GAUD_CH_BACK_CENTER = 1u << 8,
  GAUD_CH_SIDE_LEFT = 1u << 9,
  GAUD_CH_SIDE_RIGHT = 1u << 10,
  GAUD_CH_TOP_CENTER = 1u << 11,
  GAUD_CH_TOP_FRONT_LEFT = 1u << 12,
  GAUD_CH_TOP_FRONT_CENTER = 1u << 13,
  GAUD_CH_TOP_FRONT_RIGHT = 1u << 14,
  GAUD_CH_TOP_BACK_LEFT = 1u << 15,
  GAUD_CH_TOP_BACK_CENTER = 1u << 16,
  GAUD_CH_TOP_BACK_RIGHT = 1u << 17
} GAUD_Channel;

/**
 * @brief Which speaker each channel of a buffer is for.
 *
 * **A channel count is not a layout**, and conflating them is the defect this
 * type exists to prevent. "Six channels" does not say which one is the LFE:
 * WAV states it in `dwChannelMask`, MP4 in a layout tag, Opus in a mapping
 * family, FLAC by a fixed order in its specification, and AIFF not at all. A
 * file whose centre and LFE are swapped sounds wrong, is attributable to
 * nothing, and passes every test that compares energy.
 *
 * `mask` of **zero means the file did not say**, which is a different fact
 * from any particular arrangement and in particular is not the same as mono.
 * A caller that needs to know the difference can; one that does not can treat
 * an unstated layout as the conventional order for that channel count.
 */
typedef struct {
  /** A bitwise OR of ::GAUD_Channel, or 0 for "not stated". */
  uint32_t mask;
  /** How many channels there are, which is authoritative even when @p mask
   *  is 0. When @p mask is non-zero its population count equals this. */
  uint32_t channels;
} GAUD_Channel_Layout;

/** @brief The layout meaning "@p channels channels, arrangement not stated". */
GAUD_API GAUD_Channel_Layout gaud_channel_layout_unspecified(
    uint32_t channels);

/**
 * @brief The conventional layout for a channel count, as WAV assigns them.
 *
 * 1 is centre, 2 is L/R, 6 is 5.1 and so on. For a count with no convention
 * the answer is gaud_channel_layout_unspecified(), which is the honest answer -
 * inventing a mask would state something the format did not.
 */
GAUD_API GAUD_Channel_Layout gaud_channel_layout_default(uint32_t channels);

/** @brief Whether a layout is self-consistent: mask 0, or popcount == channels. */
GAUD_API bool gaud_channel_layout_valid(GAUD_Channel_Layout layout);

/** @brief The position of channel @p index, or 0 when the layout is unstated. */
GAUD_API GAUD_Channel gaud_channel_layout_at(
    GAUD_Channel_Layout layout, uint32_t index);

/** @brief A short name for one position: "FL", "LFE", "TBC". */
GAUD_API const char * gaud_channel_string(GAUD_Channel channel);

/** @brief How samples of different channels sit relative to each other. */
typedef enum {
  /** Frame-major: L R L R L R. What every container in phase 1 stores. */
  GAUD_LAYOUT_INTERLEAVED = 0,
  /** Channel-major: L L L R R R. What a codec decoding per-channel produces. */
  GAUD_LAYOUT_PLANAR
} GAUD_Sample_Layout;

/** @brief A block of samples. Opaque; the accessors below are the interface. */
typedef struct GAUD_Buffer GAUD_Buffer;

/**
 * @brief Allocate a buffer and zero it.
 *
 * Zeroed means *silence* rather than merely zeroed bytes, which differ for
 * ::GAUD_SAMPLE_U8 where silence is 128. A caller that writes every frame
 * cannot tell; one that writes a short read and hands the rest on can.
 *
 * @param allocator NULL for the default.
 * @param format How each sample is stored.
 * @param layout Which speakers, and how many channels.
 * @param sample_layout Interleaved or planar.
 * @param frames Capacity, in frames. For ::GAUD_SAMPLE_OPAQUE, in bytes.
 * @param out_buffer Receives the buffer. Written only on success.
 * @return ::GAUD_OK, ::GAUD_ERR_INVALID, or ::GAUD_ERR_OOM - including when
 *   the size arithmetic would overflow, which is why this is not a malloc the
 *   caller can do themselves.
 */
GAUD_API GAUD_Result gaud_buffer_create(const GAUD_Allocator * allocator,
    GAUD_Sample_Format format, GAUD_Channel_Layout layout,
    GAUD_Sample_Layout sample_layout, size_t frames,
    GAUD_Buffer ** out_buffer);

/** @brief Free a buffer. Safe on NULL. */
GAUD_API void gaud_buffer_destroy(GAUD_Buffer * buffer);

/** @brief How many frames the buffer can hold. Bytes, for an opaque buffer. */
GAUD_API size_t gaud_buffer_capacity(const GAUD_Buffer * buffer);

/**
 * @brief How many frames the buffer currently holds.
 *
 * A decoder sets this; it is at most the capacity. The distinction is the
 * whole reason a short read is not an error: the buffer is still the size it
 * was allocated, and this says how much of it means anything.
 */
GAUD_API size_t gaud_buffer_frames(const GAUD_Buffer * buffer);

/** @brief Say how many frames are meaningful. Refuses more than the capacity. */
GAUD_API GAUD_Result gaud_buffer_set_frames(GAUD_Buffer * buffer, size_t frames);

/** @brief The sample format. */
GAUD_API GAUD_Sample_Format gaud_buffer_format(const GAUD_Buffer * buffer);

/** @brief The channel layout. */
GAUD_API GAUD_Channel_Layout gaud_buffer_layout(const GAUD_Buffer * buffer);

/** @brief How many channels, which is the layout's count. */
GAUD_API uint32_t gaud_buffer_channels(const GAUD_Buffer * buffer);

/** @brief Interleaved or planar. */
GAUD_API GAUD_Sample_Layout gaud_buffer_sample_layout(
    const GAUD_Buffer * buffer);

/** @brief Bytes in the whole allocation. */
GAUD_API size_t gaud_buffer_size_bytes(const GAUD_Buffer * buffer);

/**
 * @brief Bytes one frame of this buffer occupies.
 *
 * 0 for a non-PCM buffer, where the question has no answer.
 */
GAUD_API size_t gaud_buffer_frame_size(const GAUD_Buffer * buffer);

/**
 * @brief Bytes of the allocation that currently mean anything.
 *
 * `gaud_buffer_frames()` frames' worth, which is what a caller writing a
 * decoded block out to something else needs - gaud_buffer_size_bytes() is
 * the whole allocation and would write the unused tail as well.
 */
GAUD_API size_t gaud_buffer_bytes_used(const GAUD_Buffer * buffer);

/** @brief The samples. Valid until the buffer is destroyed. */
GAUD_API void * gaud_buffer_data(GAUD_Buffer * buffer);

/** @brief The samples, read-only. */
GAUD_API const void * gaud_buffer_data_const(const GAUD_Buffer * buffer);

/**
 * @brief The byte offset of one sample within the allocation.
 *
 * Handles both sample layouts, so that code walking a buffer does not have to
 * branch on which one it got - the arithmetic that differs is here, once,
 * rather than at every call site where it would be got subtly wrong.
 *
 * Undefined for ::GAUD_SAMPLE_DSD1, whose samples are not byte-addressable.
 *
 * @return The offset, or `(size_t)-1` for a bad argument.
 */
GAUD_API size_t gaud_buffer_offset(
    const GAUD_Buffer * buffer, size_t frame, uint32_t channel);

/** @brief Overwrite every sample with silence, and set the frame count to 0. */
GAUD_API void gaud_buffer_silence(GAUD_Buffer * buffer);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_BUFFER_H
