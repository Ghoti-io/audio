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
 * @file meta.h
 *
 * Tags, raw carriage, and cover art.
 *
 * Two layers, which is `image`'s arrangement and for the same reason: a
 * **common** vocabulary that every scheme maps onto, so a caller can ask
 * for the title without knowing whether the file is an MP3 or a WAV, and
 * **raw** carriage of the bytes underneath it, so a round trip can put back
 * what this library did not interpret.
 *
 * Audio departs from `image` in one way that matters. `image`'s common
 * metadata is a small fixed struct - orientation, DPI, description -
 * because image metadata largely *is* a fixed set. Audio's is not: Vorbis
 * comment has no registry at all and every writer invents keys, ID3v2 has
 * some eighty frame identifiers, and a tag may legitimately occur more than
 * once (two artists, three performers). So the common layer here is an
 * **enumerable, multi-valued key/value list** over a normalised vocabulary,
 * with unmapped keys kept under their own spelling rather than discarded.
 *
 * Everything crossing this API is **UTF-8**, without exception. ID3v2's
 * four text encodings and ID3v1's Latin-1 are decoded on the way in, and
 * a frame that *claims* UTF-8 and is not is read as Latin-1 rather than
 * passed through - because a promise that holds only for well-formed
 * input is not a promise, and a caller's validator rejecting a string
 * this library handed it is this library's defect. What goes out is
 * whatever the target scheme requires.
 */

#ifndef GHOTI_IO_GAUD_META_H
#define GHOTI_IO_GAUD_META_H

#include <ghoti.io/audio/allocator.h>
#include <ghoti.io/audio/core.h>
#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A document's tags, raw blocks and pictures. */
typedef struct GAUD_Meta GAUD_Meta;

/**
 * @brief The normalised tag vocabulary.
 *
 * What every scheme is mapped onto. A tag that has no entry here keeps its
 * native spelling and is reached through gaud_meta_custom_*(), because
 * inventing an enumerator for every key any writer has ever used is not
 * possible - Vorbis comment has no registry - and silently dropping the
 * ones that do not fit would make a round trip lossy.
 *
 * ::GAUD_TAG_TRACK_NUMBER and ::GAUD_TAG_TRACK_TOTAL are separate because
 * the schemes disagree: ID3v2's `TRCK` is "4/12" in one string, Vorbis
 * comment uses two keys. Splitting on read and joining on write is done
 * once, here, rather than in each codec.
 */
typedef enum {
  GAUD_TAG_TITLE = 0,    ///< The recording's name.
  GAUD_TAG_ARTIST,       ///< The performing artist.
  GAUD_TAG_ALBUM,        ///< The release it belongs to.
  GAUD_TAG_ALBUM_ARTIST, ///< The release's artist, where it differs.
  GAUD_TAG_DATE,         ///< ISO 8601 as far as the file states it.
  GAUD_TAG_TRACK_NUMBER, ///< Position in the release.
  GAUD_TAG_TRACK_TOTAL,  ///< How many there are.
  GAUD_TAG_DISC_NUMBER,  ///< Which disc.
  GAUD_TAG_DISC_TOTAL,   ///< How many discs.
  GAUD_TAG_GENRE,        ///< As text, never as ID3v1's numeric index.
  GAUD_TAG_COMPOSER,     ///< Who wrote it.
  GAUD_TAG_PERFORMER,    ///< A performer, where distinct from the artist.
  GAUD_TAG_COMMENT,      ///< Free text.
  GAUD_TAG_COPYRIGHT,    ///< The copyright statement.
  GAUD_TAG_ENCODER,      ///< What produced the file.
  GAUD_TAG_ISRC,         ///< International Standard Recording Code.
  GAUD_TAG_LANGUAGE,     ///< As the file stated it.
  GAUD_TAG_ORGANIZATION, ///< Label or publisher.
  GAUD_TAG_LYRICS,       ///< Unsynchronised lyrics.
  GAUD_TAG_BPM,          ///< Beats per minute, as text.
  GAUD_TAG_COUNT         ///< Closes the enum so a test can check the names.
} GAUD_Tag;

/**
 * @brief The canonical name of @p tag: "title", "artist", "album"...
 * @return A static string, never NULL; "unknown" outside the enum.
 */
GAUD_API const char * gaud_tag_name(GAUD_Tag tag);

/**
 * @brief The tag a canonical name denotes.
 * @return true and writes @p out_tag, or false when @p name is not one.
 */
GAUD_API bool gaud_tag_from_name(const char * name, GAUD_Tag * out_tag);

/* ------------------------------------------------------------- lifetime */

/** @brief An empty metadata object. NULL allocator is the default. */
GAUD_API GAUD_Result gaud_meta_create(
    const GAUD_Allocator * allocator, GAUD_Meta ** out_meta);

/** @brief Release it and everything it owns. NULL is a no-op. */
GAUD_API void gaud_meta_destroy(GAUD_Meta * meta);

/**
 * @brief A deep copy, so a caller can keep a document's metadata after the
 *   document is destroyed - which is what a tagger rewriting a file does.
 */
GAUD_API GAUD_Result gaud_meta_copy(const GAUD_Meta * source,
    const GAUD_Allocator * allocator, GAUD_Meta ** out_meta);

/* ----------------------------------------------------------- common tags */

/**
 * @brief How many values @p tag has. Zero means the file did not state it.
 *
 * More than one is normal and not an error: two artists are two values, and
 * collapsing them into one string with a separator this library chose would
 * be a guess the caller cannot undo.
 */
GAUD_API size_t gaud_meta_count(const GAUD_Meta * meta, GAUD_Tag tag);

/**
 * @brief Value @p index of @p tag as UTF-8, or NULL when there is none.
 *
 * Borrowed from @p meta and valid until it is changed or destroyed.
 */
GAUD_API const char * gaud_meta_get(
    const GAUD_Meta * meta, GAUD_Tag tag, size_t index);

/** @brief Append a value to @p tag. The string is copied. */
GAUD_API GAUD_Result gaud_meta_add(
    GAUD_Meta * meta, GAUD_Tag tag, const char * value);

/** @brief Remove every value of @p tag. */
GAUD_API void gaud_meta_clear(GAUD_Meta * meta, GAUD_Tag tag);

/* ----------------------------------------------------------- custom keys */

/**
 * @brief How many keys the file carried that the vocabulary has no name for.
 *
 * Vorbis comment has no registry, ID3v2 has `TXXX`, and MP4 has `----`.
 * Everything they carry that does not map is here rather than lost.
 */
GAUD_API size_t gaud_meta_custom_count(const GAUD_Meta * meta);

/**
 * @brief Custom entry @p index: its key and value, both UTF-8.
 *
 * @param meta The metadata to read from.
 * @param index Which custom entry, from zero.
 * @param out_key May be NULL. Borrowed.
 * @param out_value May be NULL. Borrowed.
 * @return ::GAUD_OK, or ::GAUD_ERR_INVALID when @p index is past the end.
 */
GAUD_API GAUD_Result gaud_meta_custom(const GAUD_Meta * meta, size_t index,
    const char ** out_key, const char ** out_value);

/** @brief Append a custom key/value pair. Both are copied. */
GAUD_API GAUD_Result gaud_meta_custom_add(
    GAUD_Meta * meta, const char * key, const char * value);

/* ------------------------------------------------------------------- raw */

/**
 * @brief Attach the bytes of something this library did not interpret.
 *
 * @param meta Where to attach it.
 * @param scheme Which vocabulary the identifier belongs to: "id3v2",
 *   "riff", "aiff", "bext". Copied.
 * @param id The frame or chunk identifier within it, as text so that a
 *   four-character code reads as itself. Copied.
 * @param data Copied.
 * @param size How many bytes; zero is allowed and @p data may be NULL.
 */
GAUD_API GAUD_Result gaud_meta_raw_attach(GAUD_Meta * meta,
    const char * scheme, const char * id, const void * data, size_t size);

/** @brief How many raw blocks there are. */
GAUD_API size_t gaud_meta_raw_count(const GAUD_Meta * meta);

/**
 * @brief Raw block @p index. Any out pointer may be NULL.
 *
 * @param meta The metadata to read from.
 * @param index Which raw block, from zero.
 * @param out_scheme May be NULL. Borrowed.
 * @param out_id May be NULL. Borrowed.
 * @param out_data May be NULL. Borrowed from @p meta.
 * @param out_size May be NULL.
 */
GAUD_API GAUD_Result gaud_meta_raw(const GAUD_Meta * meta, size_t index,
    const char ** out_scheme, const char ** out_id,
    const void ** out_data, size_t * out_size);

/* ------------------------------------------------------------- pictures */

/** @brief What a picture is of, following ID3v2's `APIC` list. */
typedef enum {
  GAUD_PICTURE_OTHER = 0,     ///< Unstated or none of the below.
  GAUD_PICTURE_FRONT_COVER,   ///< The front of the release.
  GAUD_PICTURE_BACK_COVER,    ///< The back.
  GAUD_PICTURE_LEAFLET,       ///< A booklet page.
  GAUD_PICTURE_MEDIA,         ///< The disc or label itself.
  GAUD_PICTURE_ARTIST,        ///< Performer or band.
  GAUD_PICTURE_ICON,          ///< A small file icon.
  GAUD_PICTURE_KIND_COUNT     ///< Closes the enum.
} GAUD_Picture_Kind;

/**
 * @brief Whether a picture's stated dimensions were checked, and how it
 *   went.
 *
 * Four states and not three, which is planning/audio.md 11.4's point. "The
 * container said nothing" and "this build cannot check" are different
 * facts, and collapsing them is the difference between *your build is
 * minimal* and *this file is corrupt*. Only FLAC's `PICTURE` block states
 * dimensions at all; ID3v2's `APIC` and MP4's `covr` state none, so for
 * those the answer is always ::GAUD_PICTURE_NOT_STATED.
 */
typedef enum {
  /** The container carries no dimensions to check. */
  GAUD_PICTURE_NOT_STATED = 0,
  /** Stated, and this build has no `image` to check them against. */
  GAUD_PICTURE_UNVERIFIED,
  /** Stated, checked, and the payload agrees. */
  GAUD_PICTURE_VERIFIED,
  /** Stated, checked, and the payload disagrees or would not parse. */
  GAUD_PICTURE_MISMATCH,
  GAUD_PICTURE_STATUS_COUNT
} GAUD_Picture_Status;

/**
 * @brief One embedded picture.
 *
 * The `stated_*` fields are always what the container said, in every build.
 * The `verified_*` fields are filled only when @p status is
 * ::GAUD_PICTURE_VERIFIED or ::GAUD_PICTURE_MISMATCH. **The two builds
 * differ additively**: a caller that ignores the verified fields gets
 * correct answers either way, just fewer of them. A single `width` meaning
 * "stated" in one build and "measured" in the other would be a field whose
 * meaning the caller cannot determine.
 */
typedef struct {
  GAUD_Picture_Kind kind;      ///< What it is of.
  const char * mime_type;      ///< "image/png"; borrowed. Never NULL.
  const char * description;    ///< May be empty; borrowed. Never NULL.
  const void * data;           ///< The encoded image; borrowed.
  size_t size;                 ///< How many bytes.
  uint32_t stated_width;       ///< As the container said; 0 if it did not.
  uint32_t stated_height;      ///< As the container said; 0 if it did not.
  uint32_t stated_depth;       ///< Bits per pixel, as stated; 0 if not.
  uint32_t stated_colors;      ///< Palette size, as stated; 0 if not.
  GAUD_Picture_Status status;  ///< Whether the above were checked.
  uint32_t verified_width;     ///< Measured, when @p status says so.
  uint32_t verified_height;    ///< Measured, when @p status says so.
} GAUD_Picture;

/** @brief How many pictures the file carried. */
GAUD_API size_t gaud_meta_picture_count(const GAUD_Meta * meta);

/**
 * @brief Picture @p index, or NULL when there is none.
 *
 * Borrowed from @p meta, as is everything it points at.
 */
GAUD_API const GAUD_Picture * gaud_meta_picture(
    const GAUD_Meta * meta, size_t index);

/**
 * @brief Add a picture. @p mime_type, @p description and the bytes are all
 *   copied.
 *
 * The `stated_*` and verification fields are the reader's to fill; a
 * picture added here is stated by whoever writes it out.
 */
GAUD_API GAUD_Result gaud_meta_picture_add(GAUD_Meta * meta,
    GAUD_Picture_Kind kind, const char * mime_type, const char * description,
    const void * data, size_t size);

/* -------------------------------------------------------- save policies */

/**
 * @brief What a writer does with metadata it was given.
 *
 * No policy strips anything silently. ::GAUD_META_DROP_ALL is the one that
 * removes, and it is named.
 */
typedef enum {
  /** Write the common tags, the raw blocks and the pictures. */
  GAUD_META_PRESERVE_ALL = 0,
  /** Write nothing. The file carries no metadata at all. */
  GAUD_META_DROP_ALL,
  /** Write only what the common vocabulary covers, in the target's own
   *  spelling. Raw blocks this library did not interpret are dropped -
   *  which is what a caller transcoding between containers wants, since a
   *  RIFF chunk means nothing inside an AIFF. */
  GAUD_META_KEEP_COMMON_ONLY,
  /** Write only the raw blocks, byte for byte. For a caller who wants the
   *  file it started with and not this library's reading of it. */
  GAUD_META_KEEP_RAW_ONLY,
  GAUD_META_POLICY_COUNT
} GAUD_Meta_Policy;

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_META_H
