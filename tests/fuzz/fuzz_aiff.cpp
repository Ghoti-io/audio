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
 * Fuzz the AIFF container: open arbitrary bytes, and decode whatever they
 * described.
 *
 * The harness does not merely check for a crash. It asserts the invariants a
 * caller is entitled to rely on, so that a wrong answer is a finding rather
 * than something only a differential would eventually notice:
 *
 * - A load that fails writes no document, so there is nothing to leak and
 *   nothing to free.
 * - A load that succeeds describes a track whose rate and channel count are
 *   non-zero and within the limits it was given. A limit is a promise; a
 *   parser returning a value past one is worse than one that refused.
 * - Every read fills at most the buffer's capacity and advances the position
 *   by exactly what it reported. A decoder that over-reported would have a
 *   caller walk off the end of its own buffer.
 * - Reading to the end and then reading again yields zero frames and stays
 *   at the end, rather than wrapping or running away.
 * - A seek that succeeds lands where it said it did.
 *
 * The options byte drives the limits, because a limit that is never varied
 * is a branch the corpus cannot reach - and the error paths behind those
 * limits are the ones only a fuzzer walks.
 *
 * Build with: make fuzz-aiff
 * Run:        make fuzz-run-aiff FUZZ_TIME=300
 */

#include <ghoti.io/audio/audio.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  static bool registered = false;
  if (!registered) {
    gaud_register_builtin_codecs();
    registered = true;
  }
  if (size < 2) {
    return 0;
  }
  // The first byte is the harness's, not the file's: it drives the limits
  // so that the refusal paths are reachable at all.
  const uint8_t options = data[0];
  data += 1;
  size -= 1;

  GAUD_Limits limits;
  gaud_limits_default(&limits);
  if (options & 0x01) {
    limits.max_channels = 2;
  }
  if (options & 0x02) {
    limits.max_frames = 1000;
  }
  if (options & 0x04) {
    limits.max_sample_rate = 48000;
  }
  if (options & 0x08) {
    limits.max_element_size = 4096;
  }
  if (options & 0x10) {
    limits.max_nesting_depth = 4;
  }

  GAUD_Stream * stream = nullptr;
  if (gaud_stream_create_memory(data, size, &stream) != GAUD_OK) {
    return 0;
  }
  // Half the time, hide the fact that the stream can seek. A container
  // parser's one-pass path is real code and nothing else here runs it.
  GAUD_Stream * blind = nullptr;
  GAUD_Stream * use = stream;
  if (options & 0x20) {
    if (gaud_stream_create_unseekable(stream, &blind) == GAUD_OK) {
      use = blind;
    }
  }

  GAUD_Diagnostics diagnostics = {};
  GAUD_Doc * doc = nullptr;
  GAUD_Result result
      = gaud_doc_load(nullptr, use, &limits, &diagnostics, &doc);

  if (result != GAUD_OK) {
    if (doc != nullptr) {
      __builtin_trap(); // nothing is written to out on failure
    }
  }
  else {
    if (doc == nullptr || gaud_doc_track_count(doc) == 0) {
      __builtin_trap();
    }
    GAUD_Track * track = gaud_doc_track(doc, 0);
    uint32_t channels = gaud_track_channels(track);
    uint32_t rate = gaud_track_sample_rate(track);
    if (channels == 0 || rate == 0) {
      __builtin_trap(); // a file nothing can play must not have loaded
    }
    if (channels > limits.max_channels || rate > limits.max_sample_rate) {
      __builtin_trap(); // a limit is a promise, not a suggestion
    }
    if (gaud_track_frames(track) != UINT64_MAX
        && gaud_track_frames(track) > limits.max_frames) {
      __builtin_trap();
    }

    GAUD_Decoder * decoder = nullptr;
    if (gaud_decoder_create(track, &decoder) == GAUD_OK) {
      GAUD_Buffer * buffer = nullptr;
      // Small, and not a divisor of anything, so the short final read
      // happens on every input rather than on the lucky ones.
      if (gaud_decoder_buffer_create(decoder, nullptr, 37, &buffer)
          == GAUD_OK) {
        uint64_t guard = 0;
        for (;;) {
          uint64_t before = gaud_decoder_tell(decoder);
          if (gaud_decoder_read(decoder, buffer) != GAUD_OK) {
            break;
          }
          size_t frames = gaud_buffer_frames(buffer);
          if (frames > gaud_buffer_capacity(buffer)) {
            __builtin_trap(); // a caller would walk off its own buffer
          }
          if (gaud_decoder_tell(decoder) != before + frames) {
            __builtin_trap(); // the position must match what was reported
          }
          if (frames == 0) {
            // At the end: reading again must stay there rather than wrap.
            if (gaud_decoder_read(decoder, buffer) != GAUD_OK
                || gaud_buffer_frames(buffer) != 0
                || gaud_decoder_tell(decoder) != before) {
              __builtin_trap();
            }
            break;
          }
          if (++guard > 100000) {
            break; // a hostile header should not make this unbounded
          }
        }
        uint64_t landed = 0;
        if (gaud_decoder_seek(decoder, 0, &landed) == GAUD_OK) {
          if (landed != 0 || gaud_decoder_tell(decoder) != 0) {
            __builtin_trap(); // a seek that succeeded must have landed
          }
        }
        gaud_buffer_destroy(buffer);
      }
      gaud_decoder_destroy(decoder);
    }
    gaud_doc_destroy(doc);
  }

  gaud_diagnostics_destroy(&diagnostics);
  if (blind) {
    gaud_stream_destroy(blind);
  }
  gaud_stream_destroy(stream);
  return 0;
}
