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
 * Registering Opus, and the third probe that has to look past `OggS`.
 */

#include "opus_internal.h"
#include <string.h>

/**
 * @brief ::GAUD_Codec::probe.
 *
 * Certain or nothing. RFC 7845 requires `OpusHead` to be the first
 * packet of the first page and to be alone on it, so the magic is at a
 * fixed offset once the segment table's one byte is counted.
 */
static GAUD_Result opus_probe(
    const GAUD_Codec * codec, GAUD_Stream * stream, unsigned int * out) {
  (void)codec;
  *out = GAUD_CONFIDENCE_NONE;
  unsigned char head[OGG_HEADER_FIXED + 1u + OPUS_MAGIC_SIZE];
  uint64_t start = gaud_stream_tell(stream);
  size_t got = gaud_stream_read(stream, head, sizeof(head));
  (void)gaud_stream_seek(stream, (int64_t)start, GAUD_SEEK_SET);
  if (got != sizeof(head) || memcmp(head, OGG_MAGIC, 4) != 0) {
    return GAUD_OK;
  }
  if (head[26] != 1u) {
    return GAUD_OK;
  }
  if (memcmp(head + OGG_HEADER_FIXED + 1u, OPUS_HEAD_MAGIC,
          OPUS_MAGIC_SIZE)
      != 0) {
    return GAUD_OK;
  }
  *out = GAUD_CONFIDENCE_CERTAIN;
  return GAUD_OK;
}

static const unsigned char ogg_magic[] = {'O', 'g', 'g', 'S'};

static const GAUD_Codec_Magic ogg_magics[] = {
    {0, ogg_magic, 4},
};

/**
 * The codec itself.
 *
 * **`GAUD_CAP_DECODE` is not declared**, and this commit is the half of
 * Opus with no decoder in it: the two headers, the channel count, the
 * tags, and the length - which for Opus is the last page's granule
 * position *minus the pre-skip*, a subtraction no other format here
 * needs because no other format states one. planning/audio.md section
 * 11.18 is the argument for shipping that on its own; its last paragraph
 * is the warning that it must not become a resting place.
 *
 * The sample rate a track reports is always 48,000. That is the format's
 * decision and not this library's: a decoder may output at any of five
 * rates, the granule positions are counted at 48 kHz, and the rate in
 * `OpusHead` is marked informational by the specification that defines
 * it.
 */
static const GAUD_Codec opus_codec = {
    .abi_version = GAUD_CODEC_ABI_VERSION,
    .size = sizeof(GAUD_Codec),
    .name = "opus",
    .ctx = NULL,
    .capabilities = GAUD_CAP_METADATA_READ,
    .encoder_tier = GAUD_ENCODER_NONE,
    .magics = ogg_magics,
    .magic_count = sizeof(ogg_magics) / sizeof(ogg_magics[0]),
    .probe = opus_probe,
    .open = gaud_opus_open,
    .close = gaud_opus_close,
    .decoder_open = NULL,
    .encoder_open = NULL,
};

/** @brief Register Opus when the shared library is loaded. */
GAUD_INIT_FUNCTION(gaud_opus_register_ctor) {
  gaud_registry_register(NULL, &opus_codec);
}

GAUD_API void gaud_opus_register(void) {
  gaud_registry_register(NULL, &opus_codec);
}
