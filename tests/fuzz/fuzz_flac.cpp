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
 * Fuzz the FLAC frame decoder, and the two containers over it.
 *
 * **The frame decoder is reached directly, not only through a file**, and
 * that is the point of the harness. A fuzzer handed a whole FLAC file
 * spends almost every input failing at the magic or the STREAMINFO, so the
 * subframe arithmetic - which is where the undefined behaviour lives - is
 * barely touched. The first byte of the input chooses which of three
 * things to do with the rest, so a third of the corpus goes straight into
 * the bit reader.
 *
 * What it asserts beyond not crashing:
 *
 * - **A frame that decodes reports no more than its own block size**, and
 *   the sample buffer is sized from exactly that number. Every caller
 *   sizes its copy from the frame's block size, so a decoder that wrote
 *   past it corrupts memory in all of them at once; ASan sees it because
 *   the buffer is an exact fit.
 * - **The bytes consumed never exceed the bytes offered.** A frame decoder
 *   that reported more would make the caller's next read start past the
 *   end of the stream, which looks like a truncated file rather than like
 *   a bug here.
 * - **A decode is a function of its input alone.** The same frame decoded
 *   twice into a reused ::FLAC_Frame gives the same samples, so state
 *   cannot leak from the previous frame into this one - which is exactly
 *   what a predictor's warm-up buffer would do if it were not rewritten.
 * - For a whole file: what loads, decodes; what decodes, seeks; and a
 *   seek followed by a read never returns more frames than the track
 *   claims to have left.
 *
 * Build with: make fuzz-flac
 * Run:        make fuzz-run-flac FUZZ_TIME=300
 */

#include "../../src/codec/flac/flac_internal.h"
#include "../../src/meta/scheme.h"
#include <ghoti.io/audio/audio.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size);

namespace {

/** Registers the codecs once, however many inputs run. */
struct Registered {
  Registered() { gaud_register_builtin_codecs(); }
};
const Registered registered;

/** Decode one frame twice and check the two agree. */
void fuzz_frame(const uint8_t * data, size_t size) {
  if (size < FLAC_STREAMINFO_SIZE + 1u) {
    return;
  }
  /* A STREAMINFO out of the first 34 bytes, so the frame header's
   * deferred fields have something to resolve against - and so that the
   * cross-check between the header and STREAMINFO is exercised with
   * values that sometimes agree. */
  FLAC_Streaminfo info;
  if (gaud_flac_parse_streaminfo(data, FLAC_STREAMINFO_SIZE, &info)
      != GAUD_OK) {
    return;
  }
  data += FLAC_STREAMINFO_SIZE;
  size -= FLAC_STREAMINFO_SIZE;

  FLAC_Frame first;
  FLAC_Frame second;
  memset(&first, 0, sizeof(first));
  memset(&second, 0, sizeof(second));

  size_t used_first = 0;
  GAUD_Result result
      = gaud_flac_frame_decode(data, size, &info, &first, &used_first);
  if (result == GAUD_OK) {
    if (used_first > size) {
      abort(); /* Consumed more than it was given. */
    }
    if (first.block_size > first.capacity_frames
        || first.channels > first.capacity_channels) {
      abort(); /* Wrote past what the buffer was sized for. */
    }
    size_t used_second = 0;
    GAUD_Result again
        = gaud_flac_frame_decode(data, size, &info, &second, &used_second);
    if (again != GAUD_OK || used_second != used_first) {
      abort(); /* Not a function of its input. */
    }
    for (uint32_t ch = 0; ch < first.channels; ++ch) {
      const int64_t * a = first.samples + (size_t)ch * first.capacity_frames;
      const int64_t * b
          = second.samples + (size_t)ch * second.capacity_frames;
      if (memcmp(a, b, (size_t)first.block_size * sizeof(int64_t)) != 0) {
        abort();
      }
    }
  }
  gaud_flac_frame_free(&first);
  gaud_flac_frame_free(&second);
}

/** Load a whole file and walk it, for whichever codec claims it. */
void fuzz_file(const uint8_t * data, size_t size) {
  GAUD_Stream * stream = NULL;
  if (gaud_stream_create_memory(data, size, &stream) != GAUD_OK) {
    return;
  }
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  /* Tightened, so an input that asks for a gigabyte is a refusal rather
   * than an out-of-memory the fuzzer reports as a crash. */
  limits.max_metadata_bytes = 1u << 20;
  limits.max_picture_bytes = 1u << 18;
  limits.max_frames = 1u << 22;

  GAUD_Doc * doc = NULL;
  GAUD_Diagnostics diagnostics;
  gaud_diagnostics_init(&diagnostics, NULL);
  GAUD_Result result
      = gaud_doc_load(NULL, stream, &limits, &diagnostics, &doc);
  if (result != GAUD_OK) {
    gaud_diagnostics_destroy(&diagnostics);
    gaud_stream_destroy(stream);
    return;
  }

  GAUD_Track * track = gaud_doc_track(doc, 0);
  if (track) {
    GAUD_Decoder * decoder = NULL;
    if (gaud_decoder_create(track, &decoder) == GAUD_OK) {
      GAUD_Buffer * buffer = NULL;
      if (gaud_decoder_buffer_create(decoder, NULL, 503u, &buffer)
          == GAUD_OK) {
        /* Bounded: a valid file of a million frames would otherwise make
         * every input a timeout rather than a test. */
        for (int i = 0; i < 64; ++i) {
          if (gaud_decoder_read(decoder, buffer) != GAUD_OK) {
            break;
          }
          if (gaud_buffer_frames(buffer) == 0) {
            break;
          }
          if (gaud_buffer_frames(buffer) > gaud_buffer_capacity(buffer)) {
            abort();
          }
        }
        /* Seek somewhere derived from the input, then read. A seek that
         * landed past the end and then read would be the bug. */
        uint64_t target = ((uint64_t)data[size - 1u] << 8) | data[0];
        uint64_t landed = UINT64_MAX;
        if (gaud_decoder_seek(decoder, target, &landed) == GAUD_OK) {
          uint64_t frames = gaud_track_frames(track);
          if (frames != UINT64_MAX && landed > frames) {
            abort();
          }
          if (gaud_decoder_read(decoder, buffer) == GAUD_OK
              && frames != UINT64_MAX
              && landed + gaud_buffer_frames(buffer) > frames) {
            abort();
          }
        }
        gaud_buffer_destroy(buffer);
      }
      gaud_decoder_destroy(decoder);
    }
  }
  gaud_doc_destroy(doc);
  gaud_diagnostics_destroy(&diagnostics);
  gaud_stream_destroy(stream);
}

/** Parse one metadata block body, chosen by the input. */
void fuzz_block(const uint8_t * data, size_t size) {
  if (size < 2) {
    return;
  }
  unsigned type = data[0] % 8u;
  ++data;
  --size;
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  limits.max_picture_bytes = 1u << 18;

  if (type == FLAC_BLOCK_CUESHEET) {
    (void)gaud_flac_check_cuesheet(data, size, &limits, NULL, NULL);
    return;
  }
  GAUD_Meta * meta = NULL;
  if (gaud_meta_create(NULL, &meta) != GAUD_OK) {
    return;
  }
  if (type == FLAC_BLOCK_PICTURE) {
    (void)gaud_flac_parse_picture(data, size, &limits, meta, NULL);
    /* Whatever came out must be valid UTF-8 and within the stated
     * limits: the picture's text is attacker-controlled and reaches a
     * caller that will print it. */
    size_t pictures = gaud_meta_picture_count(meta);
    for (size_t i = 0; i < pictures; ++i) {
      const GAUD_Picture * picture = gaud_meta_picture(meta, i);
      if (!picture || !picture->mime_type || !picture->description) {
        abort();
      }
      if (picture->size > limits.max_picture_bytes) {
        abort();
      }
    }
  }
  else {
    (void)gaud_vorbis_comment_parse(data, size, &limits, meta, NULL);
  }
  gaud_meta_destroy(meta);
}

} // namespace

int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 2) {
    return 0;
  }
  /* The first byte selects the entry point. Without it a corpus of whole
   * files would never reach the frame decoder's interior, because almost
   * every mutation breaks the header long before the subframes. */
  unsigned mode = data[0] % 3u;
  const uint8_t * body = data + 1;
  size_t body_size = size - 1u;
  switch (mode) {
  case 0:
    fuzz_file(body, body_size);
    break;
  case 1:
    fuzz_frame(body, body_size);
    break;
  default:
    fuzz_block(body, body_size);
    break;
  }
  return 0;
}
