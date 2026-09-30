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
 * Sample formats, channel layouts, and the sample buffer.
 */

#include "buffer_internal.h"
/* gaud_frame_size is declared GAUD_API in codec_sdk.h. Without that
 * declaration in scope the definition below compiles with the default
 * -fvisibility=hidden and never reaches the dynamic symbol table - and
 * check-symbols cannot see it, because it asks whether every EXPORTED
 * symbol is renamed and a symbol that was never exported passes that
 * trivially. `make check-outoftree` is what caught it. */
#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/* Bits per sample. DSD is one bit and an opaque byte is eight; both are the
 * sense in which each is stored, which is what the size arithmetic needs. */
static const unsigned int format_bits[GAUD_SAMPLE_FORMAT_COUNT] = {
    [GAUD_SAMPLE_U8] = 8,
    [GAUD_SAMPLE_S8] = 8,
    [GAUD_SAMPLE_S16] = 16,
    [GAUD_SAMPLE_S24] = 24,
    [GAUD_SAMPLE_S32] = 32,
    [GAUD_SAMPLE_F32] = 32,
    [GAUD_SAMPLE_F64] = 64,
    [GAUD_SAMPLE_DSD1] = 1,
    [GAUD_SAMPLE_OPAQUE] = 8,
};

static const char * const format_names[GAUD_SAMPLE_FORMAT_COUNT] = {
    [GAUD_SAMPLE_U8] = "u8",
    [GAUD_SAMPLE_S8] = "s8",
    [GAUD_SAMPLE_S16] = "s16",
    [GAUD_SAMPLE_S24] = "s24",
    [GAUD_SAMPLE_S32] = "s32",
    [GAUD_SAMPLE_F32] = "f32",
    [GAUD_SAMPLE_F64] = "f64",
    [GAUD_SAMPLE_DSD1] = "dsd1",
    [GAUD_SAMPLE_OPAQUE] = "opaque",
};

static bool format_in_range(GAUD_Sample_Format format) {
  return (unsigned)format < (unsigned)GAUD_SAMPLE_FORMAT_COUNT;
}

bool gaud_sample_format_is_pcm(GAUD_Sample_Format format) {
  if (!format_in_range(format)) {
    return false;
  }
  return format != GAUD_SAMPLE_DSD1 && format != GAUD_SAMPLE_OPAQUE;
}

bool gaud_sample_format_is_float(GAUD_Sample_Format format) {
  return format == GAUD_SAMPLE_F32 || format == GAUD_SAMPLE_F64;
}

unsigned int gaud_sample_format_bits(GAUD_Sample_Format format) {
  return format_in_range(format) ? format_bits[format] : 0u;
}

const char * gaud_sample_format_string(GAUD_Sample_Format format) {
  if (!format_in_range(format) || !format_names[format]) {
    return "unknown";
  }
  return format_names[format];
}

size_t gaud_frame_size(GAUD_Sample_Format format, uint32_t channels) {
  /* 0 for a non-PCM format, because "how many bytes is one frame" has no
   * answer there: a DSD frame is one bit per channel, and an opaque buffer
   * has no frames at all. A caller that needs those sizes asks the buffer. */
  if (!gaud_sample_format_is_pcm(format) || channels == 0) {
    return 0;
  }
  size_t bytes = gaud_sample_format_bits(format) / 8u;
  if (bytes > SIZE_MAX / channels) {
    return 0;
  }
  return bytes * channels;
}

GAUD_Channel_Layout gaud_channel_layout_unspecified(uint32_t channels) {
  GAUD_Channel_Layout layout = {.mask = 0, .channels = channels};
  return layout;
}

/* WAV's conventional assignments for 1 to 8 channels. Anything else has no
 * convention worth inventing, and says so. */
static const uint32_t default_masks[9] = {
    [1] = GAUD_CH_FRONT_CENTER,
    [2] = GAUD_CH_FRONT_LEFT | GAUD_CH_FRONT_RIGHT,
    [3] = GAUD_CH_FRONT_LEFT | GAUD_CH_FRONT_RIGHT | GAUD_CH_FRONT_CENTER,
    [4] = GAUD_CH_FRONT_LEFT | GAUD_CH_FRONT_RIGHT | GAUD_CH_BACK_LEFT
        | GAUD_CH_BACK_RIGHT,
    [5] = GAUD_CH_FRONT_LEFT | GAUD_CH_FRONT_RIGHT | GAUD_CH_FRONT_CENTER
        | GAUD_CH_BACK_LEFT | GAUD_CH_BACK_RIGHT,
    [6] = GAUD_CH_FRONT_LEFT | GAUD_CH_FRONT_RIGHT | GAUD_CH_FRONT_CENTER
        | GAUD_CH_LOW_FREQUENCY | GAUD_CH_BACK_LEFT | GAUD_CH_BACK_RIGHT,
    [7] = GAUD_CH_FRONT_LEFT | GAUD_CH_FRONT_RIGHT | GAUD_CH_FRONT_CENTER
        | GAUD_CH_LOW_FREQUENCY | GAUD_CH_BACK_CENTER | GAUD_CH_SIDE_LEFT
        | GAUD_CH_SIDE_RIGHT,
    [8] = GAUD_CH_FRONT_LEFT | GAUD_CH_FRONT_RIGHT | GAUD_CH_FRONT_CENTER
        | GAUD_CH_LOW_FREQUENCY | GAUD_CH_BACK_LEFT | GAUD_CH_BACK_RIGHT
        | GAUD_CH_SIDE_LEFT | GAUD_CH_SIDE_RIGHT,
};

GAUD_Channel_Layout gaud_channel_layout_default(uint32_t channels) {
  GAUD_Channel_Layout layout = {.mask = 0, .channels = channels};
  if (channels >= 1 && channels <= 8) {
    layout.mask = default_masks[channels];
  }
  return layout;
}

static uint32_t popcount32(uint32_t v) {
  uint32_t n = 0;
  while (v) {
    v &= v - 1u;
    ++n;
  }
  return n;
}

bool gaud_channel_layout_valid(GAUD_Channel_Layout layout) {
  if (layout.channels == 0) {
    return false;
  }
  if (layout.mask == 0) {
    return true; /* "not stated" is valid, and is not the same as mono. */
  }
  return popcount32(layout.mask) == layout.channels;
}

GAUD_Channel gaud_channel_layout_at(
    GAUD_Channel_Layout layout, uint32_t index) {
  if (layout.mask == 0 || index >= layout.channels) {
    return (GAUD_Channel)0;
  }
  /* Channels are ordered by ascending bit position, which is what every
   * container that states a mask means by it. */
  uint32_t seen = 0;
  for (uint32_t bit = 0; bit < 32; ++bit) {
    uint32_t value = 1u << bit;
    if (layout.mask & value) {
      if (seen == index) {
        return (GAUD_Channel)value;
      }
      ++seen;
    }
  }
  return (GAUD_Channel)0;
}

const char * gaud_channel_string(GAUD_Channel channel) {
  switch (channel) {
  case GAUD_CH_FRONT_LEFT: return "FL";
  case GAUD_CH_FRONT_RIGHT: return "FR";
  case GAUD_CH_FRONT_CENTER: return "FC";
  case GAUD_CH_LOW_FREQUENCY: return "LFE";
  case GAUD_CH_BACK_LEFT: return "BL";
  case GAUD_CH_BACK_RIGHT: return "BR";
  case GAUD_CH_FRONT_LEFT_OF_CENTER: return "FLC";
  case GAUD_CH_FRONT_RIGHT_OF_CENTER: return "FRC";
  case GAUD_CH_BACK_CENTER: return "BC";
  case GAUD_CH_SIDE_LEFT: return "SL";
  case GAUD_CH_SIDE_RIGHT: return "SR";
  case GAUD_CH_TOP_CENTER: return "TC";
  case GAUD_CH_TOP_FRONT_LEFT: return "TFL";
  case GAUD_CH_TOP_FRONT_CENTER: return "TFC";
  case GAUD_CH_TOP_FRONT_RIGHT: return "TFR";
  case GAUD_CH_TOP_BACK_LEFT: return "TBL";
  case GAUD_CH_TOP_BACK_CENTER: return "TBC";
  case GAUD_CH_TOP_BACK_RIGHT: return "TBR";
  default: return "?";
  }
}

/* The byte that means silence. Zero for everything signed, 0x80 for unsigned
 * eight-bit, and 0x69 for DSD - the alternating pattern a DSD stream idles
 * at, which decodes to nothing rather than to a DC offset. */
static unsigned char silence_byte(GAUD_Sample_Format format) {
  switch (format) {
  case GAUD_SAMPLE_U8: return 0x80u;
  case GAUD_SAMPLE_DSD1: return 0x69u;
  default: return 0x00u;
  }
}

size_t gaud_buffer_bytes_for(
    GAUD_Sample_Format format, uint32_t channels, size_t frames) {
  if (!format_in_range(format) || channels == 0) {
    return 0;
  }
  if (frames == 0) {
    return 0;
  }
  if (format == GAUD_SAMPLE_OPAQUE) {
    /* `frames` is a byte count for an opaque buffer, and the channel count
     * does not multiply it: coded bytes are one stream, not one per
     * channel. */
    return frames;
  }
  if (format == GAUD_SAMPLE_DSD1) {
    /* One bit per sample, so the product is in bits and rounds up. */
    if (frames > SIZE_MAX / channels) {
      return 0;
    }
    size_t bits = frames * channels;
    if (bits > SIZE_MAX - 7u) {
      return 0;
    }
    return (bits + 7u) / 8u;
  }
  size_t frame_size = gaud_frame_size(format, channels);
  if (frame_size == 0 || frames > SIZE_MAX / frame_size) {
    return 0;
  }
  return frames * frame_size;
}

GAUD_Result gaud_buffer_create(const GAUD_Allocator * allocator,
    GAUD_Sample_Format format, GAUD_Channel_Layout layout,
    GAUD_Sample_Layout sample_layout, size_t frames,
    GAUD_Buffer ** out_buffer) {
  if (!out_buffer || !format_in_range(format)
      || !gaud_channel_layout_valid(layout)) {
    return GAUD_ERR_INVALID;
  }
  if (sample_layout != GAUD_LAYOUT_INTERLEAVED
      && sample_layout != GAUD_LAYOUT_PLANAR) {
    return GAUD_ERR_INVALID;
  }

  size_t bytes = gaud_buffer_bytes_for(format, layout.channels, frames);
  if (frames != 0 && bytes == 0) {
    /* Only reachable through overflow, since every other zero answer has
     * already been refused above. */
    return GAUD_ERR_OOM;
  }

  GAUD_Buffer * buffer = gcu_allocator_malloc(allocator, sizeof(GAUD_Buffer));
  if (!buffer) {
    return GAUD_ERR_OOM;
  }
  unsigned char * data = NULL;
  if (bytes) {
    data = gcu_allocator_malloc(allocator, bytes);
    if (!data) {
      gcu_allocator_free(allocator, buffer);
      return GAUD_ERR_OOM;
    }
    memset(data, silence_byte(format), bytes);
  }

  *buffer = (GAUD_Buffer){
      .allocator = allocator,
      .format = format,
      .layout = layout,
      .sample_layout = sample_layout,
      .capacity = frames,
      .frames = 0,
      .bytes = bytes,
      .data = data,
  };
  *out_buffer = buffer;
  return GAUD_OK;
}

void gaud_buffer_destroy(GAUD_Buffer * buffer) {
  if (!buffer) {
    return;
  }
  gcu_allocator_free(buffer->allocator, buffer->data);
  gcu_allocator_free(buffer->allocator, buffer);
}

size_t gaud_buffer_capacity(const GAUD_Buffer * buffer) {
  return buffer ? buffer->capacity : 0;
}

size_t gaud_buffer_frames(const GAUD_Buffer * buffer) {
  return buffer ? buffer->frames : 0;
}

GAUD_Result gaud_buffer_set_frames(GAUD_Buffer * buffer, size_t frames) {
  if (!buffer) {
    return GAUD_ERR_INVALID;
  }
  if (frames > buffer->capacity) {
    return GAUD_ERR_INVALID;
  }
  buffer->frames = frames;
  return GAUD_OK;
}

GAUD_Sample_Format gaud_buffer_format(const GAUD_Buffer * buffer) {
  return buffer ? buffer->format : GAUD_SAMPLE_FORMAT_COUNT;
}

GAUD_Channel_Layout gaud_buffer_layout(const GAUD_Buffer * buffer) {
  if (!buffer) {
    GAUD_Channel_Layout empty = {0, 0};
    return empty;
  }
  return buffer->layout;
}

uint32_t gaud_buffer_channels(const GAUD_Buffer * buffer) {
  return buffer ? buffer->layout.channels : 0u;
}

GAUD_Sample_Layout gaud_buffer_sample_layout(const GAUD_Buffer * buffer) {
  return buffer ? buffer->sample_layout : GAUD_LAYOUT_INTERLEAVED;
}

size_t gaud_buffer_size_bytes(const GAUD_Buffer * buffer) {
  return buffer ? buffer->bytes : 0;
}

void * gaud_buffer_data(GAUD_Buffer * buffer) {
  return buffer ? buffer->data : NULL;
}

const void * gaud_buffer_data_const(const GAUD_Buffer * buffer) {
  return buffer ? buffer->data : NULL;
}

size_t gaud_buffer_offset(
    const GAUD_Buffer * buffer, size_t frame, uint32_t channel) {
  if (!buffer || frame >= buffer->capacity
      || channel >= buffer->layout.channels) {
    return (size_t)-1;
  }
  if (buffer->format == GAUD_SAMPLE_DSD1) {
    return (size_t)-1; /* Not byte-addressable; documented. */
  }
  if (buffer->format == GAUD_SAMPLE_OPAQUE) {
    return frame; /* One stream of bytes; the channel is not a dimension. */
  }
  size_t sample = gaud_sample_format_bits(buffer->format) / 8u;
  if (buffer->sample_layout == GAUD_LAYOUT_INTERLEAVED) {
    return (frame * buffer->layout.channels + channel) * sample;
  }
  return ((size_t)channel * buffer->capacity + frame) * sample;
}

void gaud_buffer_silence(GAUD_Buffer * buffer) {
  if (!buffer) {
    return;
  }
  if (buffer->data && buffer->bytes) {
    memset(buffer->data, silence_byte(buffer->format), buffer->bytes);
  }
  buffer->frames = 0;
}

size_t gaud_buffer_frame_size(const GAUD_Buffer * buffer) {
  if (!buffer) {
    return 0;
  }
  return gaud_frame_size(buffer->format, buffer->layout.channels);
}

size_t gaud_buffer_bytes_used(const GAUD_Buffer * buffer) {
  if (!buffer) {
    return 0;
  }
  return gaud_buffer_bytes_for(
      buffer->format, buffer->layout.channels, buffer->frames);
}
