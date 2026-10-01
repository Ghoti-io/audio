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
 * Opening a native FLAC stream: the magic, the metadata blocks, the track.
 *
 * No samples are decoded here. `gaud_doc_load()` is what a tagger and a
 * library scanner call and it must stay bounded by the header, which for
 * FLAC means the metadata blocks and nothing past the first frame's first
 * byte.
 *
 * **STREAMINFO is mandatory and must come first**, and this refuses a file
 * without it rather than reconstructing one from the first frame. A frame
 * header states the rate, depth and channel count, so a reconstruction is
 * possible and libFLAC will do it; what it cannot state is the total
 * sample count or the MD5, and a document that reported an unknown
 * duration and an unverifiable decode while looking exactly like one that
 * did not is worse than a refusal.
 */

#include "../../core/meta_internal.h"
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

bool gaud_flac_format_for_depth(uint32_t bits, GAUD_Sample_Format * out) {
  switch (bits) {
  case 8u:
    *out = GAUD_SAMPLE_S8;
    return true;
  case 16u:
    *out = GAUD_SAMPLE_S16;
    return true;
  case 24u:
    *out = GAUD_SAMPLE_S24;
    return true;
  case 32u:
    *out = GAUD_SAMPLE_S32;
    return true;
  default:
    return false;
  }
}

void gaud_flac_file_free(FLAC_File * file) {
  if (!file) {
    return;
  }
  gcu_allocator_free(file->allocator, file->seek_points);
  gcu_allocator_free(file->allocator, file);
}

/**
 * Read one metadata block's header and body.
 *
 * @param out_body Receives an allocated body, or NULL for an empty block.
 *   The caller frees it.
 */
static GAUD_Result read_block(GAUD_Stream * stream, const GAUD_Limits * limits,
    const GAUD_Allocator * allocator, bool * out_last, unsigned * out_type,
    unsigned char ** out_body, size_t * out_size) {
  unsigned char header[FLAC_BLOCK_HEADER];
  if (gaud_stream_read(stream, header, sizeof(header)) != sizeof(header)) {
    return GAUD_ERR_CORRUPT;
  }
  *out_last = (header[0] & 0x80u) != 0;
  *out_type = header[0] & 0x7Fu;
  uint32_t size = ((uint32_t)header[1] << 16) | ((uint32_t)header[2] << 8)
      | (uint32_t)header[3];
  *out_size = size;
  *out_body = NULL;
  if (size > limits->max_element_size) {
    return GAUD_ERR_LIMIT;
  }
  if (size == 0) {
    return GAUD_OK;
  }
  unsigned char * body = gcu_allocator_malloc(allocator, size);
  if (!body) {
    return GAUD_ERR_OOM;
  }
  if (gaud_stream_read(stream, body, size) != size) {
    gcu_allocator_free(allocator, body);
    return GAUD_ERR_CORRUPT;
  }
  *out_body = body;
  return GAUD_OK;
}

/** @brief ::GAUD_Codec::open: the magic, the metadata blocks, the track. */
GAUD_Result gaud_flac_open(const GAUD_Codec * codec, GAUD_Stream * stream,
    const GAUD_Limits * limits, GAUD_Diagnostics * diagnostics,
    GAUD_Doc ** out_doc) {
  unsigned char magic[4];
  if (gaud_stream_read(stream, magic, sizeof(magic)) != sizeof(magic)) {
    return GAUD_ERR_CORRUPT;
  }
  if (memcmp(magic, FLAC_MAGIC, 4) != 0) {
    return GAUD_ERR_FORMAT;
  }

  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);
  GAUD_Result failure = GAUD_ERR_INTERNAL;
  GAUD_Meta * meta = NULL;
  FLAC_File * file = NULL;
  unsigned char * body = NULL;
  GAUD_Doc * doc = NULL;

  GAUD_Result result = gaud_meta_create(allocator, &meta);
  if (result != GAUD_OK) {
    return result;
  }
  file = gcu_allocator_malloc(allocator, sizeof(*file));
  if (!file) {
    failure = GAUD_ERR_OOM;
    goto Fail;
  }
  memset(file, 0, sizeof(*file));
  file->allocator = allocator;
  file->frames_length = UINT64_MAX;

  bool have_streaminfo = false;
  bool last = false;
  uint64_t metadata_bytes = 0;
  uint32_t blocks = 0;
  while (!last) {
    if (++blocks > limits->max_metadata_entries) {
      failure = GAUD_ERR_LIMIT;
      goto Fail;
    }
    uint64_t at = gaud_stream_tell(stream);
    unsigned type = 0;
    size_t size = 0;
    result = read_block(
        stream, limits, allocator, &last, &type, &body, &size);
    if (result != GAUD_OK) {
      failure = result;
      goto Fail;
    }
    metadata_bytes += size;
    if (metadata_bytes > limits->max_metadata_bytes) {
      failure = GAUD_ERR_LIMIT;
      goto Fail;
    }

    if (type == FLAC_BLOCK_STREAMINFO) {
      if (have_streaminfo) {
        /* A second STREAMINFO is not a block this file may have. Keeping
         * the first and warning would leave the document describing one
         * stream and the file claiming two. */
        failure = GAUD_ERR_CORRUPT;
        goto Fail;
      }
      if (blocks != 1u) {
        failure = GAUD_ERR_CORRUPT;
        goto Fail;
      }
      result = gaud_flac_parse_streaminfo(body, size, &file->info);
      if (result != GAUD_OK) {
        failure = result;
        goto Fail;
      }
      have_streaminfo = true;
    }
    else if (!have_streaminfo) {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
    else if (type == FLAC_BLOCK_VORBIS_COMMENT) {
      result
          = gaud_vorbis_comment_parse(body, size, limits, meta, diagnostics);
      if (result != GAUD_OK) {
        failure = result;
        goto Fail;
      }
    }
    else if (type == FLAC_BLOCK_PICTURE) {
      result = gaud_flac_parse_picture(body, size, limits, meta, diagnostics);
      if (result != GAUD_OK) {
        failure = result;
        goto Fail;
      }
    }
    else if (type == FLAC_BLOCK_SEEKTABLE) {
      result = gaud_flac_parse_seektable(
          file, body, size, limits, diagnostics);
      if (result != GAUD_OK) {
        failure = result;
        goto Fail;
      }
      /* **Parsed and deliberately not carried raw.** Every other block
       * this file does not interpret is kept verbatim so a round trip
       * puts it back; a seek table must not be, because its offsets are
       * byte positions into one particular file's frames. Re-encoding
       * moves every frame, so carrying the old table forward would write
       * a table of confident wrong answers - worse than none, since a
       * decoder with no table falls back to scanning and gets there. The
       * writer builds its own from the frames it actually wrote. */
    }
    else if (type == FLAC_BLOCK_CUESHEET) {
      uint32_t tracks = 0;
      result = gaud_flac_check_cuesheet(
          body, size, limits, &tracks, diagnostics);
      if (result == GAUD_ERR_LIMIT) {
        failure = result;
        goto Fail;
      }
      if (result != GAUD_OK) {
        note(diagnostics, at, GAUD_DIAG_WARNING,
            "cuesheet block does not add up; dropped rather than carried");
      }
      else {
        result = gaud_meta_raw_attach(meta, "flac", "CUESHEET", body, size);
        if (result != GAUD_OK) {
          failure = result;
          goto Fail;
        }
      }
    }
    else if (type == FLAC_BLOCK_PADDING) {
      /* Deliberately not carried. Padding is room an encoder left for a
       * tagger to grow into, and carrying it forward would mean a file
       * that gained a tag also kept the hole the tag was supposed to
       * fill, growing by the size of both. The writer makes its own. */
    }
    else if (type == FLAC_BLOCK_INVALID) {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
    else {
      /* APPLICATION, and whatever a later revision defines. Carried
       * verbatim under its numeric type so that a round trip puts it back
       * exactly where it was. */
      char id[16];
      if (type == FLAC_BLOCK_APPLICATION) {
        memcpy(id, "APPLICATION", 12);
      }
      else {
        /* No snprintf: two digits, written by hand, so this file needs no
         * <stdio.h> and cannot depend on a locale. */
        memcpy(id, "BLOCK_", 6);
        id[6] = (char)('0' + (type / 10u) % 10u);
        id[7] = (char)('0' + type % 10u);
        id[8] = '\0';
      }
      result = gaud_meta_raw_attach(meta, "flac", id, body, size);
      if (result != GAUD_OK) {
        failure = result;
        goto Fail;
      }
    }

    gcu_allocator_free(allocator, body);
    body = NULL;
  }

  if (!have_streaminfo) {
    failure = GAUD_ERR_CORRUPT;
    goto Fail;
  }
  if (file->info.channels > limits->max_channels) {
    failure = GAUD_ERR_LIMIT;
    goto Fail;
  }
  if (file->info.sample_rate > limits->max_sample_rate) {
    failure = GAUD_ERR_LIMIT;
    goto Fail;
  }
  if (file->info.total_samples > limits->max_frames) {
    failure = GAUD_ERR_LIMIT;
    goto Fail;
  }

  GAUD_Sample_Format format = GAUD_SAMPLE_S16;
  if (!gaud_flac_format_for_depth(file->info.bits_per_sample, &format)) {
    note(diagnostics, 0, GAUD_DIAG_ERROR,
        "bit depth has no lossless sample format in this library");
    failure = GAUD_ERR_UNSUPPORTED;
    goto Fail;
  }

  file->first_frame_offset = gaud_stream_tell(stream);
  uint64_t stream_size = 0;
  if (gaud_stream_size(stream, &stream_size) == GAUD_OK
      && stream_size >= file->first_frame_offset) {
    file->frames_length = stream_size - file->first_frame_offset;
  }

  result = gaud_doc_create_internal(codec, stream, allocator, &doc);
  if (result != GAUD_OK) {
    failure = result;
    goto Fail;
  }
  /* Attached before the track is added, not after. Once the document
   * exists, gaud_doc_destroy() calls this codec's close, and close frees
   * whatever the private pointer holds - so anything that fails between
   * here and the end is cleaned up by the ordinary path. Setting it
   * afterwards leaves a window in which a failed gaud_doc_add_track()
   * destroys a document that has forgotten about the state it owns. */
  gaud_doc_set_private(doc, file);

  GAUD_Track_Desc desc;
  memset(&desc, 0, sizeof(desc));
  desc.format = format;
  desc.coding = GAUD_CODING_FLAC;
  desc.sample_rate = file->info.sample_rate;
  desc.layout = gaud_channel_layout_default(file->info.channels);
  desc.sample_layout = GAUD_LAYOUT_INTERLEAVED;
  /* Zero total samples means "the encoder did not know", which is what a
   * stream written to a pipe says - not a file of no length. */
  desc.frames = file->info.total_samples ? file->info.total_samples
                                         : UINT64_MAX;
  desc.data_offset = file->first_frame_offset;
  desc.data_length = file->frames_length;
  desc.codec_private = file;
  result = gaud_doc_add_track(doc, &desc, NULL);
  if (result != GAUD_OK) {
    failure = result;
    goto Fail;
  }

  gaud_meta_verify_pictures(meta);
  gaud_doc_take_meta(doc, meta);
  *out_doc = doc;
  return GAUD_OK;

Fail:
  gcu_allocator_free(allocator, body);
  if (doc) {
    gaud_doc_destroy(doc);
  }
  else {
    gaud_flac_file_free(file);
  }
  gaud_meta_destroy(meta);
  return failure;
}

/** @brief ::GAUD_Codec::close: release what the open allocated. */
void gaud_flac_close(const GAUD_Codec * codec, GAUD_Doc * doc) {
  (void)codec;
  gaud_flac_file_free(gaud_doc_private(doc));
}
