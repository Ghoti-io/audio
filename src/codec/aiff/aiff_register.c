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
 * The AIFF codec's registration.
 */

#include "aiff_internal.h"
#include <ghoti.io/audio/codecs.h>
#include <string.h>

static const unsigned char form_magic[] = {'F', 'O', 'R', 'M'};
static const unsigned char aiff_magic[] = {'A', 'I', 'F', 'F'};
static const unsigned char aifc_magic[] = {'A', 'I', 'F', 'C'};

static const GAUD_Codec_Magic aiff_magics[] = {
    {0, form_magic, 4},
    {8, aiff_magic, 4},
    {8, aifc_magic, 4},
};

/* FORM at 0 is shared with every IFF format there is, so the form type at 8
 * is what actually decides. */
static GAUD_Result aiff_probe(const GAUD_Codec * codec, GAUD_Stream * stream,
    unsigned int * out_confidence) {
  (void)codec;
  *out_confidence = GAUD_CONFIDENCE_NONE;
  unsigned char header[12];
  uint64_t start = gaud_stream_tell(stream);
  size_t got = gaud_stream_read(stream, header, sizeof(header));
  gaud_stream_seek(stream, (int64_t)start, GAUD_SEEK_SET);
  if (got != sizeof(header) || memcmp(header, "FORM", 4) != 0) {
    return GAUD_OK;
  }
  if (memcmp(header + 8, "AIFF", 4) != 0
      && memcmp(header + 8, "AIFC", 4) != 0) {
    return GAUD_OK;
  }
  *out_confidence = GAUD_CONFIDENCE_CERTAIN;
  return GAUD_OK;
}

static const GAUD_Codec aiff_codec = {
    .abi_version = GAUD_CODEC_ABI_VERSION,
    .size = sizeof(GAUD_Codec),
    .name = "aiff",
    .ctx = NULL,
    .capabilities = GAUD_CAP_DECODE | GAUD_CAP_ENCODE,
    .encoder_tier = GAUD_ENCODER_EXACT,
    .magics = aiff_magics,
    .magic_count = sizeof(aiff_magics) / sizeof(aiff_magics[0]),
    .probe = aiff_probe,
    .open = gaud_aiff_open,
    .close = gaud_aiff_close,
    .decoder_open = gaud_aiff_decoder_open,
    .encoder_open = gaud_aiff_encoder_open,
};

/** @brief Register AIFF when the shared library is loaded. */
GAUD_INIT_FUNCTION(gaud_aiff_register_ctor) {
  gaud_registry_register(NULL, &aiff_codec);
}

/** @brief Register AIFF explicitly; see codecs.h for why both exist. */
void gaud_aiff_register(void) {
  gaud_registry_register(NULL, &aiff_codec);
}
