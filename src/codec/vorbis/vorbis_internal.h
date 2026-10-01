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
 * Vorbis I: what the headers state, and what a document concludes from
 * them. Never installed.
 *
 * Three things about Vorbis are different from every format already in
 * this library, and all three are visible here.
 *
 * **Vorbis has no frames.** It has packets, and a packet's decoded length
 * depends on the packet *before* it: a block of N samples overlaps the
 * previous block by half, so the samples a packet contributes are
 * (previous_block + this_block) / 4 and the first packet of a stream
 * contributes nothing at all. There is no field anywhere that says how
 * long the stream is. The only answer is the last page's granule
 * position, which is why gaud_ogg_last_granule() had to exist before this
 * file could.
 *
 * **The bit packing runs the other way.** FLAC and MPEG read bits from the
 * most significant end of each byte; Vorbis reads from the least
 * significant end, and a multi-bit field's first bit read is its *lowest*.
 * A reader that got this backwards reads the identification header
 * perfectly - it is byte-aligned apart from two nibbles - and then reads
 * the codebooks as noise.
 *
 * **Almost everything is in the setup header.** The codebooks, the floor
 * curves, the residue layout, the channel coupling and the block modes are
 * all defined per stream rather than by the specification, so there are no
 * tables in this codec to extract from a document the way phase 5's were
 * (planning/audio.md section 11.20). What a standard would have fixed, a
 * Vorbis stream states.
 *
 * This file is the identification half: the three headers, the tags, and
 * the length. See planning/audio.md section 11.18 for why that is a commit
 * of its own.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_VORBIS_VORBIS_INTERNAL_H
#define GHOTI_IO_GAUD_SRC_CODEC_VORBIS_VORBIS_INTERNAL_H

#include "../../container/ogg/ogg.h"
#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The common part of every Vorbis header packet: a type byte, then this. */
#define VORBIS_SIGNATURE "vorbis"

/** Bytes in that common part, type byte included. */
#define VORBIS_HEAD_SIZE 7u

/** Header packet types. Odd, so that no header can be mistaken for audio:
 * an audio packet's first bit is zero and a header's is one. */
enum {
  VORBIS_PACKET_IDENTIFICATION = 1,
  VORBIS_PACKET_COMMENT = 3,
  VORBIS_PACKET_SETUP = 5
};

/** Bytes of the identification header, signature included. */
#define VORBIS_IDENTIFICATION_SIZE 30u

/** The smallest block size the format allows. */
#define VORBIS_MIN_BLOCKSIZE 64u

/** The largest block size the format allows. */
#define VORBIS_MAX_BLOCKSIZE 8192u

/* --------------------------------------------------------------- numerics */

/**
 * Fractional bits in a codebook's vector values and in a residue vector.
 *
 * **Sixteen, and the number is measured.** A codebook's values come from
 * the specification's `float32_unpack`: a 21-bit mantissa and a power of
 * two whose exponent spans 2^-788 to 2^235. No fixed point holds that
 * range and no real stream uses any part of it, so what real streams
 * actually contain is what decides this, and `make vorbis-coverage`
 * reports it over the corpus.
 *
 * What it reports, over 363 codebooks from two encoders:
 *
 *   - **Every value is an integer.** The smallest nonzero magnitude in
 *     the whole corpus is exactly 1.0, which is not a coincidence: a
 *     residue codebook quantises the spectrum to integers and the floor
 *     supplies the scale, so the encoders choose `delta_value` of 1 and
 *     `minimum_value` of -(lattice-1)/2.
 *   - **The largest magnitude is 7,448**, which needs 13 integer bits.
 *
 * Q16 leaves ±32,768, which is 4.4 times the measured extreme, and 16
 * fractional bits for the streams that do use a fractional delta - which
 * the format permits and neither encoder here produces. The first draft
 * was Q20 and **saturated on two of the ten fixtures**, which is how the
 * 7,448 came to be measured rather than assumed.
 *
 * Values outside the range saturate rather than wrap, as the MPEG
 * decoder's do and for the same reason: planning/audio.md section 11.1
 * promises the same bytes everywhere, and signed overflow is undefined
 * rather than wrapping.
 */
#define VORBIS_Q 16

/** One in Q#VORBIS_Q. */
#define VORBIS_ONE (1 << VORBIS_Q)

/**
 * Fractional bits in a spectral line, after the floor has been applied.
 *
 * **Twelve more than the residue's, and the reason is the floor.** A
 * residue value is an integer up to a few thousand; the floor it is
 * multiplied by is an attenuation down to 1e-07. So a typical spectral
 * line is around 1e-05, which in the residue's own Q16 would be the
 * number 0.65 - rounding to one or to zero, a hundred per cent either
 * way. Q28 holds it to 3.7e-09, which is four orders of magnitude below
 * the 16-bit output's least significant bit.
 *
 * What Q28 gives up is range: ±8. A line cannot exceed the signal's own
 * amplitude by much, so that is ample, and the product saturates rather
 * than wrapping for the arithmetically possible but physically absurd
 * case of a full-scale floor under a residue of several thousand.
 */
#define VORBIS_SPECTRUM_Q 28

/**
 * Fractional bits in a time-domain sample, before it becomes 16-bit PCM.
 *
 * The inverse transform's own output scale depends on the block size -
 * it halves its state every second stage, so a bigger block loses more -
 * and the overlap-add adds the right half of one block to the left half
 * of the next, which may be a different size. So the transform's output
 * is normalised to this one scale before anything is added to anything,
 * and 24 is chosen so that the normalisation is a right shift for the
 * largest block size and a left shift of at most two for the smallest.
 */
#define VORBIS_TIME_Q 24

/**
 * The factor the inverse transform's own definition leaves out, in bits.
 *
 * **One bit, which is to say two, and it is measured rather than
 * derived.** The specification defines the inverse transform and says
 * that windowing its output and overlapping it with the next block
 * reconstructs the signal - but it never gives the forward transform, so
 * the scale between the two is fixed by what encoders do rather than by
 * the document. Decoded without it, every fixture comes out at exactly
 * half amplitude: sample for sample, against libsndfile, the ratio is
 * 0.500015 over 8,818 samples, which is a clean factor of two and a
 * little rounding rather than a window applied wrongly - that would vary
 * across a block instead of scaling it.
 *
 * It is a constant and not a function of the block size, because the
 * block size's own contribution is already in
 * gaud_vorbis_imdct_shift(), which was checked against the
 * specification's formula at all eight sizes.
 *
 * **Zero, as it turns out.** The factor of two the first measurement
 * showed was this library's own: `to_s16()` had an extra halving in it.
 * The constant stays, with the measurement that settled it, because
 * "the inverse transform's scale is fixed by the encoder rather than by
 * the document" is still true and the next person to change
 * ::VORBIS_TIME_Q will want to know where to look.
 */
#define VORBIS_TDAC_GAIN_BITS 0

/* ------------------------------------------------------------ bit reader */

/**
 * @brief Vorbis's bit packing: least significant bit of each byte first.
 *
 * The opposite of FLAC's and MPEG's, which is the single most
 * consequential difference between reading this format and reading the
 * ones already here. See vorbis_bits.c.
 */
typedef struct {
  const unsigned char * data; ///< The packet. Borrowed.
  size_t size;                ///< Its length.
  size_t at;                  ///< The byte being read.
  unsigned bit;               ///< Bits consumed of it, 0 to 7.
  /**
   * Whether a read ran off the end of the packet.
   *
   * Not an error by itself: the specification makes a truncated packet at
   * the end of a stream a legitimate end of decode. A read past the end
   * yields zeros and sets this, and the caller decides what it means.
   */
  bool past_end;
} VORBIS_Bits;

/** @brief Start reading @p data as a Vorbis packet. */
void gaud_vorbis_bits_init(
    VORBIS_Bits * bits, const unsigned char * data, size_t size);

/** @brief Read @p width bits, first bit read being the least significant. */
uint32_t gaud_vorbis_bits_read(VORBIS_Bits * bits, unsigned width);

/** @brief How many bits have been consumed. */
uint64_t gaud_vorbis_bits_used(const VORBIS_Bits * bits);

/**
 * @brief How many bits @p value occupies; zero for zero.
 *
 * The specification's `ilog`, and **not** a base-2 logarithm: the two
 * differ by one at every power of two, which is where a reader that used
 * a logarithm gets some files right and some wrong.
 */
unsigned gaud_vorbis_ilog(uint32_t value);

/** @brief The greatest r with r to the @p dimensions not above @p entries. */
uint32_t gaud_vorbis_lookup1_values(uint32_t entries, uint32_t dimensions);

/* -------------------------------------------------------------- codebooks */

/** The largest codebook dimension count the format can state. */
#define VORBIS_MAX_DIMENSIONS 65535u

/** The largest number of entries a codebook may have. */
#define VORBIS_MAX_ENTRIES 0xFFFFFFu

/**
 * @brief One codebook: a Huffman code, and optionally a vector per entry.
 *
 * **A codebook is two things at once and the second one is optional.**
 * Every codebook is a prefix code mapping bits to an entry number. A
 * codebook with a lookup also maps that entry number to a vector of
 * `dimensions` values, and residue decoding uses the vector while
 * classification and floor decoding use the number alone. A codebook with
 * no lookup used where a vector is wanted is a stream this refuses.
 */
typedef struct {
  uint32_t dimensions;  ///< Values per entry, for a codebook with a lookup.
  uint32_t entries;     ///< How many entries.
  unsigned lookup_type; ///< 0 none, 1 lattice, 2 explicit.
  bool sequence_p;      ///< Whether a vector's values accumulate.
  /* The rest is derived rather than read; see each field. */

  /**
   * The code lengths, one per entry, zero where the entry is unused.
   *
   * Kept as well as the decode table below because the two are checked
   * against each other: a code built from lengths that do not form a
   * prefix code is a stream this refuses, and the only way to say so is
   * to have built it.
   */
  unsigned char * lengths; ///< Owned.

  /**
   * The codewords, one per entry, assigned by the specification's rule.
   *
   * **Assigned rather than read.** A Vorbis codebook states only the
   * lengths; the codewords follow from them by a procedure that fills the
   * code tree from the shortest length upwards. So building the code is
   * where an underpopulated or overpopulated tree is detected, and a
   * decoder that skipped the check would read a well-formed stream
   * correctly and a malformed one into whatever entry it reached.
   */
  uint32_t * codewords; ///< Owned, one per used entry.
  uint32_t * entry_of;  ///< Owned, the entry each codeword stands for.
  uint32_t used;        ///< How many entries have a codeword.


  /** The vector values, `entries * dimensions` of them in Q#VORBIS_Q. */
  int32_t * values; ///< Owned; NULL when lookup_type is 0.

  /**
   * The decode tree: two `int32_t` per node, a child index each.
   *
   * Zero is an empty slot, a positive value is the index of an interior
   * node, and a negative value is a leaf holding `-(entry) - 1` - the
   * offset being what distinguishes entry zero from an empty slot. A
   * tree rather than a flat table because a codeword may be 32 bits
   * long, and walking it one bit at a time is also exactly the order the
   * bit reader hands bits over in.
   */
  int32_t * tree;    ///< Owned.
  size_t tree_nodes; ///< How many nodes it holds.
} VORBIS_Codebook;

/* ----------------------------------------------------------------- floors */

/** The most partitions a floor 1 may have. */
#define VORBIS_FLOOR1_PARTITIONS 32u

/** The most classes a floor 1 may name. */
#define VORBIS_FLOOR1_CLASSES 16u

/** The most X positions a floor 1 may state, the two implied included. */
#define VORBIS_FLOOR1_VALUES 65u

/** @brief Floor type 1: a piecewise linear curve in the log domain. */
typedef struct {
  uint32_t partitions; ///< How many partitions the curve is stated in.
  /** Which class each partition belongs to. */
  unsigned char partition_class[VORBIS_FLOOR1_PARTITIONS];
  /** How many X positions a partition of each class contributes. */
  unsigned char class_dimensions[VORBIS_FLOOR1_CLASSES];
  /** Log2 of how many subclasses each class has, 0 to 3. */
  unsigned char class_subclasses[VORBIS_FLOOR1_CLASSES];
  /** The book that codes which subclass, where a class has more than one. */
  unsigned char class_masterbook[VORBIS_FLOOR1_CLASSES];
  /** The book for each subclass, or -1 where the subclass codes nothing. */
  int16_t subclass_book[VORBIS_FLOOR1_CLASSES][8];
  uint32_t multiplier; ///< 1 to 4; scales the decoded Y values.
  uint32_t values;     ///< How many X positions, the two implied included.
  /** The X positions, in the order the stream states them. */
  uint32_t x_list[VORBIS_FLOOR1_VALUES];
  /**
   * The X positions in increasing order, as indices into x_list.
   *
   * Precomputed because rendering walks the curve in X order while
   * decoding fills it in the order the stream states, and sorting per
   * packet would be the decoder's inner loop. A repeated X position is a
   * stream this refuses: two lines at one X have no slope between them.
   */
  uint32_t sorted[VORBIS_FLOOR1_VALUES];
} VORBIS_Floor1;

/** @brief Floor type 0: a line spectral pair curve. */
typedef struct {
  uint32_t order;            ///< How many line spectral pairs.
  uint32_t rate;             ///< The curve's own notional sample rate.
  uint32_t bark_map_size;    ///< Width of the Bark-scale map.
  uint32_t amplitude_bits;   ///< Field width of the per-packet amplitude.
  uint32_t amplitude_offset; ///< Subtracted from that amplitude.
  uint32_t book_count;       ///< How many books the coefficients may use.
  unsigned char books[16];   ///< Which ones.
} VORBIS_Floor0;

/** @brief One floor configuration, of either type. */
typedef struct {
  unsigned type; ///< 0 or 1.
  /** Whichever of the two the type selects. */
  union {
    VORBIS_Floor0 zero; ///< When @p type is 0.
    VORBIS_Floor1 one;  ///< When @p type is 1.
  } u;
} VORBIS_Floor;

/* --------------------------------------------------------------- residues */

/** The most classifications a residue may have. */
#define VORBIS_RESIDUE_CLASSES 64u

/** @brief One residue configuration; types 0, 1 and 2 share this shape. */
typedef struct {
  unsigned type;            ///< 0, 1 or 2.
  uint32_t begin;           ///< First spectral line this residue codes.
  uint32_t end;             ///< One past the last.
  uint32_t partition_size;  ///< Lines per partition.
  uint32_t classifications; ///< How many classes a partition may be.
  unsigned char classbook;  ///< The book that codes a partition's class.
  /** The book for each (classification, pass), or -1 for none. */
  int16_t book[VORBIS_RESIDUE_CLASSES][8];
  /** How many passes any classification codes, which bounds the loop. */
  unsigned passes;
} VORBIS_Residue;

/* --------------------------------------------------------------- mappings */

/** The most coupling steps a mapping may state. */
#define VORBIS_MAX_COUPLING 256u

/** The most submaps a mapping may have. */
#define VORBIS_MAX_SUBMAPS 16u

/** @brief One mapping: which floor and residue each channel uses. */
typedef struct {
  uint32_t submaps;        ///< How many floor and residue pairs.
  uint32_t coupling_steps; ///< How many channel pairs are polar-coupled.
  /** The channel carrying the magnitude of each coupling step. */
  unsigned char magnitude[VORBIS_MAX_COUPLING];
  /** The channel carrying the angle of each coupling step. */
  unsigned char angle[VORBIS_MAX_COUPLING];
  unsigned char * mux;  ///< Owned, one per channel: which submap.
  /** The floor each submap uses. */
  unsigned char floor[VORBIS_MAX_SUBMAPS];
  /** The residue each submap uses. */
  unsigned char residue[VORBIS_MAX_SUBMAPS];
} VORBIS_Mapping;

/** The most modes a stream may define. */
#define VORBIS_MAX_MODES 64u

/** @brief One mode: which block size and which mapping a packet uses. */
typedef struct {
  bool block_flag;       ///< False for the short block, true for the long.
  unsigned char mapping; ///< Which mapping a packet in this mode uses.
} VORBIS_Mode;

/**
 * @brief Everything the setup header defines, which is most of the format.
 *
 * **Nothing in here is in the specification as a table.** The codebooks,
 * the floor curves, the residue layout, the channel coupling and the
 * block modes are stated per stream, so this codec has no tables to
 * extract from a document the way phase 5's were - what a standard would
 * have fixed, a Vorbis stream states. The consequence for testing is that
 * every one of these structures is a thing an encoder chose, so a corpus
 * samples encoders rather than the format even more sharply than a
 * corpus of MP3s does.
 */
typedef struct {
  /**
   * Everything here comes from it.
   *
   * **NULL is a value and not an absence**: it means the default
   * allocator, which is what gaud_stream_allocator() returns for a
   * stream opened without one. gaud_vorbis_setup_free() must not treat
   * it as "nothing was allocated", and its first draft did - see the
   * comment there for what that cost.
   */
  const GAUD_Allocator * allocator;
  uint32_t codebook_count;     ///< How many codebooks the stream defines.
  VORBIS_Codebook * codebooks; ///< Owned.
  uint32_t floor_count;        ///< How many floor configurations.
  VORBIS_Floor * floors;       ///< Owned.
  uint32_t residue_count;      ///< How many residue configurations.
  VORBIS_Residue * residues;   ///< Owned.
  uint32_t mapping_count;      ///< How many mappings.
  VORBIS_Mapping * mappings;   ///< Owned.
  uint32_t mode_count;         ///< How many modes.
  /** The modes, at most ::VORBIS_MAX_MODES of them. */
  VORBIS_Mode modes[VORBIS_MAX_MODES];
  /** Bits a packet spends on its mode number: ilog(mode_count - 1). */
  unsigned mode_bits;
} VORBIS_Setup;

/* --------------------------------------------------------- the audio path */

/** @brief What one channel's floor came out as for one packet. */
typedef struct {
  bool used; ///< Whether this channel carries anything in this packet.
  /**
   * The rendered curve, one entry per spectral line, as an index into
   * ::gaud_vorbis_floor_db.
   *
   * **An index and not a value.** Floor 1's whole output is one of 256
   * numbers, so carrying the index costs a byte per line where a
   * fixed-point value costs four and loses precision at the quiet end -
   * the table spans seven decades and no single scale holds both ends.
   * The multiply that applies it reads the table's mantissa and shift.
   */
  unsigned char * curve; ///< Owned by the decoder, n/2 entries.
} VORBIS_Channel_Floor;

/**
 * @brief Decode one channel's floor from @p bits.
 *
 * @return ::GAUD_OK with @p out_used saying whether the channel carries
 *   anything; ::GAUD_ERR_CORRUPT for a packet that does not describe a
 *   curve.
 */
GAUD_Result gaud_vorbis_floor_decode(const VORBIS_Floor * floor,
    const VORBIS_Setup * setup, VORBIS_Bits * bits, uint32_t lines,
    unsigned char * out_curve, bool * out_used);

/**
 * @brief Decode one residue into @p vectors, for the channels wanted.
 *
 * @param residue Which residue configuration.
 * @param setup For the codebooks it names.
 * @param bits The packet.
 * @param channels How many vectors @p vectors holds.
 * @param lines How many spectral lines each one has, which is n/2.
 * @param wanted One flag per channel: false leaves that vector alone.
 * @param vectors The vectors, @p channels by @p lines, in Q#VORBIS_Q.
 */
GAUD_Result gaud_vorbis_residue_decode(const VORBIS_Residue * residue,
    const VORBIS_Setup * setup, VORBIS_Bits * bits, uint32_t channels,
    uint32_t lines, const bool * wanted, int32_t * vectors);

/**
 * @brief The inverse modified discrete cosine transform of @p n points.
 *
 * @param spectrum @p n / 2 lines in Q#VORBIS_SPECTRUM_Q. Consumed: the
 *   transform works in place in @p scratch and does not alter this.
 * @param n The block size, a power of two from 64 to 8192.
 * @param scratch At least @p n / 2 complex values, which is @p n int32.
 * @param out @p n samples in Q#VORBIS_TIME_Q.
 */
void gaud_vorbis_imdct(const int32_t * spectrum, uint32_t n,
    int32_t * scratch, int32_t * out);

/**
 * @brief How many bits the transform of @p n points scales its output
 *   down by.
 *
 * The transform halves its state after every second stage, because its
 * gain on real audio is about the square root of the number of
 * coefficients and one bit per two stages is what tracks that. So the
 * output's scale depends on the block size, and the caller normalises it
 * to ::VORBIS_TIME_Q - which the overlap-add needs, because it adds the
 * right half of one block to the left half of the next and the two may
 * be different sizes.
 */
unsigned gaud_vorbis_imdct_shift(uint32_t n);

/** @brief Read a setup header, which is the third header packet. */
GAUD_Result gaud_vorbis_parse_setup(const unsigned char * data, size_t size,
    uint32_t channels, const GAUD_Limits * limits, VORBIS_Setup * out);

/** @brief Release everything a setup owns. */
void gaud_vorbis_setup_free(VORBIS_Setup * setup);

/** @brief Read one codebook from @p bits. */
GAUD_Result gaud_vorbis_parse_codebook(
    VORBIS_Bits * bits, const GAUD_Allocator * allocator,
    VORBIS_Codebook * out);

/** @brief Release everything a codebook owns. */
void gaud_vorbis_codebook_free(
    const GAUD_Allocator * allocator, VORBIS_Codebook * book);

/**
 * @brief Read one entry number from @p bits using @p book.
 *
 * @return The entry, or `UINT32_MAX` where the bits matched no codeword or
 *   the packet ended.
 */
uint32_t gaud_vorbis_codebook_decode(
    const VORBIS_Codebook * book, VORBIS_Bits * bits);

/** @brief What the identification header states, and nothing more. */
typedef struct {
  uint32_t version;      ///< Must be zero; anything else is another format.
  uint32_t channels;     ///< At least one.
  uint32_t sample_rate;  ///< At least one; no list of legal values.
  int32_t bitrate_maximum; ///< Advisory. Zero or negative means unset.
  int32_t bitrate_nominal; ///< Advisory.
  int32_t bitrate_minimum; ///< Advisory.
  uint32_t blocksize_short; ///< A power of two in [64, 8192].
  uint32_t blocksize_long;  ///< The same, and not smaller than the short.
} VORBIS_Info;

/** @brief What one Vorbis document keeps. */
typedef struct {
  VORBIS_Info info;                 ///< From the identification header.
  VORBIS_Setup setup;               ///< From the setup header.
  const GAUD_Allocator * allocator; ///< Everything here comes from it.
  /**
   * The logical stream the Vorbis is in.
   *
   * Kept for the same reason Ogg FLAC keeps it: an Ogg file may
   * multiplex, and a decoder that let the reader choose a serial afresh
   * could follow a different stream from the one the document describes.
   */
  uint32_t serial;
  uint64_t audio_offset; ///< Where the first page after the headers begins.
  uint64_t frames;       ///< The last granule position, which is the length.
  bool frames_known;     ///< Whether that could be established at all.
} VORBIS_File;

/**
 * @brief Read an identification header out of @p data.
 *
 * @return ::GAUD_OK; ::GAUD_ERR_FORMAT if it is not one; ::GAUD_ERR_CORRUPT
 *   if it is one and says something impossible.
 */
GAUD_Result gaud_vorbis_parse_identification(
    const unsigned char * data, size_t size, VORBIS_Info * out);

/** @brief Whether @p data begins a Vorbis header packet of @p type. */
bool gaud_vorbis_is_header(
    const unsigned char * data, size_t size, unsigned type);

/** @brief ::GAUD_Codec::open. */
GAUD_Result gaud_vorbis_open(const GAUD_Codec * codec, GAUD_Stream * stream,
    const GAUD_Limits * limits, GAUD_Diagnostics * diagnostics,
    GAUD_Doc ** out_doc);

/** @brief ::GAUD_Codec::close. */
void gaud_vorbis_close(const GAUD_Codec * codec, GAUD_Doc * doc);

/** @brief ::GAUD_Codec::decoder_open. */
GAUD_Result gaud_vorbis_decoder_open(
    const GAUD_Codec * codec, GAUD_Track * track, GAUD_Decoder ** out);

/**
 * @brief Which decoded channel belongs in output slot @p slot.
 *
 * Vorbis's channel order is not this library's. See the table in
 * src/codec/vorbis/vorbis_decode.c; exposed so that a test can check the
 * channel counts the corpus has no fixture for, which is three, five and
 * seven of the eight the specification defines an order for.
 */
unsigned gaud_vorbis_channel_slot(uint32_t channels, unsigned slot);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_VORBIS_VORBIS_INTERNAL_H
