/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Audio.
 *
 * Ghoti.io Audio is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 */

/**
 * @file
 *
 * Fuzz the MPEG audio frame search, the length tags, and the loader.
 *
 * **This is the harness whose input distribution is closest to the real
 * thing**, and the reason is that MPEG audio is not a container. There is
 * no magic to synthesise and no header to get right before the interesting
 * code is reached: every input is a candidate stream, the frame search
 * walks all of it, and a random byte pair matching eleven bits of sync
 * happens about twenty times in every 64 KiB. So unlike the WAV and AIFF
 * harnesses, nearly every input here reaches the code under test.
 *
 * What it asserts beyond not crashing:
 *
 * - **A header that parses is self-consistent.** Its derived fields - the
 *   frame length, the samples per frame, the channel count - are each a
 *   lookup keyed on two or three of the raw fields, and a wrong table
 *   index produces a frame length that does not reach the end of its own
 *   side information. That is checkable without knowing what the input
 *   was.
 * - **The frame search is a function of the stream alone.** Running it
 *   twice gives the same answer, so nothing it does leaves state behind -
 *   which matters because it is the one function here that seeks around
 *   while deciding.
 * - **A loaded document's extents are inside the stream.** The audio's
 *   offset and length, the first frame's offset, and the stated trim are
 *   all derived from attacker-controlled numbers, and a caller will seek
 *   to them.
 * - **A stated trim never exceeds the stated length**, which is the
 *   invariant the loader enforces by refusing an absurd LAME tag rather
 *   than clamping it.
 *
 * Build with: make fuzz-mpeg
 * Run:        make fuzz-run-mpeg FUZZ_TIME=300
 */

#include "../../src/codec/mp3/mp3_internal.h"
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

/** Parse a header at every offset, and check what came out hangs together. */
void fuzz_headers(const uint8_t * data, size_t size) {
  for (size_t at = 0; at + MP3_HEADER_SIZE <= size; ++at) {
    MP3_Header header;
    if (!gaud_mp3_header_parse(data + at, &header)) {
      continue;
    }
    if (header.sample_rate == 0u) {
      abort();
    }
    if (header.layer < 1u || header.layer > 3u) {
      abort();
    }
    if (header.channels != 1u && header.channels != 2u) {
      abort();
    }
    if (header.samples != 384u && header.samples != 576u
        && header.samples != 1152u) {
      abort();
    }
    uint32_t side = gaud_mp3_side_info_size(&header);
    if (header.frame_size != 0u) {
      if (header.frame_size > MP3_MAX_FRAME_SIZE) {
        abort();
      }
      if (header.frame_size
          < MP3_HEADER_SIZE + (header.crc_present ? 2u : 0u) + side) {
        abort();
      }
    }
    /* A header is compatible with itself. A comparison that could answer
     * otherwise would make the frame search reject every stream. */
    if (!gaud_mp3_headers_compatible(&header, &header)) {
      abort();
    }
    /* And the tag reader, on this offset treated as a frame's start. It
     * must not read past what it was given, whatever the side
     * information's length says. */
    MP3_Vbr_Tag tag;
    bool found = gaud_mp3_tag_parse(data + at, size - at, &header, &tag);
    if (found != tag.present) {
      abort();
    }
    if (tag.lame_present && !tag.present) {
      abort();
    }
  }
}

/** Run the frame search twice and check it answers the same both times. */
void fuzz_search(const uint8_t * data, size_t size) {
  GAUD_Stream * stream = NULL;
  if (gaud_stream_create_memory(data, size, &stream) != GAUD_OK) {
    return;
  }
  uint64_t id3 = gaud_mp3_id3v2_span(stream);
  uint64_t first = 0;
  uint64_t again = 0;
  MP3_Header header;
  MP3_Header header_again;
  unsigned chain = 0;
  unsigned chain_again = 0;
  GAUD_Result one = gaud_mp3_find_frame(
      stream, id3, MP3_SYNC_SEARCH, &first, &header, &chain);
  GAUD_Result two = gaud_mp3_find_frame(
      stream, id3, MP3_SYNC_SEARCH, &again, &header_again, &chain_again);
  if (one != two) {
    abort();
  }
  if (one == GAUD_OK) {
    if (first != again || chain != chain_again) {
      abort();
    }
    if (first + MP3_HEADER_SIZE > size) {
      abort();
    }
    if (memcmp(&header, &header_again, sizeof(header)) != 0) {
      abort();
    }
  }
  gaud_stream_destroy(stream);
}

/** Load a whole stream, for whichever codec claims it. */
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
  if (gaud_doc_load(NULL, stream, &limits, &diagnostics, &doc) != GAUD_OK) {
    gaud_diagnostics_destroy(&diagnostics);
    gaud_stream_destroy(stream);
    return;
  }

  GAUD_Track * track = gaud_doc_track(doc, 0);
  if (track && strcmp(gaud_doc_codec_name(doc), "mp3") == 0) {
    MP3_File * file = (MP3_File *)gaud_doc_private(doc);
    if (!file) {
      abort();
    }
    if (file->audio_offset < file->first_frame_offset) {
      abort();
    }
    if (file->audio_offset > size) {
      abort();
    }
    if (file->audio_offset + file->audio_length > size) {
      abort();
    }
    uint64_t frames = gaud_track_frames(track);
    if (frames != UINT64_MAX && frames > limits.max_frames) {
      abort();
    }
    GAUD_Trim trim = gaud_track_trim(track);
    if (trim.stated && frames != UINT64_MAX
        && trim.encoder_delay + trim.padding >= frames) {
      abort();
    }
    /* A duration is either the recording's length or -1 for "unknown",
     * and never a negative number that is not -1: a caller prints it. */
    double duration = gaud_track_duration(track);
    if (duration < 0.0 && duration != -1.0) {
      abort();
    }
  }
  gaud_doc_destroy(doc);
  gaud_diagnostics_destroy(&diagnostics);
  gaud_stream_destroy(stream);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 2) {
    return 0;
  }
  /* The first byte chooses, and the whole-file path gets half the corpus
   * because it is the one that allocates. The other two are cheap and
   * reach the arithmetic directly. */
  unsigned which = data[0] & 0x03u;
  const uint8_t * rest = data + 1;
  size_t rest_size = size - 1u;
  if (which == 0u) {
    fuzz_headers(rest, rest_size);
  }
  else if (which == 1u) {
    fuzz_search(rest, rest_size);
  }
  else {
    fuzz_file(rest, rest_size);
  }
  return 0;
}
