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
 * Documents and tracks: what a codec builds, and what a caller reads.
 */

#include "doc_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

GAUD_Result gaud_doc_create_internal(const GAUD_Codec * codec,
    GAUD_Stream * stream, const GAUD_Allocator * allocator,
    GAUD_Doc ** out_doc) {
  if (!codec || !stream || !out_doc) {
    return GAUD_ERR_INVALID;
  }
  GAUD_Doc * doc = gcu_allocator_malloc(allocator, sizeof(GAUD_Doc));
  if (!doc) {
    return GAUD_ERR_OOM;
  }
  *doc = (GAUD_Doc){
      .allocator = allocator,
      .codec = codec,
      .stream = stream,
  };
  /* Created empty rather than lazily, so gaud_doc_meta() never returns
   * NULL and no caller has to tell "this file had no tags" from "there is
   * nothing to ask". A codec that reads no metadata leaves it empty. */
  GAUD_Result result = gaud_meta_create(allocator, &doc->meta);
  if (result != GAUD_OK) {
    gcu_allocator_free(allocator, doc);
    return result;
  }
  *out_doc = doc;
  return GAUD_OK;
}

GAUD_Meta * gaud_doc_meta(const GAUD_Doc * doc) {
  return doc ? doc->meta : NULL;
}

void gaud_doc_take_meta(GAUD_Doc * doc, GAUD_Meta * meta) {
  if (!doc || !meta || doc->meta == meta) {
    return;
  }
  gaud_meta_destroy(doc->meta);
  doc->meta = meta;
}

GAUD_Result gaud_doc_add_track(
    GAUD_Doc * doc, const GAUD_Track_Desc * desc, GAUD_Track ** out_track) {
  if (!doc || !desc) {
    return GAUD_ERR_INVALID;
  }
  /* A rate or channel count of zero is a file nothing can play, and a layout
   * whose mask contradicts its count is the defect GAUD_Channel_Layout was
   * introduced to prevent. Refusing here means no codec has to remember to. */
  if (desc->sample_rate == 0 || !gaud_channel_layout_valid(desc->layout)) {
    return GAUD_ERR_INVALID;
  }
  if ((unsigned)desc->format >= (unsigned)GAUD_SAMPLE_FORMAT_COUNT) {
    return GAUD_ERR_INVALID;
  }

  if (doc->track_count == doc->track_capacity) {
    size_t next = doc->track_capacity ? doc->track_capacity * 2 : 4;
    if (next < doc->track_capacity
        || next > SIZE_MAX / sizeof(GAUD_Track *)) {
      return GAUD_ERR_OOM;
    }
    GAUD_Track ** grown = gcu_allocator_realloc(
        doc->allocator, doc->tracks, next * sizeof(GAUD_Track *));
    if (!grown) {
      return GAUD_ERR_OOM;
    }
    doc->tracks = grown;
    doc->track_capacity = next;
  }

  GAUD_Track * track
      = gcu_allocator_malloc(doc->allocator, sizeof(GAUD_Track));
  if (!track) {
    return GAUD_ERR_OOM;
  }
  *track = (GAUD_Track){
      .doc = doc,
      .index = doc->track_count,
      .desc = *desc,
  };
  doc->tracks[doc->track_count++] = track;
  if (out_track) {
    *out_track = track;
  }
  return GAUD_OK;
}

void gaud_doc_destroy(GAUD_Doc * doc) {
  if (!doc) {
    return;
  }
  /* The codec's close runs first and while everything is still standing: it
   * may want to reach a track's private state, which is about to go. */
  if (doc->codec && gaud_codec_has(doc->codec, offsetof(GAUD_Codec, close))
      && doc->codec->close) {
    doc->codec->close(doc->codec, doc);
  }
  for (size_t i = 0; i < doc->track_count; ++i) {
    gcu_allocator_free(doc->allocator, doc->tracks[i]);
  }
  gcu_allocator_free(doc->allocator, doc->tracks);
  gaud_meta_destroy(doc->meta);
  gcu_allocator_free(doc->allocator, doc);
}

size_t gaud_doc_track_count(const GAUD_Doc * doc) {
  return doc ? doc->track_count : 0;
}

GAUD_Track * gaud_doc_track(const GAUD_Doc * doc, size_t index) {
  if (!doc || index >= doc->track_count) {
    return NULL;
  }
  return doc->tracks[index];
}

const char * gaud_doc_codec_name(const GAUD_Doc * doc) {
  return (doc && doc->codec) ? doc->codec->name : NULL;
}

GAUD_Stream * gaud_doc_stream(const GAUD_Doc * doc) {
  return doc ? doc->stream : NULL;
}

void gaud_doc_set_private(GAUD_Doc * doc, void * codec_private) {
  if (doc) {
    doc->codec_private = codec_private;
  }
}

void * gaud_doc_private(const GAUD_Doc * doc) {
  return doc ? doc->codec_private : NULL;
}

GAUD_Sample_Format gaud_track_format(const GAUD_Track * track) {
  return track ? track->desc.format : GAUD_SAMPLE_FORMAT_COUNT;
}

GAUD_Sample_Coding gaud_track_coding(const GAUD_Track * track) {
  return track ? track->desc.coding : GAUD_CODING_PCM;
}

uint32_t gaud_track_sample_rate(const GAUD_Track * track) {
  return track ? track->desc.sample_rate : 0u;
}

GAUD_Channel_Layout gaud_track_layout(const GAUD_Track * track) {
  if (!track) {
    GAUD_Channel_Layout empty = {0, 0};
    return empty;
  }
  return track->desc.layout;
}

uint32_t gaud_track_channels(const GAUD_Track * track) {
  return track ? track->desc.layout.channels : 0u;
}

uint64_t gaud_track_frames(const GAUD_Track * track) {
  return track ? track->desc.frames : UINT64_MAX;
}

GAUD_Trim gaud_track_trim(const GAUD_Track * track) {
  if (!track) {
    GAUD_Trim empty = {0, 0, false};
    return empty;
  }
  return track->desc.trim;
}

double gaud_track_duration(const GAUD_Track * track) {
  if (!track || track->desc.frames == UINT64_MAX
      || track->desc.sample_rate == 0) {
    return -1.0;
  }
  uint64_t frames = track->desc.frames;
  /* The recording's duration, not the file's: the delay and the padding are
   * not part of what was recorded. Saturating rather than wrapping, because a
   * container can state a trim larger than the track and a negative duration
   * would be a worse answer than zero. */
  uint64_t trimmed = track->desc.trim.encoder_delay + track->desc.trim.padding;
  frames = trimmed >= frames ? 0 : frames - trimmed;
  return (double)frames / (double)track->desc.sample_rate;
}

GAUD_Doc * gaud_track_doc(const GAUD_Track * track) {
  return track ? track->doc : NULL;
}

size_t gaud_track_index(const GAUD_Track * track) {
  return track ? track->index : 0;
}

void * gaud_track_private(const GAUD_Track * track) {
  return track ? track->desc.codec_private : NULL;
}

uint64_t gaud_track_data_offset(const GAUD_Track * track) {
  return track ? track->desc.data_offset : 0;
}

uint64_t gaud_track_data_length(const GAUD_Track * track) {
  return track ? track->desc.data_length : UINT64_MAX;
}

GAUD_Sample_Layout gaud_track_sample_layout(const GAUD_Track * track) {
  return track ? track->desc.sample_layout : GAUD_LAYOUT_INTERLEAVED;
}
