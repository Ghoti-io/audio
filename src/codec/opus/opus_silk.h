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

/** Samples in the longest subframe, 5 ms at 16 kHz. */
#define SILK_MAX_SUBFRAME_LENGTH 80

/**
 * How much decoded output the long-term predictor can look back over.
 *
 * Twenty milliseconds, which is more than the longest pitch period the
 * format can code, so a subframe's predictor never reaches past it.
 */
#define SILK_LTP_MEM_MS 20

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

/** The expansion after a loss: 0.97 in Q16. */
#define SILK_BWE_AFTER_LOSS_Q16 63570

/** Samples of the excitation concealment draws its noise from. */
#define SILK_PLC_RAND_BUF_SIZE 128

/** @brief What concealment remembers of the last good frame. */
typedef struct {
  int32_t pitch_l_q8;                     ///< The lag to repeat, Q8.
  int16_t ltp_coef_q14[SILK_LTP_ORDER];   ///< The long-term filter to repeat.
  int16_t prev_lpc_q12[SILK_MAX_LPC_ORDER]; ///< The short-term filter.
  bool last_frame_lost;                   ///< So the next good one is faded in.
  int32_t rand_seed;                      ///< The noise generator.
  int16_t rand_scale_q14;                 ///< How loud the noise still is.
  int32_t conc_energy;                    ///< Energy of the last concealed frame.
  int conc_energy_shift;                  ///< And the shift that was applied.
  int16_t prev_ltp_scale_q14;             ///< The last frame's history scaling.
  int32_t prev_gain_q16[2];               ///< Its last two subframe gains.
  int fs_khz;                             ///< The rate this was set up for.
  int nb_subfr;                           ///< Subframes in that frame.
  int subfr_length;                       ///< And each one's length.
} SILK_Plc;

/** @brief Comfort noise: what an inactive stretch sounded like. */
typedef struct {
  int32_t exc_buf_q14[SILK_MAX_FRAME_LENGTH]; ///< Recent quiet excitation.
  int16_t smth_nlsf_q15[SILK_MAX_LPC_ORDER];  ///< A slowly moving spectrum.
  int32_t synth_state[SILK_MAX_LPC_ORDER];    ///< The noise filter's memory.
  int32_t smth_gain_q16;                      ///< A slowly moving gain.
  int32_t rand_seed;                          ///< The noise generator.
  int fs_khz;                                 ///< The rate this was set up for.
} SILK_Cng;

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
  int8_t prev_gain_index;               ///< What a delta gain is against.
  int16_t prev_nlsf_q15[SILK_MAX_LPC_ORDER]; ///< What interpolation starts from.
  bool first_after_reset;               ///< So nothing is interpolated yet.
  int ltp_mem_length;                   ///< 20 ms of samples.
  SILK_Indices indices;                 ///< The frame just parsed.
  int pulses[SILK_MAX_FRAME_LENGTH];    ///< Its excitation, signed.
  /**
   * The decoded output the long-term predictor reads back.
   *
   * Two subframes longer than it needs to be, because the re-whitening
   * at the third subframe of an interpolated frame copies this frame's
   * first half in beyond the end of the history.
   */
  int16_t out_buf[SILK_MAX_FRAME_LENGTH + 2 * SILK_MAX_SUBFRAME_LENGTH];
  int32_t lpc_state_q14[SILK_MAX_LPC_ORDER]; ///< The synthesis filter's memory.
  int32_t prev_gain_q16;                ///< What the state above is scaled by.
  SILK_Coding coding;                   ///< How the frame just parsed was coded.
  int32_t exc_q14[SILK_MAX_FRAME_LENGTH]; ///< The last frame's excitation.
  int loss_cnt;                         ///< Frames lost in a row, so far.
  int plc_signal_type;                  ///< The last good frame's type, for concealment.
  int lag_prev;                         ///< The last frame's final pitch lag.
  SILK_Plc plc;                         ///< Concealment's memory.
  SILK_Cng cng;                         ///< Comfort noise's.
} SILK_Channel;

/**
 * One frame's filters, which is what the indices named.
 *
 * Section 4.2.8 reconstructs a frame from exactly these: a gain per
 * subframe, two sets of short-term coefficients (the first half of the
 * frame may use an interpolated set), and for a voiced frame a pitch
 * lag and a five-tap filter per subframe.
 */
typedef struct {
  int32_t gains_q16[SILK_MAX_SUBFRAMES];   ///< One per subframe, linear.
  int16_t lpc_q12[2][SILK_MAX_LPC_ORDER];  ///< First half, then second.
  int pitch[SILK_MAX_SUBFRAMES];           ///< Lags in samples.
  int16_t ltp_q14[SILK_MAX_SUBFRAMES * SILK_LTP_ORDER]; ///< Five taps each.
  int16_t ltp_scale_q14;                   ///< How far the history is scaled.
} SILK_Parameters;

/** The most input samples one resampler call is given per millisecond. */
#define SILK_RESAMPLER_MAX_KHZ 16

/** Taps in the polyphase interpolator: four rows and their four mirrors. */
#define SILK_RESAMPLER_FIR_ORDER 8

/**
 * @brief The state of one channel's resampler to 48 kHz.
 *
 * RFC 6716 section 4.2.10 leaves the resampler to the implementation, but
 * section 6 does not: the conformance vectors were produced with the
 * reference's, and `opus_compare` measures how far another one strays.
 * This is the reference's, which upsamples by two with a pair of
 * all-pass chains and then interpolates the rest with a polyphase FIR.
 */
typedef struct {
  int32_t iir[6];                              ///< The two all-pass chains.
  int16_t fir[SILK_RESAMPLER_FIR_ORDER];       ///< The last input samples.
  int16_t delay_buf[SILK_RESAMPLER_MAX_KHZ];   ///< The delayed first millisecond.
  int fs_in_khz;                               ///< 8, 12 or 16.
  int input_delay;                             ///< Samples of delay to equalise.
  int32_t inv_ratio_q16;                       ///< Input steps per output step.
} SILK_Resampler;

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
  int api_channels;             ///< How many channels the caller wants out.
  int16_t s_mid[2];             ///< The last two mid samples of the last frame.
  int16_t s_side[2];            ///< And the side channel's.
  int32_t pred_prev_q13[2];     ///< The previous frame's stereo weights.
  SILK_Resampler resampler[2];  ///< One per output channel.
  bool stereo_to_mono;          ///< This packet collapses a stereo stream.
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

/**
 * @brief Turn one frame's indices into its filters. Section 4.2.7.4-6.
 *
 * Reads nothing from the bitstream, so it can be checked by value.
 *
 * @param channel The channel, whose @c indices this reads and whose
 *   gain, line-spectral and reset state it carries forward.
 * @param out Receives the gains, the coefficients, the pitch and the
 *   long-term filter.
 * @param coding Which conditional case the frame was coded under,
 *   which decides whether its first gain is absolute.
 */
void gaud_silk_decode_parameters(
    SILK_Channel * channel, SILK_Parameters * out, SILK_Coding coding);

/**
 * @brief The subframe gains, from their indices. Section 4.2.7.4.
 *
 * @param gains_q16 Receives one linear gain per subframe.
 * @param indices The decoded indices.
 * @param previous The running index, read and updated.
 * @param conditional True when the first index is a delta.
 * @param subframes 2 or 4.
 */
void gaud_silk_gains_dequant(int32_t * gains_q16, const int8_t * indices,
    int8_t * previous, bool conditional, int subframes);

/**
 * @brief The line spectral frequencies, from their indices.
 *
 * Sections 4.2.7.5.1 to 4.2.7.5.4: the stage-1 vector, the predicted
 * residual, the weighting, and the repair pass that makes the result
 * a filter.
 *
 * @param nlsf_q15 Receives the frequencies, ascending and spaced.
 * @param indices The stage-1 index and then one per coefficient.
 * @param wideband True for the sixteenth-order codebook.
 */
void gaud_silk_nlsf_decode(
    int16_t * nlsf_q15, const int8_t * indices, bool wideband);

/**
 * @brief Frequencies to prediction coefficients. Section 4.2.7.5.6.
 *
 * Including both repair loops: ten attempts at making the result fit
 * in sixteen bits and sixteen at making it stable.
 *
 * @param lpc_q12 Receives @p order coefficients.
 * @param nlsf_q15 The frequencies.
 * @param order 10 or 16.
 */
void gaud_silk_nlsf_to_lpc(
    int16_t * lpc_q12, const int16_t * nlsf_q15, int order);

/**
 * @brief One over the prediction gain, in Q30, or zero if unstable.
 *
 * Runs the Levinson recursion backwards to recover the reflection
 * coefficients; a magnitude at or beyond one means a pole outside the
 * unit circle.
 *
 * @param lpc_q12 The coefficients.
 * @param order How many.
 * @return The inverse gain, or 0 for an unstable filter.
 */
int32_t gaud_silk_lpc_inverse_gain(const int16_t * lpc_q12, int order);

/**
 * @brief The pitch lag of each subframe. Section 4.2.7.6.1.
 *
 * @param lag_index The frame's primary lag, as an offset from 2 ms.
 * @param contour_index Which per-subframe offset vector to add.
 * @param lags Receives one lag per subframe, in samples.
 * @param fs_khz 8, 12 or 16.
 * @param subframes 2 or 4.
 */
void gaud_silk_decode_pitch(int16_t lag_index, int8_t contour_index,
    int * lags, int fs_khz, int subframes);

/**
 * @brief Synthesise one frame's samples. Section 4.2.7.9 and 4.2.8.
 *
 * The excitation, then the long-term filter, then the short-term one,
 * then the gain.
 *
 * @param channel The channel, whose pulses this reads and whose
 *   filter memory and output history it carries forward.
 * @param parameters What the indices became.
 * @param out Receives @c frame_length samples.
 */
void gaud_silk_decode_core(SILK_Channel * channel,
    const SILK_Parameters * parameters, int16_t * out);

/**
 * @brief Read and discard a packet's redundancy frames: section 4.2.4.
 *
 * Every symbol of them has to be read, because they sit in front of the
 * regular frames in the same arithmetic-coded stream, and none of them
 * is wanted by a receiver that lost nothing.
 *
 * @param decoder The decoder, after gaud_silk_decode_header().
 * @param range The range decoder.
 */
void gaud_silk_skip_redundancy(SILK_Decoder * decoder, OPUS_Range * range);

/**
 * @brief Read one regular frame's symbols, for every coded channel.
 *
 * The stereo weights and the mid-only flag first, then each channel's
 * indices and excitation. Afterwards each channel's @c coding says how
 * its frame was coded, which the parameters decoder needs.
 *
 * @param decoder The decoder.
 * @param range The range decoder.
 * @param frame Which frame of the packet, from zero.
 * @return Whether the side channel was left out of this frame.
 */
bool gaud_silk_parse_frame(
    SILK_Decoder * decoder, OPUS_Range * range, int frame);

/**
 * @brief Reset one resampler for an input rate; the output is 48 kHz.
 *
 * @param resampler The state.
 * @param fs_in_khz 8, 12 or 16.
 */
void gaud_silk_resampler_init(SILK_Resampler * resampler, int fs_in_khz);

/**
 * @brief Resample one frame to 48 kHz.
 *
 * @param resampler The state, carried to the next call.
 * @param out Receives `in_len * 48 / fs_in_khz / 1000 * 1000` samples:
 *   48 for each input millisecond.
 * @param in The signal.
 * @param in_len A whole number of milliseconds, at least one.
 */
void gaud_silk_resample(SILK_Resampler * resampler, int16_t * out,
    const int16_t * in, int in_len);

/**
 * @brief Turn mid and side back into left and right: section 4.2.9.
 *
 * @param decoder Whose weights and two-sample history this carries.
 * @param mid The mid channel; becomes left. Sample `n` is at `n + 1`,
 *   and `[0]` and the two after the frame are the history's.
 * @param side The side channel; becomes right. Same layout.
 * @param pred_q13 This frame's two weights.
 * @param fs_khz 8, 12 or 16.
 * @param length Samples in the frame.
 */
void gaud_silk_stereo_ms_to_lr(SILK_Decoder * decoder, int16_t * mid,
    int16_t * side, const int32_t * pred_q13, int fs_khz, int length);

/**
 * @brief Configure for one Opus frame's SILK content, keeping state.
 *
 * What the reference does when a frame's first SILK frame arrives: the
 * channel count may have grown (the second channel then starts afresh),
 * the rate may have changed (then the channel's synthesis state does),
 * and a stream that has just become stereo at both ends starts the
 * side channel's prediction from nothing.
 *
 * @param decoder The decoder.
 * @param channels Coded channels: 1 or 2.
 * @param api_channels Channels to output: 1 or 2.
 * @param fs_khz 8, 12 or 16.
 * @param duration_ms 10, 20, 40 or 60.
 * @return False for a combination the format does not have.
 */
bool gaud_silk_configure(SILK_Decoder * decoder, int channels,
    int api_channels, int fs_khz, int duration_ms);

/**
 * @brief Decode the next SILK frame of the packet to 48 kHz samples.
 *
 * The first call after gaud_silk_configure() also reads the packet's
 * header and redundancy frames.
 *
 * @param decoder The decoder.
 * @param range The range decoder.
 * @param out Receives @c api_channels samples interleaved per output
 *   sample; room for 960 of them is enough.
 * @param out_samples Receives how many per channel.
 */
void gaud_silk_decode_frame(SILK_Decoder * decoder, OPUS_Range * range,
    int16_t * out, int * out_samples);

/**
 * @brief Decode the next SILK frame of the packet from its redundancy.
 *
 * A packet may carry a low-rate copy of the frames of the packet
 * before it (RFC 6716 section 4.2.4), so that a receiver that lost
 * that one can still play something of it. This reads the copy in
 * place of the regular frame the packet is about, and a frame the copy
 * does not cover is concealed as a lost one is. Nothing here reads the
 * regular frames, so what follows the call in the range decoder is of
 * no use.
 *
 * @param decoder The decoder, configured as for the packet's own frames.
 * @param range The range decoder, at the packet's start for the first
 *   call.
 * @param out Receives @c api_channels samples interleaved per output
 *   sample; room for 960 of them is enough.
 * @param out_samples Receives how many per channel.
 */
void gaud_silk_decode_fec(SILK_Decoder * decoder, OPUS_Range * range,
    int16_t * out, int * out_samples);

/**
 * @brief Conceal one SILK frame that was not received.
 *
 * Extrapolates from the last good frame: its pitch and spectrum carried
 * on and faded, with noise from its own excitation mixed in. Section
 * 4.4 is informative, and the reference's version is what this follows.
 *
 * @param decoder The decoder, configured as for a real frame.
 * @param out Receives @c api_channels samples interleaved per output
 *   sample.
 * @param out_samples Receives how many per channel.
 */
void gaud_silk_decode_lost(
    SILK_Decoder * decoder, int16_t * out, int * out_samples);

/**
 * @brief Run the short-term filter forwards, to recover its input.
 *
 * RFC 6716's `silk_LPC_analysis_filter`: the inverse of the synthesis
 * filter, used to turn decoded output back into the whitened history
 * the long-term predictor and concealment read.
 *
 * @param out Receives @p length samples; the first @p order are zero.
 * @param in The signal, which must have @p order samples before it.
 * @param lpc_q12 The coefficients.
 * @param length How many samples to produce.
 * @param order 10 or 16.
 */
void gaud_silk_lpc_analysis_filter(int16_t * out, const int16_t * in,
    const int16_t * lpc_q12, int length, int order);

/**
 * @brief Remember what concealment will need from a good frame.
 *
 * @param channel The channel.
 * @param parameters What the frame's indices became.
 */
void gaud_silk_plc_update(
    SILK_Channel * channel, const SILK_Parameters * parameters);

/**
 * @brief Synthesise a frame that did not arrive.
 *
 * @param channel The channel; its loss count is advanced.
 * @param frame Receives @c frame_length samples.
 */
void gaud_silk_plc_conceal(SILK_Channel * channel, int16_t * frame);

/**
 * @brief Blend a good frame in after concealed ones.
 *
 * @param channel The channel.
 * @param frame The frame, edited in place.
 * @param length Its length.
 */
void gaud_silk_plc_glue(SILK_Channel * channel, int16_t * frame, int length);

/**
 * @brief Track the comfort noise, and add it to a concealed frame.
 *
 * @param channel The channel.
 * @param parameters The frame's parameters, or NULL for a lost frame.
 * @param frame The frame, edited in place.
 * @param length Its length.
 */
void gaud_silk_cng(SILK_Channel * channel, const SILK_Parameters * parameters,
    int16_t * frame, int length);

/**
 * @brief Chirp an all-pole filter's poles towards the origin.
 *
 * @param ar The coefficients, expanded in place.
 * @param d How many.
 * @param chirp_q16 The factor, 0 to 1 in Q16.
 */
void gaud_silk_bwexpander(int16_t * ar, int d, int32_t chirp_q16);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_SILK_H
