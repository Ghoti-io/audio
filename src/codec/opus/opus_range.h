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
 * Opus's range decoder: RFC 6716 section 4.1. Never installed.
 *
 * Every number in an Opus frame comes through here, so this file is read
 * more times per decoded sample than any other in the codec, and it is
 * also the only one whose correctness can be established on its own.
 * Three things about it are unusual enough to state up front.
 *
 * **One buffer is read from both ends.** The range coder consumes bytes
 * forwards from the start of the frame; CELT's "raw bits" are packed
 * backwards from the end, least significant bit of the first value in
 * the least significant bit of the last byte. The two are *expected to
 * overlap* - section 4.1.4 says a decoder MUST allow it - because the
 * range coder buffers several bytes ahead of what it has actually used.
 * So there is no bounds check separating them, and there must not be:
 * the same byte can legitimately be read by both. What takes the place
 * of a bounds check is that both ends read zero once they run off, which
 * is what section 4.1.2.1 requires rather than a convenience.
 *
 * **Running out of input is not an error.** A frame may end mid-symbol
 * and the decoder keeps going on zero bytes, by specification. That
 * sounds like a licence to read garbage, and the reason it is safe is
 * that the number of symbols is fixed by the frame's own structure
 * rather than by how much data is left, so a truncated frame decodes to
 * a definite - if wrong-sounding - answer rather than to an unbounded
 * read. ::OPUS_Range::error is for the one case the specification does
 * call an error, in ::gaud_opus_dec_uint.
 *
 * **The bit count is an upper bound, and CELT's allocator depends on
 * the exact amount by which it overestimates.** ::gaud_opus_tell_frac
 * is not instrumentation; it is an input to decisions the encoder made
 * the same way, so a cheaper approximation of it would decode to
 * different audio. Section 4.1.6 is emphatic about this and it is worth
 * believing the first time.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_RANGE_H
#define GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_RANGE_H

#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bits in one coded digit: the range coder works a byte at a time. */
#define OPUS_SYM_BITS 8u

/** Bits in the state words `val` and `rng`. */
#define OPUS_CODE_BITS 32u

/** The largest value one digit can take. */
#define OPUS_SYM_MAX ((1u << OPUS_SYM_BITS) - 1u)

/** The ceiling `val` is held below: 2**31. */
#define OPUS_CODE_TOP (1u << (OPUS_CODE_BITS - 1u))

/** Renormalisation runs until `rng` exceeds this: 2**23. */
#define OPUS_CODE_BOT (OPUS_CODE_TOP >> OPUS_SYM_BITS)

/** Bits of the first byte that initialisation consumes. */
#define OPUS_CODE_EXTRA 7u

/** Fractional bits ::gaud_opus_tell_frac reports: eighths. */
#define OPUS_BITRES 3u

/** The most bits ::gaud_opus_dec_uint will take from the range coder. */
#define OPUS_UINT_BITS 8u

/**
 * @brief One range decoder, reading one Opus frame.
 *
 * Initialise with ::gaud_opus_range_init and then read; there is nothing
 * to free. Copying one by value is well defined and is how the CELT
 * allocator tries a decision and backs out of it.
 */
typedef struct {
  const unsigned char * buf; ///< The frame. Not owned.
  uint32_t size;             ///< Its length in bytes.
  uint32_t offset;           ///< Bytes taken from the front so far.
  uint32_t end_offset;       ///< Bytes taken from the back so far.
  uint32_t val;              ///< Range minus the coded value, minus one.
  uint32_t rng;              ///< The size of the current range.
  uint32_t rem;              ///< The byte whose last bit is still owed.
  uint32_t ext;              ///< `rng/ft` from the most recent decode.
  uint32_t end_window;       ///< Raw bits read from the back, unconsumed.
  unsigned end_bits;         ///< How many of those are valid.
  uint32_t total_bits;       ///< Section 4.1.6's `nbits_total`.
  bool error;                ///< Set by a value the format cannot mean.
} OPUS_Range;

/**
 * @brief Number of bits needed to hold @p value, and 0 for zero.
 *
 * Section 4.1.5's `ilog`, which is one more than the index of the
 * highest set bit.
 */
unsigned gaud_opus_ilog(uint32_t value);

/**
 * @brief Point a decoder at one frame and read its first byte.
 *
 * @param range The decoder.
 * @param data The frame's bytes, which must outlive @p range.
 * @param size How many there are; zero is allowed and decodes as zeros.
 */
void gaud_opus_range_init(
    OPUS_Range * range, const unsigned char * data, size_t size);

/**
 * @brief Step one, section 4.1.2: the frequency the coded value lies in.
 *
 * Must be followed by ::gaud_opus_dec_update with the symbol's own
 * three-tuple before anything else is read.
 *
 * @param range The decoder.
 * @param ft The context's total frequency.
 * @return A value in `[0, ft)`.
 */
uint32_t gaud_opus_decode(OPUS_Range * range, uint32_t ft);

/**
 * @brief ::gaud_opus_decode where @p ftb says `ft` is `1 << ftb`.
 *
 * Avoids a division and is otherwise identical.
 *
 * @param range The decoder.
 * @param ftb The base-two logarithm of the total frequency.
 * @return A value in `[0, 1 << ftb)`.
 */
uint32_t gaud_opus_decode_bin(OPUS_Range * range, unsigned ftb);

/**
 * @brief Step two, section 4.1.2: consume the symbol just identified.
 *
 * @param range The decoder.
 * @param fl The symbol's cumulative frequency.
 * @param fh That plus the symbol's own frequency.
 * @param ft The context's total frequency.
 */
void gaud_opus_dec_update(
    OPUS_Range * range, uint32_t fl, uint32_t fh, uint32_t ft);

/**
 * @brief Decode one bit whose probability of being set is `2**-logp`.
 *
 * Section 4.1.3.2, and the one entry point that needs neither a
 * multiplication nor a division.
 *
 * @param range The decoder.
 * @param logp The negated base-two logarithm of the probability.
 * @return 0 or 1.
 */
int gaud_opus_dec_bit_logp(OPUS_Range * range, unsigned logp);

/**
 * @brief Decode one symbol from an inverse cumulative table.
 *
 * Section 4.1.3.3. @p icdf holds `(1 << ftb) - fh[k]` for each symbol
 * and is terminated by a zero, so the search and the update are one
 * loop. This is how almost every symbol in SILK is read.
 *
 * @param range The decoder.
 * @param icdf The table, zero-terminated.
 * @param ftb The base-two logarithm of the total frequency.
 * @return The symbol's index.
 */
int gaud_opus_dec_icdf(
    OPUS_Range * range, const unsigned char * icdf, unsigned ftb);

/**
 * @brief Read @p bits raw bits from the back of the frame.
 *
 * Section 4.1.4. These bypass the range coder entirely and may overlap
 * the bytes it has buffered.
 *
 * @param range The decoder.
 * @param bits How many, at most 25.
 * @return Them, the first bit read in the least significant position.
 */
uint32_t gaud_opus_dec_bits(OPUS_Range * range, unsigned bits);

/**
 * @brief Decode one of @p ft equally likely values.
 *
 * Section 4.1.5. Above eight bits this is a coded symbol for the top
 * eight and raw bits for the rest, which is the one place the format
 * admits a decoder may see a value it cannot mean; that sets
 * ::OPUS_Range::error and saturates.
 *
 * @param range The decoder.
 * @param ft How many values, at least two.
 * @return A value in `[0, ft)`.
 */
uint32_t gaud_opus_dec_uint(OPUS_Range * range, uint32_t ft);

/**
 * @brief An upper bound on the whole bits used so far.
 *
 * Section 4.1.6.1. Reports 1 for a decoder that has read nothing, which
 * is the bit the encoder reserves to terminate the stream.
 *
 * @param range The decoder.
 * @return The bound.
 */
uint32_t gaud_opus_tell(const OPUS_Range * range);

/**
 * @brief The same bound to eighths of a bit.
 *
 * Section 4.1.6.2. ::gaud_opus_tell is `ceil` of this over eight, and
 * CELT's bit allocation reads this one.
 *
 * @param range The decoder.
 * @return The bound, in eighths of a bit.
 */
uint32_t gaud_opus_tell_frac(const OPUS_Range * range);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_OPUS_OPUS_RANGE_H
