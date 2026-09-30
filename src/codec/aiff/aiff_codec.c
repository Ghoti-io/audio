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
 * Decoding and encoding AIFF.
 *
 * The decode is WAV's with the byte order the other way up, which is the
 * point of having both: the swap is exercised on a little-endian host by
 * AIFF and on a big-endian one by WAV, so neither path is the one that only
 * runs somewhere nobody tests.
 */

#include "../shared/bytes.h"
#include "aiff_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

static GAUD_Result aiff_read(GAUD_Decoder * decoder, GAUD_Buffer * buffer) {
  GAUD_Track * track = gaud_decoder_track(decoder);
  AIFF_Track_State * state = gaud_track_private(track);
  GAUD_Stream * stream = gaud_doc_stream(gaud_track_doc(track));

  uint64_t position = gaud_decoder_tell(decoder);
  uint64_t total = gaud_track_frames(track);
  if (position >= total) {
    return GAUD_OK;
  }
  size_t want = gaud_buffer_capacity(buffer);
  uint64_t remaining = total - position;
  if ((uint64_t)want > remaining) {
    want = (size_t)remaining;
  }

  uint64_t at = state->data_offset + position * state->frame_size;
  if (gaud_stream_seek(stream, (int64_t)at, GAUD_SEEK_SET) != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  size_t got = gaud_stream_read(
      stream, gaud_buffer_data(buffer), want * state->frame_size);
  size_t frames = got / state->frame_size;

  if (state->needs_swap) {
    size_t width = gaud_sample_format_bits(gaud_buffer_format(buffer)) / 8u;
    gaud_swap_samples(gaud_buffer_data(buffer),
        frames * gaud_buffer_channels(buffer), width);
  }

  GAUD_Result result = gaud_buffer_set_frames(buffer, frames);
  if (result != GAUD_OK) {
    return result;
  }
  gaud_decoder_set_position(decoder, position + frames);
  return GAUD_OK;
}

static GAUD_Result aiff_seek(
    GAUD_Decoder * decoder, uint64_t frame, uint64_t * out_landed) {
  GAUD_Track * track = gaud_decoder_track(decoder);
  if (frame > gaud_track_frames(track)) {
    return GAUD_ERR_INVALID;
  }
  if (!gaud_stream_seekable(gaud_doc_stream(gaud_track_doc(track)))) {
    return GAUD_ERR_UNSUPPORTED;
  }
  gaud_decoder_set_position(decoder, frame);
  *out_landed = frame;
  return GAUD_OK;
}

static const GAUD_Decoder_Vtable aiff_decoder_vtable = {
    .read = aiff_read,
    .seek = aiff_seek,
    .close = NULL,
};

/** @brief Open a decoder over an AIFF track's sound chunk. */
GAUD_Result gaud_aiff_decoder_open(const GAUD_Codec * codec,
    GAUD_Track * track, GAUD_Decoder ** out_decoder) {
  (void)codec;
  if (!gaud_track_private(track)) {
    return GAUD_ERR_INTERNAL;
  }
  return gaud_decoder_create_internal(
      track, &aiff_decoder_vtable, gaud_track_private(track), out_decoder);
}

/* ------------------------------------------------------------------ write */

/** @brief What the AIFF encoder must remember between calls. */
typedef struct {
  uint64_t form_size_offset; ///< Where FORM's length field is.
  uint64_t comm_frames_offset; ///< Where COMM's frame count is.
  uint64_t ssnd_size_offset; ///< Where SSND's length field is.
  uint64_t data_bytes;       ///< Samples written.
  size_t frame_size;         ///< Bytes per frame.
  bool needs_swap;           ///< Whether the host's order is not the file's.
} AIFF_Encoder_State;

static GAUD_Result aiff_write(
    GAUD_Encoder * encoder, const GAUD_Buffer * buffer) {
  AIFF_Encoder_State * state = gaud_encoder_private(encoder);
  GAUD_Stream * stream = gaud_encoder_stream(encoder);
  size_t frames = gaud_buffer_frames(buffer);
  if (frames == 0) {
    return GAUD_OK;
  }
  size_t bytes = frames * state->frame_size;
  const unsigned char * data = gaud_buffer_data_const(buffer);

  GAUD_Result result;
  if (!state->needs_swap) {
    result = gaud_stream_write(stream, data, bytes);
  }
  else {
    unsigned char scratch[4096];
    size_t width = gaud_sample_format_bits(gaud_buffer_format(buffer)) / 8u;
    size_t done = 0;
    result = GAUD_OK;
    while (done < bytes && result == GAUD_OK) {
      size_t chunk = bytes - done;
      if (chunk > sizeof(scratch)) {
        chunk = sizeof(scratch);
        chunk -= chunk % width; /* never split a sample */
      }
      memcpy(scratch, data + done, chunk);
      gaud_swap_samples(scratch, chunk / width, width);
      result = gaud_stream_write(stream, scratch, chunk);
      done += chunk;
    }
  }
  if (result == GAUD_OK) {
    state->data_bytes += bytes;
    gaud_encoder_add_frames(encoder, frames);
  }
  return result;
}

static GAUD_Result patch_u32be(
    GAUD_Encoder * encoder, uint64_t offset, uint32_t value) {
  GAUD_Stream * stream = gaud_encoder_stream(encoder);
  uint64_t here = gaud_stream_tell(stream);
  if (gaud_stream_seek(stream, (int64_t)offset, GAUD_SEEK_SET) != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  unsigned char bytes[4];
  gaud_wr_u32be(bytes, value);
  GAUD_Result result = gaud_stream_write(stream, bytes, sizeof(bytes));
  if (result != GAUD_OK) {
    return result;
  }
  return gaud_stream_seek(stream, (int64_t)here, GAUD_SEEK_SET) == GAUD_OK
      ? GAUD_OK
      : GAUD_ERR_IO;
}

static GAUD_Result aiff_finish(GAUD_Encoder * encoder) {
  AIFF_Encoder_State * state = gaud_encoder_private(encoder);
  GAUD_Stream * stream = gaud_encoder_stream(encoder);

  if (state->data_bytes & 1u) {
    const unsigned char pad = 0;
    GAUD_Result result = gaud_stream_write(stream, &pad, 1);
    if (result != GAUD_OK) {
      return result;
    }
  }
  uint64_t total = gaud_stream_tell(stream);
  if (total > 0xFFFFFFFFu || state->data_bytes > 0xFFFFFFF0u) {
    return GAUD_ERR_UNSUPPORTED;
  }

  uint64_t frames = state->frame_size ? state->data_bytes / state->frame_size
                                      : 0;
  GAUD_Result result = patch_u32be(
      encoder, state->comm_frames_offset, (uint32_t)frames);
  if (result != GAUD_OK) {
    return result;
  }
  /* SSND's size covers its 8-byte preamble as well as the samples. */
  result = patch_u32be(
      encoder, state->ssnd_size_offset, (uint32_t)(state->data_bytes + 8));
  if (result != GAUD_OK) {
    return result;
  }
  return patch_u32be(encoder, state->form_size_offset, (uint32_t)(total - 8));
}

static void aiff_close(GAUD_Encoder * encoder) {
  gcu_allocator_free(
      gaud_encoder_allocator(encoder), gaud_encoder_private(encoder));
}

static const GAUD_Encoder_Vtable aiff_encoder_vtable = {
    .write = aiff_write,
    .finish = aiff_finish,
    .close = aiff_close,
};

/** @brief Write an AIFF or AIFF-C header and open an encoder. */
GAUD_Result gaud_aiff_encoder_open(const GAUD_Codec * codec,
    GAUD_Stream * stream, const GAUD_Encode_Params * params,
    GAUD_Encoder ** out_encoder) {
  (void)codec;
  if (!gaud_stream_seekable(stream)) {
    return GAUD_ERR_UNSUPPORTED;
  }

  /* Float samples need AIFF-C, because plain AIFF has no way to say a
   * sample is not an integer. Integers are written as plain AIFF, which
   * more readers accept. */
  bool is_float = gaud_sample_format_is_float(params->format);
  switch (params->format) {
  case GAUD_SAMPLE_S8:
  case GAUD_SAMPLE_S16:
  case GAUD_SAMPLE_S24:
  case GAUD_SAMPLE_S32:
  case GAUD_SAMPLE_F32:
  case GAUD_SAMPLE_F64: break;
  default:
    /* u8 has no AIFF spelling: this format's 8-bit is signed. Refusing
     * rather than shifting every sample by 128 unannounced. */
    return GAUD_ERR_UNSUPPORTED;
  }

  size_t frame_size = gaud_frame_size(params->format, params->layout.channels);
  if (frame_size == 0 || params->layout.channels > 0x7FFFu) {
    return GAUD_ERR_INVALID;
  }
  uint16_t bits = (uint16_t)gaud_sample_format_bits(params->format);

  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);
  AIFF_Encoder_State * state
      = gcu_allocator_malloc(allocator, sizeof(AIFF_Encoder_State));
  if (!state) {
    return GAUD_ERR_OOM;
  }

  /* The largest header this writes, counted rather than guessed:
   *
   *   FORM id 4 + size 4 + form type 4                        = 12
   *   FVER id 4 + size 4 + timestamp 4   (AIFF-C only)        = 12
   *   COMM id 4 + size 4 + channels 2 + frames 4 + bits 2
   *        + rate 10                                          = 26
   *        + compression 4 + Pascal string 1 + pad 1 (AIFF-C) =  6
   *   SSND id 4 + size 4 + offset 4 + block size 4            = 16
   *                                                      max  = 72
   *
   * It was 64, which is enough for plain AIFF and eight bytes short for
   * AIFF-C - so every float file overran this buffer. Every test passed:
   * the overrun landed in adjacent stack and the bytes written after it
   * were correct. ASan and UBSan caught it, which is what they are for.
   * The assertion keeps the number honest if another chunk is added. */
  unsigned char header[96];
  _Static_assert(sizeof(header) >= 72, "the largest AIFF-C header is 72");
  size_t n = 0;
  memcpy(header + n, "FORM", 4);
  n += 4;
  uint64_t form_size_offset = n;
  gaud_wr_u32be(header + n, 0);
  n += 4;
  memcpy(header + n, is_float ? "AIFC" : "AIFF", 4);
  n += 4;

  if (is_float) {
    /* AIFC requires a format-version chunk, and there is exactly one
     * legal value for it. */
    memcpy(header + n, "FVER", 4);
    n += 4;
    gaud_wr_u32be(header + n, 4);
    n += 4;
    gaud_wr_u32be(header + n, 0xA2805140u);
    n += 4;
  }

  memcpy(header + n, "COMM", 4);
  n += 4;
  /* AIFF-C's COMM carries the compression type and a Pascal string naming
   * it; the empty string is one zero byte, padded to even. */
  uint32_t comm_size = is_float ? 18u + 4u + 2u : 18u;
  gaud_wr_u32be(header + n, comm_size);
  n += 4;
  gaud_wr_u16be(header + n, (uint16_t)params->layout.channels);
  n += 2;
  uint64_t comm_frames_offset = n;
  gaud_wr_u32be(header + n, 0); /* patched by finish */
  n += 4;
  gaud_wr_u16be(header + n, bits);
  n += 2;
  gaud_aiff_write_extended(header + n, (double)params->sample_rate);
  n += 10;
  if (is_float) {
    memcpy(header + n, bits == 32 ? "fl32" : "fl64", 4);
    n += 4;
    header[n++] = 0; /* empty Pascal string */
    header[n++] = 0; /* pad to even */
  }

  memcpy(header + n, "SSND", 4);
  n += 4;
  uint64_t ssnd_size_offset = n;
  gaud_wr_u32be(header + n, 0); /* patched by finish */
  n += 4;
  gaud_wr_u32be(header + n, 0); /* offset */
  n += 4;
  gaud_wr_u32be(header + n, 0); /* block size */
  n += 4;

  GAUD_Result result = gaud_stream_write(stream, header, n);
  if (result != GAUD_OK) {
    gcu_allocator_free(allocator, state);
    return result;
  }

  *state = (AIFF_Encoder_State){
      .form_size_offset = form_size_offset,
      .comm_frames_offset = comm_frames_offset,
      .ssnd_size_offset = ssnd_size_offset,
      .data_bytes = 0,
      .frame_size = frame_size,
      /* AIFF is big-endian, so a little-endian host swaps. */
      .needs_swap = gaud_host_is_little_endian() && bits > 8,
  };
  result = gaud_encoder_create_internal(
      stream, params, allocator, &aiff_encoder_vtable, state, out_encoder);
  if (result != GAUD_OK) {
    gcu_allocator_free(allocator, state);
  }
  return result;
}
