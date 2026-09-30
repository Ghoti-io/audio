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
 * Read and write tags, for the differential to drive.
 *
 *     tag_probe dump   <file>
 *     tag_probe write  <out> <codec> [key=value ...]
 *     tag_probe genres
 *
 * `dump` prints one tab-separated record per line, which is what a
 * comparison against a reference's output can be diffed against. `write`
 * builds a file carrying exactly the tags named. `genres` prints the
 * ID3v1 genre table, so `make check-tags` can compare it against
 * mutagen's entry by entry rather than trusting a transcription.
 *
 * Under tools/ and not tests/ for the reason dump_probe is: a differential
 * needs this library's answer to leave the process so another
 * implementation can be asked the same question, and a GoogleTest binary
 * cannot be piped into a comparison.
 */

/* The genre table is an internal detail - no caller needs it, because a
 * numeric ID3v1 genre is mapped to its name before it reaches the API.
 * The differential does need it, to compare the table against mutagen's
 * row by row, and this probe is part of the test apparatus rather than
 * an example. */
#include "../../src/meta/scheme.h"
#include <ghoti.io/audio/audio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Print a value with tabs and newlines escaped, so one record is one line. */
static void put_escaped(const char * text) {
  for (const unsigned char * p = (const unsigned char *)text; *p; ++p) {
    switch (*p) {
    case '\t': fputs("\\t", stdout); break;
    case '\n': fputs("\\n", stdout); break;
    case '\r': fputs("\\r", stdout); break;
    case '\\': fputs("\\\\", stdout); break;
    default: fputc(*p, stdout); break;
    }
  }
}

static int dump(const char * path) {
  GAUD_Stream * stream = NULL;
  if (gaud_stream_create_file(path, &stream) != GAUD_OK) {
    fprintf(stderr, "cannot open %s\n", path);
    return 1;
  }
  GAUD_Diagnostics diagnostics = {0};
  GAUD_Doc * doc = NULL;
  GAUD_Result result = gaud_doc_load(NULL, stream, NULL, &diagnostics, &doc);
  if (result != GAUD_OK) {
    fprintf(stderr, "load failed: %s\n", gaud_result_string(result));
    gaud_stream_destroy(stream);
    return 1;
  }
  const GAUD_Meta * meta = gaud_doc_meta(doc);
  for (int t = 0; t < GAUD_TAG_COUNT; ++t) {
    GAUD_Tag tag = (GAUD_Tag)t;
    size_t count = gaud_meta_count(meta, tag);
    for (size_t i = 0; i < count; ++i) {
      printf("tag\t%s\t%zu\t", gaud_tag_name(tag), i);
      put_escaped(gaud_meta_get(meta, tag, i));
      putchar('\n');
    }
  }
  for (size_t i = 0; i < gaud_meta_custom_count(meta); ++i) {
    const char * key = NULL;
    const char * value = NULL;
    if (gaud_meta_custom(meta, i, &key, &value) != GAUD_OK) {
      continue;
    }
    fputs("custom\t", stdout);
    put_escaped(key);
    fputc('\t', stdout);
    put_escaped(value);
    putchar('\n');
  }
  for (size_t i = 0; i < gaud_meta_picture_count(meta); ++i) {
    const GAUD_Picture * picture = gaud_meta_picture(meta, i);
    printf("picture\t%d\t%s\t%zu\t%u\t%ux%u\t%ux%u\t", (int)picture->kind,
        picture->mime_type, picture->size, (unsigned)picture->status,
        picture->stated_width, picture->stated_height,
        picture->verified_width, picture->verified_height);
    put_escaped(picture->description);
    putchar('\n');
  }
  for (size_t i = 0; i < gaud_meta_raw_count(meta); ++i) {
    const char * scheme = NULL;
    const char * id = NULL;
    size_t size = 0;
    if (gaud_meta_raw(meta, i, &scheme, &id, NULL, &size) == GAUD_OK) {
      printf("raw\t%s\t%s\t%zu\n", scheme, id, size);
    }
  }
  printf("diagnostics\t%zu\n", diagnostics.count);
  gaud_diagnostics_destroy(&diagnostics);
  gaud_doc_destroy(doc);
  gaud_stream_destroy(stream);
  return 0;
}

static int write_file(int argc, char ** argv) {
  const char * path = argv[2];
  const char * codec = argv[3];

  GAUD_Meta * meta = NULL;
  if (gaud_meta_create(NULL, &meta) != GAUD_OK) {
    return 1;
  }
  for (int i = 4; i < argc; ++i) {
    char * equals = strchr(argv[i], '=');
    if (!equals) {
      continue;
    }
    *equals = '\0';
    GAUD_Tag tag;
    if (gaud_tag_from_name(argv[i], &tag)) {
      gaud_meta_add(meta, tag, equals + 1);
    }
    else {
      gaud_meta_custom_add(meta, argv[i], equals + 1);
    }
    *equals = '=';
  }

  GAUD_Stream * out = NULL;
  if (gaud_stream_create_file_writer(NULL, path, &out) != GAUD_OK) {
    fprintf(stderr, "cannot write %s\n", path);
    gaud_meta_destroy(meta);
    return 1;
  }
  GAUD_Encode_Params params;
  gaud_encode_params_default(&params);
  params.meta = meta;

  GAUD_Encoder * encoder = NULL;
  GAUD_Result result
      = gaud_encoder_create(codec, NULL, out, &params, &encoder);
  if (result != GAUD_OK) {
    fprintf(stderr, "encoder: %s\n", gaud_result_string(result));
    return 1;
  }
  GAUD_Buffer * buffer = NULL;
  if (gaud_buffer_create(NULL, params.format, params.layout,
          GAUD_LAYOUT_INTERLEAVED, 1024, &buffer)
      != GAUD_OK) {
    return 1;
  }
  int16_t * samples = gaud_buffer_data(buffer);
  for (size_t i = 0; i < 1024 * 2; ++i) {
    samples[i] = (int16_t)((i * 977u) % 20000u) - 10000;
  }
  gaud_buffer_set_frames(buffer, 1024);
  result = gaud_encoder_write(encoder, buffer);
  if (result == GAUD_OK) {
    result = gaud_encoder_finish(encoder);
  }
  if (result != GAUD_OK) {
    fprintf(stderr, "write: %s\n", gaud_result_string(result));
    return 1;
  }
  gaud_buffer_destroy(buffer);
  gaud_encoder_destroy(encoder);
  gaud_stream_destroy(out);
  gaud_meta_destroy(meta);
  return 0;
}

int main(int argc, char ** argv) {
  if (argc < 2) {
    fprintf(stderr,
        "usage: tag_probe dump <file> | write <out> <codec> [k=v ...] "
        "| genres\n");
    return 2;
  }
  gaud_register_builtin_codecs();

  if (strcmp(argv[1], "genres") == 0) {
    for (unsigned i = 0; i < gaud_id3v1_genre_count(); ++i) {
      printf("%u\t%s\n", i, gaud_id3v1_genre(i));
    }
    return 0;
  }
  if (strcmp(argv[1], "dump") == 0 && argc >= 3) {
    return dump(argv[2]);
  }
  if (strcmp(argv[1], "write") == 0 && argc >= 4) {
    return write_file(argc, argv);
  }
  fprintf(stderr, "unknown mode\n");
  return 2;
}
