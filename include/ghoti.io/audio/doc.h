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
 * @file doc.h
 *
 * A file, and the tracks in it.
 *
 * ::GAUD_Doc is `image`'s `GIMG_Doc` and ::GAUD_Track is its `GIMG_Item`. The
 * half of that model which transfers is that loading parses the container and
 * builds the list. The half that does not is that an item can hold its whole
 * picture and a track cannot hold its whole audio - see decoder.h.
 */

#ifndef GHOTI_IO_GAUD_DOC_H
#define GHOTI_IO_GAUD_DOC_H

#include <ghoti.io/audio/buffer.h>
#include <ghoti.io/audio/coding.h>
#include <ghoti.io/audio/core.h>
#include <ghoti.io/audio/macros.h>
#include <ghoti.io/audio/meta.h>
#include <ghoti.io/audio/stream.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A parsed file: its tracks, and what the container said about them. */
typedef struct GAUD_Doc GAUD_Doc;

/** @brief One track within a document. */
typedef struct GAUD_Track GAUD_Track;

/**
 * @brief How much of the start and end of a track is not the recording.
 *
 * Every lossy codec pads. An MP3 encoder emits a frame of silence before the
 * first real sample and pads the last frame out; Opus states a `pre_skip`;
 * AAC in MP4 uses an edit list or `iTunSMPB`. FLAC and PCM need none.
 *
 * **One common answer, or gapless playback is impossible.** A library that
 * handles this per codec produces tracks whose length depends on what they
 * were encoded as, and the difference is a few thousand frames - small enough
 * to look like a rounding error and large enough to hear as a click between
 * two tracks of one album.
 *
 * Phase 1's formats state none of this, so both fields are zero for WAV and
 * AIFF. The struct exists now because the alternative is retrofitting it into
 * every call that returns a duration.
 */
typedef struct {
  /** Frames to discard from the start before the recording begins. */
  uint64_t encoder_delay;
  /** Frames at the end that are padding rather than recording. */
  uint64_t padding;
  /** Whether the container stated these, as against them being assumed zero.
   *  Zero delay that was stated and zero delay that was never mentioned are
   *  different facts, and only one of them can be relied on. */
  bool stated;
} GAUD_Trim;

/**
 * @brief Free a document and everything it owns.
 *
 * Does not destroy the stream it was loaded from; that is the caller's, and
 * it must stay alive as long as the document does - a track decodes by
 * reading from it.
 */
GAUD_API void gaud_doc_destroy(GAUD_Doc * doc);

/** @brief How many tracks. At least one in any document that loaded. */
GAUD_API size_t gaud_doc_track_count(const GAUD_Doc * doc);

/** @brief The track at @p index, or NULL. Owned by the document. */
GAUD_API GAUD_Track * gaud_doc_track(const GAUD_Doc * doc, size_t index);

/** @brief The name of the codec that produced this document, e.g. "wav". */
GAUD_API const char * gaud_doc_codec_name(const GAUD_Doc * doc);

/** @brief The stream the document was loaded from. */
GAUD_API GAUD_Stream * gaud_doc_stream(const GAUD_Doc * doc);

/**
 * @brief The document's tags, raw blocks and pictures.
 *
 * Never NULL for a document this library loaded: a file with no metadata
 * gets an empty ::GAUD_Meta rather than one, so a caller never has to
 * distinguish "no tags" from "nothing to ask". Owned by the document and
 * destroyed with it; gaud_meta_copy() is how to outlive it.
 */
GAUD_API GAUD_Meta * gaud_doc_meta(const GAUD_Doc * doc);

/** @brief What this track's decoder puts in a buffer. */
GAUD_API GAUD_Sample_Format gaud_track_format(const GAUD_Track * track);

/**
 * @brief How the container coded this track's samples.
 *
 * ::GAUD_CODING_PCM for an uncompressed track, which is what
 * gaud_track_format() alone already described. For a companded or ADPCM
 * track this is the only place the file's own coding is reported:
 * gaud_track_format() says ::GAUD_SAMPLE_S16 for all of them, because that
 * is what comes out.
 *
 * A caller re-encoding a file passes this straight into
 * ::GAUD_Encode_Params::coding to keep the coding it had.
 */
GAUD_API GAUD_Sample_Coding gaud_track_coding(const GAUD_Track * track);

/** @brief Frames per second. */
GAUD_API uint32_t gaud_track_sample_rate(const GAUD_Track * track);

/** @brief Which speakers, and how many channels. */
GAUD_API GAUD_Channel_Layout gaud_track_layout(const GAUD_Track * track);

/** @brief How many channels. */
GAUD_API uint32_t gaud_track_channels(const GAUD_Track * track);

/**
 * @brief How many frames the container says this track has.
 *
 * **This is a claim, not a measurement.** For WAV it is the data chunk's
 * length divided by the frame size, which is exact; for a bare MP3 it would
 * be whatever a Xing frame said, or a count of frames if one bothered. A
 * caller that must be certain decodes and counts.
 *
 * `UINT64_MAX` means the container did not say and it could not be derived,
 * which a stream of unknown length can produce.
 */
GAUD_API uint64_t gaud_track_frames(const GAUD_Track * track);

/** @brief The encoder delay and padding; see ::GAUD_Trim. */
GAUD_API GAUD_Trim gaud_track_trim(const GAUD_Track * track);

/**
 * @brief The track's duration in seconds, or -1 where the frame count is
 *   unknown.
 *
 * Computed from gaud_track_frames() and the sample rate, with
 * gaud_track_trim() subtracted, so it is the duration of the *recording*
 * rather than of the file's contents.
 */
GAUD_API double gaud_track_duration(const GAUD_Track * track);

/** @brief The document this track belongs to. */
GAUD_API GAUD_Doc * gaud_track_doc(const GAUD_Track * track);

/** @brief The track's index within its document. */
GAUD_API size_t gaud_track_index(const GAUD_Track * track);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_DOC_H
