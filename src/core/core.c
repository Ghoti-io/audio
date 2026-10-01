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
 * Result strings, diagnostics, limits, and the build-capability query.
 */

#include <ghoti.io/audio/core.h>
#include <ghoti.io/cutil/allocator.h>
#include <stdint.h>
#include <string.h>

/**
 * Indexed by GAUD_Result. A test walks this against GAUD_RESULT_COUNT, which
 * is what stops a code being added without a string.
 */
static const char * const result_strings[GAUD_RESULT_COUNT] = {
    [GAUD_OK] = "success",
    [GAUD_ERR_IO] = "read, write or seek failed",
    [GAUD_ERR_FORMAT] = "not a format this library handles",
    [GAUD_ERR_UNSUPPORTED] = "this format, but an unimplemented feature",
    [GAUD_ERR_LIMIT] = "a limit was exceeded",
    [GAUD_ERR_CORRUPT] = "this format, but the bytes are wrong",
    [GAUD_ERR_OOM] = "out of memory",
    [GAUD_ERR_INVALID] = "invalid argument",
    [GAUD_ERR_INTERNAL] = "internal error",
};

const char * gaud_result_string(GAUD_Result result) {
  if ((unsigned)result >= (unsigned)GAUD_RESULT_COUNT) {
    return "unknown";
  }
  const char * s = result_strings[result];
  return s ? s : "unknown";
}

void gaud_diagnostics_init(
    GAUD_Diagnostics * diagnostics, const GAUD_Allocator * allocator) {
  if (!diagnostics) {
    return;
  }
  // The whole struct, not only the allocator.
  //
  // This set the allocator and nothing else, and the header said so - so
  // a caller had to zero the struct itself before calling a function
  // named init. Every call site in the library happened to, and the first
  // one that did not wrote through a pointer made of stack garbage on its
  // first append. A function whose name says "initialise" and whose
  // contract says "only if you already did" is a trap however carefully
  // the contract is worded; the fix is to make the name true.
  memset(diagnostics, 0, sizeof(*diagnostics));
  diagnostics->allocator = allocator;
}

GAUD_Result gaud_diagnostics_append(
    GAUD_Diagnostics * diagnostics, const GAUD_Diagnostic * diagnostic) {
  if (!diagnostics || !diagnostic) {
    return GAUD_ERR_INVALID;
  }
  const GAUD_Allocator * allocator = diagnostics->allocator;

  if (diagnostics->count == diagnostics->capacity) {
    size_t next = diagnostics->capacity ? diagnostics->capacity * 2 : 8;
    // Growth doubles, so the multiply is the one that can overflow.
    if (next < diagnostics->capacity
        || next > SIZE_MAX / sizeof(GAUD_Diagnostic)) {
      return GAUD_ERR_OOM;
    }
    GAUD_Diagnostic * grown = gcu_allocator_realloc(
        allocator, diagnostics->items, next * sizeof(GAUD_Diagnostic));
    if (!grown) {
      return GAUD_ERR_OOM;
    }
    diagnostics->items = grown;
    diagnostics->capacity = next;
  }

  diagnostics->items[diagnostics->count++] = *diagnostic;
  return GAUD_OK;
}

void gaud_diagnostics_clear(GAUD_Diagnostics * diagnostics) {
  if (!diagnostics) {
    return;
  }
  gcu_allocator_free(diagnostics->allocator, diagnostics->items);
  diagnostics->items = NULL;
  diagnostics->count = 0;
  diagnostics->capacity = 0;
}

void gaud_diagnostics_destroy(GAUD_Diagnostics * diagnostics) {
  if (!diagnostics) {
    return;
  }
  gaud_diagnostics_clear(diagnostics);
  diagnostics->allocator = NULL;
}

void gaud_limits_default(GAUD_Limits * limits) {
  if (!limits) {
    return;
  }
  // Every field is set explicitly rather than by memset-then-patch, and the
  // struct is assigned whole. A field added to GAUD_Limits and not added here
  // is then a compiler warning about a missing initialiser rather than a
  // silent zero, which for these fields would mean "refuse everything".
  *limits = (GAUD_Limits){
      .max_tracks = 1024u,
      .max_channels = 256u,
      // 3 MHz. Above every rate any container can state for PCM, and chosen
      // so that DSD64's 2.8224 MHz is inside it - a limit that refused the
      // one non-PCM format planned would be a limit nobody could raise
      // without knowing why it was there.
      .max_sample_rate = 3000000u,
      // ~31 hours at 96 kHz. A duration, not a byte count, because this is
      // what a container claims before anything is read.
      .max_frames = UINT64_C(10000000000),
      // 2 GiB of PCM. Only gaud_track_decode_all() consults this; the pull
      // decoder is bounded by the caller's own buffer.
      .max_decoded_bytes = UINT64_C(2) * 1024 * 1024 * 1024,
      .max_element_size = UINT64_C(256) * 1024 * 1024,
      .max_nesting_depth = 64u,
      .max_metadata_bytes = UINT64_C(64) * 1024 * 1024,
      .max_metadata_entries = 65536u,
      .max_picture_bytes = UINT64_C(32) * 1024 * 1024,
      .max_cue_points = 65536u,
  };
}

bool gaud_have_image_validation(void) {
#ifdef GAUD_HAVE_IMAGE
  return true;
#else
  return false;
#endif
}
