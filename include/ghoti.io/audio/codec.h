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
 * @file codec.h
 *
 * The codec SDK: the interface a codec implements, and the registry it
 * registers with.
 *
 * **This header is public on purpose, and that is a decision rather than an
 * accident of layout.** planning/audio.md 11.2 settled it. A codec that lives
 * outside this repository - `audio-aac` is the first, and third parties may
 * write others - can only exist if the vtable it fills in and the registry it
 * hands itself to are both installed headers. `compress` works this way and
 * it is the model followed here; `image` does not, and the difference is
 * instructive: `GIMG_Codec` holds its load and decode callbacks in
 * `src/codec/codec_internal.h`, so the only thing an outside party can build
 * there is a probe.
 *
 * Two rules keep this honest, and both are cheap only if kept from the start:
 *
 * 1. **Every codec in this repository registers the same way an outside one
 *    would.** There is no privileged internal path. A public interface that
 *    the shipped code bypasses is one nothing exercises, and it will be
 *    broken the first time somebody depends on it.
 * 2. **An out-of-tree codec is part of the test suite** - built against the
 *    *installed* headers, registering itself, driven by the core. Otherwise
 *    "third parties can add codecs" is a claim that has never been run.
 *
 * ### Compatibility
 *
 * The vtable carries ::GAUD_CODEC_ABI_VERSION and its own `size`, which is
 * what lets this interface grow without silently mis-reading a codec built
 * against an older header. Phase 0 defines the registry, identification and
 * the capability declarations. The decode and encode entry points arrive with
 * phase 1, appended to the end of the struct; a codec compiled against this
 * header will report the smaller `size` and the library will know not to read
 * past it.
 *
 * Note also that the versioned symbol namespacing in namespace.h works in
 * this interface's favour. An out-of-tree codec binds to one major version of
 * this library, so a mismatch is a link error rather than a wrong vtable
 * bound quietly to the right name.
 */

#ifndef GHOTI_IO_GAUD_CODEC_H
#define GHOTI_IO_GAUD_CODEC_H

#include <ghoti.io/audio/allocator.h>
#include <ghoti.io/audio/core.h>
#include <ghoti.io/audio/macros.h>
#include <ghoti.io/audio/stream.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The vtable layout this header describes.
 *
 * A codec sets ::GAUD_Codec::abi_version to this. Registration refuses a
 * value it does not know, which turns "built against a different version of
 * the SDK" into an error at the point of registration rather than a crash
 * later inside a call through a pointer that was never there.
 */
#define GAUD_CODEC_ABI_VERSION 1u

/** @brief What a codec can do. A bitmask over ::GAUD_Codec::capabilities. */
typedef enum {
  GAUD_CAP_NONE = 0,
  GAUD_CAP_DECODE = 1u << 0,         ///< Produces samples.
  GAUD_CAP_ENCODE = 1u << 1,         ///< Consumes samples.
  GAUD_CAP_METADATA_READ = 1u << 2,  ///< Reads tags this format carries.
  GAUD_CAP_METADATA_WRITE = 1u << 3, ///< Writes them back.
  GAUD_CAP_PASSTHROUGH = 1u << 4     ///< Can hand over coded bytes unchanged,
                                     ///< which is what remux needs.
} GAUD_Capabilities;

/**
 * @brief How good a codec's encoder is, as a value rather than as prose.
 *
 * planning/audio.md 11.3 decided that this library writes every format it
 * reads, and that a perceptual encoder may ship before it is competitive.
 * That is only an acceptable bargain if a caller can *ask*. A sentence in a
 * README saying "non-competitive" is not something a program can act on, and
 * it is not something anybody reads at three in the morning.
 *
 * ::GAUD_ENCODER_STUB does not mean the output is invalid. A stub still has
 * to produce a bitstream the reference decoders accept - that gate is exact
 * and is separate from this one. It means only that the rate-distortion
 * result is not competitive with the reference encoder.
 */
typedef enum {
  GAUD_ENCODER_NONE = 0,   ///< No encoder at all.
  GAUD_ENCODER_EXACT,      ///< PCM or lossless: the output is determined.
  GAUD_ENCODER_PRODUCTION, ///< Competitive with the reference encoder.
  GAUD_ENCODER_STUB,       ///< Conformant bitstream, not competitive.
  GAUD_ENCODER_TIER_COUNT
} GAUD_Encoder_Tier;

/**
 * @brief A byte signature at a fixed offset.
 *
 * The offset is part of it because audio signatures are rarely at zero. A
 * WAV is `RIFF` at 0 and `WAVE` at 8, and an MP3 can be preceded by an ID3v2
 * tag of any size at all.
 */
typedef struct {
  uint64_t offset;             ///< Where in the stream to compare.
  const unsigned char * bytes; ///< What to compare against; borrowed.
  size_t length;               ///< How many bytes.
} GAUD_Codec_Magic;

/** @brief How sure a probe is, as a percentage. */
typedef enum {
  GAUD_CONFIDENCE_NONE = 0,   ///< Not this format.
  GAUD_CONFIDENCE_WEAK = 25,  ///< Consistent with it; nothing distinctive.
  GAUD_CONFIDENCE_LIKELY = 75, ///< A signature matched.
  GAUD_CONFIDENCE_CERTAIN = 100 ///< A signature and a self-consistent header.
} GAUD_Confidence;

/** @brief Forward declaration; the vtable below is the one that uses it. */
typedef struct GAUD_Codec GAUD_Codec;

/**
 * @brief Look at a stream and say whether this codec recognises it.
 *
 * A probe **must leave the stream where it found it**. The registry probes
 * codecs in turn and the next one is entitled to a stream positioned as the
 * caller left it.
 *
 * A probe is optional. A codec that only declares magics is matched on those
 * alone, which is the common case; a probe exists for the formats where a
 * signature is not enough to tell two apart.
 *
 * @param codec The codec being asked, so one function can serve several.
 * @param stream Positioned wherever the caller had it.
 * @param out_confidence Receives a ::GAUD_Confidence, or any value 0-100.
 * @return ::GAUD_OK having written @p out_confidence, or an error. An error
 *   is not "no", it is "this probe could not answer"; to say no, answer
 *   ::GAUD_OK with ::GAUD_CONFIDENCE_NONE.
 */
typedef GAUD_Result (*GAUD_Codec_Probe_Fn)(
    const GAUD_Codec * codec, GAUD_Stream * stream, unsigned * out_confidence);

/**
 * @brief What a codec tells the registry about itself.
 *
 * A codec defines one of these, usually as a file-scope constant, and hands
 * its address to gaud_registry_register(). **The registry stores the pointer
 * and does not copy the struct**, so it must outlive the registry - which a
 * file-scope constant in the codec's own object does, and a stack temporary
 * does not.
 *
 * Set @p abi_version to ::GAUD_CODEC_ABI_VERSION and @p size to
 * `sizeof(GAUD_Codec)` **as the codec's own compiler saw it**; those two
 * together are how a codec built against a different version of this header
 * is detected rather than misread.
 */
struct GAUD_Codec {
  /** ::GAUD_CODEC_ABI_VERSION, as of the header this was compiled against. */
  uint32_t abi_version;
  /** `sizeof(GAUD_Codec)` as the codec's compiler saw it. */
  size_t size;
  /** Short lower-case name, unique in a registry: "wav", "flac", "aac". */
  const char * name;
  /** The codec's own, passed to nothing yet; for a codec serving several
   *  formats through one set of functions. */
  void * ctx;
  /** A bitmask of ::GAUD_Capabilities. */
  unsigned int capabilities;
  /** Which of ::GAUD_Encoder_Tier this codec's encoder is. */
  GAUD_Encoder_Tier encoder_tier;
  /** Signatures, tried in order. May be NULL when @p probe does the work. */
  const GAUD_Codec_Magic * magics;
  /** How many entries @p magics has. */
  size_t magic_count;
  /** Optional; see ::GAUD_Codec_Probe_Fn. */
  GAUD_Codec_Probe_Fn probe;
};

/** @brief A set of registered codecs. */
typedef struct GAUD_Registry GAUD_Registry;

/**
 * @brief The process-wide registry.
 *
 * One of the documented singletons CONVENTIONS.md section 5 permits, in the
 * same shape as cutil's thread registry and compress's method registry. It is
 * what a codec's constructor registers into and what gaud_probe() consults
 * when given NULL.
 *
 * @return Never NULL.
 */
GAUD_API GAUD_Registry * gaud_registry_default(void);

/**
 * @brief A registry of one's own.
 *
 * Useful to a test that wants a known set of codecs rather than whatever the
 * process has loaded, which is most tests.
 */
GAUD_API GAUD_Result gaud_registry_create(
    const GAUD_Allocator * allocator, GAUD_Registry ** out_registry);

/** @brief Free a registry from gaud_registry_create(). Not the default one. */
GAUD_API void gaud_registry_destroy(GAUD_Registry * registry);

/**
 * @brief Add a codec to a registry.
 *
 * @param registry The registry, or NULL for the default.
 * @param codec Borrowed, not copied; must outlive @p registry.
 * @return ::GAUD_OK; ::GAUD_ERR_INVALID for a malformed codec, an unknown
 *   @p abi_version, or a name already registered; ::GAUD_ERR_OOM.
 */
GAUD_API GAUD_Result gaud_registry_register(
    GAUD_Registry * registry, const GAUD_Codec * codec);

/** @brief How many codecs a registry holds. NULL means the default. */
GAUD_API size_t gaud_registry_count(const GAUD_Registry * registry);

/** @brief The codec at @p index, or NULL. NULL registry means the default. */
GAUD_API const GAUD_Codec * gaud_registry_by_index(
    const GAUD_Registry * registry, size_t index);

/** @brief The codec called @p name, or NULL. NULL registry means the default. */
GAUD_API const GAUD_Codec * gaud_registry_find(
    const GAUD_Registry * registry, const char * name);

/** @brief What gaud_probe() concluded. */
typedef struct {
  /**
   * The winning codec's name, or NULL when nothing matched.
   *
   * Borrowed from the codec, so it is valid as long as that codec is
   * registered and no longer. Copy it to keep it.
   */
  const char * codec_name;
  /** The winner's confidence, 0-100. */
  unsigned int confidence;
} GAUD_Probe_Result;

/**
 * @brief Identify a stream's format from its bytes.
 *
 * The extension is not consulted and is not available here, which is the
 * point: a `.wav` holding MP3 frames is a thing that exists.
 *
 * Every registered codec is asked and the most confident answer wins. The
 * stream is left where it was found.
 *
 * @param registry NULL for the default.
 * @param stream The stream to identify.
 * @param out_result Filled in; @p codec_name is NULL when nothing matched,
 *   which is ::GAUD_OK and not an error.
 * @return ::GAUD_OK or ::GAUD_ERR_INVALID.
 */
GAUD_API GAUD_Result gaud_probe(GAUD_Registry * registry, GAUD_Stream * stream,
    GAUD_Probe_Result * out_result);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_CODEC_H
