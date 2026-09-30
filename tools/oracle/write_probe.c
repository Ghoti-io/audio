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
 * Re-encode a fixture with this library's writer.
 *
 *     write_probe <in> <out> <codec>
 *
 * Reads @p in with this library, writes it back out as @p codec, and stops.
 * The samples are not touched in between, so for a lossless format the
 * output must contain exactly the input's samples - and whether it does is
 * a question for ffmpeg, sox and libsndfile rather than for us.
 *
 * **This is what scores the writer.** Our own reader reading our own writer
 * agrees by construction; it would keep agreeing if both had the same
 * misunderstanding of the format, which is precisely the failure a
 * differential exists to catch. So the bytes have to leave the process and
 * be read by something that shares no code with this library.
 */

#include <ghoti.io/audio/audio.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char ** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: write_probe <in> <out> <codec> [coding]\n");
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
    fprintf(stderr, "load: %s\n", gaud_result_string(result));
    return 1;
  }
  GAUD_Track * track = gaud_doc_track(doc, 0);

  GAUD_Encode_Params params;
  gaud_encode_params_default(&params);
  params.format = gaud_track_format(track);
  params.sample_rate = gaud_track_sample_rate(track);
  params.layout = gaud_track_layout(track);

  /* Named rather than copied from the input, so that the gate can ask for
   * a coding the source did not have - re-coding a PCM fixture as µ-law is
   * the case that exercises the encoder, and copying the input's coding
   * would only ever re-emit what was already there. */
  if (argc > 4) {
    static const struct {
      const char * name;
      GAUD_Sample_Coding coding;
    } known[] = {
        {"pcm", GAUD_CODING_PCM},
        {"ulaw", GAUD_CODING_G711_ULAW},
        {"alaw", GAUD_CODING_G711_ALAW},
        {"ima-wav", GAUD_CODING_ADPCM_IMA_WAV},
        {"ima-qt", GAUD_CODING_ADPCM_IMA_QT},
        {"ms-adpcm", GAUD_CODING_ADPCM_MS},
    };
    bool found = false;
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
      if (strcmp(argv[4], known[i].name) == 0) {
        params.coding = known[i].coding;
        found = true;
        break;
      }
    }
    if (!found) {
      fprintf(stderr, "unknown coding %s\n", argv[4]);
      return 2;
    }
    if (params.coding != GAUD_CODING_PCM) {
      /* Every coding decodes to S16, and the samples must be in that
       * format before they reach the encoder. A fixture in any other
       * format is converted below rather than refused: the gate's job is
       * to score the writer, and restricting it to the fixtures that
       * happen to be s16 would shrink the population without saying so. */
      params.format = gaud_sample_coding_format(params.coding);
    }
  }

  GAUD_Stream * out = NULL;
  if (gaud_stream_create_file_writer(NULL, argv[2], &out) != GAUD_OK) {
    fprintf(stderr, "cannot write %s\n", argv[2]);
    return 1;
  }
  GAUD_Encoder * encoder = NULL;
  result = gaud_encoder_create(argv[3], NULL, out, &params, &encoder);
  if (result != GAUD_OK) {
    fprintf(stderr, "encoder: %s\n", gaud_result_string(result));
    return 1;
  }

  GAUD_Decoder * decoder = NULL;
  if (gaud_decoder_create(track, &decoder) != GAUD_OK) {
    fprintf(stderr, "no decoder\n");
    return 1;
  }
  GAUD_Buffer * buffer = NULL;
  /* Not a divisor of any fixture's length, so the last write is always a
   * partial block. A writer that assumed full blocks would pass a corpus of
   * round numbers and fail on the first real file. */
  if (gaud_decoder_buffer_create(decoder, NULL, 997, &buffer) != GAUD_OK) {
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
    const GAUD_Buffer * to_write = buffer;
    GAUD_Buffer * converted = NULL;
    if (gaud_buffer_format(buffer) != params.format) {
      result = gaud_ops_convert_format(
          NULL, buffer, params.format, NULL, &converted);
      if (result != GAUD_OK) {
        fprintf(stderr, "convert: %s\n", gaud_result_string(result));
        return 1;
      }
      to_write = converted;
    }
    result = gaud_encoder_write(encoder, to_write);
    gaud_buffer_destroy(converted);
    if (result != GAUD_OK) {
      fprintf(stderr, "write: %s\n", gaud_result_string(result));
      return 1;
    }
  }
  result = gaud_encoder_finish(encoder);
  if (result != GAUD_OK) {
    fprintf(stderr, "finish: %s\n", gaud_result_string(result));
    return 1;
  }

  gaud_buffer_destroy(buffer);
  gaud_decoder_destroy(decoder);
  gaud_encoder_destroy(encoder);
  gaud_stream_destroy(out);
  gaud_doc_destroy(doc);
  gaud_stream_destroy(in);
  return 0;
}
