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
 * Fuzz the tag parsers below any container.
 *
 * ID3v2 is the most attackable thing in this library: a length field per
 * frame, three versions whose frame headers differ, four text encodings,
 * an unsynchronisation pass that rewrites the buffer in place, and
 * strings terminated one way for two encodings and another way for the
 * other two. Reaching it through a container would mean the fuzzer first
 * synthesising a valid RIFF chunk, so almost nothing would get through.
 *
 * What it asserts beyond not crashing:
 *
 * - **Every string handed back is valid UTF-8.** The whole point of the
 *   text layer is that a caller never sees anything else, and Latin-1
 *   copied through unchanged would satisfy a crash-only fuzzer forever
 *   while handing every caller bytes their own validator rejects.
 * - Limits are promises: with a cap of N entries, no more than N tags
 *   and custom keys come back.
 * - A parse that fails leaves nothing half-built that the destructor
 *   then walks.
 * - Round-tripping through the writer is stable - build, re-parse,
 *   re-build produces the same bytes. A writer whose output its own
 *   reader reads differently is one that loses a tag per generation.
 *
 * Build with: make fuzz-tags
 * Run:        make fuzz-run-tags FUZZ_TIME=300
 */

#include "../../src/meta/scheme.h"
#include <ghoti.io/audio/audio.h>
#include <ghoti.io/cutil/allocator.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size);

namespace {

/** Strict UTF-8, the same predicate the API promises its strings satisfy. */
bool valid_utf8(const char * text) {
  const unsigned char * p = (const unsigned char *)text;
  while (*p) {
    unsigned char c = *p;
    size_t need;
    uint32_t code;
    if (c < 0x80u) {
      ++p;
      continue;
    }
    if ((c & 0xE0u) == 0xC0u) {
      need = 1;
      code = c & 0x1Fu;
    }
    else if ((c & 0xF0u) == 0xE0u) {
      need = 2;
      code = c & 0x0Fu;
    }
    else if ((c & 0xF8u) == 0xF0u) {
      need = 3;
      code = c & 0x07u;
    }
    else {
      return false;
    }
    for (size_t i = 1; i <= need; ++i) {
      if ((p[i] & 0xC0u) != 0x80u) {
        return false;
      }
      code = (code << 6) | (p[i] & 0x3Fu);
    }
    if ((need == 1 && code < 0x80u) || (need == 2 && code < 0x800u)
        || (need == 3 && code < 0x10000u)) {
      return false;
    }
    if (code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) {
      return false;
    }
    p += need + 1u;
  }
  return true;
}

void check_strings(const GAUD_Meta * meta) {
  for (int t = 0; t < GAUD_TAG_COUNT; ++t) {
    size_t count = gaud_meta_count(meta, (GAUD_Tag)t);
    for (size_t i = 0; i < count; ++i) {
      const char * value = gaud_meta_get(meta, (GAUD_Tag)t, i);
      if (!value || !valid_utf8(value)) {
        __builtin_trap();
      }
    }
  }
  for (size_t i = 0; i < gaud_meta_custom_count(meta); ++i) {
    const char * key = nullptr;
    const char * value = nullptr;
    if (gaud_meta_custom(meta, i, &key, &value) != GAUD_OK) {
      __builtin_trap();
    }
    if (!valid_utf8(key) || !valid_utf8(value)) {
      __builtin_trap();
    }
  }
}

} // namespace

int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 2) {
    return 0;
  }
  /* One harness byte, so the limits are varied: a cap that is never
   * moved is a refusal path the corpus cannot reach. */
  const uint8_t options = data[0];
  data += 1;
  size -= 1;

  GAUD_Limits limits;
  gaud_limits_default(&limits);
  if (options & 1u) {
    limits.max_metadata_entries = (uint32_t)(options >> 4) + 1u;
  }
  if (options & 2u) {
    limits.max_metadata_bytes = (uint64_t)(options >> 3) * 16u + 1u;
  }
  if (options & 4u) {
    limits.max_picture_bytes = (uint64_t)(options >> 2) * 8u + 1u;
  }

  GAUD_Meta * meta = nullptr;
  if (gaud_meta_create(nullptr, &meta) != GAUD_OK) {
    return 0;
  }
  GAUD_Result result
      = gaud_id3v2_parse(data, size, &limits, meta, nullptr);

  if (result == GAUD_OK) {
    check_strings(meta);

    /* A limit is a promise, and the promise is about what comes back -
     * so the count is of values, the way the parser now counts them.
     * The first version of this check allowed twice the cap, reasoning
     * that one frame can yield two tags; that hid the real defect,
     * which was the parser counting frames while the limit named
     * entries. One ID3v2.4 frame can carry any number of values. */
    size_t entries = gaud_meta_custom_count(meta)
        + gaud_meta_picture_count(meta) + gaud_meta_raw_count(meta);
    for (int t = 0; t < GAUD_TAG_COUNT; ++t) {
      entries += gaud_meta_count(meta, (GAUD_Tag)t);
    }
    if (entries > limits.max_metadata_entries) {
      __builtin_trap();
    }

    /* Build, re-parse, re-build. The second block must equal the first:
     * a writer whose own reader reads it differently loses a tag per
     * generation, and nothing else here would notice. */
    unsigned char * first = nullptr;
    size_t first_size = 0;
    if (gaud_id3v2_build(meta, GAUD_META_PRESERVE_ALL, nullptr, &first,
            &first_size)
            == GAUD_OK
        && first_size > 0) {
      GAUD_Meta * again = nullptr;
      if (gaud_meta_create(nullptr, &again) == GAUD_OK) {
        GAUD_Limits wide;
        gaud_limits_default(&wide);
        if (gaud_id3v2_parse(first, first_size, &wide, again, nullptr)
            == GAUD_OK) {
          check_strings(again);
          unsigned char * second = nullptr;
          size_t second_size = 0;
          if (gaud_id3v2_build(again, GAUD_META_PRESERVE_ALL, nullptr,
                  &second, &second_size)
              == GAUD_OK) {
            if (second_size != first_size
                || (first_size > 0
                    && std::memcmp(first, second, first_size) != 0)) {
              __builtin_trap();
            }
          }
          gcu_allocator_free(gaud_allocator_default(), second);
        }
        gaud_meta_destroy(again);
      }
    }
    gcu_allocator_free(gaud_allocator_default(), first);
  }

  gaud_meta_destroy(meta);
  return 0;
}
