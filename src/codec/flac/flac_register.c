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
 * The FLAC codec's registration.
 *
 * Built the way an out-of-tree codec is built, as WAV and AIFF are: a
 * file-scope ::GAUD_Codec handed to the public registry, with no internal
 * shortcut.
 *
 * The probe is short here and that is a property of the format rather
 * than an omission. `fLaC` at offset zero is a four-byte signature that
 * nothing else claims - unlike `RIFF`, which AVI shares, and `FORM`,
 * which every IFF file shares - so the signature alone is as certain as
 * the probe could be. What the probe adds is the block header that must
 * follow: byte four's low seven bits are a block type and the first block
 * must be STREAMINFO, so a file whose fifth byte is not zero or 0x80 is a
 * `fLaC` that is not a FLAC stream.
 */

#include "flac_internal.h"
#include <string.h>

static const unsigned char flac_magic[] = {'f', 'L', 'a', 'C'};

static const GAUD_Codec_Magic flac_magics[] = {
    {0, flac_magic, 4},
};

static GAUD_Result flac_probe(const GAUD_Codec * codec, GAUD_Stream * stream,
    unsigned int * out_confidence) {
  (void)codec;
  *out_confidence = GAUD_CONFIDENCE_NONE;

  unsigned char header[5];
  uint64_t start = gaud_stream_tell(stream);
  size_t got = gaud_stream_read(stream, header, sizeof(header));
  gaud_stream_seek(stream, (int64_t)start, GAUD_SEEK_SET);
  if (got != sizeof(header)) {
    return GAUD_OK;
  }
  if (memcmp(header, FLAC_MAGIC, 4) != 0) {
    return GAUD_OK;
  }
  /* The first metadata block must be STREAMINFO, so its type is 0 and the
   * only other bit in the byte is the last-block flag. */
  if ((header[4] & 0x7Fu) != FLAC_BLOCK_STREAMINFO) {
    return GAUD_OK;
  }
  *out_confidence = GAUD_CONFIDENCE_CERTAIN;
  return GAUD_OK;
}

static const GAUD_Codec flac_codec = {
    .abi_version = GAUD_CODEC_ABI_VERSION,
    .size = sizeof(GAUD_Codec),
    .name = "flac",
    .ctx = NULL,
    .capabilities = GAUD_CAP_DECODE | GAUD_CAP_ENCODE,
    /* Lossless: what comes out of the decoder is what went into the
     * encoder, sample for sample, and the file's own MD5 says so. */
    .encoder_tier = GAUD_ENCODER_EXACT,
    .magics = flac_magics,
    .magic_count = sizeof(flac_magics) / sizeof(flac_magics[0]),
    .probe = flac_probe,
    .open = gaud_flac_open,
    .close = gaud_flac_close,
    .decoder_open = gaud_flac_decoder_open,
    .encoder_open = gaud_flac_encoder_open,
};

/** @brief Register FLAC when the shared library is loaded. */
GAUD_INIT_FUNCTION(gaud_flac_register_ctor) {
  gaud_registry_register(NULL, &flac_codec);
}

/**
 * @brief Register the FLAC codec explicitly.
 *
 * The constructor above covers a shared library and not a static archive,
 * where an object nothing references by name is dropped before its
 * constructor runs. Both exist for the reason WAV's do: a codec that only
 * worked one of those two ways would work on the author's machine.
 */
GAUD_API void gaud_flac_register(void) {
  gaud_registry_register(NULL, &flac_codec);
}
