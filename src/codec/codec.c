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
 * The codec registry and the probe over it.
 *
 * Nothing here knows about any particular format, and that is deliberate:
 * every codec, this repository's included, reaches the registry through
 * gaud_registry_register() and the public vtable in codec.h. There is no
 * internal shortcut for the built-in ones, because a public path the shipped
 * code does not take is a path nothing exercises.
 */

#include <ghoti.io/audio/codec.h>
#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/cutil/allocator.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/** @brief The set of codecs behind ::GAUD_Registry. */
struct GAUD_Registry {
  const GAUD_Allocator * allocator; ///< NULL means the default.
  const GAUD_Codec ** codecs;       ///< Borrowed pointers; see codec.h.
  size_t count;                     ///< How many codecs are registered.
  size_t capacity;                  ///< How many @p codecs has room for.
  bool is_default; ///< The singleton, which refuses to be destroyed.
};

static GAUD_Registry default_registry = {.is_default = true};

GAUD_Registry * gaud_registry_default(void) {
  return &default_registry;
}

GAUD_Result gaud_registry_create(
    const GAUD_Allocator * allocator, GAUD_Registry ** out_registry) {
  if (!out_registry) {
    return GAUD_ERR_INVALID;
  }
  GAUD_Registry * registry
      = gcu_allocator_malloc(allocator, sizeof(GAUD_Registry));
  if (!registry) {
    return GAUD_ERR_OOM;
  }
  *registry = (GAUD_Registry){.allocator = allocator};
  *out_registry = registry;
  return GAUD_OK;
}

void gaud_registry_destroy(GAUD_Registry * registry) {
  if (!registry || registry->is_default) {
    return;
  }
  gcu_allocator_free(registry->allocator, registry->codecs);
  gcu_allocator_free(registry->allocator, registry);
}

/** NULL means the default, uniformly, so no caller has to special-case it. */
static GAUD_Registry * resolve(const GAUD_Registry * registry) {
  return registry ? (GAUD_Registry *)registry : &default_registry;
}

/**
 * Whether a codec is well enough formed to register.
 *
 * The abi_version and size checks are the reason those two fields exist. A
 * codec built against a later header is refused here, by name, rather than
 * being stored and then called through a function pointer that its compiler
 * never wrote - which is the failure this whole arrangement is designed to
 * turn into an error message.
 *
 * `size` is compared with `>=` and not `==`: a codec compiled against an
 * older header of the same ABI version is shorter, and that is exactly the
 * case the field exists to make safe. Reading a field is then conditional on
 * the struct being long enough to have it. Phase 0's struct has no optional
 * tail yet, so the check is only that the codec is not claiming to be
 * smaller than the fixed part.
 */
static bool codec_is_valid(const GAUD_Codec * codec) {
  if (!codec || !codec->name || !codec->name[0]) {
    return false;
  }
  if (codec->abi_version != GAUD_CODEC_ABI_VERSION) {
    return false;
  }
  if (codec->size < sizeof(GAUD_Codec)) {
    return false;
  }
  if (codec->magic_count && !codec->magics) {
    return false;
  }
  for (size_t i = 0; i < codec->magic_count; ++i) {
    if (!codec->magics[i].bytes || !codec->magics[i].length) {
      return false;
    }
  }
  // A codec that declares an encoder must say how good it is, and one that
  // declares no encoder must not claim a tier. planning/audio.md 11.3 made
  // the tier the machine-readable half of that promise; letting it go unset
  // would put it straight back into prose.
  bool encodes = (codec->capabilities & GAUD_CAP_ENCODE) != 0;
  if (encodes != (codec->encoder_tier != GAUD_ENCODER_NONE)) {
    return false;
  }
  if ((unsigned)codec->encoder_tier >= (unsigned)GAUD_ENCODER_TIER_COUNT) {
    return false;
  }
  return true;
}

GAUD_Result gaud_registry_register(
    GAUD_Registry * registry, const GAUD_Codec * codec) {
  GAUD_Registry * r = resolve(registry);
  if (!codec_is_valid(codec)) {
    return GAUD_ERR_INVALID;
  }
  // A duplicate name is refused rather than shadowed. Two codecs answering
  // to one name would make gaud_registry_find() depend on load order, which
  // for a plugin is whatever the dynamic linker felt like.
  for (size_t i = 0; i < r->count; ++i) {
    if (strcmp(r->codecs[i]->name, codec->name) == 0) {
      return GAUD_ERR_INVALID;
    }
  }

  if (r->count == r->capacity) {
    size_t next = r->capacity ? r->capacity * 2 : 8;
    if (next < r->capacity || next > SIZE_MAX / sizeof(const GAUD_Codec *)) {
      return GAUD_ERR_OOM;
    }
    const GAUD_Codec ** grown = gcu_allocator_realloc(
        r->allocator, r->codecs, next * sizeof(const GAUD_Codec *));
    if (!grown) {
      return GAUD_ERR_OOM;
    }
    r->codecs = grown;
    r->capacity = next;
  }

  r->codecs[r->count++] = codec;
  return GAUD_OK;
}

size_t gaud_registry_count(const GAUD_Registry * registry) {
  return resolve(registry)->count;
}

const GAUD_Codec * gaud_registry_by_index(
    const GAUD_Registry * registry, size_t index) {
  GAUD_Registry * r = resolve(registry);
  return index < r->count ? r->codecs[index] : NULL;
}

const GAUD_Codec * gaud_registry_find(
    const GAUD_Registry * registry, const char * name) {
  if (!name) {
    return NULL;
  }
  GAUD_Registry * r = resolve(registry);
  for (size_t i = 0; i < r->count; ++i) {
    if (strcmp(r->codecs[i]->name, name) == 0) {
      return r->codecs[i];
    }
  }
  return NULL;
}

/**
 * Compare one signature against the stream.
 *
 * Restores the position itself. Every path out of here, including the short
 * read and the seek failure, leaves the stream where it was found, because
 * the next codec's probe is entitled to that.
 */
static bool magic_matches(
    GAUD_Stream * stream, uint64_t origin, const GAUD_Codec_Magic * magic) {
  unsigned char buffer[64];
  if (magic->length > sizeof(buffer)) {
    // Longer than any real signature. Refusing to match is right: a silent
    // truncation would compare a prefix and call it a hit.
    return false;
  }
  if (gaud_stream_seek(stream, (int64_t)(origin + magic->offset),
          GAUD_SEEK_SET)
      != GAUD_OK) {
    return false;
  }
  size_t got = gaud_stream_read(stream, buffer, magic->length);
  bool hit = got == magic->length
      && memcmp(buffer, magic->bytes, magic->length) == 0;
  gaud_stream_seek(stream, (int64_t)origin, GAUD_SEEK_SET);
  return hit;
}

GAUD_Result gaud_probe(GAUD_Registry * registry, GAUD_Stream * stream,
    GAUD_Probe_Result * out_result) {
  if (!stream || !out_result) {
    return GAUD_ERR_INVALID;
  }
  GAUD_Registry * r = resolve(registry);
  *out_result = (GAUD_Probe_Result){0};

  // Signatures are relative to where the caller left the stream, not to
  // absolute zero, so that probing works on a stream already positioned at
  // the start of an embedded object.
  uint64_t origin = gaud_stream_tell(stream);

  for (size_t i = 0; i < r->count; ++i) {
    const GAUD_Codec * codec = r->codecs[i];
    unsigned confidence = GAUD_CONFIDENCE_NONE;

    for (size_t m = 0; m < codec->magic_count; ++m) {
      if (magic_matches(stream, origin, &codec->magics[m])) {
        confidence = GAUD_CONFIDENCE_LIKELY;
        break;
      }
    }

    /* The probe runs whatever the signatures said, and **its answer
     * replaces theirs** rather than being the larger of the two.
     *
     * That is not the obvious choice and it is the necessary one. Audio
     * signatures are widely shared: "RIFF" at offset 0 is a WAV, an AVI and
     * half a dozen other things, and "FORM" is every IFF file there is. A
     * codec declaring those magics needs to be able to say *no* about a
     * file that matched them - and if the magic's answer were kept whenever
     * it was higher, a probe could raise confidence but never lower it, so
     * a WAV codec would claim every AVI it was shown.
     *
     * A codec with no probe is still matched on its magics alone, which is
     * the common case. Declaring a probe is how a codec says the signature
     * is not the whole story. */
    if (codec->probe) {
      unsigned probed = GAUD_CONFIDENCE_NONE;
      if (codec->probe(codec, stream, &probed) == GAUD_OK) {
        confidence = probed > 100u ? 100u : probed;
      }
      /* An error from the probe means "could not answer", not "no", so the
       * signature's reading stands in that case - which is the arm the
       * assignment above deliberately does not reach. */
      /* A probe is asked to restore the position and may have failed to. */
      gaud_stream_seek(stream, (int64_t)origin, GAUD_SEEK_SET);
    }

    // Strictly greater, so the first codec registered wins a tie. Ordering
    // by registration is at least stable and stated; ordering by whichever
    // comparison happened last is neither.
    if (confidence > out_result->confidence) {
      out_result->confidence = confidence;
      out_result->codec_name = codec->name;
    }
  }

  return GAUD_OK;
}

bool gaud_codec_has(const GAUD_Codec * codec, size_t field_offset) {
  if (!codec) {
    return false;
  }
  /* The codec's own `size` is what its compiler saw. A field whose offset is
   * at or past that was never written by it, and reading one would be
   * reading past the end of the object - which is the failure the size field
   * exists to prevent. `>` and not `>=` on the offset alone would admit a
   * field that merely starts inside the struct and runs off the end, so the
   * comparison is against the offset, which is where the field begins, and
   * registration has already refused anything shorter than the fixed part. */
  return field_offset < codec->size;
}

GAUD_Result gaud_doc_load(GAUD_Registry * registry, GAUD_Stream * stream,
    const GAUD_Limits * limits, GAUD_Diagnostics * diagnostics,
    GAUD_Doc ** out_doc) {
  if (!stream || !out_doc) {
    return GAUD_ERR_INVALID;
  }
  GAUD_Probe_Result probed;
  GAUD_Result result = gaud_probe(registry, stream, &probed);
  if (result != GAUD_OK) {
    return result;
  }
  if (!probed.codec_name) {
    return GAUD_ERR_FORMAT;
  }
  const GAUD_Codec * codec = gaud_registry_find(registry, probed.codec_name);
  if (!codec) {
    return GAUD_ERR_INTERNAL; /* Probed a codec the registry then lost. */
  }
  /* A codec compiled against the phase 0 header has no `open` field at all.
   * Distinguished from GAUD_ERR_FORMAT on purpose: the bytes *were*
   * recognised, and "I know what this is and cannot open it" is a different
   * thing for a caller to be told than "I do not know what this is". */
  if (!gaud_codec_has(codec, offsetof(GAUD_Codec, open)) || !codec->open) {
    return GAUD_ERR_UNSUPPORTED;
  }

  GAUD_Limits resolved;
  if (limits) {
    resolved = *limits;
  }
  else {
    gaud_limits_default(&resolved);
  }

  /* Rewound before the codec sees it: gaud_probe() restores the position it
   * was given, but a caller may have handed over a stream that was already
   * partway through, and every container parser here expects to start at
   * the beginning of its own container. */
  if (gaud_stream_seekable(stream)) {
    result = gaud_stream_seek(stream, 0, GAUD_SEEK_SET);
    if (result != GAUD_OK) {
      return result;
    }
  }
  return codec->open(codec, stream, &resolved, diagnostics, out_doc);
}
