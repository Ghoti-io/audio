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
 * A codec that lives outside this library, to prove one can.
 *
 * **This file is compiled against the INSTALLED headers and nothing else.**
 * It never sees `src/`, it never sees an internal header, and it is built by
 * `make check-outoftree` after installing into a throwaway prefix - exactly
 * as `audio-aac` or a stranger's codec would be built.
 *
 * Without this, "third parties can add codecs" is a claim nothing has ever
 * run. planning/audio.md 11.2 lists it as an obligation of making the codec
 * SDK public, and it is the only thing that can tell a genuinely open
 * interface from one that merely looks open - `image`'s registry looks open
 * and admits nothing but a probe.
 *
 * The format is deliberately trivial, because the format is not the point:
 *
 *     "TOY1"  magic
 *     u32le   sample rate
 *     u16le   channels
 *     u16le   reserved, must be zero
 *     ...     signed 16-bit little-endian interleaved samples to the end
 */

#include <ghoti.io/audio/audio.h>
#include <ghoti.io/audio/codec_sdk.h>
#include <stdlib.h>
#include <string.h>

#define TOY_HEADER 12u

typedef struct {
  uint64_t data_offset;
  size_t frame_size;
} Toy_State;

static uint32_t rd_u32le(const unsigned char * p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
      | ((uint32_t)p[3] << 24);
}

static uint16_t rd_u16le(const unsigned char * p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static GAUD_Result toy_read(GAUD_Decoder * decoder, GAUD_Buffer * buffer) {
  GAUD_Track * track = gaud_decoder_track(decoder);
  Toy_State * state = gaud_track_private(track);
  GAUD_Stream * stream = gaud_doc_stream(gaud_track_doc(track));

  uint64_t position = gaud_decoder_tell(decoder);
  uint64_t total = gaud_track_frames(track);
  if (position >= total) {
    return GAUD_OK;
  }
  size_t want = gaud_buffer_capacity(buffer);
  if ((uint64_t)want > total - position) {
    want = (size_t)(total - position);
  }
  uint64_t at = state->data_offset + position * state->frame_size;
  if (gaud_stream_seek(stream, (int64_t)at, GAUD_SEEK_SET) != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  size_t got = gaud_stream_read(
      stream, gaud_buffer_data(buffer), want * state->frame_size);
  size_t frames = got / state->frame_size;
  GAUD_Result result = gaud_buffer_set_frames(buffer, frames);
  if (result != GAUD_OK) {
    return result;
  }
  gaud_decoder_set_position(decoder, position + frames);
  return GAUD_OK;
}

static GAUD_Result toy_seek(
    GAUD_Decoder * decoder, uint64_t frame, uint64_t * out_landed) {
  if (frame > gaud_track_frames(gaud_decoder_track(decoder))) {
    return GAUD_ERR_INVALID;
  }
  gaud_decoder_set_position(decoder, frame);
  *out_landed = frame;
  return GAUD_OK;
}

static const GAUD_Decoder_Vtable toy_decoder_vtable = {
    .read = toy_read,
    .seek = toy_seek,
    .close = NULL,
};

static GAUD_Result toy_decoder_open(const GAUD_Codec * codec,
    GAUD_Track * track, GAUD_Decoder ** out_decoder) {
  (void)codec;
  return gaud_decoder_create_internal(
      track, &toy_decoder_vtable, gaud_track_private(track), out_decoder);
}

static GAUD_Result toy_open(const GAUD_Codec * codec, GAUD_Stream * stream,
    const GAUD_Limits * limits, GAUD_Diagnostics * diagnostics,
    GAUD_Doc ** out_doc) {
  (void)diagnostics;
  unsigned char header[TOY_HEADER];
  if (gaud_stream_read(stream, header, sizeof(header)) != sizeof(header)) {
    return GAUD_ERR_CORRUPT;
  }
  if (memcmp(header, "TOY1", 4) != 0) {
    return GAUD_ERR_FORMAT;
  }
  uint32_t rate = rd_u32le(header + 4);
  uint16_t channels = rd_u16le(header + 8);
  if (rate == 0 || channels == 0) {
    return GAUD_ERR_CORRUPT;
  }
  if (channels > limits->max_channels || rate > limits->max_sample_rate) {
    return GAUD_ERR_LIMIT;
  }

  uint64_t size = 0;
  if (gaud_stream_size(stream, &size) != GAUD_OK) {
    return GAUD_ERR_UNSUPPORTED;
  }
  size_t frame_size = gaud_frame_size(GAUD_SAMPLE_S16, channels);
  if (frame_size == 0) {
    return GAUD_ERR_CORRUPT;
  }
  uint64_t frames = (size - TOY_HEADER) / frame_size;

  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);
  GAUD_Doc * doc = NULL;
  GAUD_Result result
      = gaud_doc_create_internal(codec, stream, allocator, &doc);
  if (result != GAUD_OK) {
    return result;
  }
  Toy_State * state = malloc(sizeof(Toy_State));
  if (!state) {
    gaud_doc_destroy(doc);
    return GAUD_ERR_OOM;
  }
  state->data_offset = TOY_HEADER;
  state->frame_size = frame_size;

  GAUD_Track_Desc desc = {
      .format = GAUD_SAMPLE_S16,
      .sample_rate = rate,
      .layout = gaud_channel_layout_default(channels),
      .sample_layout = GAUD_LAYOUT_INTERLEAVED,
      .frames = frames,
      .trim = {0, 0, false},
      .data_offset = TOY_HEADER,
      .data_length = frames * frame_size,
      .codec_private = state,
  };
  result = gaud_doc_add_track(doc, &desc, NULL);
  if (result != GAUD_OK) {
    free(state);
    gaud_doc_destroy(doc);
    return result;
  }
  *out_doc = doc;
  return GAUD_OK;
}

static void toy_close(const GAUD_Codec * codec, GAUD_Doc * doc) {
  (void)codec;
  GAUD_Track * track = gaud_doc_track(doc, 0);
  if (track) {
    free(gaud_track_private(track));
  }
}

static const unsigned char toy_magic[] = {'T', 'O', 'Y', '1'};
static const GAUD_Codec_Magic toy_magics[] = {{0, toy_magic, 4}};

/* A file-scope constant, because the registry borrows rather than copies
 * and this has to outlive it. A stack temporary would not. */
static const GAUD_Codec toy_codec = {
    .abi_version = GAUD_CODEC_ABI_VERSION,
    /* The codec's own compiler's idea of the size, never a literal. This is
     * what lets this object keep working when the struct grows. */
    .size = sizeof(GAUD_Codec),
    .name = "toy",
    .ctx = NULL,
    .capabilities = GAUD_CAP_DECODE,
    .encoder_tier = GAUD_ENCODER_NONE,
    .magics = toy_magics,
    .magic_count = 1,
    .probe = NULL,
    .open = toy_open,
    .close = toy_close,
    .decoder_open = toy_decoder_open,
    .encoder_open = NULL,
};

/* Both registration paths, as documentation/writing-a-codec.md requires of
 * any codec that wants to work whether it is linked shared or static. */
GAUD_INIT_FUNCTION(toy_register_ctor) {
  gaud_registry_register(NULL, &toy_codec);
}

void toy_register(void);
void toy_register(void) {
  gaud_registry_register(NULL, &toy_codec);
}
