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
 * Registering the MPEG audio codec, and the probe that is the whole of its
 * identification.
 *
 * **This codec declares no magics at all**, and it is the first here that
 * cannot. A WAV is `RIFF` at offset zero; an MPEG audio stream's first
 * frame may be at offset zero, or after an ID3v2 tag of any size, or after
 * bytes that are neither - and the thing at that offset is eleven bits of
 * sync, which is not a signature but a value that occurs by chance in
 * roughly one byte pair in every few hundred of any compressed data. So the
 * probe does the work, and what makes it an answer rather than a guess is
 * that it follows the candidate frame's own stated length and requires
 * frames to be where that arithmetic says they are.
 */

#include "mp3_internal.h"

/** How many following frames the probe wants before it says "certain". */
#define MP3_PROBE_CHAIN 2u

/**
 * @brief ::GAUD_Codec::probe.
 *
 * Three answers rather than two, because the position of the first frame
 * is itself evidence:
 *
 * - **Certain**: a frame at the very start of the data - offset zero, or
 *   immediately after an ID3v2 tag - with following frames where its
 *   length says they should be. That is what every file any encoder wrote
 *   looks like.
 * - **Likely**: the same, found further in. Real files do look like this
 *   after a lossy edit or a partial download, and so does a WAV whose
 *   samples happen to contain a sync word - so this is a claim another
 *   codec's certainty can and should beat.
 * - **Weak**: a frame whose following frames could not be checked because
 *   the stream ends inside the first one. A single-frame file is a
 *   legitimate thing to open and there is nothing left to confirm it with.
 */
static GAUD_Result mp3_probe(const GAUD_Codec * codec, GAUD_Stream * stream,
    unsigned int * out_confidence) {
  (void)codec;
  *out_confidence = GAUD_CONFIDENCE_NONE;

  uint64_t origin = gaud_stream_tell(stream);
  uint64_t id3v2 = gaud_mp3_id3v2_span(stream);
  uint64_t offset = 0;
  MP3_Header header;
  unsigned chain = 0;
  GAUD_Result found = gaud_mp3_find_frame(
      stream, origin + id3v2, MP3_SYNC_SEARCH, &offset, &header, &chain);
  /* A probe must leave the stream where it found it, whatever it concluded;
   * the registry probes every codec from the same position. */
  (void)gaud_stream_seek(stream, (int64_t)origin, GAUD_SEEK_SET);
  if (found != GAUD_OK) {
    return GAUD_OK;
  }
  if (header.frame_size == 0u) {
    /* Free format, which this library refuses to open. Claiming it here
     * and refusing it in the open would make gaud_doc_load() answer
     * GAUD_ERR_UNSUPPORTED, which is the right answer - a lower confidence
     * would instead hand the file to whichever codec guessed next. */
    *out_confidence = GAUD_CONFIDENCE_LIKELY;
    return GAUD_OK;
  }
  if (chain < MP3_PROBE_CHAIN) {
    *out_confidence = GAUD_CONFIDENCE_WEAK;
  }
  else if (offset == origin + id3v2) {
    *out_confidence = GAUD_CONFIDENCE_CERTAIN;
  }
  else {
    *out_confidence = GAUD_CONFIDENCE_LIKELY;
  }
  return GAUD_OK;
}

/**
 * The codec itself.
 *
 * `GAUD_CAP_DECODE` is **not** declared and `decoder_open` is NULL: this
 * phase reads what the stream says about itself and does not yet decode a
 * sample. The capability bits are what a caller tests, so declaring one
 * that is not there would be a lie a program could act on; a track asked
 * for a decoder answers ::GAUD_ERR_UNSUPPORTED through the ordinary path.
 */
static const GAUD_Codec mp3_codec = {
    .abi_version = GAUD_CODEC_ABI_VERSION,
    .size = sizeof(GAUD_Codec),
    .name = "mp3",
    .ctx = NULL,
    .capabilities = GAUD_CAP_METADATA_READ,
    .encoder_tier = GAUD_ENCODER_NONE,
    .magics = NULL,
    .magic_count = 0,
    .probe = mp3_probe,
    .open = gaud_mp3_open,
    .close = gaud_mp3_close,
    .decoder_open = NULL,
    .encoder_open = NULL,
};

/** @brief Register MPEG audio when the shared library is loaded. */
GAUD_INIT_FUNCTION(gaud_mp3_register_ctor) {
  gaud_registry_register(NULL, &mp3_codec);
}

GAUD_API void gaud_mp3_register(void) {
  gaud_registry_register(NULL, &mp3_codec);
}
