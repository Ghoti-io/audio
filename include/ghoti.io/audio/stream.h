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
 * @file stream.h
 *
 * The byte stream every container is read through.
 *
 * One path for a file, a memory buffer and a fuzz harness's bytes, so that
 * all three exercise the same code. CONVENTIONS.md section 5 names the
 * minimum surface; this library needs one thing beyond it from the start,
 * which is that a stream may not be seekable. A pipe is an ordinary way to
 * receive audio, and a container parser that assumes it can seek is one that
 * has to be rewritten the first time somebody pipes a file into it.
 *
 * A stream may also be written to. One type rather than a separate sink,
 * because a codec that reads a format and a codec that writes it want the
 * same addressing, and gaud_stream_writable() is how anything asks.
 */

#ifndef GHOTI_IO_GAUD_STREAM_H
#define GHOTI_IO_GAUD_STREAM_H

#include <ghoti.io/audio/allocator.h>
#include <ghoti.io/audio/core.h>
#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief An opaque byte stream. */
typedef struct GAUD_Stream GAUD_Stream;

/** @brief Where a seek offset is measured from. */
typedef enum {
  GAUD_SEEK_SET = 0, ///< From the start.
  GAUD_SEEK_CUR,     ///< From the current position.
  GAUD_SEEK_END      ///< From the end; only on a stream of known size.
} GAUD_Seek_Origin;

/**
 * @brief A stream over bytes the caller owns.
 *
 * **The bytes are borrowed, not copied.** They must stay alive and unchanged
 * for the life of the stream. This is the same contract `image`'s memory
 * stream states, and it is what lets a fuzz harness hand over its input
 * without a copy per iteration.
 *
 * @param bytes The buffer. May be NULL only when @p length is zero.
 * @param length Its length.
 * @param out_stream Receives the stream. Written only on success.
 * @return ::GAUD_OK, ::GAUD_ERR_INVALID or ::GAUD_ERR_OOM.
 */
GAUD_API GAUD_Result gaud_stream_create_memory(
    const void * bytes, size_t length, GAUD_Stream ** out_stream);

/** @brief gaud_stream_create_memory() with an explicit allocator. */
GAUD_API GAUD_Result gaud_stream_create_memory_with_allocator(
    const GAUD_Allocator * allocator, const void * bytes, size_t length,
    GAUD_Stream ** out_stream);

/**
 * @brief Read up to @p count bytes.
 *
 * A short read is not an error; it means the end was reached.
 * gaud_stream_eof() distinguishes that from a stream that simply had nothing
 * ready.
 *
 * @return The number of bytes written to @p out, which is zero on any bad
 *   argument.
 */
GAUD_API size_t gaud_stream_read(
    GAUD_Stream * stream, void * out, size_t count);

/**
 * @brief Move the read position.
 *
 * @return ::GAUD_OK, ::GAUD_ERR_INVALID for a bad origin or a position
 *   outside the stream, or ::GAUD_ERR_UNSUPPORTED on a stream that cannot
 *   seek.
 */
GAUD_API GAUD_Result gaud_stream_seek(
    GAUD_Stream * stream, int64_t offset, GAUD_Seek_Origin origin);

/** @brief The current read position, or `(uint64_t)-1` on a bad argument. */
GAUD_API uint64_t gaud_stream_tell(const GAUD_Stream * stream);

/**
 * @brief The stream's total length, where it is known.
 *
 * A stream whose length is not knowable in advance - a pipe - answers
 * ::GAUD_ERR_UNSUPPORTED rather than guessing or reading to the end.
 */
GAUD_API GAUD_Result gaud_stream_size(
    const GAUD_Stream * stream, uint64_t * out_size);

/** @brief Whether the last read reached the end. */
GAUD_API bool gaud_stream_eof(const GAUD_Stream * stream);

/**
 * @brief Whether this stream can seek.
 *
 * Asked before seeking rather than after a failure, so that a parser can
 * choose a one-pass strategy instead of discovering half way through that it
 * cannot go back.
 */
GAUD_API bool gaud_stream_seekable(const GAUD_Stream * stream);

/** @brief Free a stream. Safe on NULL. Does not free borrowed bytes. */
GAUD_API void gaud_stream_destroy(GAUD_Stream * stream);

/**
 * @brief A read-only stream over a file.
 *
 * @param path The file to open.
 * @param out_stream Receives the stream. Written only on success.
 * @return ::GAUD_OK, ::GAUD_ERR_INVALID, ::GAUD_ERR_IO if it will not open,
 *   or ::GAUD_ERR_OOM.
 */
GAUD_API GAUD_Result gaud_stream_create_file(
    const char * path, GAUD_Stream ** out_stream);

/** @brief gaud_stream_create_file() with an explicit allocator. */
GAUD_API GAUD_Result gaud_stream_create_file_with_allocator(
    const GAUD_Allocator * allocator, const char * path,
    GAUD_Stream ** out_stream);

/**
 * @brief A stream that presents another stream as though it could not seek.
 *
 * This exists so the non-seekable path can be **tested**. "A pipe is an
 * ordinary way to receive audio" is a claim about code that never runs unless
 * something makes it run, and a gate that cannot reach a path reports success
 * for it. Wrapping a memory stream is how a unit test reaches the branch a
 * container parser takes when it cannot go back.
 *
 * Reads forward to @p source. Seeks answer ::GAUD_ERR_UNSUPPORTED, and
 * gaud_stream_size() does too - a pipe does not know its own length, and a
 * wrapper that passed the real length through would be testing a stream that
 * exists nowhere.
 *
 * @param source Borrowed. Must outlive the wrapper, and must not be read
 *   directly while the wrapper is in use.
 * @param out_stream Receives the wrapper. Written only on success.
 */
GAUD_API GAUD_Result gaud_stream_create_unseekable(
    GAUD_Stream * source, GAUD_Stream ** out_stream);

/**
 * @brief A growable in-memory stream that can be written to.
 *
 * Owns its bytes, unlike gaud_stream_create_memory(), because there is
 * nothing for it to borrow - it is the destination. Read them back with
 * gaud_stream_writer_bytes().
 */
GAUD_API GAUD_Result gaud_stream_create_memory_writer(
    const GAUD_Allocator * allocator, GAUD_Stream ** out_stream);

/**
 * @brief A stream that writes to a file, truncating it.
 */
GAUD_API GAUD_Result gaud_stream_create_file_writer(
    const GAUD_Allocator * allocator, const char * path,
    GAUD_Stream ** out_stream);

/** @brief Whether this stream accepts gaud_stream_write(). */
GAUD_API bool gaud_stream_writable(const GAUD_Stream * stream);

/**
 * @brief Write @p count bytes at the current position.
 *
 * Writing over the middle of what has already been written is how a header
 * whose length field was not known at the time gets patched, so this is not
 * append-only.
 *
 * @return ::GAUD_OK, ::GAUD_ERR_INVALID on a stream that is not writable,
 *   ::GAUD_ERR_IO, or ::GAUD_ERR_OOM.
 */
GAUD_API GAUD_Result gaud_stream_write(
    GAUD_Stream * stream, const void * bytes, size_t count);

/**
 * @brief What a memory writer has written so far.
 *
 * Borrowed: valid until the next write or until the stream is destroyed.
 *
 * @return ::GAUD_OK, or ::GAUD_ERR_UNSUPPORTED on a stream that is not a
 *   memory writer.
 */
GAUD_API GAUD_Result gaud_stream_writer_bytes(
    const GAUD_Stream * stream, const void ** out_bytes, size_t * out_length);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_STREAM_H
