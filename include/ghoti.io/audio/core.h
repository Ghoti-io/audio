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
 * @file core.h
 *
 * Result codes, diagnostics, strictness and limits for the Ghoti.io Audio
 * library, plus the one question a caller may need to ask about how this
 * copy of the library was built.
 */

#ifndef GHOTI_IO_GAUD_CORE_H
#define GHOTI_IO_GAUD_CORE_H

#include <ghoti.io/audio/allocator.h>
#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result code for audio library operations.
 *
 * The vocabulary is CONVENTIONS.md section 5's, unchanged. Nothing is added
 * to it without a reason, and no call reports a limit by silently truncating
 * instead of returning ::GAUD_ERR_LIMIT.
 */
typedef enum {
  GAUD_OK = 0,          ///< Operation succeeded.
  GAUD_ERR_IO,          ///< Read, write or seek failed.
  GAUD_ERR_FORMAT,      ///< Well-formed, but not a format this handles.
  GAUD_ERR_UNSUPPORTED, ///< This format, but a feature we do not implement.
  GAUD_ERR_LIMIT,       ///< A GAUD_Limits field was exceeded.
  GAUD_ERR_CORRUPT,     ///< This format, but the bytes are wrong.
  GAUD_ERR_OOM,         ///< The allocator returned NULL.
  GAUD_ERR_INVALID,     ///< A caller-supplied argument is wrong.
  GAUD_ERR_INTERNAL,    ///< The library's own invariant failed; a bug.
  GAUD_RESULT_COUNT     ///< Closes the enum so a test can check the strings.
} GAUD_Result;

/**
 * @brief A human-readable description of a result code.
 *
 * @param result The result code.
 * @return A static string, never NULL; "unknown" for a value outside the
 *   enum.
 */
GAUD_API const char * gaud_result_string(GAUD_Result result);

/**
 * @brief Severity of a diagnostic.
 */
typedef enum {
  GAUD_DIAG_WARNING = 0,    ///< Recovered; the result is still usable.
  GAUD_DIAG_ERROR,          ///< Did not recover.
  GAUD_DIAG_SEVERITY_COUNT
} GAUD_Diag_Severity;

/**
 * @brief One thing a parser noticed.
 *
 * @p codec_name and @p recommended_action are stored by reference and are
 * expected to be string literals or to outlive the diagnostics list.
 */
typedef struct {
  const char * codec_name;         ///< Which codec or container said so.
  uint64_t offset;                 ///< Stream offset, where one is meaningful.
  uint32_t element_id;             ///< Chunk, box or frame identifier.
  GAUD_Diag_Severity severity;     ///< Warning or error.
  const char * recommended_action; ///< What to do about it; may be NULL.
} GAUD_Diagnostic;

/**
 * @brief A growable list of diagnostics.
 *
 * Zero-initialising one is valid and uses the default allocator. Call
 * gaud_diagnostics_clear() or gaud_diagnostics_destroy() when done.
 */
typedef struct {
  GAUD_Diagnostic * items;         ///< The diagnostics, in the order noticed.
  size_t count;                    ///< How many are in @p items.
  size_t capacity;                 ///< How many @p items has room for.
  const GAUD_Allocator * allocator; ///< NULL means the default.
} GAUD_Diagnostics;

/** @brief Set the allocator a diagnostics list will grow with. */
GAUD_API void gaud_diagnostics_init(
    GAUD_Diagnostics * diagnostics, const GAUD_Allocator * allocator);

/**
 * @brief Append one diagnostic, growing the list.
 * @return ::GAUD_OK, or ::GAUD_ERR_OOM if the list could not grow.
 */
GAUD_API GAUD_Result gaud_diagnostics_append(
    GAUD_Diagnostics * diagnostics, const GAUD_Diagnostic * diagnostic);

/** @brief Free the list and zero the count; the allocator is kept. */
GAUD_API void gaud_diagnostics_clear(GAUD_Diagnostics * diagnostics);

/** @brief Free the list and zero the whole struct. */
GAUD_API void gaud_diagnostics_destroy(GAUD_Diagnostics * diagnostics);

/**
 * @brief How much a parser forgives.
 */
typedef enum {
  GAUD_STRICT = 0,        ///< Warnings are errors.
  GAUD_NORMAL,            ///< Safe recoveries, with a warning for each.
  GAUD_PERMISSIVE,        ///< More heuristics, with a warning for each.
  GAUD_STRICTNESS_COUNT
} GAUD_Strictness;

/**
 * @brief Caps on every unbounded quantity a parser can meet.
 *
 * Every parser takes one and `NULL` means the defaults. A field is a stated
 * promise, which is what makes the fuzzers meaningful: exceeding one returns
 * ::GAUD_ERR_LIMIT and never a truncated answer that parses.
 *
 * Zero means "no limit" for none of these. Where a caller wants no cap, the
 * value to pass is the type's maximum, so that a zeroed struct is a refusal
 * rather than an accident - a zero that means unlimited and a zero that means
 * "I did not fill this in" are the same bit pattern and cannot be told apart.
 */
typedef struct GAUD_Limits {
  /** Tracks in one file. MP4 and Matroska can carry several. */
  uint32_t max_tracks;
  /** Channels in one track. */
  uint32_t max_channels;
  /** Sample rate, in hertz, that a header may claim. */
  uint32_t max_sample_rate;
  /** Frames in one track, as the container states it before anything is
   *  decoded. This is the field that stops a four-byte header claiming a
   *  hundred-hour file. */
  uint64_t max_frames;
  /** Bytes of PCM gaud_track_decode_all() may allocate. */
  uint64_t max_decoded_bytes;
  /** Bytes in one container element - a RIFF chunk, an MP4 box, an EBML
   *  element, an ID3 frame. */
  uint64_t max_element_size;
  /** How deeply container elements may nest. MP4 boxes and EBML both do. */
  uint32_t max_nesting_depth;
  /** Total bytes of metadata carried, across every scheme in the file. */
  uint64_t max_metadata_bytes;
  /** Individual metadata entries. */
  uint32_t max_metadata_entries;
  /** Bytes in one embedded picture. */
  uint64_t max_picture_bytes;
  /** Cue points and chapters, together. */
  uint32_t max_cue_points;
} GAUD_Limits;

/**
 * @brief Fill a caller-owned limits struct with the defaults.
 *
 * @param limits The struct to initialise. Ignored if NULL.
 */
GAUD_API void gaud_limits_default(GAUD_Limits * limits);

/**
 * @brief Whether this build can check an embedded picture against its own
 *   stated dimensions.
 *
 * `ghoti.io-image` is an optional dependency, declared `?image` in
 * suite/libraries.txt, and it is used for exactly one thing: FLAC's PICTURE
 * block states a width, height, colour depth and palette size, any of which
 * the writer can have got wrong. ID3v2's APIC and MP4's covr state none, so
 * for those there is nothing to check and this makes no difference.
 *
 * The two builds differ **additively**. What a file stated is always
 * reported; what the bytes actually are is reported only here. Nothing
 * changes meaning between builds, so a caller that ignores this function
 * still gets correct answers - it just gets fewer of them.
 *
 * This is a function and not a macro on purpose. A caller that only links
 * this library, and never compiles against its headers, still needs a way to
 * ask; and a macro would answer for the *caller's* build rather than for
 * the library's, which is the wrong question whenever the two were built
 * separately.
 *
 * @return true when built against ghoti.io-image.
 */
GAUD_API bool gaud_have_image_validation(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_CORE_H
