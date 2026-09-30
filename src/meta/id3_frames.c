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
 * Which ID3v2 frame means which common tag, in both spellings.
 *
 * ID3v2.2 uses three-character identifiers and 2.3/2.4 use four, and they
 * are not related by truncation - `TT2` is 2.4's `TIT2` and `TP1` is
 * `TPE1`. So the table carries both, on one row each, because a mapping
 * split across two tables is one that gets updated in one of them.
 *
 * A frame with no row here is **kept raw**, not dropped: the round trip is
 * what makes a tagger safe to run, and a library that discarded every
 * frame it had no name for would quietly strip a file's replay gain the
 * first time it rewrote it.
 */

#include "id3_internal.h"
#include <string.h>

/** One frame identifier in both spellings, and what it means. */
typedef struct {
  const char * v24; ///< The 2.3/2.4 four-character identifier.
  const char * v22; ///< The 2.2 three-character one, or NULL.
  GAUD_Tag tag;     ///< What it maps onto.
  bool writable;    ///< Whether the writer emits this for @p tag.
} Frame_Row;

/*
 * `writable` exists because the mapping is many-to-one in one direction
 * only. Both `TPE3` and `TPE4` read as PERFORMER and both `TYER` and
 * `TDRC` read as DATE, but a writer has to pick one - and picking by
 * "first row that matches" would make the choice depend on table order
 * rather than on a decision. TDRC is 2.4's, TYER is 2.3's and deprecated.
 */
static const Frame_Row frames[] = {
    {"TIT2", "TT2", GAUD_TAG_TITLE, true},
    {"TPE1", "TP1", GAUD_TAG_ARTIST, true},
    {"TALB", "TAL", GAUD_TAG_ALBUM, true},
    {"TPE2", "TP2", GAUD_TAG_ALBUM_ARTIST, true},
    {"TDRC", NULL, GAUD_TAG_DATE, true},
    {"TYER", "TYE", GAUD_TAG_DATE, false},
    {"TRCK", "TRK", GAUD_TAG_TRACK_NUMBER, true},
    {"TPOS", "TPA", GAUD_TAG_DISC_NUMBER, true},
    {"TCON", "TCO", GAUD_TAG_GENRE, true},
    {"TCOM", "TCM", GAUD_TAG_COMPOSER, true},
    {"TPE3", "TP3", GAUD_TAG_PERFORMER, true},
    {"TPE4", "TP4", GAUD_TAG_PERFORMER, false},
    {"COMM", "COM", GAUD_TAG_COMMENT, true},
    {"TCOP", "TCR", GAUD_TAG_COPYRIGHT, true},
    {"TSSE", "TSS", GAUD_TAG_ENCODER, true},
    {"TSRC", "TRC", GAUD_TAG_ISRC, true},
    {"TLAN", "TLA", GAUD_TAG_LANGUAGE, true},
    {"TPUB", "TPB", GAUD_TAG_ORGANIZATION, true},
    {"USLT", "ULT", GAUD_TAG_LYRICS, true},
    {"TBPM", "TBP", GAUD_TAG_BPM, true},
};

/*
 * Every tag except the four that ID3 has no frame for. TRACK_TOTAL and
 * DISC_TOTAL live inside TRCK and TPOS as "3/12" and are joined by the
 * writer; PERFORMER has two frames and one is marked writable. Stated as
 * an assertion so that adding a tag without deciding how ID3 carries it
 * is a compile error rather than a tag that silently never gets written.
 */
_Static_assert(GAUD_TAG_COUNT == 20,
    "a tag was added: give it a row in frames[] with writable=true, or "
    "decide here that ID3 cannot carry it");

bool gaud_id3_frame_tag(const char * id, size_t length, GAUD_Tag * out_tag) {
  if (!id || !out_tag || (length != 3 && length != 4)) {
    return false;
  }
  for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); ++i) {
    const char * candidate = length == 4 ? frames[i].v24 : frames[i].v22;
    if (candidate && memcmp(candidate, id, length) == 0) {
      *out_tag = frames[i].tag;
      return true;
    }
  }
  return false;
}

const char * gaud_id3_tag_frame(GAUD_Tag tag) {
  for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); ++i) {
    if (frames[i].writable && frames[i].tag == tag) {
      return frames[i].v24;
    }
  }
  return NULL;
}
