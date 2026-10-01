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
 * The tag schemes, as seen by a container. Never installed.
 *
 * A container knows it has a chunk of bytes and what the chunk is called.
 * Everything past that - which scheme the bytes are in, how its text is
 * encoded, which of its identifiers map onto the common vocabulary - is
 * here, so that WAV and AIFF share one ID3 reader rather than two.
 */

#ifndef GHOTI_IO_GAUD_SRC_META_SCHEME_H
#define GHOTI_IO_GAUD_SRC_META_SCHEME_H

#include "../core/meta_internal.h"
#include <ghoti.io/audio/core.h>
#include <ghoti.io/audio/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ ID3 */

/**
 * @brief Parse an ID3v2 block into @p meta.
 *
 * @param data The whole block including its ten-byte header.
 * @param size How many bytes that is.
 * @param limits Never NULL; caps entries, total bytes and picture size.
 * @param meta Where tags, custom keys and pictures go.
 * @param diagnostics May be NULL.
 * @return ::GAUD_OK even for a block with frames this does not know - they
 *   are kept raw. ::GAUD_ERR_CORRUPT only when the block's own framing is
 *   broken, and ::GAUD_ERR_LIMIT when a cap is exceeded.
 */
GAUD_Result gaud_id3v2_parse(const unsigned char * data, size_t size,
    const GAUD_Limits * limits, GAUD_Meta * meta,
    GAUD_Diagnostics * diagnostics);

/**
 * @brief Build an ID3v2.4 block from @p meta.
 *
 * **2.4 and not 2.3**, for one reason that decides it: 2.4 has UTF-8 as a
 * text encoding and 2.3 does not. Writing 2.3 would mean every non-Latin-1
 * string going out as UTF-16, which is larger, has a byte-order mark to get
 * wrong, and is the encoding every reader has a bug in.
 *
 * @param meta What to write.
 * @param policy Decides whether raw blocks are written back.
 * @param allocator NULL for the default.
 * @param out_data Receives an allocated block; the caller frees it with
 *   @p allocator. NULL and zero when there is nothing to write.
 * @param out_size Receives its length, or zero.
 */
GAUD_Result gaud_id3v2_build(const GAUD_Meta * meta, GAUD_Meta_Policy policy,
    const GAUD_Allocator * allocator, unsigned char ** out_data,
    size_t * out_size);

/**
 * @brief Parse the 128 bytes of an ID3v1 trailer into @p meta.
 *
 * Only fills a tag the block has and @p meta does not: an ID3v2 block is
 * richer and is read first, so ID3v1 is a fallback for fields nothing else
 * supplied rather than an override. A file carrying both usually carries
 * the same thing truncated to 30 characters in the older one.
 *
 * @return false when @p data is not an ID3v1 trailer.
 */
bool gaud_id3v1_parse(const unsigned char data[128], GAUD_Meta * meta);

/** @brief The genre name ID3v1 number @p index denotes, or NULL. */
const char * gaud_id3v1_genre(unsigned index);

/** @brief How many genre numbers gaud_id3v1_genre() knows. */
unsigned gaud_id3v1_genre_count(void);

/**
 * @brief Map an APIC-registry picture type onto ::GAUD_Picture_Kind.
 *
 * Here rather than in the ID3 reader because FLAC's PICTURE block uses the
 * same registry; see the definition in `id3_frames.c`.
 */
GAUD_Picture_Kind gaud_picture_kind_from_apic(unsigned type);

/** @brief The APIC type number ::GAUD_Picture_Kind @p kind is written as. */
unsigned gaud_picture_kind_to_apic(GAUD_Picture_Kind kind);

/* ------------------------------------------------------- RIFF LIST INFO */

/**
 * @brief Parse the body of a `LIST` chunk whose type is `INFO`.
 *
 * @param data Positioned after the four-character list type.
 * @param size How many bytes follow it.
 * @param limits Never NULL; caps how many entries are kept.
 * @param meta Where the tags go.
 * @param diagnostics May be NULL.
 */
GAUD_Result gaud_riff_info_parse(const unsigned char * data, size_t size,
    const GAUD_Limits * limits, GAUD_Meta * meta,
    GAUD_Diagnostics * diagnostics);

/**
 * @brief Build a `LIST`/`INFO` chunk, header and all, from @p meta.
 *
 * @param meta What to write.
 * @param allocator NULL for the default.
 * @param out_data Receives an allocated chunk; NULL when there is
 *   nothing to write.
 * @param out_size Receives its length, or zero.
 */
GAUD_Result gaud_riff_info_build(const GAUD_Meta * meta,
    const GAUD_Allocator * allocator, unsigned char ** out_data,
    size_t * out_size);

/* ------------------------------------------------------------ BWF bext */

/**
 * @brief Parse a `bext` chunk into @p meta's common tags.
 *
 * @param data The chunk body.
 * @param size How many bytes; under 602 is refused, which is the fixed
 *   part's length in every version of the specification.
 * @param meta Where the description and originator go.
 * @param diagnostics May be NULL.
 */
GAUD_Result gaud_bext_parse(const unsigned char * data, size_t size,
    GAUD_Meta * meta, GAUD_Diagnostics * diagnostics);

/* -------------------------------------------------- Vorbis comment */

/**
 * @brief Parse a Vorbis comment block into @p meta.
 *
 * Here rather than under `src/codec/flac/` because FLAC's VORBIS_COMMENT
 * block, Vorbis's comment header packet and Opus's `OpusTags` packet are
 * byte-for-byte the same structure. Phase 6 adds the other two callers.
 *
 * @param data The block body: the vendor length onwards, with no framing
 *   bit and no packet header.
 * @param size How many bytes that is.
 * @param limits Never NULL; caps how many comments are kept.
 * @param meta Where the tags go. The **vendor string does not reach it**:
 *   it names the software that wrote the file rather than anything about
 *   the recording, and the writer replaces it, so a reader that collected
 *   it would make the round trip gain a value per generation.
 * @param diagnostics May be NULL.
 * @return ::GAUD_ERR_CORRUPT when a length runs past the block, which is
 *   what a reader that applied FLAC's big-endian order to these
 *   little-endian fields would see on the very first one.
 */
GAUD_Result gaud_vorbis_comment_parse(const unsigned char * data, size_t size,
    const GAUD_Limits * limits, GAUD_Meta * meta,
    GAUD_Diagnostics * diagnostics);

/**
 * @brief Build a Vorbis comment block from @p meta.
 *
 * @param meta What to write.
 * @param vendor The vendor string, or NULL for an empty one.
 * @param allocator NULL for the default.
 * @param out_data Receives an allocated block; the caller frees it.
 * @param out_size Receives its length.
 */
GAUD_Result gaud_vorbis_comment_build(const GAUD_Meta * meta,
    const char * vendor, const GAUD_Allocator * allocator,
    unsigned char ** out_data, size_t * out_size);

/* ------------------------------------------------- AIFF's text chunks */

/**
 * @brief Map an AIFF text chunk id onto a tag.
 *
 * @param id Four characters, not NUL-terminated.
 * @param out_tag Written only when the answer is true.
 * @return true for `NAME`, `AUTH`, `(c) ` and `ANNO`, which are the only
 *   four text chunks AIFF has.
 */
bool gaud_aiff_text_tag(const char id[4], GAUD_Tag * out_tag);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_META_SCHEME_H
