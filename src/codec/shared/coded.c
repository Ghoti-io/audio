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
 * The block cache, and the arithmetic that turns a frame number into a
 * block number.
 *
 * Blocks are uniform: block N starts at `data_offset + N * block_bytes`
 * and yields `block_frames` frames, except the last, which may be short.
 * That uniformity is what makes seeking O(1) and is true of all five
 * codings - the two ADPCM families because their containers say so, and
 * G.711 because a stateless coding may pick any block size and this one
 * picks a uniform one.
 */

#include "coded.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/** What one coded decoder owns. */
typedef struct {
  GAUD_Coded_Geometry geometry; ///< What to decode, and how big a block is.
  uint64_t data_offset;         ///< Where the first block starts.
  uint64_t data_length;         ///< How many bytes of blocks there are.
  int16_t * cache;        ///< One decoded block, interleaved.
  unsigned char * raw;    ///< One block's bytes, as read.
  uint64_t cached_index;  ///< Which block `cache` holds.
  size_t cached_frames;   ///< How many frames it decoded to.
  bool valid;             ///< Whether `cached_index` means anything.
} Coded_State;

static void coded_close(GAUD_Decoder * decoder) {
  Coded_State * state = gaud_decoder_private(decoder);
  if (!state) {
    return;
  }
  const GAUD_Allocator * allocator = gaud_decoder_allocator(decoder);
  gcu_allocator_free(allocator, state->cache);
  gcu_allocator_free(allocator, state->raw);
  gcu_allocator_free(allocator, state);
}

/** Make sure `cache` holds block @p index. */
static GAUD_Result ensure_block(
    GAUD_Decoder * decoder, Coded_State * state, uint64_t index) {
  if (state->valid && state->cached_index == index) {
    return GAUD_OK;
  }
  GAUD_Track * track = gaud_decoder_track(decoder);
  GAUD_Stream * stream = gaud_doc_stream(gaud_track_doc(track));

  uint64_t at = index * (uint64_t)state->geometry.block_bytes;
  if (at >= state->data_length) {
    state->valid = true;
    state->cached_index = index;
    state->cached_frames = 0;
    return GAUD_OK;
  }
  uint64_t available = state->data_length - at;
  size_t want = state->geometry.block_bytes;
  if ((uint64_t)want > available) {
    want = (size_t)available;
  }

  /* Positioned on every block rather than trusting where the last read
   * left the stream, for the reason wav_decode.c gives for PCM: two
   * decoders on one document share the stream. */
  if (gaud_stream_seek(stream, (int64_t)(state->data_offset + at),
          GAUD_SEEK_SET)
      != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  size_t got = gaud_stream_read(stream, state->raw, want);
  /* A block cut short by a truncated file decodes to fewer frames rather
   * than failing: the loader already warned about the length, and the
   * frames that are present are real. */
  state->cached_frames
      = gaud_coded_decode_block(&state->geometry, state->raw, got,
          state->cache);
  state->cached_index = index;
  state->valid = true;
  return GAUD_OK;
}

static GAUD_Result coded_read(GAUD_Decoder * decoder, GAUD_Buffer * buffer) {
  Coded_State * state = gaud_decoder_private(decoder);
  GAUD_Track * track = gaud_decoder_track(decoder);
  uint64_t position = gaud_decoder_tell(decoder);
  uint64_t total = gaud_track_frames(track);
  if (position >= total) {
    return GAUD_OK; /* End of track: zero frames, and not an error. */
  }

  uint32_t channels = state->geometry.channels;
  size_t capacity = gaud_buffer_capacity(buffer);
  int16_t * out = gaud_buffer_data(buffer);
  size_t filled = 0;

  while (filled < capacity && position + filled < total) {
    uint64_t frame = position + filled;
    uint64_t index = frame / state->geometry.block_frames;
    size_t within = (size_t)(frame % state->geometry.block_frames);

    GAUD_Result result = ensure_block(decoder, state, index);
    if (result != GAUD_OK) {
      return result;
    }
    if (within >= state->cached_frames) {
      /* The block held fewer frames than the track claimed. Stopping
       * here rather than returning zeros: the caller learns the real
       * length from the short read, which is what it would get from a
       * truncated PCM file too. */
      break;
    }

    size_t run = state->cached_frames - within;
    if (run > capacity - filled) {
      run = capacity - filled;
    }
    if ((uint64_t)run > total - frame) {
      run = (size_t)(total - frame);
    }
    memcpy(out + (size_t)filled * channels,
        state->cache + within * channels,
        run * channels * sizeof(int16_t));
    filled += run;
  }

  GAUD_Result result = gaud_buffer_set_frames(buffer, filled);
  if (result != GAUD_OK) {
    return result;
  }
  gaud_decoder_set_position(decoder, position + filled);
  return GAUD_OK;
}

static GAUD_Result coded_seek(
    GAUD_Decoder * decoder, uint64_t frame, uint64_t * out_landed) {
  GAUD_Track * track = gaud_decoder_track(decoder);
  if (frame > gaud_track_frames(track)) {
    return GAUD_ERR_INVALID;
  }
  if (!gaud_stream_seekable(gaud_doc_stream(gaud_track_doc(track)))) {
    return GAUD_ERR_UNSUPPORTED;
  }
  /* Every block of every coding here carries its own predictor and step
   * state, so the block containing `frame` can be decoded without
   * touching any earlier one and the landing is exact. `out_landed` is
   * still written, and is still the frame asked for; phase 5's bit
   * reservoir is where that stops being true. */
  gaud_decoder_set_position(decoder, frame);
  *out_landed = frame;
  return GAUD_OK;
}

static const GAUD_Decoder_Vtable coded_vtable = {
    .read = coded_read,
    .seek = coded_seek,
    .close = coded_close,
};

GAUD_Result gaud_coded_decoder_open(GAUD_Track * track,
    const GAUD_Coded_Geometry * geometry, uint64_t data_offset,
    uint64_t data_length, GAUD_Decoder ** out_decoder) {
  if (!track || !geometry || !out_decoder || geometry->channels == 0u
      || geometry->block_frames == 0u) {
    return GAUD_ERR_INVALID;
  }
  const GAUD_Allocator * allocator
      = gaud_stream_allocator(gaud_doc_stream(gaud_track_doc(track)));

  Coded_State * state = gcu_allocator_malloc(allocator, sizeof(Coded_State));
  if (!state) {
    return GAUD_ERR_OOM;
  }
  memset(state, 0, sizeof(*state));
  state->geometry = *geometry;
  state->data_offset = data_offset;
  state->data_length = data_length;

  size_t samples = (size_t)geometry->block_frames * geometry->channels;
  state->cache = gcu_allocator_malloc(allocator, samples * sizeof(int16_t));
  state->raw = gcu_allocator_malloc(allocator, geometry->block_bytes);
  if (!state->cache || !state->raw) {
    gcu_allocator_free(allocator, state->cache);
    gcu_allocator_free(allocator, state->raw);
    gcu_allocator_free(allocator, state);
    return GAUD_ERR_OOM;
  }

  GAUD_Result result
      = gaud_decoder_create_internal(track, &coded_vtable, state, out_decoder);
  if (result != GAUD_OK) {
    gcu_allocator_free(allocator, state->cache);
    gcu_allocator_free(allocator, state->raw);
    gcu_allocator_free(allocator, state);
  }
  return result;
}

/* --------------------------------------------------------------- writer */

GAUD_Result gaud_coded_writer_init(GAUD_Coded_Writer * writer,
    const GAUD_Coded_Geometry * geometry, const GAUD_Allocator * allocator) {
  if (!writer || !geometry) {
    return GAUD_ERR_INVALID;
  }
  memset(writer, 0, sizeof(*writer));
  writer->geometry = *geometry;
  size_t samples = (size_t)geometry->block_frames * geometry->channels;
  writer->pending = gcu_allocator_malloc(allocator, samples * sizeof(int16_t));
  writer->raw = gcu_allocator_malloc(allocator, geometry->block_bytes);
  if (!writer->pending || !writer->raw) {
    gaud_coded_writer_free(writer, allocator);
    return GAUD_ERR_OOM;
  }
  return GAUD_OK;
}

void gaud_coded_writer_free(
    GAUD_Coded_Writer * writer, const GAUD_Allocator * allocator) {
  if (!writer) {
    return;
  }
  gcu_allocator_free(allocator, writer->pending);
  gcu_allocator_free(allocator, writer->raw);
  writer->pending = NULL;
  writer->raw = NULL;
}

/** Encode `pending` as one block and write it. */
static GAUD_Result emit(GAUD_Coded_Writer * writer, GAUD_Stream * stream) {
  size_t used = gaud_coded_encode_block(
      &writer->geometry, writer->pending, writer->pending_frames, writer->raw);
  if (used == 0) {
    return GAUD_OK;
  }
  /* A stateless coding writes only the bytes its frames occupy; a block
   * coding writes the whole block because its header stated the length.
   * Getting this backwards gives either a file whose data chunk is longer
   * than its samples or a final block no reader can parse. */
  size_t bytes = writer->geometry.block_bytes;
  if (gaud_sample_coding_is_pcm(writer->geometry.coding)
      || writer->geometry.coding == GAUD_CODING_G711_ULAW
      || writer->geometry.coding == GAUD_CODING_G711_ALAW) {
    bytes = used * writer->geometry.channels;
  }
  GAUD_Result result = gaud_stream_write(stream, writer->raw, bytes);
  if (result != GAUD_OK) {
    return result;
  }
  writer->bytes += bytes;
  writer->pending_frames = 0;
  return GAUD_OK;
}

GAUD_Result gaud_coded_writer_push(GAUD_Coded_Writer * writer,
    GAUD_Stream * stream, const int16_t * samples, size_t frames) {
  if (!writer || !stream || (!samples && frames > 0)) {
    return GAUD_ERR_INVALID;
  }
  uint32_t channels = writer->geometry.channels;
  size_t at = 0;
  while (at < frames) {
    size_t room = writer->geometry.block_frames - writer->pending_frames;
    size_t take = frames - at < room ? frames - at : room;
    memcpy(writer->pending + writer->pending_frames * channels,
        samples + at * channels, take * channels * sizeof(int16_t));
    writer->pending_frames += take;
    writer->frames += take;
    at += take;
    if (writer->pending_frames == writer->geometry.block_frames) {
      GAUD_Result result = emit(writer, stream);
      if (result != GAUD_OK) {
        return result;
      }
    }
  }
  return GAUD_OK;
}

GAUD_Result gaud_coded_writer_flush(
    GAUD_Coded_Writer * writer, GAUD_Stream * stream) {
  if (!writer || !stream) {
    return GAUD_ERR_INVALID;
  }
  if (writer->pending_frames == 0) {
    return GAUD_OK;
  }
  return emit(writer, stream);
}
