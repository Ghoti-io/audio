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
 * RIFF's `LIST`/`INFO` chunk, and BWF's `bext`.
 *
 * `INFO` is the simplest scheme here: a list of sub-chunks whose
 * four-character ids are the keys and whose bodies are NUL-terminated
 * strings. Two things about it are easy to get wrong and both are here.
 *
 * **The strings are Latin-1, not UTF-8.** The specification says
 * "ZSTR" - a NUL-terminated string in the file's code page - and in
 * practice that is Latin-1 or whatever the writer's system used. Passing
 * the bytes through unchanged puts invalid UTF-8 in front of a caller
 * whose validator then rejects the whole tag.
 *
 * **The stated length includes the terminator and the chunk is padded to
 * even.** A reader that takes the length as the string's length gets a
 * trailing NUL inside its string, and one that forgets the pad byte finds
 * the next sub-chunk one byte late from the first odd-length entry on.
 */

#include "scheme.h"
#include "id3_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <stdio.h>
#include <string.h>

/** One `INFO` sub-chunk id, and the tag it means. */
typedef struct {
  const char id[5]; ///< The four-character sub-chunk identifier.
  GAUD_Tag tag;     ///< What it maps onto.
  bool writable;    ///< Whether the writer emits this for @p tag.
} Info_Row;

/*
 * `IART` and `IPRD` are the ones every writer agrees on. The duplicates
 * exist because the vocabulary grew twice: `ICRD` and `IDIT` both carry a
 * date, `ICMT` and `ISBJ` both a comment. The writer picks one per tag
 * and the reader accepts all of them.
 */
static const Info_Row info_rows[] = {
    {"INAM", GAUD_TAG_TITLE, true},
    {"IART", GAUD_TAG_ARTIST, true},
    {"IPRD", GAUD_TAG_ALBUM, true},
    {"ICRD", GAUD_TAG_DATE, true},
    {"IDIT", GAUD_TAG_DATE, false},
    {"ITRK", GAUD_TAG_TRACK_NUMBER, true},
    {"IPRT", GAUD_TAG_TRACK_NUMBER, false},
    {"IGNR", GAUD_TAG_GENRE, true},
    {"IMUS", GAUD_TAG_COMPOSER, true},
    {"ISTR", GAUD_TAG_PERFORMER, true},
    {"ICMT", GAUD_TAG_COMMENT, true},
    {"ISBJ", GAUD_TAG_COMMENT, false},
    {"ICOP", GAUD_TAG_COPYRIGHT, true},
    {"ISFT", GAUD_TAG_ENCODER, true},
    {"ILNG", GAUD_TAG_LANGUAGE, true},
    {"IPUB", GAUD_TAG_ORGANIZATION, true},
};

static void note(GAUD_Diagnostics * diagnostics, uint64_t offset,
    const char * action) {
  if (!diagnostics) {
    return;
  }
  GAUD_Diagnostic entry = {.codec_name = "riff-info",
      .offset = offset,
      .element_id = 0,
      .severity = GAUD_DIAG_WARNING,
      .recommended_action = action};
  gaud_diagnostics_append(diagnostics, &entry);
}

GAUD_Result gaud_riff_info_parse(const unsigned char * data, size_t size,
    const GAUD_Limits * limits, GAUD_Meta * meta,
    GAUD_Diagnostics * diagnostics) {
  if (!data || !meta || !limits) {
    return GAUD_ERR_INVALID;
  }
  const GAUD_Allocator * allocator = gaud_meta_allocator(meta);
  size_t at = 0;
  uint32_t entries = 0;

  while (at + 8u <= size) {
    char id[5] = {0};
    memcpy(id, data + at, 4);
    uint32_t length = (uint32_t)data[at + 4] | ((uint32_t)data[at + 5] << 8)
        | ((uint32_t)data[at + 6] << 16) | ((uint32_t)data[at + 7] << 24);
    at += 8;
    if (length > size - at) {
      note(diagnostics, at,
          "an INFO entry claims more bytes than the list holds; the rest of "
          "the list is ignored");
      break;
    }
    if (++entries > limits->max_metadata_entries) {
      return GAUD_ERR_LIMIT;
    }

    /* The stated length includes the terminator, and some writers state
     * it and some do not - so the string ends at the first NUL or at the
     * stated length, whichever comes first. */
    size_t text_length = 0;
    while (text_length < length && data[at + text_length] != 0) {
      ++text_length;
    }
    char * text = gaud_id3_bytes_to_utf8(allocator, data + at, text_length);
    if (text) {
      GAUD_Tag tag;
      bool mapped = false;
      for (size_t i = 0; i < sizeof(info_rows) / sizeof(info_rows[0]); ++i) {
        if (memcmp(info_rows[i].id, id, 4) == 0) {
          tag = info_rows[i].tag;
          mapped = true;
          break;
        }
      }
      if (text[0] != '\0') {
        if (mapped) {
          gaud_meta_add_unique(meta, tag, text);
        }
        else {
          /* An id with no mapping keeps its four characters as the key,
           * so a round trip can put it back and a caller can see it. */
          gaud_meta_custom_add_unique(meta, id, text);
        }
      }
      gcu_allocator_free(allocator, text);
    }
    at += length + (length & 1u); /* Padded to even, like every RIFF chunk. */
  }
  return GAUD_OK;
}

/** Append a `LIST` sub-chunk: id, length, the string with its NUL, a pad. */
static bool append_entry(const GAUD_Allocator * allocator,
    unsigned char ** buffer, size_t * size, size_t * capacity,
    const char * id, const char * text) {
  size_t length = strlen(text) + 1u;
  size_t need = *size + 8u + length + (length & 1u);
  if (need > *capacity) {
    size_t next = *capacity ? *capacity : 256u;
    while (next < need) {
      next *= 2u;
    }
    unsigned char * grown = gcu_allocator_realloc(allocator, *buffer, next);
    if (!grown) {
      return false;
    }
    *buffer = grown;
    *capacity = next;
  }
  unsigned char * out = *buffer + *size;
  memcpy(out, id, 4);
  out[4] = (unsigned char)(length & 0xFFu);
  out[5] = (unsigned char)((length >> 8) & 0xFFu);
  out[6] = (unsigned char)((length >> 16) & 0xFFu);
  out[7] = (unsigned char)((length >> 24) & 0xFFu);
  memcpy(out + 8, text, length);
  if (length & 1u) {
    out[8 + length] = 0;
  }
  *size += 8u + length + (length & 1u);
  return true;
}

GAUD_Result gaud_riff_info_build(const GAUD_Meta * meta,
    const GAUD_Allocator * allocator, unsigned char ** out_data,
    size_t * out_size) {
  if (!meta || !out_data || !out_size) {
    return GAUD_ERR_INVALID;
  }
  *out_data = NULL;
  *out_size = 0;
  if (!allocator) {
    allocator = gaud_allocator_default();
  }

  unsigned char * body = NULL;
  size_t size = 0, capacity = 0;
  bool ok = true;

  for (size_t i = 0;
      ok && i < sizeof(info_rows) / sizeof(info_rows[0]); ++i) {
    if (!info_rows[i].writable) {
      continue;
    }
    size_t count = gaud_meta_count(meta, info_rows[i].tag);
    for (size_t v = 0; ok && v < count; ++v) {
      /* INFO has no way to say "two artists": each id may appear once in
       * practice, and readers take the first. Every value is written
       * anyway rather than silently dropping the second, because a file
       * with two INAM chunks is what a reader that keeps them expects
       * and a reader that does not keeps the first - which is the same
       * answer it would have got. */
      ok = append_entry(allocator, &body, &size, &capacity,
          info_rows[i].id, gaud_meta_get(meta, info_rows[i].tag, v));
    }
  }
  for (size_t i = 0; ok && i < gaud_meta_custom_count(meta); ++i) {
    const char * key = NULL;
    const char * value = NULL;
    if (gaud_meta_custom(meta, i, &key, &value) != GAUD_OK) {
      continue;
    }
    /* Only a key that is already a four-character id can go here: INFO
     * has no user-defined entry, so a longer key has no spelling and
     * truncating one would invent an id that means something else. */
    if (strlen(key) == 4) {
      ok = append_entry(allocator, &body, &size, &capacity, key, value);
    }
  }

  if (!ok) {
    gcu_allocator_free(allocator, body);
    return GAUD_ERR_OOM;
  }
  if (size == 0) {
    gcu_allocator_free(allocator, body);
    return GAUD_OK;
  }

  /* The whole chunk: "LIST", the length, "INFO", then the entries. */
  size_t total = 12u + size;
  unsigned char * chunk = gcu_allocator_malloc(allocator, total);
  if (!chunk) {
    gcu_allocator_free(allocator, body);
    return GAUD_ERR_OOM;
  }
  memcpy(chunk, "LIST", 4);
  uint32_t payload = (uint32_t)(size + 4u); /* "INFO" counts. */
  chunk[4] = (unsigned char)(payload & 0xFFu);
  chunk[5] = (unsigned char)((payload >> 8) & 0xFFu);
  chunk[6] = (unsigned char)((payload >> 16) & 0xFFu);
  chunk[7] = (unsigned char)((payload >> 24) & 0xFFu);
  memcpy(chunk + 8, "INFO", 4);
  memcpy(chunk + 12, body, size);
  gcu_allocator_free(allocator, body);

  *out_data = chunk;
  *out_size = total;
  return GAUD_OK;
}

/* ------------------------------------------------------------ BWF bext */

GAUD_Result gaud_bext_parse(const unsigned char * data, size_t size,
    GAUD_Meta * meta, GAUD_Diagnostics * diagnostics) {
  if (!data || !meta) {
    return GAUD_ERR_INVALID;
  }
  /* 602 bytes to the end of Reserved, then a variable coding history.
   * Version 1 added a UMID and version 2 the loudness fields, both
   * inside the fixed part, so the minimum does not change. */
  if (size < 602u) {
    note(diagnostics, 0,
        "a bext chunk shorter than its fixed part; it is kept raw and not "
        "interpreted");
    return GAUD_ERR_CORRUPT;
  }
  const GAUD_Allocator * allocator = gaud_meta_allocator(meta);

  /* Description, 256 bytes, Latin-1, space- or NUL-padded. It is the
   * closest thing bext has to a comment and is mapped to one; the whole
   * chunk is also kept raw, because the origination time, the timecode
   * and the loudness fields have nowhere in the common vocabulary and
   * losing them on a round trip would be worse than not reading them. */
  struct {
    size_t offset, length;
    GAUD_Tag tag;
  } fields[] = {
      {0, 256, GAUD_TAG_COMMENT},
      {256, 32, GAUD_TAG_ORGANIZATION}, /* Originator. */
  };
  for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); ++f) {
    const unsigned char * field = data + fields[f].offset;
    size_t length = 0;
    while (length < fields[f].length && field[length] != 0) {
      ++length;
    }
    while (length > 0 && field[length - 1] == ' ') {
      --length;
    }
    if (length == 0) {
      continue;
    }
    char * text = gaud_id3_latin1_to_utf8(allocator, field, length);
    if (text) {
      gaud_meta_add_unique(meta, fields[f].tag, text);
      gcu_allocator_free(allocator, text);
    }
  }
  return GAUD_OK;
}

/* -------------------------------------------------- AIFF's text chunks */

bool gaud_aiff_text_tag(const char id[4], GAUD_Tag * out_tag) {
  if (!id || !out_tag) {
    return false;
  }
  /* AIFF has four and only four: a name, an author, a copyright notice
   * and an annotation. Everything else a tagger wants to say goes in an
   * `ID3 ` chunk, which is why AIFF files in the wild carry one. */
  if (memcmp(id, "NAME", 4) == 0) {
    *out_tag = GAUD_TAG_TITLE;
    return true;
  }
  if (memcmp(id, "AUTH", 4) == 0) {
    *out_tag = GAUD_TAG_ARTIST;
    return true;
  }
  if (memcmp(id, "(c) ", 4) == 0) {
    *out_tag = GAUD_TAG_COPYRIGHT;
    return true;
  }
  if (memcmp(id, "ANNO", 4) == 0) {
    *out_tag = GAUD_TAG_COMMENT;
    return true;
  }
  return false;
}
