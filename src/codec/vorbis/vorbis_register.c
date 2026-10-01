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
 * Registering Vorbis, and the probe that has to look past a shared magic.
 *
 * `OggS` says only that this is an Ogg file. FLAC, Vorbis, Opus, Speex and
 * Theora all begin with it, and three of those are registered here - so
 * the magic selects a family and the probe decides which member. This is
 * the arrangement planning/audio.md's phase 1 note anticipated: a probe
 * must be able to veto its own magic.
 */

#include "vorbis_internal.h"
#include <string.h>

/**
 * @brief ::GAUD_Codec::probe.
 *
 * Certain or nothing, with no middle answer, and that is the format being
 * easy rather than this being lax. The identification header is required
 * to be the first packet of the first page and to be alone on it, so the
 * signature is at a fixed offset from the start of the file once the
 * segment table's one byte is counted. There is no "likely": either those
 * seven bytes are there or this is some other Ogg mapping.
 */
static GAUD_Result vorbis_probe(const GAUD_Codec * codec,
    GAUD_Stream * stream, unsigned int * out_confidence) {
  (void)codec;
  *out_confidence = GAUD_CONFIDENCE_NONE;
  unsigned char head[OGG_HEADER_FIXED + 1u + VORBIS_HEAD_SIZE];
  uint64_t start = gaud_stream_tell(stream);
  size_t got = gaud_stream_read(stream, head, sizeof(head));
  /* A probe must leave the stream where it found it: the registry probes
   * every codec from the same position. */
  (void)gaud_stream_seek(stream, (int64_t)start, GAUD_SEEK_SET);
  if (got != sizeof(head) || memcmp(head, OGG_MAGIC, 4) != 0) {
    return GAUD_OK;
  }
  /* One segment in the first page's table. The identification header is
   * 30 bytes, so it is always a single lacing value. */
  if (head[26] != 1u) {
    return GAUD_OK;
  }
  if (!gaud_vorbis_is_header(head + OGG_HEADER_FIXED + 1u, VORBIS_HEAD_SIZE,
          VORBIS_PACKET_IDENTIFICATION)) {
    return GAUD_OK;
  }
  *out_confidence = GAUD_CONFIDENCE_CERTAIN;
  return GAUD_OK;
}

static const unsigned char ogg_magic[] = {'O', 'g', 'g', 'S'};

static const GAUD_Codec_Magic ogg_magics[] = {
    {0, ogg_magic, 4},
};

/**
 * The codec itself.
 *
 * `GAUD_CAP_DECODE` holds for every floor type 1 stream, which is every
 * stream any encoder writes. **Floor type 0 is identified and refused**,
 * per track rather than by a capability bit: it is a line spectral pair
 * curve needing a cosine and a square root per spectral line in a
 * decoder this library promises to keep integer and byte-identical
 * everywhere, and - the deciding reason - nothing in the oracle image
 * produces one, so a fixed-point approximation of it could not be
 * scored. src/codec/vorbis/vorbis_floor.c says it at length.
 *
 * No encoder: phase 8 brings the perceptual ones, with the two-gate
 * harness their output needs.
 */
static const GAUD_Codec vorbis_codec = {
    .abi_version = GAUD_CODEC_ABI_VERSION,
    .size = sizeof(GAUD_Codec),
    .name = "vorbis",
    .ctx = NULL,
    .capabilities = GAUD_CAP_DECODE | GAUD_CAP_METADATA_READ,
    .encoder_tier = GAUD_ENCODER_NONE,
    .magics = ogg_magics,
    .magic_count = sizeof(ogg_magics) / sizeof(ogg_magics[0]),
    .probe = vorbis_probe,
    .open = gaud_vorbis_open,
    .close = gaud_vorbis_close,
    .decoder_open = gaud_vorbis_decoder_open,
    .encoder_open = NULL,
};

/** @brief Register Vorbis when the shared library is loaded. */
GAUD_INIT_FUNCTION(gaud_vorbis_register_ctor) {
  gaud_registry_register(NULL, &vorbis_codec);
}

GAUD_API void gaud_vorbis_register(void) {
  gaud_registry_register(NULL, &vorbis_codec);
}
