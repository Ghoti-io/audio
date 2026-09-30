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
 * Drive the out-of-tree codec, and require the core to use it.
 *
 * Compiled against the installed headers, linked against the installed
 * library and the toy codec beside it. If this runs, a codec in another
 * repository works.
 */

#include <ghoti.io/audio/audio.h>
#include <stdio.h>
#include <string.h>

void toy_register(void);

#define CHECK(cond, what)                                                    \
  do {                                                                       \
    if (!(cond)) {                                                           \
      fprintf(stderr, "FAIL: %s\n", (what));                                 \
      return 1;                                                              \
    }                                                                        \
    printf("  ok: %s\n", (what));                                            \
  } while (0)

int main(void) {
  /* The explicit path, which is the one that matters for a static link: a
   * constructor in an object nothing references by name is dropped. */
  toy_register();

  const GAUD_Codec * codec = gaud_registry_find(NULL, "toy");
  CHECK(codec != NULL, "an out-of-tree codec registered itself");
  CHECK(codec->capabilities & GAUD_CAP_DECODE, "it declares decode");
  CHECK(codec->encoder_tier == GAUD_ENCODER_NONE,
      "it declares no encoder, and the tier agrees");

  /* A file in the toy format: header, then a ramp so a stride mistake shows
   * as a wrong value rather than as silence. */
  unsigned char file[12 + 40];
  memcpy(file, "TOY1", 4);
  file[4] = 0x44; file[5] = 0xAC; file[6] = 0; file[7] = 0;  /* 44100 */
  file[8] = 2; file[9] = 0;                                   /* stereo */
  file[10] = 0; file[11] = 0;
  for (int i = 0; i < 20; ++i) {
    int16_t v = (int16_t)(i * 1000 - 10000);
    memcpy(file + 12 + i * 2, &v, 2);
  }

  GAUD_Stream * stream = NULL;
  CHECK(gaud_stream_create_memory(file, sizeof(file), &stream) == GAUD_OK,
      "a stream over it");

  GAUD_Probe_Result probed;
  memset(&probed, 0, sizeof(probed));
  CHECK(gaud_probe(NULL, stream, &probed) == GAUD_OK
          && probed.codec_name != NULL
          && strcmp(probed.codec_name, "toy") == 0,
      "the core's probe identified the out-of-tree format");

  GAUD_Doc * doc = NULL;
  CHECK(gaud_doc_load(NULL, stream, NULL, NULL, &doc) == GAUD_OK,
      "the core opened a document through the out-of-tree codec");

  GAUD_Track * track = gaud_doc_track(doc, 0);
  CHECK(gaud_track_sample_rate(track) == 44100
          && gaud_track_channels(track) == 2
          && gaud_track_frames(track) == 10,
      "the track it described is right");

  GAUD_Decoder * decoder = NULL;
  CHECK(gaud_decoder_create(track, &decoder) == GAUD_OK,
      "the core made a decoder from it");

  GAUD_Buffer * buffer = NULL;
  CHECK(gaud_decoder_buffer_create(decoder, NULL, 4, &buffer) == GAUD_OK,
      "a buffer shaped for it");

  /* Read it all, in blocks that do not divide the length. */
  size_t total = 0;
  int16_t got[64];
  for (;;) {
    if (gaud_decoder_read(decoder, buffer) != GAUD_OK) {
      fprintf(stderr, "FAIL: read\n");
      return 1;
    }
    size_t frames = gaud_buffer_frames(buffer);
    if (frames == 0) {
      break;
    }
    memcpy(got + total * 2, gaud_buffer_data_const(buffer),
        gaud_buffer_bytes_used(buffer));
    total += frames;
  }
  CHECK(total == 10, "it decoded every frame, across a short final block");

  int same = 1;
  for (int i = 0; i < 20; ++i) {
    if (got[i] != (int16_t)(i * 1000 - 10000)) {
      same = 0;
    }
  }
  CHECK(same, "the samples came back exactly");

  uint64_t landed = 0;
  CHECK(gaud_decoder_seek(decoder, 5, &landed) == GAUD_OK && landed == 5,
      "seeking through the out-of-tree codec reports where it landed");

  gaud_buffer_destroy(buffer);
  gaud_decoder_destroy(decoder);
  gaud_doc_destroy(doc);
  gaud_stream_destroy(stream);
  printf("\nAn out-of-tree codec works against the installed headers.\n");
  return 0;
}
