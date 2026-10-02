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
 * SILK: the speech half of Opus. RFC 6716 section 4.2. Never installed.
 *
 * SILK is a linear-prediction coder and shares nothing with CELT but the
 * range decoder. A frame is an excitation signal run through a
 * long-term predictor (the pitch) and then a short-term one (the vocal
 * tract), with a gain per 5 ms subframe. What is transmitted is the
 * filters and the excitation; what makes it a *codec* rather than a
 * filter bank is that almost every parameter is coded against the one
 * before it, in time and across the subframe.
 *
 * Four things about this layer decide how the code below is written.
 *
 * **Parsing and reconstruction are separable, and separating them is
 * worth doing.** Every symbol SILK reads is decided by the frame's own
 * structure and by flags decoded earlier - never by a reconstructed
 * sample value. So the bitstream can be consumed correctly by code that
 * does no arithmetic at all, and section 6's strict requirement, that
 * the final range decoder state match the reference exactly, is a
 * property of *only* that half. The parse is therefore written first
 * and checked against 20,075 conformance packets before a single
 * sample is reconstructed.
 *
 * **The frame is not the packet.** An Opus packet carrying 40 or 60 ms
 * holds two or three 20 ms SILK frames per channel, and their header
 * bits - one voice-activity flag per frame per channel, then one
 * low-bitrate-redundancy flag per channel - are all decoded up front,
 * before any of the frames. Those come first precisely so that a
 * receiver can see whether a packet contains any active speech without
 * running the range decoder at all: they are binary symbols with
 * uniform probability, so they are the top bits of the first byte.
 *
 * **Conditional coding is a property of position, not of content.**
 * Whether a frame codes its first gain absolutely or as a delta, and
 * whether it codes an LTP scaling factor at all, depends on where the
 * frame sits in the packet and on whether the previous frame of the
 * same kind was coded - not on anything in the frame. ::SILK_Coding
 * names the three cases, and getting one wrong changes how many
 * symbols are read.
 *
 * **The redundancy frames are parsed and thrown away.** A packet may
 * carry a low-bitrate copy of an earlier frame for a receiver that lost
 * it. A receiver that did not lose anything still has to decode every
 * symbol of it, because it sits in front of the frames that matter in
 * the same arithmetic-coded stream. Skipping it is not possible; only
 * discarding the result is.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_SILK_H
#define GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_SILK_H

#include "opus_range.h"
#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 20 ms SILK frames in one Opus packet: 60 ms is the longest. */
#define SILK_MAX_FRAMES 3

/** Subframes in a 20 ms SILK frame; a 10 ms frame has two. */
#define SILK_MAX_SUBFRAMES 4

/** Taps in the long-term predictor. */
#define SILK_LTP_ORDER 5

/** The wideband LPC order; narrowband and medium band use ten. */
#define SILK_MAX_LPC_ORDER 16

/** Samples in a shell block, which is the excitation's unit. */
#define SILK_SHELL_BLOCK 16

/** Shell blocks in the longest frame: 20 ms of wideband is 320 samples. */
#define SILK_MAX_SHELL_BLOCKS 20

/** Samples in the longest frame, 20 ms at 16 kHz. */
#define SILK_MAX_FRAME_LENGTH 320

/** How far a stage-2 LSF index goes before the extension takes over. */
#define SILK_NLSF_MAX_AMPLITUDE 4

/** The largest pulse count a shell block codes without extra LSBs. */
#define SILK_MAX_PULSES 16

/** Section 4.2.7.3's three signal types. */
typedef enum {
  SILK_SIGNAL_INACTIVE = 0, ///< No voice activity.
  SILK_SIGNAL_UNVOICED = 1, ///< Speech with no pitch.
  SILK_SIGNAL_VOICED = 2,   ///< Speech with a pitch, so an LTP filter.
} SILK_SignalType;

/**
 * How a frame codes what it could have coded against its predecessor.
 *
 * The three cases differ in two places: the first subframe's gain is
 * either absolute or a delta, and the LTP scaling factor is present
 * only in the first. The middle case exists because a frame can be
 * independent of the previous frame's *parameters* while still being
 * able to reuse its *samples* - which is what happens at the start of
 * an Opus packet whose predecessor was decoded normally.
 */
typedef enum {
  SILK_CODE_INDEPENDENT = 0,          ///< Absolute gain, LTP scaling coded.
  SILK_CODE_INDEPENDENT_NO_SCALE = 1, ///< Absolute gain, no LTP scaling.
  SILK_CODE_CONDITIONAL = 2,          ///< Delta gain, no LTP scaling.
} SILK_Coding;

/**
 * Everything one SILK frame reads before its excitation.
 *
 * These are indices rather than values: section 4.2.7 decodes them and
 * section 4.2.8 turns them into gains, filters and lags. Keeping the
 * two apart is what lets the parse be checked on its own.
 */
typedef struct {
  int8_t signal_type;       ///< ::SILK_SignalType.
  int8_t quant_offset_type; ///< 0 for low, 1 for high; see Table 53.
  int8_t gains[SILK_MAX_SUBFRAMES];            ///< Log-gain indices.
  int8_t nlsf[SILK_MAX_LPC_ORDER + 1];         ///< Stage 1, then the residual.
  int8_t nlsf_interp_q2;                       ///< 0 to 4; 4 means no interp.
  int16_t lag_index;                           ///< The primary pitch lag.
  int8_t contour_index;                        ///< Per-subframe lag offsets.
  int8_t per_index;                            ///< Which LTP codebook.
  int8_t ltp[SILK_MAX_SUBFRAMES];              ///< An index into it each.
  int8_t ltp_scale_index;                      ///< 0, 1 or 2; see Table 42.
  int8_t seed;                                 ///< The noise generator's seed.
} SILK_Indices;

/**
 * One channel's decoder: the bitstream state that outlives a frame.
 *
 * Only three fields here are genuinely carried *between* frames by the
 * parse - the previous signal type and lag index, which the pitch
 * delta coding reads, and the flags decoded for the whole packet. The
 * rest describe the configuration, which changes only when the packet's
 * bandwidth or duration does.
 */
typedef struct {
  int fs_khz;         ///< 8, 12 or 16.
  int nb_subfr;       ///< 2 for a 10 ms frame, 4 for 20 ms.
  int lpc_order;      ///< 16 for wideband, 10 otherwise.
  int subfr_length;   ///< 5 ms of samples.
  int frame_length;   ///< @c nb_subfr times that.
  int shell_blocks;   ///< Table 44; 10 ms medium band rounds up.
  int frames_per_packet;                ///< 1, 2 or 3.
  int frames_decoded;                   ///< How many of them are done.
  bool vad[SILK_MAX_FRAMES];            ///< One voice-activity flag each.
  bool lbrr_flag;                       ///< Any redundancy in this packet.
  bool lbrr[SILK_MAX_FRAMES];           ///< Which frames carry it.
  int8_t prev_signal_type;              ///< For the pitch delta's condition.
  int16_t prev_lag_index;               ///< What that delta is against.
  SILK_Indices indices;                 ///< The frame just parsed.
  int pulses[SILK_MAX_FRAME_LENGTH];    ///< Its excitation, signed.
} SILK_Channel;

/** Both channels, plus the stereo prediction the mid one is coded under. */
typedef struct {
  int channels;                 ///< 1 or 2 coded channels.
  SILK_Channel channel[2];      ///< Mid and side, or just the one.
  int32_t stereo_pred_q13[2];   ///< This frame's two prediction weights.
  bool mid_only;                ///< The side channel is not coded.
  /**
   * Whether the previous frame omitted the side channel.
   *
   * This outlives the packet, and it has to: it decides whether the
   * side channel of the next frame codes an LTP scaling factor, so a
   * decoder that reset it at a packet boundary would read the wrong
   * number of symbols on the first frame after a mid-only one.
   */
  bool prev_mid_only;
} SILK_Decoder;

/**
 * @brief Configure a decoder for one Opus packet's SILK content.
 *
 * @param decoder Receives it; previous bitstream history is kept only
 *   when the configuration is unchanged, which is what the format
 *   requires of a decoder following a stream.
 * @param channels 1 or 2.
 * @param fs_khz 8, 12 or 16.
 * @param duration_ms 10, 20, 40 or 60.
 * @return False for a combination the format does not have.
 */
bool gaud_silk_decoder_init(
    SILK_Decoder * decoder, int channels, int fs_khz, int duration_ms);

/**
 * @brief Read the per-packet header bits: section 4.2.3 and 4.2.4.
 *
 * One voice-activity flag per SILK frame per channel and then one
 * redundancy flag per channel, all as uniform binary symbols, followed
 * by the per-frame redundancy flags for packets longer than 20 ms.
 *
 * @param decoder The decoder, whose flags this fills in.
 * @param range The range decoder, positioned at the start of the packet.
 */
void gaud_silk_decode_header(SILK_Decoder * decoder, OPUS_Range * range);

/**
 * @brief Read the stereo prediction weights: section 4.2.7.1.
 *
 * Three symbols code a shared high-order index and then a low index and
 * an interpolation offset for each weight, which between them name a
 * point between two entries of Table 7.
 *
 * @param range The range decoder.
 * @param pred_q13 Receives the two weights, the first already reduced
 *   by the second the way the reference leaves them.
 */
void gaud_silk_decode_stereo_pred(OPUS_Range * range, int32_t pred_q13[2]);

/**
 * @brief Read the flag saying the side channel is absent: section 4.2.7.2.
 *
 * @param range The range decoder.
 * @return True when only the mid channel is coded.
 */
bool gaud_silk_decode_mid_only(OPUS_Range * range);

/**
 * @brief Read one SILK frame's parameters: section 4.2.7.1 to 4.2.7.7.
 *
 * Everything but the excitation, in the order Table 5 gives.
 *
 * @param channel The channel, whose @c indices this fills in and whose
 *   previous signal type and lag index it both reads and updates.
 * @param range The range decoder.
 * @param frame Which 20 ms frame of the packet this is.
 * @param lbrr True when this is a redundancy frame, which uses the
 *   active frame-type distribution regardless of the voice-activity
 *   flag because a redundancy frame is only ever sent for speech.
 * @param coding Which of the three conditional cases applies.
 */
void gaud_silk_decode_indices(SILK_Channel * channel, OPUS_Range * range,
    int frame, bool lbrr, SILK_Coding coding);

/**
 * @brief Read one frame's excitation: section 4.2.7.8.
 *
 * The rate level, then a pulse count per shell block, then each
 * block's pulse positions by recursive halving, then any extra
 * least-significant bits, then a sign for every non-zero sample.
 *
 * @param channel The channel, whose @c pulses this fills in.
 * @param range The range decoder.
 */
void gaud_silk_decode_pulses(SILK_Channel * channel, OPUS_Range * range);

/**
 * @brief Consume one Opus packet's whole SILK payload.
 *
 * The header bits, then the redundancy frames, then every regular
 * frame of every coded channel, in bitstream order. Nothing is
 * reconstructed: this reads exactly the symbols the format says are
 * there, which is what section 6's range-state requirement is about.
 *
 * @param decoder The decoder, already configured for this packet.
 * @param range The range decoder, positioned at the start of the
 *   packet's SILK section.
 */
void gaud_silk_parse_packet(SILK_Decoder * decoder, OPUS_Range * range);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_SILK_H
