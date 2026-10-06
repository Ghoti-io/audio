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
 * Write an MP3 from any file this library can read.
 *
 *     make examples
 *     ./build/linux/release/apps/examples/encode_mp3 in.flac out.mp3
 *     ./build/linux/release/apps/examples/encode_mp3 in.wav out.mp3 abr 128
 *     ./build/linux/release/apps/examples/encode_mp3 in.wav out.mp3 vbr 60
 *
 * The third argument is the rate policy - `cbr` (the default), `abr` or
 * `vbr` - and the fourth is its number: kilobits per second for the first
 * two, quality from 1 to 100 for the third. What the file's tags say is
 * carried over as ID3v2.
 *
 * The one thing worth noticing is the conversion. The encoder takes signed
 * 16-bit samples and says so rather than choosing a dither for a 24-bit
 * source, so this program asks for the conversion explicitly, with the
 * dither it prefers, which is the decision a caller should make once and see.
 */

#include <ghoti.io/audio/audio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Read a track and write it as MP3. */
int main(int argc, char ** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: encode_mp3 <in> <out.mp3> [cbr|abr|vbr [number]]\n");
    return 2;
  }
  gaud_register_builtin_codecs();

  GAUD_Stream * in = NULL;
  if (gaud_stream_create_file(argv[1], &in) != GAUD_OK) {
    fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }
  GAUD_Doc * doc = NULL;
  GAUD_Result result = gaud_doc_load(NULL, in, NULL, NULL, &doc);
  if (result != GAUD_OK) {
    fprintf(stderr, "%s: %s\n", argv[1], gaud_result_string(result));
    return 1;
  }
  GAUD_Track * track = gaud_doc_track(doc, 0);

  GAUD_Encode_Params params;
  gaud_encode_params_default(&params);
  params.format = GAUD_SAMPLE_S16;
  params.coding = GAUD_CODING_MPEG_LAYER3;
  params.sample_rate = gaud_track_sample_rate(track);
  params.layout = gaud_track_layout(track);
  params.meta = gaud_doc_meta(doc);

  const char * policy = argc > 3 ? argv[3] : "cbr";
  int number = argc > 4 ? atoi(argv[4]) : 0;
  if (strcmp(policy, "abr") == 0) {
    params.rate_control = GAUD_RATE_ABR;
    params.bitrate = (uint32_t)number * 1000u;
  }
  else if (strcmp(policy, "vbr") == 0) {
    params.rate_control = GAUD_RATE_VBR;
    params.quality = (uint32_t)number;
  }
  else {
    params.rate_control = GAUD_RATE_CBR;
    params.bitrate = (uint32_t)number * 1000u;
  }

  GAUD_Stream * out = NULL;
  if (gaud_stream_create_file_writer(NULL, argv[2], &out) != GAUD_OK) {
    fprintf(stderr, "cannot write %s\n", argv[2]);
    return 1;
  }
  GAUD_Encoder * encoder = NULL;
  result = gaud_encoder_create("mp3", NULL, out, &params, &encoder);
  if (result != GAUD_OK) {
    /* The likely ones: a sampling frequency MPEG has no row for, more than
     * two channels, or a rate that is not in the version's table. */
    fprintf(stderr, "cannot write MP3 at %u Hz, %u channels, %s %d: %s\n",
        params.sample_rate, params.layout.channels, policy, number,
        gaud_result_string(result));
    return 1;
  }

  GAUD_Decoder * decoder = NULL;
  GAUD_Buffer * buffer = NULL;
  if (gaud_decoder_create(track, &decoder) != GAUD_OK
      || gaud_decoder_buffer_create(decoder, NULL, 4096, &buffer) != GAUD_OK) {
    fprintf(stderr, "no decoder for this track\n");
    return 1;
  }
  for (;;) {
    if (gaud_decoder_read(decoder, buffer) != GAUD_OK) {
      fprintf(stderr, "read failed\n");
      return 1;
    }
    if (gaud_buffer_frames(buffer) == 0) {
      break;
    }
    /* The encoder wants signed 16-bit; a 24-bit source is converted here,
     * with TPDF dither, because that is a choice the caller is entitled to
     * make and the library will not make silently. */
    const GAUD_Buffer * to_write = buffer;
    GAUD_Buffer * converted = NULL;
    if (gaud_buffer_format(buffer) != GAUD_SAMPLE_S16) {
      GAUD_Convert_Options options;
      gaud_convert_options_default(&options);
      options.dither = GAUD_DITHER_TRIANGULAR;
      if (gaud_ops_convert_format(NULL, buffer, GAUD_SAMPLE_S16, &options,
              &converted) != GAUD_OK) {
        fprintf(stderr, "cannot convert to 16 bits\n");
        return 1;
      }
      to_write = converted;
    }
    result = gaud_encoder_write(encoder, to_write);
    gaud_buffer_destroy(converted);
    if (result != GAUD_OK) {
      fprintf(stderr, "write failed: %s\n", gaud_result_string(result));
      return 1;
    }
  }
  result = gaud_encoder_finish(encoder);
  if (result != GAUD_OK) {
    fprintf(stderr, "finish failed: %s\n", gaud_result_string(result));
    return 1;
  }
  printf("%s -> %s: %llu frames\n", argv[1], argv[2],
      (unsigned long long)gaud_encoder_frames_written(encoder));

  gaud_buffer_destroy(buffer);
  gaud_decoder_destroy(decoder);
  gaud_encoder_destroy(encoder);
  gaud_stream_destroy(out);
  gaud_doc_destroy(doc);
  gaud_stream_destroy(in);
  return 0;
}
