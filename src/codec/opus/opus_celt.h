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

/** The most pulses one band can be given; RFC 6716's MAX_PULSES. */
#define CELT_MAX_PULSES 128

/**
 * @brief `V(N,K)`, and a row of the `U` it is built from.
 *
 * On return @p u holds `U(n,i)` for every `i` up to `k+1`, which is what
 * ::gaud_celt_decode_pulses walks. Needs @p u to have at least `k+2`
 * entries, and @p n of at least two.
 *
 * @param n Dimensions.
 * @param k Pulses.
 * @param u Receives the row.
 * @return `V(n,k)`: how many vectors of @p n integers sum in absolute
 *   value to @p k.
 */
uint32_t gaud_celt_pvq_urow(unsigned n, unsigned k, uint32_t * u);

/**
 * @brief `V(N,K)` on its own, including the degenerate cases.
 *
 * @param n Dimensions.
 * @param k Pulses.
 * @return `V(n,k)`; 1 when @p k is zero and 0 when @p n is zero.
 */
uint32_t gaud_celt_pvq_v(unsigned n, unsigned k);

/**
 * @brief Turn one codebook index back into its pulse vector.
 *
 * Section 4.3.4.2's enumeration, separated from the reading of the
 * index so that it can be driven directly - which is how it is checked
 * against the reference implementation, over every index the
 * conformance vectors actually produce.
 *
 * @param y Receives the vector; @p n entries.
 * @param n Dimensions, at least two.
 * @param k Pulses, at least one and at most ::CELT_MAX_PULSES.
 * @param index Below `V(n,k)`.
 */
void gaud_celt_pulses_from_index(
    int * y, unsigned n, unsigned k, uint32_t index);

/**
 * @brief Read one band's pulse vector, section 4.3.4.2.
 *
 * Reads a single uniformly distributed index and turns it back into the
 * vector of @p n signed integers whose absolute values sum to @p k.
 *
 * @param range The range decoder.
 * @param y Receives the vector; @p n entries.
 * @param n Dimensions, at least two.
 * @param k Pulses, at least one and at most ::CELT_MAX_PULSES.
 */
void gaud_celt_decode_pulses(
    OPUS_Range * range, int * y, unsigned n, unsigned k);

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

/** The widest band, in frame bins: 22 bins of 2.5 ms at `lm = 3`. */
#define CELT_MAX_BAND_BINS 176

/** Spread: no rotation at all. */
#define CELT_SPREAD_NONE 0

/** Spread: the weakest rotation, `f_r` of 15. */
#define CELT_SPREAD_LIGHT 1

/** Spread: `f_r` of 10, and the value a decoder assumes when none is sent. */
#define CELT_SPREAD_NORMAL 2

/** Spread: the strongest rotation, `f_r` of 5. */
#define CELT_SPREAD_AGGRESSIVE 3

/**
 * @brief Scale a band back to unit norm.
 *
 * Section 4.3.4.2 leaves the shape vector with whatever length the
 * pulses gave it; this is what makes it a direction. Used on its own
 * after folding and after a stereo rotation, where the vector being
 * normalised did not come from ::gaud_celt_alg_unquant.
 *
 * **The energy accumulator is 32 bits and that is a real ceiling.** A
 * band of @p n entries at full scale sums to `n << 28`, which overflows
 * past @p n of eight. CELT never gets near it - every vector reaching
 * here is about unit norm, so the sum is about `2^28` whatever @p n is,
 * and even an intensity-stereo sum of two of them stays inside - but it
 * is a property of the input rather than of this code, and RFC 6716
 * accumulates in 32 bits too. A sweep of full-scale bands finds it
 * immediately, which is how it came to be written down.
 *
 * @param x The band, @p n entries, adjusted in place.
 * @param n How many bins; at least one.
 * @param gain The length to give it, in Q15.
 */
void gaud_celt_renormalise_vector(int16_t * x, uint32_t n, int16_t gain);

/**
 * @brief Decode one band's shape: the pulses, normalised and rotated.
 *
 * Sections 4.3.4.2 and 4.3.4.3 together, which is how the reference
 * packages them. Reads one codebook index, turns it into the pulse
 * vector, scales that to @p gain, and then rotates it to spread the
 * pulses out - because a handful of pulses in a wide band is a comb
 * filter, and the rotation turns it back into noise.
 *
 * The rotation is by `pi*g_r*g_r/4` where `g_r = N/(N + f_r*K)`, with
 * `f_r` from @p spread; a band of 8 bins or more per block is rotated
 * twice, the first time by the complementary angle and at a stride of
 * `round(sqrt(N/B))`. All of that is section 4.3.4.3's prose, which for
 * once states the arithmetic completely.
 *
 * @param x Receives the band, @p n entries.
 * @param n How many bins; at least two, at most ::CELT_MAX_BAND_BINS.
 * @param k How many pulses; at least one.
 * @param spread One of the four ::CELT_SPREAD_NONE values.
 * @param blocks How many time blocks the band spans.
 * @param range The range decoder.
 * @param gain The length to give the result, in Q15.
 * @return The collapse mask: a set bit per time block that got a pulse.
 *   Section 4.3.5 needs it to decide which blocks were emptied.
 */
unsigned gaud_celt_alg_unquant(int16_t * x, uint32_t n, uint32_t k,
    unsigned spread, uint32_t blocks, OPUS_Range * range, int16_t gain);

/** Steps in the binary search over the pulse cache: 2**6 is 64 codes. */
#define CELT_LOG_MAX_PSEUDO 6

/**
 * @brief The cache row for one band at one frame size.
 *
 * Entry zero is how many pulse counts the row holds; entry `q` is the
 * cost in eighths of a bit, less one, of coding `q` pseudo-pulses in
 * that band. The table is generated from the appendix rather than
 * computed, because what it holds is the *reference's* rounding of a
 * logarithm and not the logarithm.
 *
 * **The row is chosen by @p lm, not by `mode->lm`.** Section 4.3.4.4's
 * recursion decrements the frame size at every split, and each level
 * must read the row for the size it is actually coding - a band split
 * once at the shortest frame reads row zero, which is the row for the
 * `-1` that nothing else ever asks for. Reading `mode->lm` here instead
 * gives the right answer at the top level and the wrong one below it.
 *
 * **Eight (frame size, band) pairs have no row at all**, and the index
 * table marks them with -1: the bands that are one bin wide at the
 * shortest frame cannot be split, so nothing ever asks what a split of
 * one of them would cost. RFC 6716 adds that -1 to the base pointer
 * and relies on never dereferencing the result; this returns NULL
 * instead, which is the same thing without forming a pointer outside
 * the array. A caller that would have read garbage now crashes, and
 * ::gaud_celt_quant_all_bands is shown by test never to get there.
 *
 * @param lm The frame size at this level of the recursion, -1 upwards.
 * @param band Which band.
 * @return Its row, or NULL where there is none.
 */
const unsigned char * gaud_celt_pulse_cache(int lm, uint32_t band);

/**
 * @brief Turn a pseudo-pulse count into the real one.
 *
 * Above seven the counts are spaced logarithmically, eight to a octave,
 * so that one byte of cache covers up to ::CELT_MAX_PULSES.
 *
 * @param index The pseudo-pulse count.
 * @return How many pulses that is.
 */
static inline int gaud_celt_get_pulses(int index) {
  return index < 8 ? index : (8 + (index & 7)) << ((index >> 3) - 1);
}

/**
 * @brief The pseudo-pulse count whose cost is closest to @p bits.
 *
 * A binary search of fixed depth over the cache row, then one
 * comparison to pick whichever neighbour is nearer. Fixed depth rather
 * than a loop with a test so that it runs in constant time, which the
 * reference wanted for a different reason than this library does.
 *
 * @param lm The frame size at this level of the recursion.
 * @param band Which band.
 * @param bits The budget, in eighths of a bit.
 * @return The pseudo-pulse count.
 */
int gaud_celt_bits_to_pulses(int lm, uint32_t band, int32_t bits);

/**
 * @brief What @p pulses pseudo-pulses actually cost.
 *
 * @param lm The frame size at this level of the recursion.
 * @param band Which band.
 * @param pulses The pseudo-pulse count.
 * @return Its cost in eighths of a bit; zero for zero.
 */
int32_t gaud_celt_pulses_to_bits(int lm, uint32_t band, int pulses);

/**
 * @brief How much scratch ::gaud_celt_quant_all_bands needs, in entries.
 *
 * The normalised spectrum for each channel - which later bands fold
 * from, so it has to outlive the band that produced it - then two
 * band-sized areas: one for the Hadamard reorderings and one for the
 * copy of a fold source that is about to be transformed.
 *
 * @param mode The frame size.
 * @param channels 1 or 2.
 * @return How many `int16_t` the scratch must hold.
 */
static inline uint32_t gaud_celt_quant_scratch(
    const CELT_Mode * mode, uint32_t channels) {
  return channels * ((1u << mode->lm) * (uint32_t)mode->edges[CELT_BANDS])
      + 2u * CELT_MAX_BAND_BINS;
}

/** How far the split's resolution is biased, in eighths of a bit. */
#define CELT_QTHETA_OFFSET 4

/** The same, for the two-phase stereo case where N is 2. */
#define CELT_QTHETA_OFFSET_TWOPHASE 16

/**
 * @brief The folding generator, RFC 6716's `celt_lcg_rand`.
 *
 * A band with no pulses is filled either with a copy of a lower band or,
 * when there is no lower band to copy, with noise from this. Both ends
 * run the same sequence from the same seed, so the "noise" is agreed on.
 *
 * @param seed The state.
 * @return The next state.
 */
static inline uint32_t gaud_celt_lcg_rand(uint32_t seed) {
  return 1664525u * seed + 1013904223u;
}

/**
 * @brief Decode every band's shape, section 4.3.4 entire.
 *
 * The recursion of section 4.3.4.4 on top of the single-band coder:
 * each band is split until its codebook fits in 32 bits, with a gain
 * parameter coded at each split saying how the energy divides, and
 * stereo handled by the same mechanism with the two channels as the two
 * halves. Around that sits the time-frequency resolution change of
 * section 4.3.4.5, applied as Hadamard rotations before the split and
 * undone after it, and the folding that fills bands the allocator gave
 * nothing to.
 *
 * @param range The range decoder.
 * @param mode The frame size.
 * @param start First band to decode.
 * @param end One past the last.
 * @param x Receives the first channel, @p mode->size entries.
 * @param y Receives the second, or NULL for mono.
 * @param collapse Receives one mask per band per channel, for §4.3.5.
 * @param pulses The allocation, in eighths of a bit per band.
 * @param short_blocks Whether the frame is transient.
 * @param spread One of the ::CELT_SPREAD_NONE values.
 * @param dual_stereo Whether the channels are coded separately.
 * @param intensity The first band coded as intensity stereo.
 * @param tf_res One resolution flag per band.
 * @param total_bits The frame's budget in eighths of a bit.
 * @param balance Bits carried in from the fine-energy split.
 * @param coded_bands How many bands the allocator decided to code.
 * @param seed The folding generator's state, advanced in place.
 * @param scratch Working space, ::gaud_celt_quant_scratch entries.
 */
void gaud_celt_quant_all_bands(OPUS_Range * range, const CELT_Mode * mode,
    uint32_t start, uint32_t end, int16_t * x, int16_t * y,
    unsigned char * collapse, const int32_t * pulses, bool short_blocks,
    unsigned spread, bool dual_stereo, uint32_t intensity, const int * tf_res,
    int32_t total_bits, int32_t balance, uint32_t coded_bands, uint32_t * seed,
    int16_t * scratch);

/**
 * @brief Refill the time blocks that ended up with no energy at all.
 *
 * Section 4.3.5. A transient frame divides each band into time blocks,
 * and a block the allocator could not afford to code comes out of
 * section 4.3.4 as silence. Silence between two loud blocks is heard as
 * a gap - the artifact the whole transient machinery exists to avoid -
 * so a collapsed block is filled with noise instead, at a level derived
 * from how far this frame's energy has risen above the two before it.
 *
 * The collapse masks ::gaud_celt_quant_all_bands produced are what says
 * which blocks those are. Both ends run the same generator from the
 * same seed, so the noise is agreed.
 *
 * @param mode The frame size.
 * @param x The spectrum, adjusted in place.
 * @param collapse One mask per band per channel.
 * @param channels 1 or 2.
 * @param size Entries per channel in @p x.
 * @param start First band.
 * @param end One past the last.
 * @param energy This frame's band energies, in Q(::CELT_DB_SHIFT);
 *   `2 * ::CELT_BANDS` entries whatever @p channels is, because a mono
 *   frame still reads the second channel's history - the stream may
 *   have been stereo a frame ago.
 * @param previous1 The frame before's, same shape.
 * @param previous2 The one before that, same shape.
 * @param pulses The allocation, in eighths of a bit per band.
 * @param seed The folding generator's state; not advanced for the
 *   caller, because the reference passes it by value here.
 */
void gaud_celt_anti_collapse(const CELT_Mode * mode, int16_t * x,
    const unsigned char * collapse, uint32_t channels, uint32_t size,
    uint32_t start, uint32_t end, const int16_t * energy,
    const int16_t * previous1, const int16_t * previous2,
    const int32_t * pulses, uint32_t seed);

/**
 * @brief Turn the band energies back into linear amplitudes.
 *
 * RFC 6716's `log2Amp`. The envelope is coded as a base-two logarithm
 * relative to a per-band mean, so this adds the mean back and raises
 * two to it. Bands outside `[start, end)` are zeroed rather than left
 * alone, because the caller may be reusing the array.
 *
 * @param amplitude Receives one per band per channel.
 * @param energy The decoded envelope, in Q(::CELT_DB_SHIFT).
 * @param start First band.
 * @param end One past the last.
 * @param channels 1 or 2.
 */
void gaud_celt_log2_amp(int32_t * amplitude, const int16_t * energy,
    uint32_t start, uint32_t end, uint32_t channels);

/**
 * @brief Give each band back the energy the envelope says it has.
 *
 * Section 4.3.6, the inverse of the normalisation the encoder did: the
 * shape vectors are unit-norm, so this is a multiply per bin. Bins
 * above @p end are zeroed, which is what makes a band-limited frame
 * band-limited.
 *
 * @param mode The frame size.
 * @param x The normalised spectrum, @p channels by @p mode->size.
 * @param out Receives the result, same shape.
 * @param amplitude One per band per channel, from ::gaud_celt_log2_amp.
 * @param end One past the last coded band.
 * @param channels 1 or 2.
 */
void gaud_celt_denormalise_bands(const CELT_Mode * mode, const int16_t * x,
    int32_t * out, const int32_t * amplitude, uint32_t end,
    uint32_t channels);

/**
 * @brief The inverse MDCT, section 4.3.7.
 *
 * An inverse MDCT of `1920 >> shift` points: a rotation, a complex
 * inverse FFT of a quarter the size, another rotation, and then the
 * windowing that makes consecutive frames add back to the signal.
 *
 * **It adds into the first `overlap` samples rather than writing
 * them.** That is the time-domain alias cancellation: the tail the
 * previous frame left there is half of what the output should be, and
 * this supplies the other half. A caller that clears the buffer first
 * gets a frame of windowed nonsense.
 *
 * @param in The spectrum, `1920 >> shift >> 1` values at @p stride.
 * @param out Receives the samples. Writing starts
 *   `((N/2) - overlap) / 2` entries *before* this pointer, where N is
 *   `1920 >> shift`, so the caller's buffer must have that much room
 *   behind it.
 * @param window The overlap window in Q15, @p overlap entries.
 * @param overlap How many samples the frames share; 120 at 48 kHz.
 * @param shift 0 for a 20 ms frame through 3 for 2.5 ms.
 * @param stride How far apart @p in's values are; the short MDCTs of a
 *   transient frame are interleaved in one array.
 */
void gaud_celt_imdct(const int32_t * in, int32_t * out, const int16_t * window,
    uint32_t overlap, int shift, uint32_t stride);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_CELT_H
