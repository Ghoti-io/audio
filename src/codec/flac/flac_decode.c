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
 * The pull decoder over a FLAC stream.
 *
 * A FLAC frame holds a whole block - up to 65,535 sample frames - and the
 * caller's buffer is whatever size the caller chose, so the two do not
 * line up and something has to hold the remainder. That is what makes this
 * file longer than the arithmetic suggests: a decoded frame is kept and
 * drained across as many `read` calls as it takes, and a `seek` in the
 * middle of one throws the remainder away.
 *
 * **The frame is read into memory before it is decoded, and its length is
 * not known until it has been.** A FLAC frame states no size anywhere -
 * not in its header, not in STREAMINFO beyond a maximum - so the only way
 * to find its end is to decode it and see where the CRC lands. The reader
 * therefore grows a window until the decode stops answering
 * ::GAUD_ERR_IO, which is exactly the "read more" signal
 * gaud_flac_frame_decode() returns for a short buffer.
 */

#include "../shared/bytes.h"
#include "flac_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/**
 * How much of a frame to read before trying to decode it.
 *
 * STREAMINFO's maximum frame size is the right answer when the file states
 * one, and most do. When it does not, this is the first guess and the
 * window doubles from there - so a file with no stated maximum costs one
 * extra read per frame at worst, rather than being refused.
 */
#define FLAC_WINDOW_FIRST 8192u

/**
 * The most a single frame may occupy.
 *
 * Derived rather than chosen: eight channels of 65,535 samples at 32 bits
 * cannot exceed this even coded verbatim, with room over for the headers.
 * It is here so that a corrupt stream cannot make the window grow without
 * bound while the decoder looks for a CRC that will never match.
 */
#define FLAC_FRAME_CEILING (1u << 24)

/** What one decoder holds between calls. */
typedef struct {
  FLAC_File * file;      ///< Borrowed from the document.
  GAUD_Stream * stream;  ///< Borrowed.
  FLAC_Frame frame;      ///< The frame being drained, if any.
  uint32_t frame_used;   ///< How many of its sample frames are gone.
  bool frame_live;       ///< Whether @p frame holds anything.
  uint64_t next_offset;  ///< Where the next frame starts in the stream.
  uint64_t position;     ///< The sample number of @p frame's first sample.
  unsigned char * window; ///< Scratch for one frame's bytes. Owned.
  size_t window_size;    ///< How large @p window is.
  const GAUD_Allocator * allocator; ///< Everything here comes from it.
} FLAC_Decoder;

static void decoder_close(GAUD_Decoder * decoder) {
  FLAC_Decoder * state = gaud_decoder_private(decoder);
  if (!state) {
    return;
  }
  gaud_flac_frame_free(&state->frame);
  gcu_allocator_free(state->allocator, state->window);
  gcu_allocator_free(state->allocator, state);
}

/** Make sure the scratch window holds at least @p want bytes. */
static GAUD_Result reserve_window(FLAC_Decoder * state, size_t want) {
  if (state->window_size >= want) {
    return GAUD_OK;
  }
  unsigned char * grown = gcu_allocator_malloc(state->allocator, want);
  if (!grown) {
    return GAUD_ERR_OOM;
  }
  gcu_allocator_free(state->allocator, state->window);
  state->window = grown;
  state->window_size = want;
  return GAUD_OK;
}

/**
 * Decode the frame at @p state->next_offset into @p state->frame.
 *
 * @return ::GAUD_OK, or ::GAUD_ERR_FORMAT at a clean end of stream - which
 *   is how the caller learns there is no next frame, and is distinct from
 *   ::GAUD_ERR_CORRUPT for a frame that is there and wrong.
 */
static GAUD_Result decode_next(FLAC_Decoder * state) {
  size_t want = state->file->info.max_frame_size
      ? state->file->info.max_frame_size
      : FLAC_WINDOW_FIRST;
  if (want < 64u) {
    want = 64u;
  }

  for (;;) {
    GAUD_Result result = reserve_window(state, want);
    if (result != GAUD_OK) {
      return result;
    }
    if (gaud_stream_seek(state->stream, (int64_t)state->next_offset,
            GAUD_SEEK_SET)
        != GAUD_OK) {
      return GAUD_ERR_IO;
    }
    size_t got = gaud_stream_read(state->stream, state->window, want);
    if (got == 0) {
      return GAUD_ERR_FORMAT; /* A clean end of stream. */
    }
    size_t used = 0;
    result = gaud_flac_frame_decode(
        state->window, got, &state->file->info, &state->frame, &used);
    if (result == GAUD_OK) {
      state->next_offset += used;
      state->frame_used = 0;
      state->frame_live = true;
      return GAUD_OK;
    }
    if (result != GAUD_ERR_IO) {
      return result;
    }
    if (got < want) {
      /* The stream ended inside what should have been a frame. */
      return GAUD_ERR_CORRUPT;
    }
    if (want >= FLAC_FRAME_CEILING) {
      return GAUD_ERR_CORRUPT;
    }
    want *= 2u;
    if (want > FLAC_FRAME_CEILING) {
      want = FLAC_FRAME_CEILING;
    }
  }
}

void gaud_flac_emit(const FLAC_Frame * frame, uint32_t from,
    GAUD_Buffer * buffer, size_t at_frame, uint32_t count) {
  uint32_t channels = frame->channels;
  GAUD_Sample_Format format = gaud_buffer_format(buffer);
  unsigned char * data = gaud_buffer_data(buffer);
  size_t frame_size = gaud_buffer_frame_size(buffer);
  size_t width = frame_size / channels;
  /* Asked once for the whole block rather than once per sample: it is a
   * constant the compiler folds anyway, and hoisting it makes that true
   * whatever the optimisation level, including the -O0 the debug build
   * uses. */
  bool little = gaud_host_is_little_endian();

  for (uint32_t ch = 0; ch < channels; ++ch) {
    const int64_t * source
        = frame->samples + (size_t)ch * frame->capacity_frames + from;
    unsigned char * target = data + at_frame * frame_size + (size_t)ch * width;
    /* The switch is outside the sample loop. A decoded sample already fits
     * its stated depth, so the narrowing is a store of the low bytes and
     * nothing else - clamping here would hide a decoder defect rather than
     * correct one. */
    switch (format) {
    case GAUD_SAMPLE_S8:
      for (uint32_t i = 0; i < count; ++i) {
        target[i * frame_size] = (unsigned char)((uint64_t)source[i] & 0xFFu);
      }
      break;
    case GAUD_SAMPLE_S16:
      for (uint32_t i = 0; i < count; ++i) {
        uint16_t value = (uint16_t)(uint64_t)source[i];
        unsigned char * at = target + i * frame_size;
        if (little) {
          gaud_wr_u16le(at, value);
        }
        else {
          gaud_wr_u16be(at, value);
        }
      }
      break;
    case GAUD_SAMPLE_S24:
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t value = (uint32_t)(uint64_t)source[i];
        unsigned char * at = target + i * frame_size;
        if (little) {
          at[0] = (unsigned char)(value & 0xFFu);
          at[1] = (unsigned char)((value >> 8) & 0xFFu);
          at[2] = (unsigned char)((value >> 16) & 0xFFu);
        }
        else {
          at[0] = (unsigned char)((value >> 16) & 0xFFu);
          at[1] = (unsigned char)((value >> 8) & 0xFFu);
          at[2] = (unsigned char)(value & 0xFFu);
        }
      }
      break;
    case GAUD_SAMPLE_S32:
    default:
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t value = (uint32_t)(uint64_t)source[i];
        unsigned char * at = target + i * frame_size;
        if (little) {
          gaud_wr_u32le(at, value);
        }
        else {
          gaud_wr_u32be(at, value);
        }
      }
      break;
    }
  }
}

static GAUD_Result decoder_read(GAUD_Decoder * decoder, GAUD_Buffer * buffer) {
  FLAC_Decoder * state = gaud_decoder_private(decoder);
  size_t capacity = gaud_buffer_capacity(buffer);
  size_t filled = 0;

  while (filled < capacity) {
    if (!state->frame_live || state->frame_used >= state->frame.block_size) {
      if (state->file->frames_length != UINT64_MAX
          && state->next_offset
              >= state->file->first_frame_offset
                  + state->file->frames_length) {
        break;
      }
      GAUD_Result result = decode_next(state);
      if (result == GAUD_ERR_FORMAT) {
        break; /* End of stream. */
      }
      if (result != GAUD_OK) {
        return result;
      }
    }
    uint32_t available = state->frame.block_size - state->frame_used;
    size_t room = capacity - filled;
    uint32_t take = available < room ? available : (uint32_t)room;
    gaud_flac_emit(&state->frame, state->frame_used, buffer, filled, take);
    state->frame_used += take;
    filled += take;
    state->position += take;
  }

  /* The stated total is the authority on where the track ends. A FLAC
   * file's last frame is a whole block like any other and carries no mark
   * saying how much of it is signal, so a decoder that trusted the frames
   * alone would hand back the encoder's padding as audio. */
  uint64_t total = state->file->info.total_samples;
  if (total && state->position > total) {
    uint64_t excess = state->position - total;
    if (excess >= filled) {
      filled = 0;
    }
    else {
      filled -= (size_t)excess;
    }
    state->position = total;
    /* The frame in hand no longer lines up with the position, and the
     * seek path's "is the target already decoded" test derives the
     * frame's first sample from exactly that difference. Dropping it is
     * one store; leaving it would make a seek after the end of the track
     * land in the wrong place. */
    state->frame_live = false;
    state->frame_used = 0;
  }

  gaud_decoder_set_position(decoder, state->position);
  return gaud_buffer_set_frames(buffer, filled);
}

/**
 * Find the frame to start at for @p frame, using the seek table.
 *
 * Returns the stream offset of a frame whose first sample is at or before
 * @p frame, and writes that sample number to @p out_sample. With no seek
 * table this is the first frame, and the caller decodes forward - which is
 * correct and slow rather than wrong and fast.
 */
static uint64_t seek_target(
    const FLAC_File * file, uint64_t frame, uint64_t * out_sample) {
  uint64_t best_offset = file->first_frame_offset;
  uint64_t best_sample = 0;
  /* Linear rather than binary. A seek table has hundreds of points, a
   * seek happens at human speed, and a binary search over a table whose
   * ordering was checked at parse time is the kind of code that is wrong
   * for one input in a thousand and never noticed. */
  for (size_t i = 0; i < file->seek_count; ++i) {
    if (file->seek_points[i].sample > frame) {
      break;
    }
    best_sample = file->seek_points[i].sample;
    best_offset = file->first_frame_offset + file->seek_points[i].offset;
  }
  *out_sample = best_sample;
  return best_offset;
}

static GAUD_Result decoder_seek(
    GAUD_Decoder * decoder, uint64_t frame, uint64_t * out_landed) {
  FLAC_Decoder * state = gaud_decoder_private(decoder);
  uint64_t total = state->file->info.total_samples;
  if (total && frame > total) {
    frame = total;
  }

  uint64_t landed = 0;
  uint64_t offset = seek_target(state->file, frame, &landed);
  /* Seeking backwards within the frame already decoded would re-read it
   * for nothing, so the only state kept is the frame itself when it
   * already contains the target. */
  if (state->frame_live && state->position - state->frame_used <= frame
      && frame < state->position - state->frame_used
              + state->frame.block_size) {
    state->frame_used
        = (uint32_t)(frame - (state->position - state->frame_used));
    state->position = frame;
    gaud_decoder_set_position(decoder, frame);
    *out_landed = frame;
    return GAUD_OK;
  }

  state->next_offset = offset;
  state->position = landed;
  state->frame_live = false;
  state->frame_used = 0;

  /* Decode forward to the target. The seek table's granularity is the
   * encoder's choice and is usually a second or so, so this is bounded by
   * that rather than by the file. */
  while (state->position < frame) {
    GAUD_Result result = decode_next(state);
    if (result == GAUD_ERR_FORMAT) {
      break;
    }
    if (result != GAUD_OK) {
      return result;
    }
    uint64_t block = state->frame.block_size;
    if (state->position + block > frame) {
      state->frame_used = (uint32_t)(frame - state->position);
      state->position = frame;
      break;
    }
    state->position += block;
    state->frame_live = false;
  }

  gaud_decoder_set_position(decoder, state->position);
  *out_landed = state->position;
  return GAUD_OK;
}

static const GAUD_Decoder_Vtable flac_decoder_vtable = {
    .read = decoder_read,
    .seek = decoder_seek,
    .close = decoder_close,
};

/** @brief ::GAUD_Codec::decoder_open: a pull decoder over this track. */
GAUD_Result gaud_flac_decoder_open(
    const GAUD_Codec * codec, GAUD_Track * track, GAUD_Decoder ** out_decoder) {
  (void)codec;
  FLAC_File * file = gaud_track_private(track);
  if (!file) {
    return GAUD_ERR_INTERNAL;
  }
  GAUD_Doc * doc = gaud_track_doc(track);
  GAUD_Stream * stream = gaud_doc_stream(doc);
  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);

  FLAC_Decoder * state = gcu_allocator_malloc(allocator, sizeof(*state));
  if (!state) {
    return GAUD_ERR_OOM;
  }
  memset(state, 0, sizeof(*state));
  state->file = file;
  state->stream = stream;
  state->allocator = allocator;
  state->frame.allocator = allocator;
  state->next_offset = file->first_frame_offset;

  GAUD_Result result = gaud_decoder_create_internal(
      track, &flac_decoder_vtable, state, out_decoder);
  if (result != GAUD_OK) {
    gcu_allocator_free(allocator, state);
  }
  return result;
}
