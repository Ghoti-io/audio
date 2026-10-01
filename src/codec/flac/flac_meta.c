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
 * FLAC's metadata blocks, as RFC 9639 section 8 specifies them.
 *
 * These are byte-aligned records with big-endian lengths, so nothing here
 * goes through the bit reader except STREAMINFO - whose fields are 20, 3, 5
 * and 36 bits and cross byte boundaries three times.
 *
 * **VORBIS_COMMENT is the exception to the byte order and it is not a
 * mistake in this file.** Every length in a FLAC metadata block is
 * big-endian except the ones inside a Vorbis comment, which are little-
 * endian because the block is Vorbis's structure carried unchanged. A
 * reader that applies FLAC's order to it reads a 42-byte vendor string as
 * 704643072 bytes. That parser lives in `src/meta/vorbis_comment.c`,
 * because Opus and Vorbis carry the identical structure in phase 6 and it
 * is one thing with three homes rather than three things.
 */

#include "../../meta/id3_internal.h"
#include "../../meta/scheme.h"
#include "../shared/bytes.h"
#include "flac_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

static void note(GAUD_Diagnostics * diagnostics, uint64_t offset,
    GAUD_Diag_Severity severity, const char * message) {
  if (!diagnostics) {
    return;
  }
  GAUD_Diagnostic entry = {
      .codec_name = "flac",
      .offset = offset,
      .element_id = 0,
      .severity = severity,
      .recommended_action = message,
  };
  (void)gaud_diagnostics_append(diagnostics, &entry);
}

GAUD_Result gaud_flac_parse_streaminfo(
    const unsigned char * data, size_t size, FLAC_Streaminfo * out) {
  if (size < FLAC_STREAMINFO_SIZE) {
    return GAUD_ERR_CORRUPT;
  }
  FLAC_Bits br;
  gaud_flac_bits_init(&br, data, FLAC_STREAMINFO_SIZE);
  out->min_block_size = (uint16_t)gaud_flac_bits_read(&br, 16u);
  out->max_block_size = (uint16_t)gaud_flac_bits_read(&br, 16u);
  out->min_frame_size = gaud_flac_bits_read(&br, 24u);
  out->max_frame_size = gaud_flac_bits_read(&br, 24u);
  out->sample_rate = gaud_flac_bits_read(&br, 20u);
  out->channels = gaud_flac_bits_read(&br, 3u) + 1u;
  out->bits_per_sample = gaud_flac_bits_read(&br, 5u) + 1u;
  out->total_samples = gaud_flac_bits_read64(&br, 36u);
  memcpy(out->md5, data + 18, sizeof(out->md5));

  /* The format's own floors, not this library's policy - GAUD_Limits is
   * where policy lives and it is applied by the caller. A block size below
   * 16 and a bit depth below 4 are forbidden by RFC 9639 section 8.2; a
   * sample rate of zero means a stream that is not audio, which is legal
   * in the block and not something this codec can produce a track from. */
  if (out->min_block_size < 16u || out->max_block_size < 16u) {
    return GAUD_ERR_CORRUPT;
  }
  if (out->min_block_size > out->max_block_size) {
    return GAUD_ERR_CORRUPT;
  }
  if (out->bits_per_sample < 4u || out->bits_per_sample > 32u) {
    return GAUD_ERR_CORRUPT;
  }
  if (out->sample_rate == 0) {
    return GAUD_ERR_UNSUPPORTED;
  }
  return GAUD_OK;
}

GAUD_Result gaud_flac_parse_seektable(FLAC_File * file,
    const unsigned char * data, size_t size, const GAUD_Limits * limits,
    GAUD_Diagnostics * diagnostics) {
  if (size % 18u != 0) {
    note(diagnostics, 0, GAUD_DIAG_WARNING,
        "seek table length is not a multiple of a seek point; ignored");
    return GAUD_OK;
  }
  size_t stated = size / 18u;
  if (stated > limits->max_cue_points) {
    return GAUD_ERR_LIMIT;
  }
  if (stated == 0) {
    return GAUD_OK;
  }

  FLAC_Seek_Point * points
      = gcu_allocator_malloc(file->allocator, stated * sizeof(*points));
  if (!points) {
    return GAUD_ERR_OOM;
  }
  size_t kept = 0;
  uint64_t previous_sample = 0;
  bool ordered = true;
  for (size_t i = 0; i < stated; ++i) {
    const unsigned char * p = data + i * 18u;
    uint64_t sample = ((uint64_t)gaud_rd_u32be(p) << 32) | gaud_rd_u32be(p + 4);
    if (sample == UINT64_MAX) {
      /* A placeholder. An encoder writes these to reserve room it will
       * fill in on a second pass, and one left in a finished file carries
       * no offset at all - keeping it would put a point in the table whose
       * only effect is to send a seek to byte zero. */
      continue;
    }
    if (kept && sample <= previous_sample) {
      ordered = false;
    }
    previous_sample = sample;
    points[kept].sample = sample;
    points[kept].offset
        = ((uint64_t)gaud_rd_u32be(p + 8) << 32) | gaud_rd_u32be(p + 12);
    points[kept].frame_samples = gaud_rd_u16be(p + 16);
    ++kept;
  }

  if (!ordered) {
    /* RFC 9639 requires the table to be sorted and unique, and a seek
     * against an unsorted one lands wherever the binary search happens to
     * stop. Dropping the whole table costs a slower seek and nothing else,
     * because the decoder can always scan; trusting it costs correctness. */
    note(diagnostics, 0, GAUD_DIAG_WARNING,
        "seek table is not in ascending sample order; ignored");
    gcu_allocator_free(file->allocator, points);
    return GAUD_OK;
  }

  gcu_allocator_free(file->allocator, file->seek_points);
  file->seek_points = points;
  file->seek_count = kept;
  return GAUD_OK;
}

GAUD_Result gaud_flac_parse_picture(const unsigned char * data, size_t size,
    const GAUD_Limits * limits, GAUD_Meta * meta,
    GAUD_Diagnostics * diagnostics) {
  /* Four 32-bit fields before the MIME string and five after it, so a
   * block shorter than 32 bytes cannot even hold its own lengths. */
  if (size < 32u) {
    note(diagnostics, 0, GAUD_DIAG_WARNING,
        "picture block too short for its own header; ignored");
    return GAUD_OK;
  }
  size_t at = 0;
  uint32_t type = gaud_rd_u32be(data + at);
  at += 4;

  uint32_t mime_length = gaud_rd_u32be(data + at);
  at += 4;
  if (mime_length > size - at) {
    return GAUD_ERR_CORRUPT;
  }
  const unsigned char * mime = data + at;
  at += mime_length;

  if (size - at < 4u) {
    return GAUD_ERR_CORRUPT;
  }
  uint32_t description_length = gaud_rd_u32be(data + at);
  at += 4;
  if (description_length > size - at) {
    return GAUD_ERR_CORRUPT;
  }
  const unsigned char * description = data + at;
  at += description_length;

  if (size - at < 20u) {
    return GAUD_ERR_CORRUPT;
  }
  uint32_t width = gaud_rd_u32be(data + at);
  uint32_t height = gaud_rd_u32be(data + at + 4);
  uint32_t depth = gaud_rd_u32be(data + at + 8);
  uint32_t colors = gaud_rd_u32be(data + at + 12);
  uint32_t payload_length = gaud_rd_u32be(data + at + 16);
  at += 20;
  if (payload_length > size - at) {
    return GAUD_ERR_CORRUPT;
  }
  if (payload_length > limits->max_picture_bytes) {
    return GAUD_ERR_LIMIT;
  }

  /* Both strings are length-prefixed and neither is terminated, so they
   * are copied out rather than pointed at. The MIME type is ASCII by the
   * specification and the description is UTF-8; `gaud_id3_bytes_to_utf8`
   * is the shared "make this valid UTF-8 or say why not" path, and using
   * it here rather than a second copy is what keeps a description that is
   * mis-encoded in a FLAC file behaving the way one in an ID3 frame does. */
  const GAUD_Allocator * allocator = gaud_meta_allocator(meta);
  char * mime_text = gaud_id3_bytes_to_utf8(allocator, mime, mime_length);
  char * description_text
      = gaud_id3_bytes_to_utf8(allocator, description, description_length);
  GAUD_Result result = GAUD_ERR_OOM;
  if (mime_text && description_text) {
    /* The kind mapping is ID3's, shared rather than copied: the PICTURE
     * block's type numbers are the APIC registry. */
    result = gaud_meta_picture_add_stated(meta,
        gaud_picture_kind_from_apic(type), mime_text, description_text,
        data + at, payload_length, width, height, depth, colors);
  }
  gcu_allocator_free(allocator, mime_text);
  gcu_allocator_free(allocator, description_text);
  return result;
}

GAUD_Result gaud_flac_check_cuesheet(const unsigned char * data, size_t size,
    const GAUD_Limits * limits, uint32_t * out_tracks,
    GAUD_Diagnostics * diagnostics) {
  /*
   * The fixed header, counted in bits because that is how RFC 9639
   * section 8.6 states it and counting it in bytes is how this was first
   * got wrong: 128 bytes of catalogue number, 64 bits of lead-in, **1 bit
   * of CD flag plus 7 + 258 x 8 bits reserved**, and 8 bits of track
   * count. The flag and the reserved field together are 259 bytes and the
   * count is a 260th, so the header is 396 bytes and not 395.
   *
   * One byte out made every cuesheet in the world fail the check below
   * and be dropped - and because the check exists to refuse corrupt
   * blocks, that read as the blocks being corrupt. A structural check
   * that is slightly wrong does not pass the bad case, it refuses the
   * good one, and nothing in this library could tell the difference:
   * `metaflac` reading the file afterwards is what said the block had
   * been there.
   */
  const size_t fixed = 128u + 8u + 259u + 1u;
  if (size < fixed) {
    note(diagnostics, 0, GAUD_DIAG_WARNING,
        "cuesheet shorter than its fixed header; carried without checking");
    return GAUD_ERR_CORRUPT;
  }
  uint32_t tracks = data[fixed - 1u];
  size_t at = fixed;
  uint32_t total_indices = 0;
  for (uint32_t t = 0; t < tracks; ++t) {
    /* 8 bytes of offset, 1 of number, 12 of ISRC, 14 of flags and
     * reserved, 1 of index count. */
    if (size - at < 36u) {
      return GAUD_ERR_CORRUPT;
    }
    uint32_t indices = data[at + 35u];
    at += 36u;
    if (indices > (size - at) / 12u) {
      return GAUD_ERR_CORRUPT;
    }
    at += (size_t)indices * 12u;
    total_indices += indices;
    if (total_indices > limits->max_cue_points) {
      return GAUD_ERR_LIMIT;
    }
  }
  if (at != size) {
    /* Trailing bytes mean the counts and the block length disagree, and a
     * reader that believed the counts would be reading someone else's
     * data. This is the whole reason the block is checked rather than
     * simply carried. */
    return GAUD_ERR_CORRUPT;
  }
  if (out_tracks) {
    *out_tracks = tracks;
  }
  return GAUD_OK;
}
