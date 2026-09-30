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
 * The WAV codec's registration.
 *
 * Built exactly the way an out-of-tree codec is built - a file-scope
 * ::GAUD_Codec handed to the public registry - because a public path the
 * shipped codecs bypass is one nothing exercises. There is no internal
 * shortcut for the built-in ones and there will not be.
 */

#include "wav_internal.h"
#include <string.h>

/*
 * RIFF at 0 and WAVE at 8. Both are needed: "RIFF" alone says only that
 * this is a RIFF file, and AVI and several others are RIFF files too.
 */
static const unsigned char riff_magic[] = {'R', 'I', 'F', 'F'};
static const unsigned char rf64_magic[] = {'R', 'F', '6', '4'};
static const unsigned char bw64_magic[] = {'B', 'W', '6', '4'};
static const unsigned char wave_magic[] = {'W', 'A', 'V', 'E'};

static const GAUD_Codec_Magic wav_magics[] = {
    {0, riff_magic, 4},
    {0, rf64_magic, 4},
    {0, bw64_magic, 4},
    {8, wave_magic, 4},
};

/*
 * A signature match alone is weak here, because three of the four above are
 * shared with every other RIFF-family format. The probe requires the form
 * type as well, which is what actually distinguishes a WAV.
 */
static GAUD_Result wav_probe(const GAUD_Codec * codec, GAUD_Stream * stream,
    unsigned int * out_confidence) {
  (void)codec;
  *out_confidence = GAUD_CONFIDENCE_NONE;

  unsigned char header[12];
  uint64_t start = gaud_stream_tell(stream);
  size_t got = gaud_stream_read(stream, header, sizeof(header));
  gaud_stream_seek(stream, (int64_t)start, GAUD_SEEK_SET);
  if (got != sizeof(header)) {
    return GAUD_OK;
  }

  bool riff = memcmp(header, "RIFF", 4) == 0
      || memcmp(header, "RF64", 4) == 0 || memcmp(header, "BW64", 4) == 0;
  if (!riff) {
    return GAUD_OK;
  }
  if (memcmp(header + 8, "WAVE", 4) != 0) {
    /* A RIFF file that is not a WAVE. Saying no rather than weakly yes: an
     * AVI is not a thing this codec should be given a chance to open. */
    return GAUD_OK;
  }
  *out_confidence = GAUD_CONFIDENCE_CERTAIN;
  return GAUD_OK;
}

static const GAUD_Codec wav_codec = {
    .abi_version = GAUD_CODEC_ABI_VERSION,
    .size = sizeof(GAUD_Codec),
    .name = "wav",
    .ctx = NULL,
    .capabilities = GAUD_CAP_DECODE | GAUD_CAP_ENCODE,
    /* PCM in a chunk: what comes out is determined by what went in, so the
     * encoder is exact rather than merely good. */
    .encoder_tier = GAUD_ENCODER_EXACT,
    .magics = wav_magics,
    .magic_count = sizeof(wav_magics) / sizeof(wav_magics[0]),
    .probe = wav_probe,
    .open = gaud_wav_open,
    .close = gaud_wav_close,
    .decoder_open = gaud_wav_decoder_open,
    .encoder_open = gaud_wav_encoder_open,
};

/** @brief Register WAV when the shared library is loaded. */
GAUD_INIT_FUNCTION(gaud_wav_register_ctor) {
  gaud_registry_register(NULL, &wav_codec);
}

/**
 * @brief Register the WAV codec explicitly.
 *
 * The constructor above covers a shared library. It does **not** cover a
 * static archive: a constructor in an object nothing references by name is
 * dropped by the linker unless the consumer passes `--whole-archive`. This
 * is the path an application takes instead, and the reason both exist is
 * that a codec which only worked one of those two ways would work on the
 * author's machine and not on a user's.
 */
GAUD_API void gaud_wav_register(void) {
  gaud_registry_register(NULL, &wav_codec);
}
