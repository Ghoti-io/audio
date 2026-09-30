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
 * Writing WAV.
 *
 * The header states how much data follows, and that is not known until the
 * data has been written - so the header goes out with zeroes in those two
 * fields and gaud_encoder_finish() seeks back and patches them. That is why
 * an encoder needs a seekable sink and says so at creation rather than
 * producing a file with a wrong length in it.
 *
 * `WAVE_FORMAT_EXTENSIBLE` is written when, and only when, there is
 * something it can say that the plain header cannot: more than two channels,
 * or a stated channel mask. A file that does not need it is written in the
 * plain form, because that is the one every reader accepts.
 */

#include "../shared/bytes.h"
#include "wav_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/** @brief What the WAV encoder must remember between calls. */
typedef struct {
  uint64_t data_size_offset; ///< Where the data chunk's length field is.
  uint64_t riff_size_offset; ///< Where the RIFF length field is.
  uint64_t data_bytes;       ///< How many written so far.
  size_t frame_size;         ///< Bytes per frame.
  bool needs_swap;           ///< Whether the host's order is not the file's.
} WAV_Encoder_State;

static GAUD_Result put(GAUD_Encoder * encoder, const void * bytes,
    size_t count) {
  return gaud_stream_write(gaud_encoder_stream(encoder), bytes, count);
}

static GAUD_Result wav_write(
    GAUD_Encoder * encoder, const GAUD_Buffer * buffer) {
  WAV_Encoder_State * state = gaud_encoder_private(encoder);
  size_t frames = gaud_buffer_frames(buffer);
  if (frames == 0) {
    return GAUD_OK;
  }
  size_t bytes = frames * state->frame_size;

  const unsigned char * data = gaud_buffer_data_const(buffer);
  if (!state->needs_swap) {
    GAUD_Result result = put(encoder, data, bytes);
    if (result == GAUD_OK) {
      state->data_bytes += bytes;
      gaud_encoder_add_frames(encoder, frames);
    }
    return result;
  }

  /* A big-endian host has to byte-swap on the way out, and the caller's
   * buffer is const - so the swap happens in a scratch copy rather than in
   * their memory. Chunked, so that writing an hour of audio does not
   * allocate an hour of audio. */
  unsigned char scratch[4096];
  size_t width = gaud_sample_format_bits(gaud_buffer_format(buffer)) / 8u;
  size_t done = 0;
  while (done < bytes) {
    size_t chunk = bytes - done;
    if (chunk > sizeof(scratch)) {
      chunk = sizeof(scratch);
      /* Never split a sample across two chunks, or the swap would reverse
       * two halves of different samples. */
      chunk -= chunk % width;
    }
    memcpy(scratch, data + done, chunk);
    gaud_swap_samples(scratch, chunk / width, width);
    GAUD_Result result = put(encoder, scratch, chunk);
    if (result != GAUD_OK) {
      return result;
    }
    done += chunk;
  }
  state->data_bytes += bytes;
  gaud_encoder_add_frames(encoder, frames);
  return GAUD_OK;
}

/* Patch a 32-bit little-endian value already written at `offset`. */
static GAUD_Result patch_u32(
    GAUD_Encoder * encoder, uint64_t offset, uint32_t value) {
  GAUD_Stream * stream = gaud_encoder_stream(encoder);
  uint64_t here = gaud_stream_tell(stream);
  if (gaud_stream_seek(stream, (int64_t)offset, GAUD_SEEK_SET) != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  unsigned char bytes[4];
  gaud_wr_u32le(bytes, value);
  GAUD_Result result = gaud_stream_write(stream, bytes, sizeof(bytes));
  if (result != GAUD_OK) {
    return result;
  }
  if (gaud_stream_seek(stream, (int64_t)here, GAUD_SEEK_SET) != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  return GAUD_OK;
}

static GAUD_Result wav_finish(GAUD_Encoder * encoder) {
  WAV_Encoder_State * state = gaud_encoder_private(encoder);
  GAUD_Stream * stream = gaud_encoder_stream(encoder);

  /* RIFF chunks are padded to an even length. The pad byte is not counted
   * in the chunk's own size and is counted in the RIFF size, which is the
   * detail a writer gets wrong and no reader complains about until one
   * does. */
  if (state->data_bytes & 1u) {
    const unsigned char pad = 0;
    GAUD_Result result = gaud_stream_write(stream, &pad, 1);
    if (result != GAUD_OK) {
      return result;
    }
  }

  uint64_t total = gaud_stream_tell(stream);
  if (state->data_bytes > 0xFFFFFFFFu || total < 8) {
    /* Past 4 GiB this would have to be RF64, which this writer does not
     * emit. Refusing is the honest answer; writing a truncated 32-bit
     * length would produce a file that opens and is wrong. */
    return GAUD_ERR_UNSUPPORTED;
  }

  GAUD_Result result
      = patch_u32(encoder, state->data_size_offset, (uint32_t)state->data_bytes);
  if (result != GAUD_OK) {
    return result;
  }
  /* The RIFF size covers everything after its own 8-byte header. */
  return patch_u32(encoder, state->riff_size_offset, (uint32_t)(total - 8));
}

static void wav_close(GAUD_Encoder * encoder) {
  gcu_allocator_free(
      gaud_encoder_allocator(encoder), gaud_encoder_private(encoder));
}

static const GAUD_Encoder_Vtable wav_encoder_vtable = {
    .write = wav_write,
    .finish = wav_finish,
    .close = wav_close,
};

/** @brief Write a WAVE header and open an encoder over the rest. */
GAUD_Result gaud_wav_encoder_open(const GAUD_Codec * codec,
    GAUD_Stream * stream, const GAUD_Encode_Params * params,
    GAUD_Encoder ** out_encoder) {
  (void)codec;
  if (!gaud_stream_seekable(stream)) {
    /* Stated at creation rather than discovered at finish, so a caller
     * cannot get halfway through writing an hour of audio before finding
     * out. */
    return GAUD_ERR_UNSUPPORTED;
  }

  uint16_t tag;
  switch (params->format) {
  case GAUD_SAMPLE_U8:
  case GAUD_SAMPLE_S16:
  case GAUD_SAMPLE_S24:
  case GAUD_SAMPLE_S32: tag = WAV_FORMAT_PCM; break;
  case GAUD_SAMPLE_F32:
  case GAUD_SAMPLE_F64: tag = WAV_FORMAT_IEEE_FLOAT; break;
  default:
    /* s8 has no WAV spelling: the format's 8-bit is unsigned, full stop.
     * Refusing by name beats writing it as u8 and moving every sample by
     * 128 without saying so. */
    return GAUD_ERR_UNSUPPORTED;
  }

  size_t frame_size = gaud_frame_size(params->format, params->layout.channels);
  if (frame_size == 0) {
    return GAUD_ERR_INVALID;
  }
  uint16_t bits = (uint16_t)gaud_sample_format_bits(params->format);
  uint32_t channels = params->layout.channels;
  if (channels > 0xFFFFu) {
    return GAUD_ERR_INVALID;
  }

  /* Extensible only when it earns its place: more than two channels, or a
   * mask worth stating. Every reader accepts the plain form and some old
   * ones do not accept the other. */
  bool extensible = channels > 2 || params->layout.mask != 0;
  uint32_t fmt_size = extensible ? 40u : 16u;

  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);
  WAV_Encoder_State * state
      = gcu_allocator_malloc(allocator, sizeof(WAV_Encoder_State));
  if (!state) {
    return GAUD_ERR_OOM;
  }

  unsigned char header[12 + 8 + 40 + 8];
  size_t n = 0;
  memcpy(header + n, "RIFF", 4);
  n += 4;
  gaud_wr_u32le(header + n, 0); /* patched by finish */
  uint64_t riff_size_offset = n;
  n += 4;
  memcpy(header + n, "WAVE", 4);
  n += 4;

  memcpy(header + n, "fmt ", 4);
  n += 4;
  gaud_wr_u32le(header + n, fmt_size);
  n += 4;
  gaud_wr_u16le(header + n, extensible ? WAV_FORMAT_EXTENSIBLE : tag);
  n += 2;
  gaud_wr_u16le(header + n, (uint16_t)channels);
  n += 2;
  gaud_wr_u32le(header + n, params->sample_rate);
  n += 4;
  /* Bytes per second and block align: derived, and derived here rather than
   * by the caller, because a reader that trusts them and a writer that got
   * them wrong is a file that plays at the wrong speed. */
  gaud_wr_u32le(header + n, (uint32_t)(params->sample_rate * frame_size));
  n += 4;
  gaud_wr_u16le(header + n, (uint16_t)frame_size);
  n += 2;
  gaud_wr_u16le(header + n, bits);
  n += 2;

  if (extensible) {
    gaud_wr_u16le(header + n, 22); /* cbSize */
    n += 2;
    gaud_wr_u16le(header + n, bits); /* wValidBitsPerSample */
    n += 2;
    gaud_wr_u32le(header + n, params->layout.mask);
    n += 4;
    gaud_wr_u16le(header + n, tag);
    n += 2;
    static const unsigned char guid_suffix[14] = {0x00, 0x00, 0x00, 0x00,
        0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    memcpy(header + n, guid_suffix, sizeof(guid_suffix));
    n += sizeof(guid_suffix);
  }

  memcpy(header + n, "data", 4);
  n += 4;
  uint64_t data_size_offset = n;
  gaud_wr_u32le(header + n, 0); /* patched by finish */
  n += 4;

  GAUD_Result result = gaud_stream_write(stream, header, n);
  if (result != GAUD_OK) {
    gcu_allocator_free(allocator, state);
    return result;
  }

  *state = (WAV_Encoder_State){
      .data_size_offset = data_size_offset,
      .riff_size_offset = riff_size_offset,
      .data_bytes = 0,
      .frame_size = frame_size,
      .needs_swap = !gaud_host_is_little_endian() && bits > 8,
  };

  result = gaud_encoder_create_internal(
      stream, params, allocator, &wav_encoder_vtable, state, out_encoder);
  if (result != GAUD_OK) {
    gcu_allocator_free(allocator, state);
  }
  return result;
}
