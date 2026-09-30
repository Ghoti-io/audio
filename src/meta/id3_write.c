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
 * Writing ID3v2.4.
 *
 * Everything goes out as UTF-8, encoding byte 3, which is the reason for
 * writing 2.4 rather than 2.3: 2.3 has no UTF-8, so every string that is
 * not Latin-1 would have to be UTF-16 with a byte-order mark, and UTF-16
 * is the encoding every reader has a bug in.
 *
 * No unsynchronisation. It exists so that no byte pair inside a tag can be
 * mistaken for an MPEG frame sync, which matters when the tag is glued to
 * the front of an MPEG stream. Phase 3's tags live in a RIFF or IFF chunk
 * with a stated length, where nothing is scanning for a sync word - and a
 * writer that applies it anyway produces a larger tag that some readers
 * then fail to un-apply. Phase 5 brings MP3 and with it the decision to
 * make this conditional.
 *
 * The header's size field and 2.4's frame sizes are both syncsafe, which
 * caps a single frame at 256 MiB and the whole tag at the same. A picture
 * larger than that is refused rather than truncated into a length field
 * that wraps.
 */

#include "id3_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <stdio.h>
#include <string.h>

/** A growable byte buffer, so the frames can be built before the size. */
typedef struct {
  unsigned char * data;             ///< Owned by @p allocator.
  size_t size;                      ///< How much is written.
  size_t capacity;                  ///< Room in @p data.
  const GAUD_Allocator * allocator; ///< Where @p data comes from.
  bool failed; ///< Sticky: checked once at the end rather than per append.
} Builder;

static void put(Builder * b, const void * bytes, size_t count) {
  if (b->failed) {
    return;
  }
  if (b->size + count > b->capacity) {
    size_t next = b->capacity ? b->capacity : 256u;
    while (next < b->size + count) {
      if (next > SIZE_MAX / 2u) {
        b->failed = true;
        return;
      }
      next *= 2u;
    }
    unsigned char * grown
        = gcu_allocator_realloc(b->allocator, b->data, next);
    if (!grown) {
      b->failed = true;
      return;
    }
    b->data = grown;
    b->capacity = next;
  }
  if (count > 0) {
    memcpy(b->data + b->size, bytes, count);
  }
  b->size += count;
}

static void put_byte(Builder * b, unsigned char value) {
  put(b, &value, 1);
}

/** Seven bits per byte, top bit clear. */
static void put_syncsafe(Builder * b, uint32_t value) {
  unsigned char out[4] = {(unsigned char)((value >> 21) & 0x7Fu),
      (unsigned char)((value >> 14) & 0x7Fu),
      (unsigned char)((value >> 7) & 0x7Fu), (unsigned char)(value & 0x7Fu)};
  put(b, out, 4);
}

/** The largest a syncsafe length can express. */
#define ID3_MAX_SYNCSAFE 0x0FFFFFFFu

/** One frame: four-character id, syncsafe size, two flag bytes. */
static void put_frame_flagged(Builder * b, const char * id, const void * body,
    size_t size, uint16_t flags) {
  if (size > ID3_MAX_SYNCSAFE) {
    b->failed = true;
    return;
  }
  put(b, id, 4);
  put_syncsafe(b, (uint32_t)size);
  put_byte(b, (unsigned char)(flags >> 8));
  put_byte(b, (unsigned char)(flags & 0xFFu));
  put(b, body, size);
}

/** A frame this library built itself, which has nothing to flag. */
static void put_frame(
    Builder * b, const char * id, const void * body, size_t size) {
  put_frame_flagged(b, id, body, size, 0);
}

/** A text frame: encoding byte 3, then UTF-8, with no terminator. */
static void put_text_frame(Builder * b, const char * id, const char * text) {
  size_t length = strlen(text);
  Builder body = {.allocator = b->allocator};
  put_byte(&body, 3);
  put(&body, text, length);
  if (body.failed) {
    b->failed = true;
  }
  else {
    put_frame(b, id, body.data, body.size);
  }
  gcu_allocator_free(b->allocator, body.data);
}

/**
 * Several values in one frame, separated by the encoding's terminator.
 *
 * 2.4's own spelling for a repeated tag. Two separate frames work
 * equally well - measured: ffmpeg reports the first value and mutagen
 * reports both, identically for either form - so compatibility does not
 * decide it. What does is that writing the specification's form makes
 * this library's own round trip exercise the reader's in-frame split,
 * which no fixture would otherwise reach.
 */
static void put_multi_text_frame(Builder * b, const GAUD_Meta * meta,
    const char * id, GAUD_Tag tag, size_t count) {
  Builder body = {.allocator = b->allocator};
  put_byte(&body, 3);
  for (size_t i = 0; i < count; ++i) {
    if (i > 0) {
      put_byte(&body, 0);
    }
    const char * value = gaud_meta_get(meta, tag, i);
    put(&body, value, strlen(value));
  }
  if (body.failed) {
    b->failed = true;
  }
  else {
    put_frame(b, id, body.data, body.size);
  }
  gcu_allocator_free(b->allocator, body.data);
}

/** `COMM` and `USLT`: encoding, language, terminated description, text. */
static void put_described_frame(
    Builder * b, const char * id, const char * text) {
  Builder body = {.allocator = b->allocator};
  put_byte(&body, 3);
  /* "XXX" is the specification's "undefined language". Writing "eng"
   * instead would be asserting something the caller never said. */
  put(&body, "XXX", 3);
  put_byte(&body, 0); /* The empty description, terminated. */
  put(&body, text, strlen(text));
  if (body.failed) {
    b->failed = true;
  }
  else {
    put_frame(b, id, body.data, body.size);
  }
  gcu_allocator_free(b->allocator, body.data);
}

/** `TXXX`: encoding, terminated key, value. */
static void put_custom_frame(
    Builder * b, const char * key, const char * value) {
  Builder body = {.allocator = b->allocator};
  put_byte(&body, 3);
  put(&body, key, strlen(key) + 1u); /* The terminator is part of it. */
  put(&body, value, strlen(value));
  if (body.failed) {
    b->failed = true;
  }
  else {
    put_frame(b, "TXXX", body.data, body.size);
  }
  gcu_allocator_free(b->allocator, body.data);
}

/** `APIC`: encoding, terminated MIME, type byte, terminated description. */
static void put_picture_frame(Builder * b, const GAUD_Picture * picture) {
  Builder body = {.allocator = b->allocator};
  put_byte(&body, 3);
  /* The MIME type is Latin-1 in every version, never the frame's
   * encoding - which is why it is written as bytes and not through the
   * text path. */
  put(&body, picture->mime_type, strlen(picture->mime_type) + 1u);
  unsigned char kind = 0;
  switch (picture->kind) {
  case GAUD_PICTURE_FRONT_COVER: kind = 3; break;
  case GAUD_PICTURE_BACK_COVER: kind = 4; break;
  case GAUD_PICTURE_LEAFLET: kind = 5; break;
  case GAUD_PICTURE_MEDIA: kind = 6; break;
  case GAUD_PICTURE_ARTIST: kind = 8; break;
  case GAUD_PICTURE_ICON: kind = 1; break;
  default: kind = 0; break;
  }
  put_byte(&body, kind);
  put(&body, picture->description, strlen(picture->description) + 1u);
  put(&body, picture->data, picture->size);
  if (body.failed) {
    b->failed = true;
  }
  else {
    put_frame(b, "APIC", body.data, body.size);
  }
  gcu_allocator_free(b->allocator, body.data);
}

/**
 * Join a number and a total into ID3's one string.
 *
 * The inverse of the split the reader does. Both live next to their
 * opposite number so that a change to one is a change in front of the
 * other; splitting in the reader and joining three files away is how the
 * two come to disagree about whether "3/" is valid.
 */
static void put_pair_frame(Builder * b, const GAUD_Meta * meta,
    const char * id, GAUD_Tag number, GAUD_Tag total) {
  const char * n = gaud_meta_get(meta, number, 0);
  const char * t = gaud_meta_get(meta, total, 0);
  if (!n && !t) {
    return;
  }
  if (!n) {
    /* A total with no number cannot be written as "/12": readers split
     * on the slash and get an empty number. The total is dropped and the
     * raw block, if any, still carries it. */
    return;
  }
  if (!t) {
    put_text_frame(b, id, n);
    return;
  }
  size_t length = strlen(n) + strlen(t) + 2u;
  char * joined = gcu_allocator_malloc(b->allocator, length);
  if (!joined) {
    b->failed = true;
    return;
  }
  snprintf(joined, length, "%s/%s", n, t);
  put_text_frame(b, id, joined);
  gcu_allocator_free(b->allocator, joined);
}

GAUD_Result gaud_id3v2_build(const GAUD_Meta * meta, GAUD_Meta_Policy policy,
    const GAUD_Allocator * allocator, unsigned char ** out_data,
    size_t * out_size) {
  if (!meta || !out_data || !out_size) {
    return GAUD_ERR_INVALID;
  }
  *out_data = NULL;
  *out_size = 0;
  if (!allocator) {
    /* Substituted here rather than trusted to the allocator functions:
     * every other entry point in this library takes NULL for the
     * default, and one that did not would be a trap for exactly the
     * callers who never pass an allocator. */
    allocator = gaud_allocator_default();
  }
  if (policy == GAUD_META_DROP_ALL) {
    return GAUD_OK;
  }

  Builder body = {.allocator = allocator};
  bool common = policy == GAUD_META_PRESERVE_ALL
      || policy == GAUD_META_KEEP_COMMON_ONLY;
  bool raw = policy == GAUD_META_PRESERVE_ALL
      || policy == GAUD_META_KEEP_RAW_ONLY;

  if (common) {
    for (int t = 0; t < GAUD_TAG_COUNT; ++t) {
      GAUD_Tag tag = (GAUD_Tag)t;
      /* The two pair tags are written by their partner and the totals
       * have no frame of their own. */
      if (tag == GAUD_TAG_TRACK_TOTAL || tag == GAUD_TAG_DISC_TOTAL) {
        continue;
      }
      if (tag == GAUD_TAG_TRACK_NUMBER) {
        put_pair_frame(&body, meta, "TRCK", GAUD_TAG_TRACK_NUMBER,
            GAUD_TAG_TRACK_TOTAL);
        continue;
      }
      if (tag == GAUD_TAG_DISC_NUMBER) {
        put_pair_frame(&body, meta, "TPOS", GAUD_TAG_DISC_NUMBER,
            GAUD_TAG_DISC_TOTAL);
        continue;
      }
      const char * id = gaud_id3_tag_frame(tag);
      if (!id) {
        continue;
      }
      size_t count = gaud_meta_count(meta, tag);
      if (count == 0) {
        continue;
      }
      if (tag == GAUD_TAG_COMMENT || tag == GAUD_TAG_LYRICS) {
        /* COMM and USLT are not text frames: each carries a language
         * and a description before its value, so several values are
         * several frames and there is no in-frame separator. */
        for (size_t i = 0; i < count; ++i) {
          put_described_frame(&body, id, gaud_meta_get(meta, tag, i));
        }
      }
      else if (count == 1) {
        put_text_frame(&body, id, gaud_meta_get(meta, tag, 0));
      }
      else {
        put_multi_text_frame(&body, meta, id, tag, count);
      }
    }
    for (size_t i = 0; i < gaud_meta_custom_count(meta); ++i) {
      const char * key = NULL;
      const char * value = NULL;
      if (gaud_meta_custom(meta, i, &key, &value) == GAUD_OK) {
        put_custom_frame(&body, key, value);
      }
    }
    for (size_t i = 0; i < gaud_meta_picture_count(meta); ++i) {
      put_picture_frame(&body, gaud_meta_picture(meta, i));
    }
  }

  if (raw) {
    for (size_t i = 0; i < gaud_meta_raw_count(meta); ++i) {
      const char * scheme = NULL;
      const char * id = NULL;
      const void * data = NULL;
      size_t size = 0;
      if (gaud_meta_raw(meta, i, &scheme, &id, &data, &size) != GAUD_OK) {
        continue;
      }
      /* Only this scheme's own blocks. A RIFF chunk means nothing inside
       * an ID3 tag, and writing one would produce a frame whose
       * four-character identifier happens to be a chunk name. */
      if (strcmp(scheme, "id3v2") != 0 || strlen(id) != 4) {
        continue;
      }
      /* With the flags it arrived with. They are the reason it is raw. */
      put_frame_flagged(&body, id, data, size, gaud_meta_raw_flags(meta, i));
    }
  }

  if (body.failed) {
    gcu_allocator_free(allocator, body.data);
    return GAUD_ERR_OOM;
  }
  if (body.size == 0) {
    gcu_allocator_free(allocator, body.data);
    return GAUD_OK; /* Nothing to say, so no tag at all. */
  }
  if (body.size > ID3_MAX_SYNCSAFE) {
    gcu_allocator_free(allocator, body.data);
    return GAUD_ERR_UNSUPPORTED;
  }

  Builder whole = {.allocator = allocator};
  put(&whole, "ID3", 3);
  put_byte(&whole, 4); /* Major version. */
  put_byte(&whole, 0); /* Revision. */
  put_byte(&whole, 0); /* No unsynchronisation, no extended header. */
  put_syncsafe(&whole, (uint32_t)body.size);
  put(&whole, body.data, body.size);
  gcu_allocator_free(allocator, body.data);

  if (whole.failed) {
    gcu_allocator_free(allocator, whole.data);
    return GAUD_ERR_OOM;
  }
  *out_data = whole.data;
  *out_size = whole.size;
  return GAUD_OK;
}
