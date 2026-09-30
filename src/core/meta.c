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
 * The metadata container: tags, custom keys, raw blocks and pictures.
 *
 * Four growable arrays and a lot of string copying, which is the whole of
 * it. The interesting decisions are in the header; what is here is the
 * bookkeeping, and the one rule it enforces is that **`meta` owns every
 * byte a caller can see**. Everything handed in is copied and everything
 * handed out is borrowed, so a caller can destroy the document it read
 * from and keep the tags.
 */

#include "meta_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <stdlib.h>
#include <string.h>

/* The canonical names, in enum order. Indexed by GAUD_Tag, so a name
 * added in the wrong place is a wrong answer rather than a compile
 * error - which is what the assertion below is for. */
static const char * const tag_names[GAUD_TAG_COUNT] = {
    "title", "artist", "album", "albumartist", "date", "tracknumber",
    "tracktotal", "discnumber", "disctotal", "genre", "composer",
    "performer", "comment", "copyright", "encoder", "isrc", "language",
    "organization", "lyrics", "bpm",
};

_Static_assert(GAUD_TAG_COUNT == 20,
    "a tag was added: give it a name in tag_names[], a mapping in each "
    "scheme that can carry it, and a row in the round-trip test");

const char * gaud_tag_name(GAUD_Tag tag) {
  if (tag < 0 || tag >= GAUD_TAG_COUNT || !tag_names[tag]) {
    return "unknown";
  }
  return tag_names[tag];
}

bool gaud_tag_from_name(const char * name, GAUD_Tag * out_tag) {
  if (!name || !out_tag) {
    return false;
  }
  for (int i = 0; i < GAUD_TAG_COUNT; ++i) {
    if (tag_names[i] && strcmp(tag_names[i], name) == 0) {
      *out_tag = (GAUD_Tag)i;
      return true;
    }
  }
  return false;
}

/* --------------------------------------------------------------- helpers */

const GAUD_Allocator * gaud_meta_allocator(const GAUD_Meta * meta) {
  return meta ? meta->allocator : gaud_allocator_default();
}

/** Copy a string into @p allocator's memory. NULL becomes "". */
char * gaud_meta_dup(const GAUD_Allocator * allocator, const char * text) {
  if (!text) {
    text = "";
  }
  size_t length = strlen(text) + 1u;
  char * copy = gcu_allocator_malloc(allocator, length);
  if (copy) {
    memcpy(copy, text, length);
  }
  return copy;
}

/**
 * Room for one more element in an array of @p element_size.
 *
 * Returns the new base, or NULL on failure, and updates @p capacity only
 * when it succeeds. Written to take and return `void *` rather than a
 * `void ***` out-parameter: casting a `char ***` to `void ***` is a
 * strict-aliasing violation, and this library builds with
 * -Wstrict-aliasing=1 -Werror precisely so that it does not compile.
 */
static void * grow_array(const GAUD_Allocator * allocator, void * items,
    size_t count, size_t * capacity, size_t element_size) {
  if (count < *capacity) {
    return items;
  }
  size_t next = *capacity ? *capacity * 2u : 8u;
  /* Overflow is checked rather than assumed away: these counts come from
   * a file, and a file can claim a great many entries. */
  if (next < *capacity || next > SIZE_MAX / element_size) {
    return NULL;
  }
  void * grown
      = gcu_allocator_realloc(allocator, items, next * element_size);
  if (!grown) {
    return NULL;
  }
  *capacity = next;
  return grown;
}

/* -------------------------------------------------------------- lifetime */

GAUD_Result gaud_meta_create(
    const GAUD_Allocator * allocator, GAUD_Meta ** out_meta) {
  if (!out_meta) {
    return GAUD_ERR_INVALID;
  }
  if (!allocator) {
    allocator = gaud_allocator_default();
  }
  GAUD_Meta * meta = gcu_allocator_malloc(allocator, sizeof(GAUD_Meta));
  if (!meta) {
    return GAUD_ERR_OOM;
  }
  memset(meta, 0, sizeof(*meta));
  meta->allocator = allocator;
  *out_meta = meta;
  return GAUD_OK;
}

void gaud_meta_destroy(GAUD_Meta * meta) {
  if (!meta) {
    return;
  }
  const GAUD_Allocator * allocator = meta->allocator;
  for (int t = 0; t < GAUD_TAG_COUNT; ++t) {
    for (size_t i = 0; i < meta->tags[t].count; ++i) {
      gcu_allocator_free(allocator, meta->tags[t].values[i]);
    }
    gcu_allocator_free(allocator, meta->tags[t].values);
  }
  for (size_t i = 0; i < meta->custom_count; ++i) {
    gcu_allocator_free(allocator, meta->custom[i].key);
    gcu_allocator_free(allocator, meta->custom[i].value);
  }
  gcu_allocator_free(allocator, meta->custom);
  for (size_t i = 0; i < meta->raw_count; ++i) {
    gcu_allocator_free(allocator, meta->raw[i].scheme);
    gcu_allocator_free(allocator, meta->raw[i].id);
    gcu_allocator_free(allocator, meta->raw[i].data);
  }
  gcu_allocator_free(allocator, meta->raw);
  for (size_t i = 0; i < meta->picture_count; ++i) {
    gcu_allocator_free(allocator, meta->pictures[i].owned_mime);
    gcu_allocator_free(allocator, meta->pictures[i].owned_description);
    gcu_allocator_free(allocator, meta->pictures[i].owned_data);
  }
  gcu_allocator_free(allocator, meta->pictures);
  gcu_allocator_free(allocator, meta);
}

/* ----------------------------------------------------------- common tags */

size_t gaud_meta_count(const GAUD_Meta * meta, GAUD_Tag tag) {
  if (!meta || tag < 0 || tag >= GAUD_TAG_COUNT) {
    return 0;
  }
  return meta->tags[tag].count;
}

const char * gaud_meta_get(
    const GAUD_Meta * meta, GAUD_Tag tag, size_t index) {
  if (!meta || tag < 0 || tag >= GAUD_TAG_COUNT
      || index >= meta->tags[tag].count) {
    return NULL;
  }
  return meta->tags[tag].values[index];
}

GAUD_Result gaud_meta_add(
    GAUD_Meta * meta, GAUD_Tag tag, const char * value) {
  if (!meta || tag < 0 || tag >= GAUD_TAG_COUNT || !value) {
    return GAUD_ERR_INVALID;
  }
  GAUD_Tag_Values * slot = &meta->tags[tag];
  char ** grown = grow_array(meta->allocator, slot->values, slot->count,
      &slot->capacity, sizeof(char *));
  if (!grown) {
    return GAUD_ERR_OOM;
  }
  slot->values = grown;
  char * copy = gaud_meta_dup(meta->allocator, value);
  if (!copy) {
    return GAUD_ERR_OOM;
  }
  slot->values[slot->count++] = copy;
  return GAUD_OK;
}

GAUD_Result gaud_meta_add_unique(
    GAUD_Meta * meta, GAUD_Tag tag, const char * value) {
  if (!meta || tag < 0 || tag >= GAUD_TAG_COUNT || !value) {
    return GAUD_ERR_INVALID;
  }
  for (size_t i = 0; i < meta->tags[tag].count; ++i) {
    if (strcmp(meta->tags[tag].values[i], value) == 0) {
      return GAUD_OK;
    }
  }
  return gaud_meta_add(meta, tag, value);
}

GAUD_Result gaud_meta_custom_add_unique(
    GAUD_Meta * meta, const char * key, const char * value) {
  if (!meta || !key || !value) {
    return GAUD_ERR_INVALID;
  }
  for (size_t i = 0; i < meta->custom_count; ++i) {
    if (strcmp(meta->custom[i].key, key) == 0
        && strcmp(meta->custom[i].value, value) == 0) {
      return GAUD_OK;
    }
  }
  return gaud_meta_custom_add(meta, key, value);
}

void gaud_meta_clear(GAUD_Meta * meta, GAUD_Tag tag) {
  if (!meta || tag < 0 || tag >= GAUD_TAG_COUNT) {
    return;
  }
  GAUD_Tag_Values * slot = &meta->tags[tag];
  for (size_t i = 0; i < slot->count; ++i) {
    gcu_allocator_free(meta->allocator, slot->values[i]);
  }
  slot->count = 0;
}

/* ----------------------------------------------------------- custom keys */

size_t gaud_meta_custom_count(const GAUD_Meta * meta) {
  return meta ? meta->custom_count : 0;
}

GAUD_Result gaud_meta_custom(const GAUD_Meta * meta, size_t index,
    const char ** out_key, const char ** out_value) {
  if (!meta || index >= meta->custom_count) {
    return GAUD_ERR_INVALID;
  }
  if (out_key) {
    *out_key = meta->custom[index].key;
  }
  if (out_value) {
    *out_value = meta->custom[index].value;
  }
  return GAUD_OK;
}

GAUD_Result gaud_meta_custom_add(
    GAUD_Meta * meta, const char * key, const char * value) {
  if (!meta || !key || !value || key[0] == '\0') {
    return GAUD_ERR_INVALID;
  }
  GAUD_Custom_Entry * grown_custom
      = grow_array(meta->allocator, meta->custom, meta->custom_count,
          &meta->custom_capacity, sizeof(GAUD_Custom_Entry));
  if (!grown_custom) {
    return GAUD_ERR_OOM;
  }
  meta->custom = grown_custom;
  char * key_copy = gaud_meta_dup(meta->allocator, key);
  char * value_copy = gaud_meta_dup(meta->allocator, value);
  if (!key_copy || !value_copy) {
    gcu_allocator_free(meta->allocator, key_copy);
    gcu_allocator_free(meta->allocator, value_copy);
    return GAUD_ERR_OOM;
  }
  meta->custom[meta->custom_count].key = key_copy;
  meta->custom[meta->custom_count].value = value_copy;
  ++meta->custom_count;
  return GAUD_OK;
}

/* ------------------------------------------------------------------- raw */

GAUD_Result gaud_meta_raw_attach(GAUD_Meta * meta, const char * scheme,
    const char * id, const void * data, size_t size) {
  return gaud_meta_raw_attach_flagged(meta, scheme, id, data, size, 0);
}

uint16_t gaud_meta_raw_flags(const GAUD_Meta * meta, size_t index) {
  if (!meta || index >= meta->raw_count) {
    return 0;
  }
  return meta->raw[index].flags;
}

GAUD_Result gaud_meta_raw_attach_flagged(GAUD_Meta * meta,
    const char * scheme, const char * id, const void * data, size_t size,
    uint16_t flags) {
  if (!meta || !scheme || !id || (!data && size > 0)) {
    return GAUD_ERR_INVALID;
  }
  GAUD_Raw_Block * grown_raw = grow_array(meta->allocator, meta->raw,
      meta->raw_count, &meta->raw_capacity, sizeof(GAUD_Raw_Block));
  if (!grown_raw) {
    return GAUD_ERR_OOM;
  }
  meta->raw = grown_raw;
  char * scheme_copy = gaud_meta_dup(meta->allocator, scheme);
  char * id_copy = gaud_meta_dup(meta->allocator, id);
  unsigned char * data_copy = NULL;
  if (size > 0) {
    data_copy = gcu_allocator_malloc(meta->allocator, size);
    if (data_copy) {
      memcpy(data_copy, data, size);
    }
  }
  if (!scheme_copy || !id_copy || (size > 0 && !data_copy)) {
    gcu_allocator_free(meta->allocator, scheme_copy);
    gcu_allocator_free(meta->allocator, id_copy);
    gcu_allocator_free(meta->allocator, data_copy);
    return GAUD_ERR_OOM;
  }
  meta->raw[meta->raw_count].scheme = scheme_copy;
  meta->raw[meta->raw_count].id = id_copy;
  meta->raw[meta->raw_count].data = data_copy;
  meta->raw[meta->raw_count].size = size;
  meta->raw[meta->raw_count].flags = flags;
  ++meta->raw_count;
  return GAUD_OK;
}

size_t gaud_meta_raw_count(const GAUD_Meta * meta) {
  return meta ? meta->raw_count : 0;
}

GAUD_Result gaud_meta_raw(const GAUD_Meta * meta, size_t index,
    const char ** out_scheme, const char ** out_id, const void ** out_data,
    size_t * out_size) {
  if (!meta || index >= meta->raw_count) {
    return GAUD_ERR_INVALID;
  }
  if (out_scheme) {
    *out_scheme = meta->raw[index].scheme;
  }
  if (out_id) {
    *out_id = meta->raw[index].id;
  }
  if (out_data) {
    *out_data = meta->raw[index].data;
  }
  if (out_size) {
    *out_size = meta->raw[index].size;
  }
  return GAUD_OK;
}

/* --------------------------------------------------------------- pictures */

size_t gaud_meta_picture_count(const GAUD_Meta * meta) {
  return meta ? meta->picture_count : 0;
}

const GAUD_Picture * gaud_meta_picture(
    const GAUD_Meta * meta, size_t index) {
  if (!meta || index >= meta->picture_count) {
    return NULL;
  }
  return &meta->pictures[index].picture;
}

GAUD_Result gaud_meta_picture_add(GAUD_Meta * meta, GAUD_Picture_Kind kind,
    const char * mime_type, const char * description, const void * data,
    size_t size) {
  return gaud_meta_picture_add_stated(
      meta, kind, mime_type, description, data, size, 0, 0, 0, 0);
}

GAUD_Result gaud_meta_picture_add_stated(GAUD_Meta * meta,
    GAUD_Picture_Kind kind, const char * mime_type, const char * description,
    const void * data, size_t size, uint32_t stated_width,
    uint32_t stated_height, uint32_t stated_depth, uint32_t stated_colors) {
  if (!meta || kind < 0 || kind >= GAUD_PICTURE_KIND_COUNT
      || (!data && size > 0)) {
    return GAUD_ERR_INVALID;
  }
  GAUD_Picture_Entry * grown_pictures
      = grow_array(meta->allocator, meta->pictures, meta->picture_count,
          &meta->picture_capacity, sizeof(GAUD_Picture_Entry));
  if (!grown_pictures) {
    return GAUD_ERR_OOM;
  }
  if (grown_pictures != meta->pictures) {
    meta->pictures = grown_pictures;
    /* The GAUD_Picture each entry exposes points into the entry's own
     * copies, and realloc moved every one of them. Repointing here
     * rather than storing offsets: a caller holding a pointer from
     * before an add is holding a dangling one either way, and the header
     * says the pointer is valid until meta changes. */
    for (size_t i = 0; i < meta->picture_count; ++i) {
      meta->pictures[i].picture.mime_type = meta->pictures[i].owned_mime;
      meta->pictures[i].picture.description
          = meta->pictures[i].owned_description;
      meta->pictures[i].picture.data = meta->pictures[i].owned_data;
    }
  }

  GAUD_Picture_Entry * entry = &meta->pictures[meta->picture_count];
  memset(entry, 0, sizeof(*entry));
  entry->owned_mime = gaud_meta_dup(meta->allocator, mime_type);
  entry->owned_description = gaud_meta_dup(meta->allocator, description);
  if (size > 0) {
    entry->owned_data = gcu_allocator_malloc(meta->allocator, size);
    if (entry->owned_data) {
      memcpy(entry->owned_data, data, size);
    }
  }
  if (!entry->owned_mime || !entry->owned_description
      || (size > 0 && !entry->owned_data)) {
    gcu_allocator_free(meta->allocator, entry->owned_mime);
    gcu_allocator_free(meta->allocator, entry->owned_description);
    gcu_allocator_free(meta->allocator, entry->owned_data);
    memset(entry, 0, sizeof(*entry));
    return GAUD_ERR_OOM;
  }

  entry->picture.kind = kind;
  entry->picture.mime_type = entry->owned_mime;
  entry->picture.description = entry->owned_description;
  entry->picture.data = entry->owned_data;
  entry->picture.size = size;
  entry->picture.stated_width = stated_width;
  entry->picture.stated_height = stated_height;
  entry->picture.stated_depth = stated_depth;
  entry->picture.stated_colors = stated_colors;
  /* Nothing has been checked yet. The reader calls
   * gaud_meta_verify_pictures() once the whole file is parsed, which is
   * the only place the ?image arm is consulted. */
  entry->picture.status = (stated_width || stated_height)
      ? GAUD_PICTURE_UNVERIFIED
      : GAUD_PICTURE_NOT_STATED;
  ++meta->picture_count;
  return GAUD_OK;
}

/* ------------------------------------------------------------------ copy */

GAUD_Result gaud_meta_copy(const GAUD_Meta * source,
    const GAUD_Allocator * allocator, GAUD_Meta ** out_meta) {
  if (!source || !out_meta) {
    return GAUD_ERR_INVALID;
  }
  GAUD_Meta * copy = NULL;
  GAUD_Result result = gaud_meta_create(allocator, &copy);
  if (result != GAUD_OK) {
    return result;
  }
  for (int t = 0; t < GAUD_TAG_COUNT && result == GAUD_OK; ++t) {
    for (size_t i = 0; i < source->tags[t].count && result == GAUD_OK; ++i) {
      result = gaud_meta_add(copy, (GAUD_Tag)t, source->tags[t].values[i]);
    }
  }
  for (size_t i = 0; i < source->custom_count && result == GAUD_OK; ++i) {
    result = gaud_meta_custom_add(
        copy, source->custom[i].key, source->custom[i].value);
  }
  for (size_t i = 0; i < source->raw_count && result == GAUD_OK; ++i) {
    result = gaud_meta_raw_attach_flagged(copy, source->raw[i].scheme,
        source->raw[i].id, source->raw[i].data, source->raw[i].size,
        source->raw[i].flags);
  }
  for (size_t i = 0; i < source->picture_count && result == GAUD_OK; ++i) {
    const GAUD_Picture * p = &source->pictures[i].picture;
    result = gaud_meta_picture_add_stated(copy, p->kind, p->mime_type,
        p->description, p->data, p->size, p->stated_width, p->stated_height,
        p->stated_depth, p->stated_colors);
    if (result == GAUD_OK) {
      /* The verification travelled with the picture: re-deriving it would
       * mean re-decoding every image on every copy, and a copy made in a
       * build without `image` would silently downgrade a VERIFIED to an
       * UNVERIFIED and lose a fact. */
      GAUD_Picture_Entry * entry = &copy->pictures[copy->picture_count - 1u];
      entry->picture.status = p->status;
      entry->picture.verified_width = p->verified_width;
      entry->picture.verified_height = p->verified_height;
    }
  }
  if (result != GAUD_OK) {
    gaud_meta_destroy(copy);
    return result;
  }
  *out_meta = copy;
  return GAUD_OK;
}
