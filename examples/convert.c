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
 * Describe a file, then convert it and write it back out.
 *
 *     make examples
 *     ./build/linux/release/apps/examples/convert in.wav out.aiff
 *
 * Shows the shape of an ordinary use: load parses and decodes nothing, the
 * samples come a block at a time, and the one conversion happens because
 * this program asked for it.
 */

#include <ghoti.io/audio/audio.h>
#include <stdio.h>
#include <string.h>

/** @brief Describe a file, then convert it and write it back out. */
int main(int argc, char ** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: convert <in> <out.wav|out.aiff>\n");
    return 2;
  }
  gaud_register_builtin_codecs();

  /* The output format is chosen from the extension only because this is a
   * command-line program and something has to choose. The library never
   * consults an extension: gaud_doc_load() below identifies the INPUT from
   * its bytes, which is the point - a .wav holding MP3 frames exists. */
  const char * dot = strrchr(argv[2], '.');
  const char * codec = (dot && strcmp(dot, ".aiff") == 0) ? "aiff" : "wav";

  GAUD_Stream * in = NULL;
  if (gaud_stream_create_file(argv[1], &in) != GAUD_OK) {
    fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }

  GAUD_Diagnostics diagnostics = {0};
  GAUD_Doc * doc = NULL;
  GAUD_Result result = gaud_doc_load(NULL, in, NULL, &diagnostics, &doc);
  if (result != GAUD_OK) {
    fprintf(stderr, "%s: %s\n", argv[1], gaud_result_string(result));
    return 1;
  }

  GAUD_Track * track = gaud_doc_track(doc, 0);
  GAUD_Channel_Layout layout = gaud_track_layout(track);
  printf("%s: %s, %s, %u Hz, %u channels, %.3f s\n", argv[1],
      gaud_doc_codec_name(doc),
      gaud_sample_format_string(gaud_track_format(track)),
      gaud_track_sample_rate(track), layout.channels,
      gaud_track_duration(track));
  if (layout.mask) {
    printf("  channels:");
    for (uint32_t i = 0; i < layout.channels; ++i) {
      printf(" %s", gaud_channel_string(gaud_channel_layout_at(layout, i)));
    }
    printf("\n");
  }
  else {
    printf("  channel layout: not stated by the file\n");
  }
  for (size_t i = 0; i < diagnostics.count; ++i) {
    printf("  note: %s\n", diagnostics.items[i].recommended_action
            ? diagnostics.items[i].recommended_action : "(unspecified)");
  }

  GAUD_Encode_Params params;
  gaud_encode_params_default(&params);
  params.format = gaud_track_format(track);
  params.sample_rate = gaud_track_sample_rate(track);
  params.layout = layout;

  GAUD_Stream * out = NULL;
  if (gaud_stream_create_file_writer(NULL, argv[2], &out) != GAUD_OK) {
    fprintf(stderr, "cannot write %s\n", argv[2]);
    return 1;
  }
  GAUD_Encoder * encoder = NULL;
  result = gaud_encoder_create(codec, NULL, out, &params, &encoder);
  if (result != GAUD_OK) {
    /* The likely one: WAV has no signed 8-bit and AIFF no unsigned 8-bit,
     * and this library refuses rather than shifting every sample by 128
     * without saying so. */
    fprintf(stderr, "cannot write %s as %s: %s\n", argv[2], codec,
        gaud_result_string(result));
    return 1;
  }

  GAUD_Decoder * decoder = NULL;
  if (gaud_decoder_create(track, &decoder) != GAUD_OK) {
    fprintf(stderr, "no decoder for this track\n");
    return 1;
  }
  GAUD_Buffer * buffer = NULL;
  /* A block, not the track. An hour of 96 kHz stereo is about 2 GB. */
  if (gaud_decoder_buffer_create(decoder, NULL, 4096, &buffer) != GAUD_OK) {
    return 1;
  }

  double peak = 0.0;
  uint64_t frames = 0;
  for (;;) {
    if (gaud_decoder_read(decoder, buffer) != GAUD_OK) {
      fprintf(stderr, "read failed\n");
      return 1;
    }
    if (gaud_buffer_frames(buffer) == 0) {
      break; /* the end; a short read is not an error */
    }
    double block_peak = 0.0;
    gaud_ops_peak(buffer, &block_peak);
    if (block_peak > peak) {
      peak = block_peak;
    }
    frames += gaud_buffer_frames(buffer);
    if (gaud_encoder_write(encoder, buffer) != GAUD_OK) {
      fprintf(stderr, "write failed\n");
      return 1;
    }
  }

  /* Must be called and must be checked: this is where the header's length
   * is patched, and an encoder merely destroyed leaves a file describing a
   * size it does not have. */
  result = gaud_encoder_finish(encoder);
  if (result != GAUD_OK) {
    fprintf(stderr, "finish: %s\n", gaud_result_string(result));
    return 1;
  }
  printf("wrote %s: %llu frames, peak %.4f of full scale\n", argv[2],
      (unsigned long long)frames, peak);

  gaud_buffer_destroy(buffer);
  gaud_decoder_destroy(decoder);
  gaud_encoder_destroy(encoder);
  gaud_stream_destroy(out);
  gaud_diagnostics_destroy(&diagnostics);
  gaud_doc_destroy(doc);
  gaud_stream_destroy(in);
  return 0;
}
