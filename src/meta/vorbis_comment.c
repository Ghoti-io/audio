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
 * Vorbis comment: `FIELD=value`, UTF-8, little-endian lengths.
 *
 * FLAC carries it in a VORBIS_COMMENT metadata block; Vorbis and Opus each
 * carry the identical structure in their own header packets in phase 6.
 * That is why it is in `src/meta/` beside the ID3 reader rather than in
 * `src/codec/flac/`: three formats, one parser.
 *
 * **Every length here is little-endian, in a format whose every other
 * length is big-endian.** Vorbis's structure was carried into FLAC
 * unchanged, byte order included, so a FLAC reader that applies its own
 * order to this block reads a 42-byte vendor string as 704,643,072 bytes
 * and refuses the file. It is the single most likely thing to get wrong
 * here and it fails loudly, which is the good case.
 *
 * Three decisions about the vocabulary:
 *
 * **There is no registry.** Vorbis's specification suggests field names
 * and mandates none, so `ALBUMARTIST`, `ALBUM ARTIST` and `albumartist`
 * are all in the wild and all mean one thing. The table below maps the
 * spellings that are actually written; everything else becomes a custom
 * entry under the name the file used, which round-trips exactly.
 *
 * **Field names are case-insensitive**, which the specification does say,
 * and ASCII-only, which it also says - so the folding is a plain ASCII one
 * and must not become a Unicode case fold. `TITLE` and `title` are one
 * field; the Turkish dotless i is not involved and a locale-sensitive
 * comparison here would be a bug in waiting.
 *
 * **A field may repeat, and repetition is meaningful.** Two ARTIST lines
 * are two artists. That is the reason `GAUD_Meta` holds a list per tag
 * rather than a string, and it is this scheme that forced it.
 */

#include "../codec/shared/bytes.h"
#include "../core/meta_internal.h"
#include "id3_internal.h"
#include "scheme.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/** The field name this library writes for each tag it knows. */
typedef struct {
  const char * field; ///< The canonical spelling, upper case.
  GAUD_Tag tag;       ///< What it means.
  bool writable;      ///< Whether the builder emits this name for @p tag.
} Field_Row;

/*
 * Rows marked unwritable are alternate spellings that are read and never
 * written: a file saying `ALBUM ARTIST` is understood, and comes back out
 * as `ALBUMARTIST`, which is the spelling everything reads.
 */
static const Field_Row fields[] = {
    {"TITLE", GAUD_TAG_TITLE, true},
    {"ARTIST", GAUD_TAG_ARTIST, true},
    {"ALBUM", GAUD_TAG_ALBUM, true},
    {"ALBUMARTIST", GAUD_TAG_ALBUM_ARTIST, true},
    {"ALBUM ARTIST", GAUD_TAG_ALBUM_ARTIST, false},
    {"DATE", GAUD_TAG_DATE, true},
    {"YEAR", GAUD_TAG_DATE, false},
    {"TRACKNUMBER", GAUD_TAG_TRACK_NUMBER, true},
    {"TRACKTOTAL", GAUD_TAG_TRACK_TOTAL, true},
    {"TOTALTRACKS", GAUD_TAG_TRACK_TOTAL, false},
    {"DISCNUMBER", GAUD_TAG_DISC_NUMBER, true},
    {"DISCTOTAL", GAUD_TAG_DISC_TOTAL, true},
    {"TOTALDISCS", GAUD_TAG_DISC_TOTAL, false},
    {"GENRE", GAUD_TAG_GENRE, true},
    {"COMPOSER", GAUD_TAG_COMPOSER, true},
    {"PERFORMER", GAUD_TAG_PERFORMER, true},
    {"COPYRIGHT", GAUD_TAG_COPYRIGHT, true},
    {"DESCRIPTION", GAUD_TAG_COMMENT, true},
    {"COMMENT", GAUD_TAG_COMMENT, false},
    {"ORGANIZATION", GAUD_TAG_ORGANIZATION, true},
    {"LABEL", GAUD_TAG_ORGANIZATION, false},
    {"PUBLISHER", GAUD_TAG_ORGANIZATION, false},
    {"ENCODER", GAUD_TAG_ENCODER, true},
    {"ISRC", GAUD_TAG_ISRC, true},
    {"LYRICS", GAUD_TAG_LYRICS, true},
    {"UNSYNCEDLYRICS", GAUD_TAG_LYRICS, false},
    {"BPM", GAUD_TAG_BPM, true},
    {"LANGUAGE", GAUD_TAG_LANGUAGE, true},
};

/** ASCII-only, locale-independent case-insensitive comparison. */
static bool field_is(
    const char * canonical, const unsigned char * name, size_t length) {
  if (strlen(canonical) != length) {
    return false;
  }
  for (size_t i = 0; i < length; ++i) {
    unsigned char a = (unsigned char)canonical[i];
    unsigned char b = name[i];
    if (b >= 'a' && b <= 'z') {
      b = (unsigned char)(b - 'a' + 'A');
    }
    if (a != b) {
      return false;
    }
  }
  return true;
}

GAUD_Result gaud_vorbis_comment_parse(const unsigned char * data, size_t size,
    const GAUD_Limits * limits, GAUD_Meta * meta,
    GAUD_Diagnostics * diagnostics) {
  (void)diagnostics;
  const GAUD_Allocator * allocator = gaud_meta_allocator(meta);
  if (size < 8u) {
    return GAUD_ERR_CORRUPT;
  }
  size_t at = 0;
  uint32_t vendor_length = gaud_rd_u32le(data + at);
  at += 4;
  if (vendor_length > size - at) {
    return GAUD_ERR_CORRUPT;
  }
  /* **The vendor string is read past and not kept, and that is a
   * decision.** It names the software that wrote the file, which is not
   * a property of the recording and is not a tag: the comment list has
   * an `ENCODER` field for what a caller means by that, and this is the
   * block's own signature. Mapping it onto a tag is what the first draft
   * did, and the round trip then gained a value every generation -
   * because the reader turned our predecessor's signature into a tag and
   * the writer stamped its own beside it. A field the writer overwrites
   * must not also be a field the reader collects. */
  at += vendor_length;

  if (size - at < 4u) {
    return GAUD_ERR_CORRUPT;
  }
  uint32_t count = gaud_rd_u32le(data + at);
  at += 4;
  /* Four bytes of length per comment at the very least, so a count that
   * could not fit in what is left is a corrupt block rather than an
   * allocation to attempt. */
  if (count > (size - at) / 4u) {
    return GAUD_ERR_CORRUPT;
  }
  if (count > limits->max_metadata_entries) {
    return GAUD_ERR_LIMIT;
  }

  for (uint32_t i = 0; i < count; ++i) {
    if (size - at < 4u) {
      return GAUD_ERR_CORRUPT;
    }
    uint32_t length = gaud_rd_u32le(data + at);
    at += 4;
    if (length > size - at) {
      return GAUD_ERR_CORRUPT;
    }
    const unsigned char * entry = data + at;
    at += length;

    const unsigned char * equals = memchr(entry, '=', length);
    if (!equals) {
      /* A comment with no separator is not a field at all. The
       * specification says to ignore it, and ignoring it is also what
       * keeps a zero-length name out of the custom table. */
      continue;
    }
    size_t name_length = (size_t)(equals - entry);
    const unsigned char * value = equals + 1;
    size_t value_length = length - name_length - 1u;
    if (name_length == 0) {
      continue;
    }

    char * text = gaud_id3_bytes_to_utf8(allocator, value, value_length);
    if (!text) {
      return GAUD_ERR_OOM;
    }

    GAUD_Result result = GAUD_OK;
    bool mapped = false;
    for (size_t row = 0; row < sizeof(fields) / sizeof(fields[0]); ++row) {
      if (field_is(fields[row].field, entry, name_length)) {
        result = gaud_meta_add_unique(meta, fields[row].tag, text);
        mapped = true;
        break;
      }
    }
    if (!mapped) {
      /* Kept under the name the file used, not an upper-cased one: the
       * round trip has to put back what was there, and a tagger that
       * rewrote every unknown field's capitalisation would rewrite half
       * the file's tags on a metadata-only edit. */
      char * key = gaud_id3_bytes_to_utf8(allocator, entry, name_length);
      if (!key) {
        gcu_allocator_free(allocator, text);
        return GAUD_ERR_OOM;
      }
      result = gaud_meta_custom_add_unique(meta, key, text);
      gcu_allocator_free(allocator, key);
    }
    gcu_allocator_free(allocator, text);
    if (result != GAUD_OK) {
      return result;
    }
  }
  return GAUD_OK;
}

/** A growable byte buffer, as the ID3 builder uses. */
typedef struct {
  unsigned char * data;
  size_t size;
  size_t capacity;
  const GAUD_Allocator * allocator;
  bool failed;
} Builder;

static void put(Builder * b, const void * bytes, size_t count) {
  if (b->failed) {
    return;
  }
  if (b->size + count > b->capacity) {
    size_t want = b->capacity ? b->capacity * 2u : 256u;
    while (want < b->size + count) {
      want *= 2u;
    }
    unsigned char * grown = gcu_allocator_malloc(b->allocator, want);
    if (!grown) {
      b->failed = true;
      return;
    }
    if (b->data) {
      memcpy(grown, b->data, b->size);
      gcu_allocator_free(b->allocator, b->data);
    }
    b->data = grown;
    b->capacity = want;
  }
  memcpy(b->data + b->size, bytes, count);
  b->size += count;
}

static void put_u32le(Builder * b, uint32_t value) {
  unsigned char bytes[4];
  gaud_wr_u32le(bytes, value);
  put(b, bytes, sizeof(bytes));
}

/** Append one `NAME=value` comment with its length prefix. */
static void put_comment(Builder * b, const char * name, const char * value) {
  size_t name_length = strlen(name);
  size_t value_length = strlen(value);
  put_u32le(b, (uint32_t)(name_length + 1u + value_length));
  put(b, name, name_length);
  put(b, "=", 1u);
  put(b, value, value_length);
}

GAUD_Result gaud_vorbis_comment_build(const GAUD_Meta * meta,
    const char * vendor, const GAUD_Allocator * allocator,
    unsigned char ** out_data, size_t * out_size) {
  *out_data = NULL;
  *out_size = 0;
  /* A NULL @p meta is "no tags", not "no block". The vendor string and the
   * count are the block's fixed head and a reader is entitled to them, so
   * an empty block is eight bytes and never zero - which is what the first
   * draft wrote, and `flac -t` rejected every file for bad metadata. */
  Builder b = {NULL, 0, 0, allocator, false};

  const char * vendor_text = vendor ? vendor : "";
  put_u32le(&b, (uint32_t)strlen(vendor_text));
  put(&b, vendor_text, strlen(vendor_text));

  /* The count is patched once the comments are written, because a tag with
   * several values is several comments and counting them first would be
   * the same loop written twice. */
  size_t count_at = b.size;
  put_u32le(&b, 0);

  uint32_t written = 0;
  for (size_t row = 0; meta && row < sizeof(fields) / sizeof(fields[0]);
       ++row) {
    if (!fields[row].writable) {
      continue;
    }
    size_t values = gaud_meta_count(meta, fields[row].tag);
    for (size_t i = 0; i < values; ++i) {
      const char * value = gaud_meta_get(meta, fields[row].tag, i);
      if (!value) {
        continue;
      }
      put_comment(&b, fields[row].field, value);
      ++written;
    }
  }
  size_t customs = meta ? gaud_meta_custom_count(meta) : 0;
  for (size_t i = 0; i < customs; ++i) {
    const char * key = NULL;
    const char * value = NULL;
    if (gaud_meta_custom(meta, i, &key, &value) != GAUD_OK || !key
        || !value) {
      continue;
    }
    /* A custom key holding an '=' would produce a comment whose name ends
     * early and whose value begins in the middle of the key, so the next
     * reader would see a different field. Dropping it loses a tag; writing
     * it corrupts a neighbour. */
    if (strchr(key, '=')) {
      continue;
    }
    put_comment(&b, key, value);
    ++written;
  }

  if (b.failed) {
    gcu_allocator_free(allocator, b.data);
    return GAUD_ERR_OOM;
  }
  gaud_wr_u32le(b.data + count_at, written);
  *out_data = b.data;
  *out_size = b.size;
  return GAUD_OK;
}
