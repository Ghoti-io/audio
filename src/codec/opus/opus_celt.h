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
 * CELT: the transform half of Opus. RFC 6716 section 4.3. Never installed.
 *
 * CELT divides the MDCT spectrum into 21 bands that roughly follow the
 * ear's critical bands, and codes each band as a **gain and a shape**
 * separately: the energy explicitly, and then a unit-norm vector for
 * what is left. Coding the gain on its own is the design's central
 * idea - it means the spectral envelope survives whatever the bit
 * budget does to the shape.
 *
 * Three things about this layer decide how the code below is written.
 *
 * **The bit allocation must be reproduced exactly, and it is not sent.**
 * Section 4.3.3 is blunt: the allocation "MUST be recovered exactly so
 * that identical coding decisions are made in the encoder and decoder",
 * and "any deviation from the reference's resulting bit allocation will
 * result in corrupted output". The number of bits each band gets is
 * computed from the frame size, the channel count, a handful of decoded
 * flags and the number of bits left - and that computation then decides
 * how many symbols are read next. So an allocator that is one bit out
 * in one band does not degrade the sound; it desynchronises the range
 * decoder and everything after it is noise.
 *
 * **Everything is integer, and that is this library's requirement
 * rather than the format's.** RFC 6716 defines normative behaviour as
 * the floating-point configuration, and ships a fixed-point one beside
 * it. planning/audio.md section 11.1 requires byte-identical output on
 * every architecture, which floating point cannot promise, so this
 * follows the fixed-point arithmetic. The consequence is visible in the
 * conformance numbers and is not a defect: the reference's own
 * fixed-point build scores 98.5% on test vector 11 where its
 * floating-point build scores 99.8%, and `opus_compare` passes both.
 * What an integer decoder must still match *exactly* is the final range
 * decoder state, because that depends only on which symbols were read.
 *
 * **Two shifts need care.** The format's arithmetic relies on right
 * shifts of negative values behaving as division by a power of two,
 * which C makes implementation-defined and every target here
 * implements as an arithmetic shift; RFC 6716 Appendix A states the
 * same reliance. Left shifts of negative values, on the other hand, are
 * undefined behaviour rather than implementation-defined, so
 * ::gaud_celt_shl32 goes through an unsigned type. The Vorbis decoder
 * learned that one from UBSan.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_CELT_H
#define GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_CELT_H

#include "opus_range.h"
#include "opus_tables.h"
#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Fractional bits the allocator works in: eighths. */
#define CELT_BITRES 3

/** Fixed-point position of the energy's base-two logarithm. */
#define CELT_DB_SHIFT 10

/** Bands in the standard mode; Opus Custom may differ and is not read. */
#define CELT_BANDS 21

/** Samples of overlap between consecutive frames, at 48 kHz. */
#define CELT_OVERLAP 120

/** Samples in the shortest MDCT, which is 2.5 ms at 48 kHz. */
#define CELT_SHORT_MDCT 120

/** The largest `LM`: a 20 ms frame is eight short MDCTs. */
#define CELT_MAX_LM 3

/** The most MDCT bins one frame can have: 120 << 3. */
#define CELT_MAX_SIZE (CELT_SHORT_MDCT << CELT_MAX_LM)

/** The ceiling on fine energy bits for one band. */
#define CELT_MAX_FINE_BITS 8

/** One in Q15. Not 32768, which Q15 cannot hold. */
#define CELT_Q15ONE 32767

/**
 * @brief Everything about a frame size that does not change per packet.
 *
 * Built by ::gaud_celt_mode_init rather than tabulated, because the
 * only input is `LM` and deriving it keeps the four cases from drifting.
 */
typedef struct {
  unsigned lm;              ///< 0 to 3: the frame is 120 << lm samples.
  uint32_t size;            ///< MDCT bins in the frame, 120 << lm.
  uint32_t shorts;          ///< Short MDCTs the frame splits into, 1 << lm.
  const int16_t * edges;    ///< Band edges, in short-MDCT bins.
  uint32_t bands;           ///< Always ::CELT_BANDS here.
} CELT_Mode;

/**
 * @brief Set up @p mode for a frame of `120 << lm` samples.
 *
 * @param mode Receives it.
 * @param lm 0, 1, 2 or 3.
 * @return False for an `lm` the format does not have.
 */
bool gaud_celt_mode_init(CELT_Mode * mode, unsigned lm);

/**
 * @brief The first and last bin of band @p band, in frame bins.
 *
 * The edge table is in short-MDCT bins, so a frame of `1 << lm` short
 * MDCTs scales every edge by the same factor. Getting that scaling
 * wrong moves every band at once, which the band layout tests catch.
 *
 * @param mode The frame size.
 * @param band Which band.
 * @return Its first bin; the band runs to the next band's first bin.
 */
static inline uint32_t gaud_celt_band_start(
    const CELT_Mode * mode, uint32_t band) {
  return (uint32_t)mode->edges[band] << mode->lm;
}

/** Arithmetic right shift, rounding to nearest. */
static inline int32_t gaud_celt_pshr32(int32_t value, unsigned shift) {
  return (value + ((int32_t)1 << shift >> 1)) >> shift;
}

/**
 * Left shift that is defined for negative values.
 *
 * Shifting a negative left is undefined behaviour in C, not merely
 * implementation-defined the way shifting one right is, so this goes
 * through an unsigned type and back. The conversion back is
 * implementation-defined and every target here is two's complement,
 * which RFC 6716 Appendix A also states it relies on.
 */
static inline int32_t gaud_celt_shl32(int32_t value, unsigned shift) {
  return (int32_t)((uint32_t)value << shift);
}

/** The smaller of two signed values. */
static inline int32_t gaud_celt_min32(int32_t a, int32_t b) {
  return a < b ? a : b;
}

/** The larger of two signed values. */
static inline int32_t gaud_celt_max32(int32_t a, int32_t b) {
  return a > b ? a : b;
}

/**
 * @brief The per-band ceiling on allocation, section 4.3.3.
 *
 * Read from the generated cache, which is indexed by frame size and
 * channel count together - eight combinations of 21 bands.
 *
 * @param mode The frame size.
 * @param cap Receives one ceiling per band, in eighths of a bit.
 * @param channels 1 or 2.
 */
void gaud_celt_init_caps(
    const CELT_Mode * mode, int32_t * cap, uint32_t channels);

/**
 * @brief Read the per-band time-frequency resolution flags.
 *
 * Section 4.3.1. One flag per band, each toggling the resolution
 * relative to the band before, and then a select flag that chooses
 * between two rows of the change table - **read only when the two rows
 * would give different answers**, which is the easiest bit in the
 * frame to read when it is not there.
 *
 * @param range The range decoder.
 * @param mode The frame size.
 * @param start First band.
 * @param end One past the last.
 * @param transient Whether the frame set the transient flag.
 * @param tf_res Receives the resolution change for each band.
 */
void gaud_celt_tf_decode(OPUS_Range * range, const CELT_Mode * mode,
    uint32_t start, uint32_t end, bool transient, int * tf_res);

/**
 * @brief Read the band boosts, section 4.3.3's "band boost".
 *
 * One of the three transmitted adjustments to the implicit allocation.
 * Each band may be boosted repeatedly, and the probability of a boost
 * rises once any band has had one, so the cost of boosting several
 * neighbours is much less than the first.
 *
 * @param range The range decoder.
 * @param mode The frame size.
 * @param start First band.
 * @param end One past the last.
 * @param channels 1 or 2.
 * @param cap The per-band ceilings; a boost cannot exceed one.
 * @param total_bits The budget, in eighths of a bit.
 * @param offsets Receives each band's boost.
 * @return What is left of the budget.
 */
int32_t gaud_celt_decode_boosts(OPUS_Range * range, const CELT_Mode * mode,
    uint32_t start, uint32_t end, uint32_t channels, const int32_t * cap,
    int32_t total_bits, int32_t * offsets);

/**
 * @brief Work out how many bits every band gets, section 4.3.3.
 *
 * The one computation in this codec that has to be exactly right for
 * reasons that are not about quality: its result decides how many
 * symbols are read next, so an allocator one eighth of a bit out
 * desynchronises the range decoder rather than degrading a band.
 *
 * @param range The range decoder; this reads the skip, intensity and
 *   dual stereo decisions from it.
 * @param mode The frame size.
 * @param start First band.
 * @param end One past the last.
 * @param offsets The boosts.
 * @param cap The per-band ceilings.
 * @param alloc_trim The tilt, 0 to 10.
 * @param intensity Receives the first intensity-coded band.
 * @param dual_stereo Receives whether joint coding is off.
 * @param total The budget, in eighths of a bit.
 * @param out_balance Receives bits left over for the shape coder.
 * @param pulses Receives each band's shape allowance.
 * @param ebits Receives each band's fine energy bits.
 * @param fine_priority Receives which bands want leftover bits first.
 * @param channels 1 or 2.
 * @return How many bands are coded; the rest are skipped.
 */
uint32_t gaud_celt_compute_allocation(OPUS_Range * range,
    const CELT_Mode * mode, uint32_t start, uint32_t end,
    const int32_t * offsets, const int32_t * cap, int alloc_trim,
    uint32_t * intensity, bool * dual_stereo, int32_t total,
    int32_t * out_balance, int32_t * pulses, int * ebits,
    int * fine_priority, uint32_t channels);

/**
 * @brief Decode one Laplace-distributed value.
 *
 * Section 4.3.2.1 codes the coarse energy's prediction error this way.
 * The distribution is described by a starting frequency and a decay,
 * both drawn from the generated model table, and it is unbounded - past
 * the decaying part every remaining value has the same small
 * probability, which is what lets a single large error be coded at all.
 *
 * @param range The range decoder.
 * @param fs The frequency of zero, in a total of 32768.
 * @param decay How fast the tail falls, in Q15.
 * @return The value, which may be negative.
 */
int gaud_celt_laplace_decode(OPUS_Range * range, uint32_t fs, int decay);

/**
 * @brief Read the coarse energy of every band, section 4.3.2.1.
 *
 * Six decibels of resolution, predicted both from the previous frame
 * and from the previous band, with the time half disabled for an
 * "intra" frame. @p energy carries the previous frame's values in and
 * this frame's out, which is what makes the prediction work and also
 * what makes a dropped packet audible two frames later.
 *
 * @param range The range decoder.
 * @param mode The frame size.
 * @param energy Base-two log energy per band per channel, Q10.
 *   Sixteen bits, because the prediction is a 16-by-16 multiply.
 * @param start First band to read.
 * @param end One past the last.
 * @param intra Whether the frame predicts from its own history.
 * @param channels 1 or 2.
 */
void gaud_celt_decode_coarse_energy(OPUS_Range * range,
    const CELT_Mode * mode, int16_t * energy, uint32_t start, uint32_t end,
    bool intra, uint32_t channels);

/**
 * @brief Refine the coarse energy, section 4.3.2.2.
 *
 * Each band was allocated some number of bits by the allocator; this
 * reads exactly that many as raw bits and shifts the band's energy by
 * the correction they describe.
 *
 * @param range The range decoder.
 * @param mode The frame size.
 * @param energy Adjusted in place.
 * @param fine How many bits each band was given.
 * @param start First band.
 * @param end One past the last.
 * @param channels 1 or 2.
 */
void gaud_celt_decode_fine_energy(OPUS_Range * range,
    const CELT_Mode * mode, int16_t * energy, const int * fine,
    uint32_t start, uint32_t end, uint32_t channels);

/**
 * @brief Spend whatever bits are left on one more step of fine energy.
 *
 * Section 4.3.2.2's last paragraph. Bands of priority 0 get one more
 * bit per channel first, from band 0 upwards; then bands of priority 1;
 * anything still left is simply unused.
 *
 * @param range The range decoder.
 * @param mode The frame size.
 * @param energy Adjusted in place.
 * @param fine How many fine bits each band already had.
 * @param priority Each band's priority, 0 or 1.
 * @param left How many bits remain.
 * @param start First band.
 * @param end One past the last.
 * @param channels 1 or 2.
 */
void gaud_celt_decode_final_energy(OPUS_Range * range,
    const CELT_Mode * mode, int16_t * energy, const int * fine,
    const int * priority, int32_t left, uint32_t start, uint32_t end,
    uint32_t channels);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_CELT_H
