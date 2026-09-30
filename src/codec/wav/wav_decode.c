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
 * Decoding WAV: copy bytes out of the data chunk, and swap them if this host
 * is not the one the format was designed for.
 *
 * "Decoding" PCM is reading. The only work is the byte order, and the only
 * machine on which that work is visible is a big-endian one - which is why
 * `make check-golden` runs the corpus in the cross container rather than
 * trusting that this loop is obviously right.
 */

#include "../shared/bytes.h"
#include "../shared/coded.h"
#include "wav_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

static GAUD_Result wav_read(GAUD_Decoder * decoder, GAUD_Buffer * buffer) {
  GAUD_Track * track = gaud_decoder_track(decoder);
  WAV_Track_State * state = gaud_track_private(track);
  GAUD_Stream * stream = gaud_doc_stream(gaud_track_doc(track));

  uint64_t position = gaud_decoder_tell(decoder);
  uint64_t total = gaud_track_frames(track);
  if (position >= total) {
    return GAUD_OK; /* End of track: zero frames, and not an error. */
  }

  size_t want = gaud_buffer_capacity(buffer);
  uint64_t remaining = total - position;
  if ((uint64_t)want > remaining) {
    want = (size_t)remaining;
  }

  /* Positioned on every read rather than relying on where the last one
   * left the stream. Two decoders on one document share the stream, and a
   * decoder that assumed it was still where it had been would read the
   * other's samples - silently, since PCM has no framing to disagree
   * with. */
  uint64_t at = state->data_offset + position * state->frame_size;
  if (gaud_stream_seek(stream, (int64_t)at, GAUD_SEEK_SET) != GAUD_OK) {
    return GAUD_ERR_IO;
  }

  size_t bytes = want * state->frame_size;
  size_t got = gaud_stream_read(stream, gaud_buffer_data(buffer), bytes);
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

static GAUD_Result wav_seek(
    GAUD_Decoder * decoder, uint64_t frame, uint64_t * out_landed) {
  GAUD_Track * track = gaud_decoder_track(decoder);
  if (frame > gaud_track_frames(track)) {
    return GAUD_ERR_INVALID;
  }
  if (!gaud_stream_seekable(gaud_doc_stream(gaud_track_doc(track)))) {
    return GAUD_ERR_UNSUPPORTED;
  }
  /* PCM has no inter-frame state, so the frame asked for is the frame
   * landed on - exactly. This is the case out_landed exists to be trivially
   * true for, so that the callers written against it keep working when a
   * codec arrives for which it is not. */
  gaud_decoder_set_position(decoder, frame);
  *out_landed = frame;
  return GAUD_OK;
}

static const GAUD_Decoder_Vtable wav_decoder_vtable = {
    .read = wav_read,
    .seek = wav_seek,
    .close = NULL,
};

/** @brief Open a decoder over a WAV track's data chunk. */
GAUD_Result gaud_wav_decoder_open(const GAUD_Codec * codec,
    GAUD_Track * track, GAUD_Decoder ** out_decoder) {
  (void)codec;
  WAV_Track_State * state = gaud_track_private(track);
  if (!state) {
    return GAUD_ERR_INTERNAL;
  }
  if (state->coding != GAUD_CODING_PCM) {
    /* Coded tracks go through the shared block decoder, which allocates
     * per-decoder state because it caches a decoded block. PCM keeps the
     * phase 1 arrangement of hanging read-only state off the track. */
    return gaud_coded_decoder_open(track, &state->geometry,
        state->data_offset, state->data_length, out_decoder);
  }
  return gaud_decoder_create_internal(
      track, &wav_decoder_vtable, state, out_decoder);
}
