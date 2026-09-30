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
 * ID3's text encodings and frame table, shared between the reader, the
 * writer and their tests. Never installed.
 */

#ifndef GHOTI_IO_GAUD_SRC_META_ID3_INTERNAL_H
#define GHOTI_IO_GAUD_SRC_META_ID3_INTERNAL_H

#include "scheme.h"
#include <ghoti.io/audio/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Latin-1 bytes to an allocated UTF-8 string. */
char * gaud_id3_latin1_to_utf8(
    const GAUD_Allocator * allocator, const unsigned char * in, size_t size);

/**
 * @brief UTF-16 to an allocated UTF-8 string.
 *
 * @param allocator Where the result comes from; NULL for the default.
 * @param in The UTF-16 bytes.
 * @param size How many.
 * @param big_endian The order to assume when there is no mark.
 * @param allow_bom Whether a leading mark may override that.
 */
char * gaud_id3_utf16_to_utf8(const GAUD_Allocator * allocator,
    const unsigned char * in, size_t size, bool big_endian, bool allow_bom);

/**
 * @brief Bytes from a scheme that states no encoding, to UTF-8.
 *
 * For RIFF `INFO` and AIFF's text chunks, which have no encoding field.
 * Valid UTF-8 is kept; anything else is read as Latin-1.
 */
char * gaud_id3_bytes_to_utf8(
    const GAUD_Allocator * allocator, const unsigned char * in, size_t size);

/** @brief Decode by ID3's encoding byte: 0 Latin-1, 1/2 UTF-16, 3 UTF-8. */
char * gaud_id3_decode_text(const GAUD_Allocator * allocator,
    unsigned encoding, const unsigned char * in, size_t size);

/** @brief One byte for Latin-1 and UTF-8, two for either UTF-16. */
size_t gaud_id3_terminator_width(unsigned encoding);

/**
 * @brief Where a string ends, or @p size when it is not terminated.
 *
 * An unterminated final field is normal - the frame's length ends it.
 */
size_t gaud_id3_find_terminator(
    const unsigned char * in, size_t size, unsigned encoding);

/**
 * @brief The common tag an ID3v2 frame identifier maps onto.
 *
 * @param id Four characters for 2.3 and 2.4, three for 2.2.
 * @param length 3 or 4.
 * @param out_tag Written only when the answer is true.
 * @return true and writes @p out_tag, or false for a frame with no mapping
 *   - which is kept raw rather than dropped.
 */
bool gaud_id3_frame_tag(const char * id, size_t length, GAUD_Tag * out_tag);

/** @brief The 2.4 frame identifier to write @p tag as, or NULL. */
const char * gaud_id3_tag_frame(GAUD_Tag tag);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_META_ID3_INTERNAL_H
