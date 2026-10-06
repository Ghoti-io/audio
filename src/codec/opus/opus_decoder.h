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
 * The Opus decoder proper: one packet in, 48 kHz samples out. Never
 * installed.
 *
 * Everything under this directory decodes one thing; this is what
 * decides which, in what order, and what to do with the places where
 * the three coders meet. RFC 6716 describes the pieces in sections
 * 4.2 and 4.3 and leaves the joining to the reference decoder, which
 * is normative where they differ (section 6), so most of what is
 * below is a translation of `opus_decoder.c` rather than of the prose.
 *
 * **A mode change is the hard part, and it is handled three ways.**
 * When a stream moves from CELT to SILK or hybrid, or back, the two
 * coders' overlapping memories do not line up, so the encoder may
 * include a *redundancy frame*: a short CELT-coded copy of the 5 ms
 * around the switch, which the decoder cross-fades with. When it did
 * not, the decoder invents one by concealing a packet that was never
 * lost, so that the boundary is smooth anyway. And on the switch
 * from hybrid to SILK-only, where there is no CELT frame to carry the
 * high band's tail out, a silent one is decoded to let it fade.
 *
 * **A packet that did not arrive is decoded too.** Section 4.4 is not
 * normative, but a decoder that stops at a gap is not usable, and the
 * transition handling above needs the concealment anyway.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_DECODER_H
#define GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_DECODER_H

#include <ghoti.io/audio/macros.h>
#include "opus_celt.h"
#include "opus_internal.h"
#include "opus_silk.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The most samples per channel one packet can decode to: 120 ms. */
#define OPUS_MAX_PACKET_SAMPLES 5760u

/** @brief One Opus stream's decoder state, mono or stereo. */
typedef struct {
  uint32_t channels;        ///< Channels out: 1 or 2.
  uint32_t stream_channels; ///< Channels the last packet coded.
  int prev_mode;            ///< 0 before any packet, else an ::OPUS_Mode + 1.
  int mode;                 ///< The mode of the packet being decoded, same scale.
  unsigned bandwidth;       ///< 0 to 4, as in ::OPUS_Toc.
  uint32_t frame_size;      ///< Samples per frame of the last packet, at 48 kHz.
  bool prev_redundancy;     ///< Whether the last frame ended with a redundant one.
  uint32_t range_final;     ///< The last frame's final range decoder state.
  int redundancy;           ///< The last frame's: 0 none, 1 CELT to SILK, 2 SILK to CELT.
  int silk_khz;             ///< The internal SILK rate the last packet used.
  int silk_channels;        ///< And its coded channel count.
  SILK_Decoder silk;        ///< Everything SILK carries between frames.
  CELT_Decoder celt;        ///< And CELT.
  CELT_Scratch scratch;     ///< CELT's working memory, not state.
} OPUS_Decoder;

/**
 * @brief Make a decoder.
 *
 * @param allocator Where it comes from.
 * @param channels 1 or 2.
 * @param out Receives it.
 * @return ::GAUD_OK, ::GAUD_ERR_INVALID or ::GAUD_ERR_OOM.
 */
GAUD_Result gaud_opus_decoder_create(const GAUD_Allocator * allocator,
    uint32_t channels, OPUS_Decoder ** out);

/**
 * @brief Free a decoder. Safe on NULL.
 *
 * @param allocator The one it came from.
 * @param decoder The decoder.
 */
void gaud_opus_decoder_destroy(
    const GAUD_Allocator * allocator, OPUS_Decoder * decoder);

/**
 * @brief Forget everything: the state a decoder has after a seek.
 *
 * @param decoder The decoder.
 */
void gaud_opus_decoder_reset(OPUS_Decoder * decoder);

/**
 * @brief Decode one packet.
 *
 * @param decoder The decoder.
 * @param data The packet.
 * @param size Its length; zero means the packet was lost, and the
 *   decoder conceals @p lost_samples of it instead.
 * @param pcm Receives interleaved 16-bit samples.
 * @param capacity Room in @p pcm, in samples per channel.
 * @param lost_samples For a lost packet, how many samples per channel
 *   to produce; ignored otherwise.
 * @param out_samples Receives how many samples per channel were written.
 * @return ::GAUD_OK, ::GAUD_ERR_CORRUPT for a packet the format
 *   refuses, or ::GAUD_ERR_INVALID if @p capacity is too small.
 */
GAUD_Result gaud_opus_decode_packet(OPUS_Decoder * decoder,
    const unsigned char * data, size_t size, int16_t * pcm, uint32_t capacity,
    uint32_t lost_samples, uint32_t * out_samples);

/**
 * @brief Decode what a packet carries of the packet before it.
 *
 * Forward error correction, RFC 6716's `decode_fec`. A SILK or hybrid
 * packet may hold a low-rate copy of the frames of the one before it,
 * and a receiver that lost that one calls this with the next packet to
 * play the copy in its place, then ::gaud_opus_decode_packet with the
 * same packet to play the packet itself.
 *
 * What comes out is as long as @p data's own frames, which is the
 * duration it assumes the lost packet had. A frame the copy does not
 * cover, and all of a CELT-only packet, is concealed as a lost one is,
 * so a packet with no copy still yields something. The decoder's state
 * moves as for a decode of the lost packet, and the packet is then
 * decoded as the one that follows it.
 *
 * Not done: the libopus wrapper's handling of a lost span longer or
 * shorter than the next packet; the caller chooses what to ask for.
 *
 * @param decoder The decoder.
 * @param data The packet *after* the lost one.
 * @param size Its length.
 * @param pcm Receives interleaved 16-bit samples.
 * @param capacity Room in @p pcm, in samples per channel.
 * @param out_samples Receives samples per channel.
 * @return ::GAUD_OK, ::GAUD_ERR_CORRUPT for a packet the format
 *   refuses, or ::GAUD_ERR_INVALID if @p capacity is too small or there
 *   is no packet.
 */
GAUD_Result gaud_opus_decode_fec(OPUS_Decoder * decoder,
    const unsigned char * data, size_t size, int16_t * pcm, uint32_t capacity,
    uint32_t * out_samples);

/**
 * @brief Decode one packet of a multistream's several, or the only one.
 *
 * What ::gaud_opus_decode_packet does, for a packet that may be
 * followed by the next stream's: with @p self_delimited set, the last
 * frame states its own length (RFC 6716 Appendix B), and @p consumed
 * says where the next stream's packet begins.
 *
 * @param decoder The decoder.
 * @param data The packet.
 * @param size Bytes available, which may include later streams'.
 * @param self_delimited Whether the framing is Appendix B's.
 * @param pcm Receives interleaved 16-bit samples.
 * @param capacity Room in @p pcm, in samples per channel.
 * @param out_samples Receives samples per channel.
 * @param consumed Receives how many bytes of @p data this packet took.
 * @return ::GAUD_OK, ::GAUD_ERR_CORRUPT, or ::GAUD_ERR_INVALID.
 */
GAUD_Result gaud_opus_decode_stream_packet(OPUS_Decoder * decoder,
    const unsigned char * data, size_t size, bool self_delimited,
    int16_t * pcm, uint32_t capacity, uint32_t * out_samples,
    size_t * consumed);

/**
 * @brief Scale decoded samples by an `OpusHead` output gain.
 *
 * RFC 7845 section 5.1: a decoder SHOULD apply it, and libopus does so
 * in fixed point as below, rounding and then clamping to +/-32767.
 *
 * @param pcm The samples, scaled in place.
 * @param count How many samples in all.
 * @param gain_q8 The gain in Q7.8 decibels; zero leaves them alone.
 */
void gaud_opus_apply_gain(int16_t * pcm, size_t count, int32_t gain_q8);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_DECODER_H
