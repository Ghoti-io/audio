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
 * FLAC, as RFC 9639 specifies it: private declarations. Never installed.
 *
 * The split across the files here follows the format's own seam. A FLAC
 * stream is a header of *metadata blocks* followed by a run of *frames*,
 * and the two have nothing in common: the blocks are byte-aligned records
 * with big-endian lengths, and the frames are a bit stream. So
 * `flac_meta.c` reads the first and `flac_frame.c` the second, over the
 * bit reader in `flac_bits.c`, and `flac_load.c` is the part that knows
 * one follows the other.
 *
 * Two of these files are not about the native container at all.
 * `flac_ogg.c` maps the same frames into Ogg pages, and it shares every
 * line of the frame decoder - which is the reason §1 of the plan asked for
 * `src/container/` separately from `src/codec/`. The FLAC bitstream is one
 * thing and the two ways of carrying it are another.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_FLAC_FLAC_INTERNAL_H
#define GHOTI_IO_GAUD_SRC_CODEC_FLAC_FLAC_INTERNAL_H

#include "../../core/meta_internal.h"
#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The C++ unit tests link these objects directly, as the shared codec and
 * metadata headers already do, so the declarations need C linkage. */
#ifdef __cplusplus
extern "C" {
#endif

/** The four bytes a native FLAC stream begins with. */
#define FLAC_MAGIC "fLaC"

/** How many bytes a metadata block header occupies. */
#define FLAC_BLOCK_HEADER 4u

/** How many bytes a STREAMINFO block's body occupies. Always exactly this. */
#define FLAC_STREAMINFO_SIZE 34u

/**
 * The most channels a FLAC frame can describe.
 *
 * Three bits of channel assignment below 0b1000, so eight. This is a
 * property of the format and not a policy of ours; ::GAUD_Limits::max_channels
 * is the policy, and it is checked separately.
 */
#define FLAC_MAX_CHANNELS 8u

/** The largest block size the format's 16-bit field can express. */
#define FLAC_MAX_BLOCK_SIZE 65535u

/** The highest LPC order a subframe header can encode. */
#define FLAC_MAX_LPC_ORDER 32u

/** The highest fixed-predictor order. */
#define FLAC_MAX_FIXED_ORDER 4u

/** Metadata block types, from RFC 9639 section 8.1. */
enum {
  FLAC_BLOCK_STREAMINFO = 0,
  FLAC_BLOCK_PADDING = 1,
  FLAC_BLOCK_APPLICATION = 2,
  FLAC_BLOCK_SEEKTABLE = 3,
  FLAC_BLOCK_VORBIS_COMMENT = 4,
  FLAC_BLOCK_CUESHEET = 5,
  FLAC_BLOCK_PICTURE = 6,
  /** 127 is forbidden so that a block header cannot look like a frame sync. */
  FLAC_BLOCK_INVALID = 127
};

/* ------------------------------------------------------------ bit reader */

/**
 * @brief A most-significant-bit-first reader over a buffer.
 *
 * FLAC frames are a bit stream and nothing in them is byte-aligned except
 * by accident, so every read goes through this. It is deliberately a reader
 * over *memory* rather than over a ::GAUD_Stream: a frame is read into a
 * buffer whole, because the frame's own CRC-16 covers every byte of it and
 * cannot be checked until they are all in hand anyway.
 *
 * @p overrun is the only error state. A read past the end returns zero and
 * latches it, so the caller may decode a whole frame and ask once at the
 * end rather than testing every field - which is what keeps the subframe
 * code readable. Nothing downstream of an overrun is trusted.
 */
typedef struct {
  const unsigned char * data; ///< Borrowed.
  size_t size;                ///< Bytes in @p data.
  size_t pos;                 ///< Next byte to pull into @p cache.
  uint64_t cache;             ///< Right-aligned; @p bits are valid.
  unsigned bits;              ///< How many bits @p cache holds. At most 64.
  bool overrun;               ///< A read asked for more than there was.
} FLAC_Bits;

/** @brief Start reading @p size bytes at @p data. */
void gaud_flac_bits_init(
    FLAC_Bits * br, const unsigned char * data, size_t size);

/**
 * @brief Read @p n bits, most significant first.
 *
 * @param br The reader.
 * @param n 0 to 32. Zero reads nothing and returns zero, which is what
 *   makes a zero-width field - an escaped Rice partition of raw width 0 -
 *   an ordinary case rather than a special one.
 */
uint32_t gaud_flac_bits_read(FLAC_Bits * br, unsigned n);

/** @brief Read @p n bits and sign-extend from bit @p n-1. @p n is 0 to 32. */
int32_t gaud_flac_bits_read_signed(FLAC_Bits * br, unsigned n);

/**
 * @brief As gaud_flac_bits_read_signed(), for @p n of 0 to 64.
 *
 * Needed because a subframe is not always as wide as its stream. The side
 * channel of a decorrelated pair carries one bit more than the frame's
 * stated depth, so a 32-bit FLAC file - which RFC 9639 allows and libFLAC
 * writes - has 33-bit subframes in it, and a decoder whose widest read is
 * 32 bits cannot open one.
 */
int64_t gaud_flac_bits_read_signed64(FLAC_Bits * br, unsigned n);

/** @brief Read up to 64 bits, for the fields wider than a subframe sample. */
uint64_t gaud_flac_bits_read64(FLAC_Bits * br, unsigned n);

/**
 * @brief Count zero bits up to and including the next one bit.
 *
 * The quotient of a Rice code. A run longer than the buffer latches
 * ::FLAC_Bits::overrun and returns what it counted.
 */
uint32_t gaud_flac_bits_read_unary(FLAC_Bits * br);

/** @brief Discard @p n bits. */
void gaud_flac_bits_skip(FLAC_Bits * br, uint64_t n);

/** @brief Discard bits up to the next byte boundary. */
void gaud_flac_bits_align(FLAC_Bits * br);

/** @brief How many bits have been consumed, including a partial byte. */
uint64_t gaud_flac_bits_consumed(const FLAC_Bits * br);

/** @brief How many bits remain unread. Zero once @p overrun is set. */
uint64_t gaud_flac_bits_left(const FLAC_Bits * br);

/**
 * @brief Read the format's variable-length coded number.
 *
 * RFC 9639 section 9.1.5. The encoding is UTF-8's, stretched to seven bytes
 * so that a 36-bit sample number fits, and it is **not** UTF-8: the values
 * it carries are frame and sample numbers, so the surrogate range and the
 * overlong forms that a UTF-8 decoder must reject are all legal here. A
 * reader that borrowed a UTF-8 validator for this would refuse valid files.
 *
 * @param br The reader.
 * @param out Receives the value.
 * @return false when the leading byte is not a legal length marker, or a
 *   continuation byte is missing - which is also how a false frame sync is
 *   usually caught before the CRC is even reached.
 */
bool gaud_flac_bits_read_coded_number(FLAC_Bits * br, uint64_t * out);

/* ----------------------------------------------------------- bit writer */

/**
 * @brief A most-significant-bit-first writer into a growable buffer.
 *
 * The mirror of ::FLAC_Bits, and @p failed latches the same way @p overrun
 * does: an encoder writes thousands of fields per frame and checking each
 * one would bury the arithmetic, so the caller asks once and a frame whose
 * buffer could not grow is never emitted.
 */
typedef struct {
  unsigned char * data;  ///< Owned; grows by doubling.
  size_t size;           ///< Complete bytes written.
  size_t capacity;       ///< Room in @p data.
  unsigned char cache;   ///< The partial byte, left-aligned as it fills.
  unsigned bits;         ///< How many bits of @p cache are used. 0 to 7.
  const GAUD_Allocator * allocator; ///< Where @p data comes from.
  bool failed;           ///< An allocation failed; nothing after is valid.
} FLAC_Bit_Writer;

/** @brief Start an empty writer. */
void gaud_flac_bitw_init(
    FLAC_Bit_Writer * bw, const GAUD_Allocator * allocator);

/** @brief Release its buffer. */
void gaud_flac_bitw_free(FLAC_Bit_Writer * bw);

/** @brief Empty it without releasing its buffer, to write the next frame. */
void gaud_flac_bitw_reset(FLAC_Bit_Writer * bw);

/** @brief Append the low @p n bits of @p value, most significant first. */
void gaud_flac_bitw_write(FLAC_Bit_Writer * bw, uint64_t value, unsigned n);

/** @brief Append @p zeros zero bits and then a one: a Rice quotient. */
void gaud_flac_bitw_write_unary(FLAC_Bit_Writer * bw, uint32_t zeros);

/** @brief Pad with zero bits to the next byte boundary. */
void gaud_flac_bitw_align(FLAC_Bit_Writer * bw);

/** @brief Append @p value in the format's variable-length coding. */
void gaud_flac_bitw_write_coded_number(FLAC_Bit_Writer * bw, uint64_t value);

/* ------------------------------------------------------------------ CRCs */

/** @brief FLAC's CRC-8 (polynomial 0x07) over @p size bytes. */
uint8_t gaud_flac_crc8(const unsigned char * data, size_t size);

/** @brief FLAC's CRC-16 (polynomial 0x8005) over @p size bytes. */
uint16_t gaud_flac_crc16(const unsigned char * data, size_t size);

/* -------------------------------------------------------------- the file */

/** What STREAMINFO says. Every field as the block spells it. */
typedef struct {
  uint16_t min_block_size; ///< Frames. Equal to the max for a fixed stream.
  uint16_t max_block_size; ///< Frames.
  uint32_t min_frame_size; ///< Bytes, or zero for "not stated".
  uint32_t max_frame_size; ///< Bytes, or zero for "not stated".
  uint32_t sample_rate;    ///< Hertz. Zero means a non-audio stream.
  uint32_t channels;       ///< 1 to 8.
  uint32_t bits_per_sample; ///< 4 to 32.
  uint64_t total_samples;  ///< Interchannel samples, or zero for "unknown".
  unsigned char md5[16];   ///< Of the unencoded audio; all-zero if not taken.
} FLAC_Streaminfo;

/** One SEEKTABLE point. */
typedef struct {
  uint64_t sample;      ///< First sample of the target frame.
  uint64_t offset;      ///< Bytes from the first frame's first byte.
  uint16_t frame_samples; ///< Frames in the target frame.
} FLAC_Seek_Point;

/**
 * @brief Everything the metadata blocks told us, kept for the decoder.
 *
 * Lives on the document as its codec-private state, and on the track's too:
 * a native FLAC file has exactly one track, so there is nothing to
 * distinguish and the same pointer serves both.
 */
typedef struct {
  FLAC_Streaminfo info; ///< What the mandatory first block said.
  /** SEEKTABLE points, sorted and with placeholders dropped. May be NULL. */
  FLAC_Seek_Point * seek_points;
  size_t seek_count;    ///< How many of them survived the parse.
  /** Where the first frame starts, which is where an offset is measured from. */
  uint64_t first_frame_offset;
  /** How many bytes of frames there are, or `UINT64_MAX` when not knowable. */
  uint64_t frames_length;
  const GAUD_Allocator * allocator; ///< Everything here comes from it.
} FLAC_File;

/* --------------------------------------------------------- frame decoding */

/**
 * @brief One decoded frame: its header's claims and its samples.
 *
 * Samples are **planar and 64 bits wide whatever the file's depth**, which
 * is not what the caller receives - they are narrowed once, on the way out.
 * Two separate things force the width and either one alone would:
 *
 * A side channel carries one bit more than the frame's stated depth, so a
 * 32-bit stream has 33-bit subframes and there is no narrower type that
 * holds one. And the predictors accumulate across up to 32 coefficients of
 * up to 15 bits over samples of up to 32, which is signed overflow - not a
 * wrong answer, undefined behaviour - in anything narrower.
 *
 * The cost is bounded and small: eight bytes per sample over at most
 * 65,535 frames and 8 channels is 4 MiB in the worst case the format
 * admits, and about 64 KiB for the block size an encoder actually picks.
 */
typedef struct {
  uint32_t block_size;      ///< Frames in this frame.
  uint32_t sample_rate;     ///< As the header stated, or STREAMINFO's.
  uint32_t channels;        ///< 1 to 8.
  uint32_t bits_per_sample; ///< As the header stated, or STREAMINFO's.
  bool variable_block_size; ///< Whether the coded number is a sample number.
  uint64_t number;          ///< Frame number, or first sample number.
  /** capacity_channels * capacity_frames values, planar. Owned. */
  int64_t * samples;
  uint32_t capacity_frames;   ///< Frames @p samples holds per channel.
  uint32_t capacity_channels; ///< Channels @p samples holds.
  const GAUD_Allocator * allocator; ///< Where @p samples comes from.
} FLAC_Frame;

/** @brief Release a frame's sample buffer. */
void gaud_flac_frame_free(FLAC_Frame * frame);

/**
 * @brief Make sure @p frame can hold @p channels x @p block_size samples.
 *
 * Grows and never shrinks, so a decoder that meets one large frame does not
 * reallocate for every small one after it.
 */
GAUD_Result gaud_flac_frame_reserve(
    FLAC_Frame * frame, uint32_t channels, uint32_t block_size);

/**
 * @brief Decode one frame from @p data into @p frame.
 *
 * @param data The frame, from its sync code onwards. More than one frame's
 *   worth is fine; @p out_used says how much was taken.
 * @param size How many bytes are available.
 * @param info STREAMINFO, for the fields a frame header may defer to it.
 * @param frame Reused across calls; its buffer grows as needed.
 * @param out_used Bytes consumed, including the CRC-16. Written on success.
 * @return ::GAUD_ERR_CORRUPT for a bad sync, a failed CRC or a subframe
 *   that does not add up; ::GAUD_ERR_UNSUPPORTED for a reserved encoding
 *   that a later revision of the format may define; ::GAUD_ERR_IO when the
 *   frame is longer than @p size, which is how a caller learns to read more.
 */
GAUD_Result gaud_flac_frame_decode(const unsigned char * data, size_t size,
    const FLAC_Streaminfo * info, FLAC_Frame * frame, size_t * out_used);

/**
 * @brief Read a frame header far enough to answer "is this a frame here?".
 *
 * Used when resynchronising - seeking into a stream with no seek table, or
 * recovering after a corrupt frame. It checks the sync code, the reserved
 * bits, the forbidden field values and the header's own CRC-8, which
 * together make a false positive rare enough to be worth acting on.
 *
 * @param data Where a frame header might begin.
 * @param size How many bytes are readable there.
 * @param info STREAMINFO, for the fields a header may defer to it and for
 *   the shape a frame of this stream must agree with.
 * @param out_block_size May be NULL.
 * @param out_number May be NULL. The frame or sample number.
 * @param out_is_sample_number May be NULL. Which of those two it is.
 * @return true when a frame header starts at @p data.
 */
bool gaud_flac_frame_peek(const unsigned char * data, size_t size,
    const FLAC_Streaminfo * info, uint32_t * out_block_size,
    uint64_t * out_number, bool * out_is_sample_number);

/**
 * @brief Copy @p count of a decoded frame's samples into @p buffer.
 *
 * Where the samples stop being 64 bits wide and become the track's format.
 * Shared by the native decoder and the Ogg one, which differ in how they
 * find a frame and not at all in what they do with it.
 *
 * @param frame The decoded frame to take samples from.
 * @param from The first sample within the frame to take.
 * @param buffer Where they go, in the track's own format.
 * @param at_frame Where in @p buffer to put them.
 * @param count How many sample frames to copy.
 */
void gaud_flac_emit(const FLAC_Frame * frame, uint32_t from,
    GAUD_Buffer * buffer, size_t at_frame, uint32_t count);

/* ------------------------------------------------------ metadata blocks */

/**
 * @brief Parse STREAMINFO's 34 bytes.
 *
 * @return ::GAUD_ERR_CORRUPT when a field the format forbids is present -
 *   a zero channel count, a block size below 16, a bit depth outside 4 to 32.
 */
GAUD_Result gaud_flac_parse_streaminfo(
    const unsigned char * data, size_t size, FLAC_Streaminfo * out);

/**
 * @brief Parse a SEEKTABLE block into @p file.
 *
 * Placeholder points - the ones whose sample number is all ones - are
 * dropped rather than kept, because they carry no offset and exist only so
 * that an encoder can reserve room it has not filled in yet.
 */
GAUD_Result gaud_flac_parse_seektable(FLAC_File * file,
    const unsigned char * data, size_t size, const GAUD_Limits * limits,
    GAUD_Diagnostics * diagnostics);

/**
 * @brief Parse a PICTURE block into @p meta.
 *
 * **The block that made the four-state picture status worth having.** It is
 * the only cover-art carrier of the three that states width, height, depth
 * and palette size, so it is the only one whose claim can be checked
 * against its own payload - and the only one that can be found lying.
 */
GAUD_Result gaud_flac_parse_picture(const unsigned char * data, size_t size,
    const GAUD_Limits * limits, GAUD_Meta * meta,
    GAUD_Diagnostics * diagnostics);

/**
 * @brief Check a CUESHEET block's structure, then leave it to raw carriage.
 *
 * This deliberately does **not** produce a public cue model; see the
 * library's `documentation/flac.md`. What it does is refuse a block whose
 * own framing does not add up, so that a corrupt cuesheet is a diagnostic
 * rather than a blob we hand back unchanged and unexamined.
 *
 * @param data The block body.
 * @param size How many bytes it holds.
 * @param limits Never NULL; caps the index points across every track.
 * @param out_tracks May be NULL. How many tracks the sheet declares.
 * @param diagnostics May be NULL.
 * @return ::GAUD_OK, ::GAUD_ERR_CORRUPT when the counts and the length
 *   disagree, or ::GAUD_ERR_LIMIT.
 */
GAUD_Result gaud_flac_check_cuesheet(const unsigned char * data, size_t size,
    const GAUD_Limits * limits, uint32_t * out_tracks,
    GAUD_Diagnostics * diagnostics);

/* ------------------------------------------------------- codec entry points */

/** @brief ::GAUD_Codec::open for the native FLAC codec. */
GAUD_Result gaud_flac_open(const GAUD_Codec * codec, GAUD_Stream * stream,
    const GAUD_Limits * limits, GAUD_Diagnostics * diagnostics,
    GAUD_Doc ** out_doc);

/** @brief ::GAUD_Codec::close for it. */
void gaud_flac_close(const GAUD_Codec * codec, GAUD_Doc * doc);

/** @brief ::GAUD_Codec::decoder_open for it. */
GAUD_Result gaud_flac_decoder_open(
    const GAUD_Codec * codec, GAUD_Track * track, GAUD_Decoder ** out_decoder);

/** @brief ::GAUD_Codec::encoder_open for it. */
GAUD_Result gaud_flac_encoder_open(const GAUD_Codec * codec,
    GAUD_Stream * stream, const GAUD_Encode_Params * params,
    GAUD_Encoder ** out_encoder);

/**
 * @brief The same encoder, writing Ogg pages instead of a native stream.
 *
 * One function rather than a second encoder, because everything from the
 * block buffer down - the predictors, the Rice coding, the digest - is
 * identical and only the framing differs.
 */
GAUD_Result gaud_flac_ogg_encoder_open(const GAUD_Codec * codec,
    GAUD_Stream * stream, const GAUD_Encode_Params * params,
    GAUD_Encoder ** out_encoder);

/** @brief Free a ::FLAC_File and everything it owns. */
void gaud_flac_file_free(FLAC_File * file);

/**
 * @brief Map a FLAC bit depth onto the buffer format that holds it exactly.
 *
 * @param bits The stream's bit depth, 4 to 32.
 * @param out Written only when the answer is true.
 * @return false for the depths this library does not carry. RFC 9639 allows
 *   4 to 32 and the buffer has 8, 16, 24 and 32; for 20-bit audio there is
 *   no lossless landing place, and the two obvious ones both lie. Leaving
 *   the value where it is makes the track quieter than the file by a factor
 *   the caller cannot see, and shifting it up - which is what ffmpeg does -
 *   changes the sample values and so breaks the round trip this codec is
 *   here to promise. Refusing is the only answer that is true, and the fix
 *   when someone asks is a bit depth on the track, which WAV's
 *   `wValidBitsPerSample` already wants.
 */
bool gaud_flac_format_for_depth(uint32_t bits, GAUD_Sample_Format * out);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_FLAC_FLAC_INTERNAL_H
