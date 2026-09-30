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
 * Register a codec and identify a stream with it.
 *
 * This is the whole phase 0 surface in one program, and it is written the way
 * a codec in *another* repository would be written: a file-scope GAUD_Codec
 * handed to the public registry, with nothing private to this library
 * involved. `audio-aac` will look like this.
 *
 *     make examples && ./build/linux/release/apps/examples/register_codec
 */

#include <ghoti.io/audio/audio.h>
#include <stdio.h>
#include <string.h>

/* FLAC's signature is four bytes at offset zero. A real codec would declare
 * its decode entry points too; phase 0's vtable carries identification only. */
static const unsigned char flac_magic[] = {'f', 'L', 'a', 'C'};
static const GAUD_Codec_Magic flac_magics[] = {{0, flac_magic, 4}};

static const GAUD_Codec flac_codec = {
    .abi_version = GAUD_CODEC_ABI_VERSION,
    .size = sizeof(GAUD_Codec),
    .name = "flac",
    .ctx = NULL,
    .capabilities = GAUD_CAP_DECODE,
    /* Decode only, so there is no encoder to rate. A codec that set
     * GAUD_CAP_ENCODE and left this at NONE would be refused. */
    .encoder_tier = GAUD_ENCODER_NONE,
    .magics = flac_magics,
    .magic_count = 1,
    .probe = NULL,
};

/* WAV needs two signatures: "RIFF" at 0 says only that this is a RIFF file,
 * and "WAVE" at 8 is what makes it audio. */
static const unsigned char riff_magic[] = {'R', 'I', 'F', 'F'};
static const unsigned char wave_magic[] = {'W', 'A', 'V', 'E'};
static const GAUD_Codec_Magic wav_magics[] = {
    {0, riff_magic, 4},
    {8, wave_magic, 4},
};

static const GAUD_Codec wav_codec = {
    .abi_version = GAUD_CODEC_ABI_VERSION,
    .size = sizeof(GAUD_Codec),
    .name = "wav",
    .ctx = NULL,
    .capabilities = GAUD_CAP_DECODE | GAUD_CAP_ENCODE,
    /* PCM in a RIFF chunk: what comes out is determined by what went in. */
    .encoder_tier = GAUD_ENCODER_EXACT,
    .magics = wav_magics,
    .magic_count = 2,
    .probe = NULL,
};

static void identify(
    GAUD_Registry * registry, const char * label, const unsigned char * bytes,
    size_t length) {
  GAUD_Stream * stream = NULL;
  if (gaud_stream_create_memory(bytes, length, &stream) != GAUD_OK) {
    return;
  }

  GAUD_Probe_Result result;
  memset(&result, 0, sizeof(result));
  if (gaud_probe(registry, stream, &result) == GAUD_OK) {
    printf("  %-18s -> %-12s (confidence %u)\n", label,
        result.codec_name ? result.codec_name : "unrecognised",
        result.confidence);
  }

  gaud_stream_destroy(stream);
}

/** @brief Register two codecs, then identify three streams with them. */
int main(void) {
  printf("ghoti.io-audio %s\n", gaud_version_string());
  printf("cover-art validation: %s\n\n",
      gaud_have_image_validation() ? "available" : "not built in");

  /* A registry of its own rather than the process-wide default, so this
   * program's codecs cannot be confused with anything else's. */
  GAUD_Registry * registry = NULL;
  if (gaud_registry_create(NULL, &registry) != GAUD_OK) {
    fprintf(stderr, "could not create a registry\n");
    return 1;
  }
  if (gaud_registry_register(registry, &flac_codec) != GAUD_OK
      || gaud_registry_register(registry, &wav_codec) != GAUD_OK) {
    fprintf(stderr, "could not register a codec\n");
    gaud_registry_destroy(registry);
    return 1;
  }

  printf("%zu codecs registered:\n", gaud_registry_count(registry));
  for (size_t i = 0; i < gaud_registry_count(registry); ++i) {
    const GAUD_Codec * codec = gaud_registry_by_index(registry, i);
    printf("  %-6s decode:%s encode:%s\n", codec->name,
        (codec->capabilities & GAUD_CAP_DECODE) ? "yes" : "no ",
        (codec->capabilities & GAUD_CAP_ENCODE) ? "yes" : "no ");
  }

  static const unsigned char a_flac[] = {'f', 'L', 'a', 'C', 0, 0, 0, 34};
  static const unsigned char a_wav[]
      = {'R', 'I', 'F', 'F', 36, 0, 0, 0, 'W', 'A', 'V', 'E'};
  static const unsigned char an_aiff[]
      = {'F', 'O', 'R', 'M', 0, 0, 0, 4, 'A', 'I', 'F', 'F'};

  printf("\nidentifying, from the bytes and never the extension:\n");
  identify(registry, "a FLAC file", a_flac, sizeof(a_flac));
  identify(registry, "a WAV file", a_wav, sizeof(a_wav));
  /* AIFF is also a chunked container beginning with a four-byte tag, and no
   * codec here claims it. "Unrecognised" is an answer, not a failure. */
  identify(registry, "an AIFF file", an_aiff, sizeof(an_aiff));

  gaud_registry_destroy(registry);
  return 0;
}
