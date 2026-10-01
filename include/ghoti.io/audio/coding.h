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
 * @file coding.h
 *
 * How a container codes its samples, as distinct from what those samples
 * are once decoded.
 *
 * ::GAUD_Sample_Format answers "what is in the buffer". This answers "what
 * was in the file". For most of phase 1 the two were the same question -
 * PCM is stored as it is used - but WAV and AIFF-C both carry codings that
 * decode *to* PCM without being it: G.711 companding, and two ADPCM
 * families. A µ-law file's samples are ::GAUD_SAMPLE_S16 in every buffer
 * this library hands out, and ::GAUD_CODING_G711_ULAW in the file.
 *
 * Keeping them apart is what stops "8-bit" meaning two things. A µ-law
 * sample occupies one byte and carries about fourteen bits of range; an
 * ::GAUD_SAMPLE_U8 sample occupies one byte and carries eight. Folding the
 * first into the second would make gaud_frame_size() wrong, would make
 * ops.h operate on companded bytes as though they were amplitudes, and
 * would lose the distinction on a round trip.
 *
 * A zeroed ::GAUD_Encode_Params or ::GAUD_Track_Desc therefore means
 * ::GAUD_CODING_PCM, which is the behaviour every phase 1 caller already
 * has.
 */

#ifndef GHOTI_IO_GAUD_CODING_H
#define GHOTI_IO_GAUD_CODING_H

#include <ghoti.io/audio/buffer.h>
#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief How samples are coded in the container.
 *
 * The two IMA entries are **one nibble algorithm in two framings**, and they
 * are separate values because a caller asking for one and getting the other
 * would get a file no reader accepts. WAV's framing puts a four-byte
 * preamble per channel at the head of a block whose size the header states;
 * QuickTime's puts a two-byte preamble at the head of a fixed 34-byte packet
 * holding exactly 64 sample frames. ffmpeg names them `adpcm_ima_wav` and
 * `adpcm_ima_qt` for the same reason.
 */
typedef enum {
  /** Stored as ::GAUD_Sample_Format says, with no coding applied. */
  GAUD_CODING_PCM = 0,
  /** ITU-T G.711 µ-law. One byte in, one ::GAUD_SAMPLE_S16 out. */
  GAUD_CODING_G711_ULAW,
  /** ITU-T G.711 A-law. */
  GAUD_CODING_G711_ALAW,
  /** IMA/DVI ADPCM in WAV's block framing (`WAVE_FORMAT_IMA_ADPCM`). */
  GAUD_CODING_ADPCM_IMA_WAV,
  /** The same algorithm in QuickTime's 34-byte packets (AIFF-C `ima4`). */
  GAUD_CODING_ADPCM_IMA_QT,
  /** Microsoft ADPCM (`WAVE_FORMAT_ADPCM`). WAV only; AIFF-C has no
   *  spelling for it, and a writer asked for one answers
   *  ::GAUD_ERR_UNSUPPORTED rather than inventing a compression type. */
  GAUD_CODING_ADPCM_MS,
  /**
   * FLAC, as RFC 9639 defines it.
   *
   * **The first coding here that is a codec rather than a sample
   * representation**, and the first whose decoded format is not fixed: a
   * FLAC stream is 8, 16, 24 or 32 bits and the track says which. It is a
   * coding rather than only a codec name because the bitstream outlives
   * its container - it is carried natively, in Ogg from phase 4, and in
   * MP4 and Matroska later, and a caller asking "what is in this track"
   * wants one answer for all four.
   */
  GAUD_CODING_FLAC,
  /**
   * MPEG-1, MPEG-2 or MPEG-2.5 Layer I.
   *
   * **Three values and not one**, because the three layers are three
   * bitstreams and not three settings of one. A Layer II frame and a Layer
   * III frame of the same length at the same rate share a four-byte header
   * and nothing after it: Layer I and II quantise subbands directly, Layer
   * III adds an MDCT, Huffman coding and a bit reservoir that spans
   * frames. A caller asking what is in a track and being told "mpeg" would
   * have to go and read the frame header to find out whether anything it
   * knows can play it, which is the question it just asked.
   *
   * The version is deliberately *not* in these values. MPEG-2's low
   * sampling frequency extension changes the sample rate, the granule
   * count and the scalefactor tables, but a Layer III decoder decodes both
   * - so the version is a property of the stream that gaud_track_frames()
   * and the sample rate already report, not a different coding.
   */
  GAUD_CODING_MPEG_LAYER1,
  /** MPEG-1, MPEG-2 or MPEG-2.5 Layer II. */
  GAUD_CODING_MPEG_LAYER2,
  /** MPEG-1, MPEG-2 or MPEG-2.5 Layer III, which is to say MP3. */
  GAUD_CODING_MPEG_LAYER3,
  /** Closes the enum so a test can check the names. */
  GAUD_CODING_COUNT
} GAUD_Sample_Coding;

/**
 * @brief A short lower-case name: "pcm", "ulaw", "alaw", "ima-wav",
 *   "ima-qt", "ms-adpcm", "flac", "mp1", "mp2", "mp3".
 *
 * @return A static string, never NULL; "unknown" outside the enum.
 */
GAUD_API const char * gaud_sample_coding_name(GAUD_Sample_Coding coding);

/**
 * @brief Whether @p coding stores samples uncoded.
 *
 * True only for ::GAUD_CODING_PCM. Every other value needs a decode step
 * between the file's bytes and a buffer, which is what makes the byte
 * arithmetic in gaud_frame_size() inapplicable to the file's side of it.
 */
GAUD_API bool gaud_sample_coding_is_pcm(GAUD_Sample_Coding coding);

/**
 * @brief The ::GAUD_Sample_Format that @p coding decodes to and encodes
 *   from.
 *
 * ::GAUD_SAMPLE_S16 for G.711 and both ADPCM families, which is not a
 * simplification: G.711 is defined onto a 14-bit (µ-law) or 13-bit (A-law)
 * range that is conventionally carried left-aligned in 16 bits, and both
 * ADPCM families predict in 16-bit signed arithmetic. There is no wider
 * form to lose.
 *
 * ::GAUD_SAMPLE_S16 for all three MPEG layers, and that one is a policy
 * rather than a property of the format: an MPEG audio frame carries
 * quantised spectral values with no bit depth anywhere in it, so the depth
 * of the output is chosen by the decoder. This library chooses 16, which is
 * what every integer MPEG decoder has produced since the format existed and
 * what the reference decoders are compared at.
 *
 * For ::GAUD_CODING_PCM **and ::GAUD_CODING_FLAC** there is no single
 * answer and this returns ::GAUD_SAMPLE_FORMAT_COUNT, which is not a
 * format. The two reach it from opposite directions - PCM is not coded at
 * all, and FLAC is coded but carries its own bit depth - and in both cases
 * the track is what to ask. Phase 4 is where this stopped being "every
 * coded value answers S16"; a caller that had hard-coded that assumption
 * would be wrong about FLAC, so the contract says it out loud rather than
 * leaving the new value to surprise it.
 */
GAUD_API GAUD_Sample_Format gaud_sample_coding_format(
    GAUD_Sample_Coding coding);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_CODING_H
