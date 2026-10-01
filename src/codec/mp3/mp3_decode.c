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
 * The pull decoder over an MPEG audio stream.
 *
 * Three things make this longer than the arithmetic suggests, and all
 * three are properties of the format rather than of this library:
 *
 * **A frame decodes to 1,152 samples - or 384 in Layer I - and the
 * caller's buffer is whatever size the caller chose**, so a decoded frame is
 * held and drained across as many `read` calls as it takes - the same shape as
 * the FLAC decoder's.
 *
 * **A seek has to count.** There is no index, so the only exact way to
 * find the frame holding sample N is to walk the frame headers from the
 * start, which is one four-byte read per frame. A Xing table of contents
 * would be faster and is accurate to a hundredth of the file, which is
 * not a sample - and a seek that reported a position it had not landed on
 * would be worse than a slow one.
 *
 * **A seek needs a run-up.** The bit reservoir reaches back up to 511
 * bytes, the hybrid filterbank overlaps with the previous granule, and the
 * polyphase filterbank carries 1,024 values of history. So the frames
 * before the target are decoded and thrown away; ::MP3_PREROLL says how
 * many and why.
 */

#include "mp3_internal.h"
#include "mp3_tables.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/**
 * How many frames to decode and discard before the one a seek wants.
 *
 * Six, derived rather than chosen. The reservoir can reach back 511
 * bytes, and the shortest MPEG-1 Layer III frame this library will decode
 * is 96 bytes (32 kbit/s at 48 kHz), so six frames is the most that 511
 * bytes can span. Decoding those gives the target frame a grounded
 * reservoir, a warm hybrid overlap and a warm polyphase history - the
 * three things that make a decode depend on its past.
 */
#define MP3_PREROLL 6u

/** What one decoder holds between calls. */
typedef struct {
  MP3_File * file;                  ///< Borrowed from the document.
  GAUD_Stream * stream;             ///< Borrowed.
  const GAUD_Allocator * allocator; ///< Everything here comes from it.
  MP3_Layer3 layer3;                ///< Layer III's state.
  MP3_Layer12 layer12;              ///< Layer I's and II's, which is less.
  uint64_t next_offset;             ///< Where the next frame begins.
  uint64_t position;                ///< The sample frame about to be read.
  /** The frame just decoded, interleaved, Q28. */
  int32_t pcm[1152u * 2u];
  uint32_t pcm_frames; ///< Sample frames it holds.
  uint32_t pcm_used;   ///< How many of them have been handed out.
  unsigned char frame[MP3_MAX_FRAME_SIZE]; ///< The frame's bytes.
  uint32_t resyncs; ///< Times the stream had to be searched again.
} MP3_Decoder;

/** Where the audio ends, so a walk knows to stop. */
static uint64_t audio_limit(const MP3_File * file) {
  return file->audio_offset + file->audio_length;
}

static void decoder_close(GAUD_Decoder * decoder) {
  MP3_Decoder * state = gaud_decoder_private(decoder);
  if (!state) {
    return;
  }
  gcu_allocator_free(state->allocator, state);
}

/** Read @p want bytes at @p offset, answering how many arrived. */
static size_t read_at(
    GAUD_Stream * stream, uint64_t offset, unsigned char * into, size_t want) {
  if (gaud_stream_seek(stream, (int64_t)offset, GAUD_SEEK_SET) != GAUD_OK) {
    return 0;
  }
  return gaud_stream_read(stream, into, want);
}

/**
 * Put the next frame of this stream in @p state->frame.
 *
 * @return ::GAUD_ERR_FORMAT at a clean end of the audio, which is how the
 *   caller learns there is no next frame and is distinct from
 *   ::GAUD_ERR_CORRUPT for a frame that is there and wrong.
 */
static GAUD_Result next_frame(MP3_Decoder * state, MP3_Header * out_header) {
  uint64_t limit = audio_limit(state->file);
  if (state->next_offset + MP3_HEADER_SIZE > limit) {
    return GAUD_ERR_FORMAT;
  }
  unsigned char header_bytes[MP3_HEADER_SIZE];
  if (read_at(
          state->stream, state->next_offset, header_bytes, sizeof(header_bytes))
      != sizeof(header_bytes)) {
    return GAUD_ERR_FORMAT;
  }
  MP3_Header header;
  bool usable = gaud_mp3_header_parse(header_bytes, &header)
      && gaud_mp3_headers_compatible(&state->file->first, &header)
      && header.frame_size != 0u;
  if (!usable) {
    /* Something that is not a frame of this stream: a tag the loader did
     * not know to exclude, a splice, or damage. The search is the same
     * one the loader uses, so the same two-frame confirmation applies -
     * a decoder that resynchronised on the first sync word it saw would
     * turn a damaged byte into a burst of noise. */
    uint64_t found = 0;
    unsigned chain = 0;
    if (gaud_mp3_find_frame(state->stream, state->next_offset, MP3_SYNC_SEARCH,
            &found, &header, &chain)
            != GAUD_OK
        || !gaud_mp3_headers_compatible(&state->file->first, &header)
        || header.frame_size == 0u) {
      return GAUD_ERR_FORMAT;
    }
    ++state->resyncs;
    MP3_TRACE(MP3_T_RESYNC);
    state->next_offset = found;
  }
  if (state->next_offset + header.frame_size > limit) {
    /* The last frame is cut short. Not decoded: a partial frame decodes
     * to partial nonsense, and the loader has already excluded it from
     * the frame count for the same reason. */
    return GAUD_ERR_FORMAT;
  }
  if (read_at(
          state->stream, state->next_offset, state->frame, header.frame_size)
      != header.frame_size) {
    return GAUD_ERR_FORMAT;
  }
  *out_header = header;
  return GAUD_OK;
}

/** Decode the next frame into @p state->pcm. */
static GAUD_Result decode_next(MP3_Decoder * state) {
  MP3_Header header;
  GAUD_Result result = next_frame(state, &header);
  if (result != GAUD_OK) {
    return result;
  }
  state->pcm_frames = 0;
  state->pcm_used = 0;
  result = header.layer == 3u
      ? gaud_mp3_layer3_frame(&state->layer3, &header, state->frame,
            header.frame_size, state->pcm)
      : gaud_mp3_layer12_frame(&state->layer12, &header, state->frame,
            header.frame_size, state->pcm);
  if (result != GAUD_OK) {
    return result;
  }
  state->pcm_frames = header.samples;
  state->next_offset += header.frame_size;
  return GAUD_OK;
}

/** Q28 to signed 16-bit, rounded to nearest and clipped. */
static int16_t to_s16(int32_t value) {
  /* The rounding term is added in 64 bits because `value` reaches the
   * ends of its own range: the decoder saturates there rather than
   * wrapping, so INT32_MAX is a value this sees, and INT32_MAX plus the
   * rounding term is undefined in 32. */
  int64_t scaled = ((int64_t)value + (1 << (MP3_Q - 16))) >> (MP3_Q - 15);
  if (scaled > 32767) {
    return 32767;
  }
  if (scaled < -32768) {
    return -32768;
  }
  return (int16_t)scaled;
}

static GAUD_Result decoder_read(GAUD_Decoder * decoder, GAUD_Buffer * buffer) {
  MP3_Decoder * state = gaud_decoder_private(decoder);
  size_t capacity = gaud_buffer_capacity(buffer);
  unsigned channels = state->file->first.channels;
  unsigned char * data = gaud_buffer_data(buffer);
  size_t filled = 0;

  while (filled < capacity) {
    if (state->pcm_used >= state->pcm_frames) {
      GAUD_Result result = decode_next(state);
      if (result == GAUD_ERR_FORMAT) {
        break; /* The end of the audio. */
      }
      if (result != GAUD_OK) {
        return result;
      }
      if (state->pcm_frames == 0) {
        continue;
      }
    }
    uint32_t available = state->pcm_frames - state->pcm_used;
    size_t room = capacity - filled;
    uint32_t take = available < room ? available : (uint32_t)room;
    int16_t * out
        = (int16_t *)(void *)(data + filled * channels * sizeof(int16_t));
    const int32_t * from = state->pcm + (size_t)state->pcm_used * channels;
    for (size_t i = 0; i < (size_t)take * channels; ++i) {
      out[i] = to_s16(from[i]);
    }
    state->pcm_used += take;
    filled += take;
    state->position += take;
  }

  /* The stated total is the authority on where the track ends, as it is
   * for FLAC: an MPEG frame is 1,152 samples whether or not the recording
   * fills it, and the loader's frame count already accounts for that. */
  uint64_t total = gaud_track_frames(gaud_decoder_track(decoder));
  if (total != UINT64_MAX && state->position > total) {
    uint64_t excess = state->position - total;
    filled = excess >= filled ? 0 : filled - (size_t)excess;
    state->position = total;
  }
  gaud_decoder_set_position(decoder, state->position);
  return gaud_buffer_set_frames(buffer, filled);
}

static GAUD_Result decoder_seek(
    GAUD_Decoder * decoder, uint64_t frame, uint64_t * out_landed) {
  MP3_Decoder * state = gaud_decoder_private(decoder);
  uint64_t total = gaud_track_frames(gaud_decoder_track(decoder));
  if (total != UINT64_MAX && frame > total) {
    frame = total;
  }
  uint32_t samples = state->file->first.samples;
  uint64_t target = frame / samples;

  /* Count frames from the start. There is no index in the format and a
   * Xing table of contents resolves to a hundredth of the file, so this
   * is the only way to land on a stated sample rather than near one. One
   * four-byte read per frame, which is milliseconds for a track. */
  uint64_t start = target > MP3_PREROLL ? target - MP3_PREROLL : 0u;
  uint64_t at = state->file->audio_offset;
  uint64_t limit = audio_limit(state->file);
  uint64_t index = 0;
  while (index < start) {
    unsigned char bytes[MP3_HEADER_SIZE];
    if (at + MP3_HEADER_SIZE > limit
        || read_at(state->stream, at, bytes, sizeof(bytes)) != sizeof(bytes)) {
      break;
    }
    MP3_Header header;
    if (!gaud_mp3_header_parse(bytes, &header)
        || !gaud_mp3_headers_compatible(&state->file->first, &header)
        || header.frame_size == 0u || at + header.frame_size > limit) {
      break;
    }
    at += header.frame_size;
    ++index;
  }

  gaud_mp3_layer3_reset(&state->layer3);
  gaud_mp3_layer12_reset(&state->layer12);
  state->next_offset = at;
  state->position = index * samples;
  state->pcm_frames = 0;
  state->pcm_used = 0;

  /* The run-up: decode the frames between where the walk stopped and the
   * frame that holds the target sample, and throw them away. */
  while (state->position + samples <= frame) {
    GAUD_Result result = decode_next(state);
    if (result == GAUD_ERR_FORMAT) {
      break;
    }
    if (result != GAUD_OK) {
      return result;
    }
    state->position += state->pcm_frames;
    state->pcm_used = state->pcm_frames;
  }
  /* And the part of the holding frame before the target. */
  if (state->position < frame) {
    GAUD_Result result = decode_next(state);
    if (result == GAUD_OK) {
      uint32_t skip = (uint32_t)(frame - state->position);
      if (skip > state->pcm_frames) {
        skip = state->pcm_frames;
      }
      state->pcm_used = skip;
      state->position += skip;
    }
  }
  gaud_decoder_set_position(decoder, state->position);
  *out_landed = state->position;
  return GAUD_OK;
}

static const GAUD_Decoder_Vtable mp3_decoder_vtable = {
    .read = decoder_read,
    .seek = decoder_seek,
    .close = decoder_close,
};

GAUD_Result gaud_mp3_decoder_open(
    const GAUD_Codec * codec, GAUD_Track * track, GAUD_Decoder ** out) {
  (void)codec;
  MP3_File * file = gaud_track_private(track);
  if (!file) {
    return GAUD_ERR_INTERNAL;
  }
  unsigned row = 0;
  if (file->first.layer == 3u && !gaud_mp3_band_row(&file->first, &row)) {
    /* No reserved version reaches this point - gaud_mp3_header_parse
     * refuses those - so this is unreachable for a header that parsed.
     * It stays because gaud_mp3_band_row() is the one place that knows
     * which rows exist, and a future rate added to the header tables
     * without a row would otherwise index past the band tables. */
    return GAUD_ERR_UNSUPPORTED;
  }

  GAUD_Doc * doc = gaud_track_doc(track);
  GAUD_Stream * stream = gaud_doc_stream(doc);
  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);
  MP3_Decoder * state = gcu_allocator_malloc(allocator, sizeof(*state));
  if (!state) {
    return GAUD_ERR_OOM;
  }
  memset(state, 0, sizeof(*state));
  state->file = file;
  state->stream = stream;
  state->allocator = allocator;
  state->next_offset = file->audio_offset;
  gaud_mp3_layer3_reset(&state->layer3);
  gaud_mp3_layer12_reset(&state->layer12);

  GAUD_Result result
      = gaud_decoder_create_internal(track, &mp3_decoder_vtable, state, out);
  if (result != GAUD_OK) {
    gcu_allocator_free(allocator, state);
  }
  return result;
}
