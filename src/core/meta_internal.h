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
 * GAUD_Meta's layout, and the entry points a codec needs that a caller
 * does not. Never installed.
 */

#ifndef GHOTI_IO_GAUD_SRC_CORE_META_INTERNAL_H
#define GHOTI_IO_GAUD_SRC_CORE_META_INTERNAL_H

#include <ghoti.io/audio/macros.h>
#include <ghoti.io/audio/doc.h>
#include <ghoti.io/audio/meta.h>

/* The metadata tests are C++ and link these objects directly, so the
 * declarations need C linkage - as src/codec/shared's do, and for the
 * same reason. */
#ifdef __cplusplus
extern "C" {
#endif

/** The values of one common tag. */
typedef struct {
  char ** values;  ///< Owned, UTF-8, NUL-terminated.
  size_t count;    ///< How many.
  size_t capacity; ///< Room in @p values.
} GAUD_Tag_Values;

/** One key the vocabulary has no name for. */
typedef struct {
  char * key;   ///< Owned, as the file spelled it.
  char * value; ///< Owned, UTF-8.
} GAUD_Custom_Entry;

/** Bytes this library did not interpret. */
typedef struct {
  char * scheme;         ///< Owned: "id3v2", "riff", "aiff", "bext".
  char * id;             ///< Owned: the frame or chunk identifier.
  unsigned char * data;  ///< Owned. The element's BODY, not its header.
  size_t size;           ///< How many bytes.
  /**
   * The element's own header flags, where its scheme has any.
   *
   * ID3v2 frames do, and they decide how the frame is to be read: a
   * frame marked compressed or encrypted is one this library keeps raw
   * *because* of the flag. Writing it back with the flag cleared
   * relabels it as an ordinary frame, and the next reader then tries to
   * parse it, finds it malformed, and the frame is gone. The fuzzer
   * found exactly that: build, re-parse, re-build lost a frame per
   * generation.
   *
   * Zero for every scheme that has no such field.
   */
  uint16_t flags;
} GAUD_Raw_Block;

/**
 * A picture, plus the buffers its ::GAUD_Picture points into.
 *
 * The public struct holds borrowed pointers, and they have to point at
 * something this object owns - so the owner and the view live together
 * and the view is repointed whenever the array moves.
 */
typedef struct {
  GAUD_Picture picture;          ///< What a caller sees.
  char * owned_mime;             ///< Backs picture.mime_type.
  char * owned_description;      ///< Backs picture.description.
  unsigned char * owned_data;    ///< Backs picture.data.
} GAUD_Picture_Entry;

/** @brief Everything a document's metadata holds. */
struct GAUD_Meta {
  const GAUD_Allocator * allocator;      ///< Everything here comes from it.
  GAUD_Tag_Values tags[GAUD_TAG_COUNT];  ///< The common vocabulary.
  GAUD_Custom_Entry * custom;            ///< Owned.
  size_t custom_count;                   ///< How many.
  size_t custom_capacity;                ///< Room in @p custom.
  GAUD_Raw_Block * raw;                  ///< Owned.
  size_t raw_count;                      ///< How many.
  size_t raw_capacity;                   ///< Room in @p raw.
  GAUD_Picture_Entry * pictures;         ///< Owned.
  size_t picture_count;                  ///< How many.
  size_t picture_capacity;               ///< Room in @p pictures.
};

/** @brief The allocator @p meta was created with. */
const GAUD_Allocator * gaud_meta_allocator(const GAUD_Meta * meta);

/**
 * @brief Add a value to @p tag unless that exact value is already there.
 *
 * **What every scheme reader uses, and never gaud_meta_add().** A file
 * legitimately carries the same title in two places - a WAV written by
 * this library has it in `LIST`/`INFO` and again in `id3 `, because a
 * reader that knows only one should still find it - and a reader that
 * appended both would report two titles for a file that has one.
 *
 * The public gaud_meta_add() does not de-duplicate, because a caller
 * adding the same value twice has said something and the library is not
 * entitled to overrule it.
 */
GAUD_Result gaud_meta_add_unique(
    GAUD_Meta * meta, GAUD_Tag tag, const char * value);

/**
 * @brief Attach a raw block that carries scheme-level header flags.
 *
 * The public gaud_meta_raw_attach() is this with zero flags, which is
 * right for every scheme whose elements have none.
 */
GAUD_Result gaud_meta_raw_attach_flagged(GAUD_Meta * meta,
    const char * scheme, const char * id, const void * data, size_t size,
    uint16_t flags);

/** @brief The header flags raw block @p index carried. */
uint16_t gaud_meta_raw_flags(const GAUD_Meta * meta, size_t index);

/** @brief As gaud_meta_add_unique(), for a custom key. */
GAUD_Result gaud_meta_custom_add_unique(
    GAUD_Meta * meta, const char * key, const char * value);

/** @brief Copy a string into @p allocator's memory. NULL becomes "". */
char * gaud_meta_dup(const GAUD_Allocator * allocator, const char * text);

/**
 * @brief Add a picture whose container stated its dimensions.
 *
 * The public gaud_meta_picture_add() is this with the stated fields
 * zeroed, because a caller adding a picture has not read one out of a
 * container and has nothing to state.
 */
GAUD_Result gaud_meta_picture_add_stated(GAUD_Meta * meta,
    GAUD_Picture_Kind kind, const char * mime_type, const char * description,
    const void * data, size_t size, uint32_t stated_width,
    uint32_t stated_height, uint32_t stated_depth, uint32_t stated_colors);

/**
 * @brief Replace a document's metadata with @p meta, taking ownership.
 *
 * A container parses its chunks before it knows whether the file has a
 * track, so the metadata is built into a standalone ::GAUD_Meta and
 * moved in once the document exists. The document's previous (empty)
 * metadata is destroyed.
 */
void gaud_doc_take_meta(struct GAUD_Doc * doc, GAUD_Meta * meta);

/**
 * @brief Check every picture's stated dimensions against its bytes.
 *
 * Does nothing in a build without `image`, leaving each picture at
 * ::GAUD_PICTURE_UNVERIFIED - which is the state that exists to say "this
 * build cannot tell you", as distinct from "the container said nothing".
 * Called once by a reader when the file is fully parsed.
 */
void gaud_meta_verify_pictures(GAUD_Meta * meta);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CORE_META_INTERNAL_H
