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
 * MPEG-1, MPEG-2 and MPEG-2.5 audio: private declarations. Never installed.
 *
 * **This is not a container, and that is the whole shape of the file.** A
 * WAV states its length, its rate and its channel count once, in a header
 * at a known offset. An MPEG audio stream states them in *every frame*, has
 * no index, no end marker, and no field anywhere saying how many frames
 * there are - and a file may begin with an ID3v2 tag of any size, end with
 * an ID3v1 trailer, and carry bytes between frames that are not frames at
 * all. So the first job of this codec is to decide where a frame is, which
 * is a judgement rather than a lookup, and `mp3_header.c` exists for that
 * one question.
 *
 * The split across these files follows the layers of the format itself,
 * because they genuinely nest:
 *
 * - `mp3_header.c` - the four-byte frame header, and finding one.
 * - `mp3_tags.c` - Xing, Info, VBRI and the LAME extension: the only
 *   places a length or an encoder delay is ever stated.
 * - `mp3_load.c` - the ID3 tags on the ends, the first frame, and what the
 *   document says about the track.
 *
 * Nothing here decodes a sample yet; see the status note in `mp3_load.c`.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3_INTERNAL_H
#define GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3_INTERNAL_H

#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/audio/macros.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The C++ unit tests link these objects directly, as the other codecs' do,
 * so the declarations need C linkage. */
#ifdef __cplusplus
extern "C" {
#endif

/** How many bytes a frame header occupies. Always exactly four. */
#define MP3_HEADER_SIZE 4u

/**
 * The largest a frame can be, in bytes.
 *
 * Derived, not chosen. The worst case over every legal combination is
 * MPEG-1 Layer II at 384 kbit/s and 32 kHz: 144 x 384000 / 32000 = 1728,
 * plus a padding byte. Layer I's own worst case is 12 x 448000 / 32000 x 4
 * = 672 plus four. Rounded up to a power of two with room for the free
 * format case, which states no bitrate and whose frame length is therefore
 * the distance to the next sync and not an arithmetic result.
 */
#define MP3_MAX_FRAME_SIZE 2880u

/**
 * How far into a stream to look for the first frame.
 *
 * A file that begins with something other than a frame or an ID3v2 tag is
 * either not this format or has been damaged at the front, and the
 * difference cannot be told apart by looking further. 64 KiB is well past
 * any amount of leading junk a real encoder produces (the usual amount is
 * zero) and far short of a scan that would make opening ten thousand files
 * expensive.
 */
#define MP3_SYNC_SEARCH 65536u

/** Which MPEG audio standard a frame header names. */
typedef enum {
  /** ISO/IEC 11172-3. 32, 44.1 and 48 kHz. */
  MP3_MPEG1 = 0,
  /** ISO/IEC 13818-3, the "low sampling frequency" extension. Half those. */
  MP3_MPEG2,
  /**
   * A quarter of those, so 8, 11.025 and 12 kHz.
   *
   * **Not in any standard**, which is why it is spelled 2.5 and not 3: it
   * is an extension by the authors of the original encoder, carried by the
   * version field's otherwise reserved `00`. Every decoder in wide use
   * reads it, so refusing it would refuse files that work everywhere else.
   */
  MP3_MPEG25
} MP3_Version;

/** The channel mode field, in the order the header encodes it. */
typedef enum {
  MP3_MODE_STEREO = 0,    ///< Two channels, coded independently.
  MP3_MODE_JOINT_STEREO,  ///< Two channels, sharing information.
  MP3_MODE_DUAL_CHANNEL,  ///< Two unrelated monophonic programmes.
  MP3_MODE_SINGLE_CHANNEL ///< One channel.
} MP3_Mode;

/**
 * @brief Everything the four bytes of a frame header say, plus what follows
 *   from them.
 *
 * The three derived fields at the end - @p bitrate, @p sample_rate,
 * @p frame_size - are here rather than recomputed at each use because every
 * one of them is a table lookup keyed on two or three of the raw fields,
 * and the arithmetic for @p frame_size differs by layer in a way that is
 * easy to write twice and get wrong once.
 */
typedef struct {
  MP3_Version version;     ///< Which standard.
  unsigned layer;          ///< 1, 2 or 3. The header encodes it inverted.
  bool crc_present;        ///< A 16-bit CRC follows the header.
  unsigned bitrate_index;  ///< 0 for free format; 1 to 14 for a stated rate.
  unsigned rate_index;     ///< Which of the version's three rates.
  bool padding;            ///< This frame is one slot longer.
  bool private_bit;        ///< Carried, never interpreted.
  MP3_Mode mode;           ///< Channel mode.
  unsigned mode_extension; ///< What joint stereo shares. Layer-specific.
  bool copyright;          ///< Carried.
  bool original;           ///< Carried.
  unsigned emphasis;       ///< 0 none, 1 50/15 us, 3 CCITT J.17; 2 reserved.

  uint32_t bitrate;     ///< Bits per second, or 0 for free format.
  uint32_t sample_rate; ///< Frames per second.
  uint32_t samples;     ///< Sample frames this frame decodes to.
  unsigned channels;    ///< 1 for single channel, otherwise 2.
  /**
   * Bytes from the start of this header to the start of the next, or 0 for
   * free format, where the header states no bitrate and the length is the
   * distance to the next sync word.
   */
  uint32_t frame_size;
} MP3_Header;

/**
 * @brief Read a frame header.
 *
 * Refuses every reserved encoding rather than guessing past it: a reserved
 * layer, a reserved sample rate index, the reserved bitrate index 15, and
 * the reserved version `01`. A header that fails any of those is not a
 * header at some offset in a valid file, it is an accident of the data -
 * which matters because finding a frame means testing thousands of
 * candidate offsets and every one of these checks is what makes a false
 * positive unlikely.
 *
 * @param bytes Four bytes, MSB first as the format stores them.
 * @param out_header Filled in only when this returns true.
 * @return false when @p bytes do not begin a frame header.
 */
bool gaud_mp3_header_parse(
    const unsigned char * bytes, MP3_Header * out_header);

/**
 * @brief The shortest a frame of this stream could possibly be, in bytes.
 *
 * The length at the lowest bitrate the version and layer define, with no
 * padding. It is a bound rather than a measurement, and it exists for one
 * job: a Xing or VBRI tag states a frame count, nothing checks it, and a
 * count larger than the file could possibly hold is a fabricated duration
 * that a caller would otherwise act on. @p audio_length divided by this is
 * the most frames there can be.
 *
 * Zero when the question has no answer, which is a @p header this did not
 * come from a successful parse of.
 */
uint32_t gaud_mp3_min_frame_size(const MP3_Header * header);

/**
 * @brief How many bytes of side information follow the header and CRC.
 *
 * Layer III only; 0 for Layers I and II, which have no side information.
 * The size depends on the version and the channel count and on nothing
 * else: 32 or 17 bytes for MPEG-1 stereo or mono, 17 or 9 for MPEG-2 and
 * 2.5, whose granule count is halved.
 */
uint32_t gaud_mp3_side_info_size(const MP3_Header * header);

/**
 * @brief Whether two headers describe the same stream.
 *
 * Version, layer, sample rate and channel count, and deliberately **not**
 * the bitrate or the channel *mode*. A variable-rate file changes the
 * bitrate every frame, which is the whole point of one - and a real
 * encoder changes the mode too: the corpus's TwoLAME fixture is joint
 * stereo in its first frame, plain stereo in its next two and joint
 * stereo with a different bound in its fourth. Both are two channels,
 * which is what a decoder has to keep constant. ffmpeg's demuxer
 * compares the mode and drops that file's first frame as junk; this one
 * decodes all four. This is the test that turns a candidate sync word
 * into a found frame - a random byte pair matching 11 bits of sync is
 * common, and two of them agreeing about the stream at exactly the
 * distance the first one's length states is not.
 */
bool gaud_mp3_headers_compatible(
    const MP3_Header * first, const MP3_Header * second);

/* ----------------------------------------------------- the length tags */

/**
 * @brief What a Xing, Info or VBRI frame states, and what LAME adds.
 *
 * **These tags are the only place an MPEG audio stream ever states its own
 * length.** Everything else about the format is per-frame, so a file
 * without one of these has no length until something counts, and a file
 * with one is trusting whatever wrote it. The fields are therefore kept
 * separate from what the loader concludes: @p frames is what the tag said,
 * and whether that is believed is decided in `mp3_load.c` where the
 * alternatives can be compared.
 *
 * The encoder delay is in the same place for the same reason. An MPEG
 * stream's first granule is not the recording's first sample - the encoder
 * primed its filterbank with it - and nothing in the format says so. LAME
 * writes the number down, so a file it produced can be played gapless and
 * one from an encoder that did not cannot. ::GAUD_Trim::stated is how a
 * caller tells those apart.
 */
typedef struct {
  bool present;     ///< A tag of some kind was found.
  bool vbri;        ///< Fraunhofer's VBRI rather than Xing's spelling.
  bool is_info;     ///< "Info", Xing's own marker for a constant-rate file.
  bool has_frames;  ///< @p frames was stated.
  bool has_bytes;   ///< @p bytes was stated.
  bool has_toc;     ///< A seek table was stated; @p toc holds it.
  bool has_quality; ///< @p quality was stated.
  uint32_t frames;  ///< Frames the tag claims, excluding its own.
  uint32_t bytes;   ///< Bytes the tag claims, including its own frame.
  uint32_t quality; ///< 0 to 100, the encoder's own self-assessment.
  /**
   * A hundred one-byte offsets, each a 256th of @p bytes, at a 100th of
   * the duration. Coarse by construction: the finest it can resolve is a
   * hundredth of the file, and the quantisation of the offset itself is
   * @p bytes / 256. It is a starting point for a seek and never a
   * position.
   */
  unsigned char toc[100];

  bool lame_present; ///< The 36-byte LAME extension was found and trusted.
  char encoder[10];  ///< Its nine-character encoder string, NUL-terminated.
  /** Frames of encoder delay as LAME states it, *before* adding the
   *  decoder's own 529; see gaud_mp3_tag_trim(). */
  uint32_t lame_delay;
  /** Frames of padding in the last frame, as LAME states it. */
  uint32_t lame_padding;
  /** Bytes of audio data LAME counted, or 0. A cross-check on @p bytes. */
  uint32_t music_length;
} MP3_Vbr_Tag;

/**
 * @brief The 529 frames of delay the decoder itself adds.
 *
 * Not an encoder's doing and not in any file: it is the group delay of the
 * analysis and synthesis filterbanks in series, 528 frames from the
 * polyphase filter plus one. Every decoder has it, which is why LAME's
 * stated delay is short by exactly this much and why every player that
 * does gapless playback adds the same constant back.
 */
#define MP3_DECODER_DELAY 529u

/**
 * @brief Look for a Xing, Info or VBRI tag in the first frame.
 *
 * @param frame The whole first frame, from its sync word.
 * @param size How many bytes of it are in hand.
 * @param header That frame's parsed header; the side information's length
 *   decides where a Xing tag can be.
 * @param out_tag Zeroed and then filled in. @p present says whether
 *   anything was found.
 * @return false when there is no tag, which is not an error: most MPEG
 *   audio ever encoded has none.
 */
bool gaud_mp3_tag_parse(const unsigned char * frame, size_t size,
    const MP3_Header * header, MP3_Vbr_Tag * out_tag);

/**
 * @brief Turn a tag's stated delay and padding into a ::GAUD_Trim.
 *
 * Adds ::MP3_DECODER_DELAY to the delay and subtracts it from the padding,
 * because what LAME writes down is the delay it introduced and what a
 * caller needs is the delay in the samples this library hands back, which
 * includes the decoder's own. A padding smaller than the constant floors
 * at zero rather than wrapping.
 *
 * @p stated is false when no LAME extension was present, so that a caller
 * cannot tell a file whose delay is unknown from one whose delay is zero.
 */
GAUD_Trim gaud_mp3_tag_trim(const MP3_Vbr_Tag * tag);

/* ------------------------------------------------------------ the file */

/** How the frame count in a document was arrived at. */
typedef enum {
  /** Nothing could establish it; the track reports `UINT64_MAX`. */
  MP3_LENGTH_UNKNOWN = 0,
  /** A Xing, Info or VBRI frame stated it. */
  MP3_LENGTH_STATED,
  /** Every frame was counted, because the file was short enough to walk. */
  MP3_LENGTH_COUNTED,
  /**
   * Derived from the data size and one bitrate, having checked that the
   * bitrate does not change over the frames that were looked at.
   *
   * **This is an estimate and is reported as one.** It is exact for a
   * constant-rate file and wrong for a variable-rate file whose encoder
   * wrote no tag, and no bounded amount of looking can tell those apart
   * with certainty - so the diagnostic says what was assumed.
   */
  MP3_LENGTH_ESTIMATED
} MP3_Length_Source;

/** What the loader concluded about a stream, and what the decoder needs. */
typedef struct {
  const GAUD_Allocator * allocator; ///< Everything here comes from it.
  MP3_Header first;                 ///< The first frame's header.
  MP3_Vbr_Tag tag;                  ///< Its Xing/Info/VBRI tag, if any.
  /** Where the first frame of any kind is, past any ID3v2 tag. */
  uint64_t first_frame_offset;
  /**
   * Where the first frame carrying audio is.
   *
   * Different from @p first_frame_offset exactly when a Xing, Info or VBRI
   * tag was found: that tag lives *in* a frame, in the space its samples
   * would occupy, so the frame is not audio and decoding it would put a
   * frame of silence at the front of every file an encoder tagged.
   */
  uint64_t audio_offset;
  /** Bytes of audio, with ID3v1 and any APE trailer excluded. */
  uint64_t audio_length;
  /** Sample frames in the file, or `UINT64_MAX`. */
  uint64_t frames;
  MP3_Length_Source length_source; ///< How @p frames was arrived at.
} MP3_File;

/** @brief Release what gaud_mp3_open() allocated. */
void gaud_mp3_file_free(MP3_File * file);

/** @brief ::GAUD_Codec::open for a bare MPEG audio stream. */
GAUD_Result gaud_mp3_open(const GAUD_Codec * codec, GAUD_Stream * stream,
    const GAUD_Limits * limits, GAUD_Diagnostics * diagnostics,
    GAUD_Doc ** out_doc);

/** @brief ::GAUD_Codec::close. */
void gaud_mp3_close(const GAUD_Codec * codec, GAUD_Doc * doc);

/**
 * @brief ::GAUD_Codec::decoder_open: a pull decoder over this track.
 *
 * @return ::GAUD_ERR_UNSUPPORTED for a Layer I or II track, which this
 *   library identifies and does not yet decode, and for MPEG-2.5 Layer
 *   III, whose scalefactor band tables are in no standard.
 */
GAUD_Result gaud_mp3_decoder_open(
    const GAUD_Codec * codec, GAUD_Track * track, GAUD_Decoder ** out);

/**
 * @brief Find the first frame at or after @p start, and parse its header.
 *
 * A candidate sync word is only accepted when the frames that its own
 * stated length points at are there too; see
 * gaud_mp3_headers_compatible(). @p out_chain receives how many following
 * frames agreed, which is what the probe turns into a confidence.
 *
 * @param stream Left wherever the search ended; the caller re-seeks.
 * @param start Where to begin looking.
 * @param search How many bytes past @p start to look at.
 * @param out_offset Where the frame begins. Written only on success.
 * @param out_header Its parsed header. Written only on success.
 * @param out_chain How many following frames agreed, at most two. May be
 *   NULL.
 * @return ::GAUD_ERR_FORMAT when no frame was found in that span.
 */
GAUD_Result gaud_mp3_find_frame(GAUD_Stream * stream, uint64_t start,
    uint64_t search, uint64_t * out_offset, MP3_Header * out_header,
    unsigned * out_chain);

/**
 * @brief How many bytes of ID3v2 tag sit at the front of @p stream.
 *
 * Zero when there is none. The stream is left where it was found.
 */
uint64_t gaud_mp3_id3v2_span(GAUD_Stream * stream);

/* ------------------------------------------------- the main data reader */

/**
 * @brief A most-significant-bit-first reader over bytes already in memory.
 *
 * The position is in **bits** and is settable, because Layer III needs it
 * to be: every granule's main data is `part2_3_length` bits long whether
 * or not its Huffman data fills that, so the reader is positioned
 * explicitly at each granule rather than carried along by what was read.
 *
 * @p overrun is the only error state, latched on the first read past the
 * end, exactly as the FLAC reader does it: a granule can be decoded to its
 * end and asked once.
 */
typedef struct {
  const unsigned char * data; ///< Borrowed.
  size_t size;                ///< Bytes in @p data.
  size_t at;                  ///< The next bit to read.
  bool overrun;               ///< A read asked for more than there was.
} MP3_Bits;

/** @brief Point a reader at @p size bytes of @p data, at bit zero. */
void gaud_mp3_bits_init(
    MP3_Bits * bits, const unsigned char * data, size_t size);

/** @brief Read @p count bits, most significant first. At most 32. */
uint32_t gaud_mp3_bits_read(MP3_Bits * bits, unsigned count);

/** @brief Read @p count bits as a two's complement signed value. */
int32_t gaud_mp3_bits_signed(MP3_Bits * bits, unsigned count);

/** @brief Move to bit @p position. Past the end latches the overrun. */
void gaud_mp3_bits_seek(MP3_Bits * bits, size_t position);

/**
 * The most bytes a frame's main data can reach back for.
 *
 * `main_data_begin` is nine bits in MPEG-1 and eight in the low sampling
 * frequency versions, so 511 is the largest back-pointer the format can
 * state. One more than that is kept, which costs nothing and means the
 * arithmetic never sits exactly on the boundary.
 */
#define MP3_RESERVOIR_KEEP 512u

/**
 * @brief The bit reservoir: the main data of the frames just gone past.
 *
 * **The one piece of state that makes an MPEG audio frame not
 * self-contained.** A Layer III frame's main data starts
 * `main_data_begin` bytes before its own header, so decoding a frame
 * needs the bytes of the frames before it. The buffer holds the tail of
 * those followed by this frame's own, and bit zero of it is bit zero of
 * this frame's main data - which is what gaud_mp3_reservoir_push()
 * arranges.
 */
typedef struct {
  /** The carried bytes followed by this frame's own. Large enough for
   *  the largest back-pointer the format can state plus the largest
   *  frame it can describe, which is the most that can ever be in it. */
  unsigned char data[MP3_RESERVOIR_KEEP + MP3_MAX_FRAME_SIZE];
  size_t length; ///< Bytes held.
} MP3_Reservoir;

/** @brief Forget everything held, as a seek must. */
void gaud_mp3_reservoir_reset(MP3_Reservoir * reservoir);

/**
 * @brief Keep what @p main_data_begin reaches back for and append @p size
 *   bytes of this frame's main data.
 *
 * @return false when the frame reaches back further than there is
 *   history, which is the first frames of a stream and the first frame
 *   after a seek. The caller must not decode the granule in that case:
 *   what is in the buffer is either nothing or another part of the file.
 */
bool gaud_mp3_reservoir_push(MP3_Reservoir * reservoir,
    uint32_t main_data_begin, const unsigned char * data, size_t size);

/** @brief Drop everything a future back-pointer could not reach. */
void gaud_mp3_reservoir_trim(MP3_Reservoir * reservoir);

/* ---------------------------------------------- the synthesis filterbank */

/**
 * @brief The state of one channel's polyphase synthesis filterbank.
 *
 * 1024 values of history, which is what the format's 512-tap window over
 * 32 subbands needs, plus where the oldest of them currently is. Every
 * layer uses this and it is the only state Layers I and II carry at all.
 */
typedef struct {
  int32_t state[1024]; ///< V[] of 11172-3 Figure 3-A.2, Q28.
  unsigned at;         ///< Where V[0] lives; see mp3_synth.c.
} MP3_Synth;

/** @brief Zero the filterbank, as the start of a stream or a seek must. */
void gaud_mp3_synth_reset(MP3_Synth * synth);

/**
 * @brief Turn 32 subband values into 32 consecutive audio samples.
 *
 * @param synth This channel's filterbank; see ::MP3_Synth.
 * @param subband The 32 values, Q28.
 * @param out Where the samples go; @p stride apart, so that an
 *   interleaved buffer can be written directly.
 * @param stride How far apart consecutive samples are in @p out: the
 *   channel count, for an interleaved buffer.
 */
void gaud_mp3_synth_run(
    MP3_Synth * synth, const int32_t subband[32], int32_t * out, size_t stride);

/* ---------------------------------------------------------- Layer III */

/** One granule of one channel's side information. */
typedef struct {
  uint32_t part2_3_length;    ///< Bits of main data this granule occupies.
  uint32_t big_values;        ///< Pairs of values coded in the three regions.
  uint32_t global_gain;       ///< The quantiser step, logarithmically.
  uint32_t scalefac_compress; ///< Selects the scalefactor field widths.
  uint8_t block_type;         ///< 0 normal, 1 start, 2 three short, 3 stop.
  bool window_switching;      ///< Anything other than a normal window.
  bool mixed_block;           ///< The lowest subbands stay long.
  bool preflag;               ///< Add ::gaud_mp3_pretab to the scalefactors.
  bool scalefac_scale;        ///< The scalefactors' step is 2 rather than √2.
  bool count1table_select;    ///< Which quadruple table the top region uses.
  uint8_t table_select[3];    ///< A Huffman table per region.
  uint8_t subblock_gain[3];   ///< A further gain per short window.
  uint8_t region0_count;      ///< One less than the bands in region 0.
  uint8_t region1_count;      ///< One less than the bands in region 1.
} MP3_Granule;

/** A frame's side information: the part before the main data. */
typedef struct {
  uint32_t main_data_begin; ///< Bytes back the main data starts.
  uint8_t scfsi[2];         ///< Scalefactor reuse, four bands per channel.
  /** [granule][channel]; MPEG-2 and 2.5 have one granule. */
  MP3_Granule granule[2][2];
} MP3_Side_Info;

/** What one channel of a Layer III stream carries between granules. */
typedef struct {
  MP3_Synth synth;         ///< The polyphase filterbank's history.
  int32_t overlap[32][18]; ///< The hybrid filterbank's, Q28.
  /** The long scalefactors, kept across granules because scfsi lets the
   *  second granule reuse the first's rather than transmitting them. */
  int32_t scalefac_long[23];
  /** The short ones, [band][window]. Never reused across granules: scfsi
   *  is zero for a frame in which either granule is short. */
  int32_t scalefac_short[13][3];
} MP3_Layer3_Channel;

/** Everything a Layer III decoder holds. */
typedef struct {
  MP3_Reservoir reservoir;       ///< The bit reservoir.
  MP3_Layer3_Channel channel[2]; ///< Per-channel history.
  MP3_Side_Info side;            ///< The frame being decoded.
  int32_t xr[2][576];            ///< The requantised spectrum, Q28.
  int32_t scratch[576];          ///< Reordering and the hybrid's output.
  unsigned nonzero[2];           ///< Values decoded, for the stereo bound.
  /**
   * Granules that could not be decoded because the reservoir did not
   * reach back far enough, which is the first frames of a stream and the
   * first frame after a seek. They are emitted as silence - which every
   * decoder does and which the references do - and counted, because
   * "silence this library could not avoid" and "silence the file
   * contains" must not look the same from outside.
   */
  uint32_t ungrounded;
  /** Samples that saturated rather than wrapped. A file that does this is
   *  either very loud or wrong, and the count is how a caller can tell
   *  this decoder is clipping rather than the recording. */
  uint32_t saturated;
} MP3_Layer3;

/** @brief Forget every carried value: a seek, or the start of a stream. */
void gaud_mp3_layer3_reset(MP3_Layer3 * state);

/**
 * @brief Decode one Layer III frame into @p pcm.
 *
 * @param state The decoder's carried state; see ::MP3_Layer3.
 * @param header That frame's parsed header.
 * @param frame The whole frame, from its sync word.
 * @param size How many bytes that is; at least the header, the CRC and
 *   the side information.
 * @param pcm Receives `header->samples` frames of interleaved Q28
 *   samples, channels interleaved, so `samples * channels` values.
 * @return ::GAUD_ERR_CORRUPT for side information the format forbids.
 *   A granule whose main data is missing is silence and not an error;
 *   see ::MP3_Layer3::ungrounded.
 */
GAUD_Result gaud_mp3_layer3_frame(MP3_Layer3 * state, const MP3_Header * header,
    const unsigned char * frame, size_t size, int32_t * pcm);

/**
 * @brief Which row of the scalefactor band tables this stream uses.
 *
 * Rows 0 to 2 are MPEG-1 and rows 3 to 5 are MPEG-2, by the header's rate
 * index. @return false for MPEG-2.5, whose band tables are in no
 * standard; see the refusal in gaud_mp3_decoder_open().
 */
bool gaud_mp3_band_row(const MP3_Header * header, unsigned * out_row);

/* ------------------------------------------------------ Layers I and II */

/**
 * @brief Everything a Layer I or II decoder holds.
 *
 * Which is nothing but the filterbank. These layers have no bit
 * reservoir, no overlapping transform and no scalefactor reuse across
 * frames: every frame is self-contained, which is why a Layer II stream
 * survives being cut with a text editor and a Layer III one does not.
 */
typedef struct {
  MP3_Synth synth[2]; ///< One polyphase filterbank per channel.
} MP3_Layer12;

/** @brief Zero the filterbanks: a seek, or the start of a stream. */
void gaud_mp3_layer12_reset(MP3_Layer12 * state);

/**
 * @brief Decode one Layer I or Layer II frame into @p pcm.
 *
 * @param state The filterbanks; see ::MP3_Layer12.
 * @param header That frame's parsed header.
 * @param frame The whole frame, from its sync word.
 * @param size How many bytes that is.
 * @param pcm Receives `header->samples` frames of interleaved Q28
 *   samples - 384 for Layer I, 1,152 for Layer II.
 * @return ::GAUD_ERR_CORRUPT for an allocation the format forbids, or a
 *   frame whose samples run past its own length.
 */
GAUD_Result gaud_mp3_layer12_frame(MP3_Layer12 * state,
    const MP3_Header * header, const unsigned char * frame, size_t size,
    int32_t * pcm);

/* ------------------------------------------------- the coverage counters */

/**
 * Which arms of this decoder a corpus reaches.
 *
 * planning/audio.md 11.14 asked for this in phase 5 from the first day,
 * and the reason is phase 4's most uncomfortable measurement: FLAC's
 * decode agreed byte for byte with every reference on every fixture
 * while a third of the decoder had never executed. A codec's branches
 * are selected by choices the *encoder* made, so a corpus is a sample of
 * encoders' habits rather than of the format - and MPEG audio has far
 * more of that shape than FLAC: 32 Huffman tables, four window types,
 * two joint stereo modes, a bit reservoir, and two layers below the one
 * everybody means.
 *
 * Compiled to nothing without `GAUD_MP3_TRACE`, which only
 * `make mpeg-coverage` defines. It is **not a gate**: a zero in the
 * report is a question about the corpus, and some of the rows can be
 * reached by no encoder that exists.
 */
#ifdef GAUD_MP3_TRACE
/** The arms counted. Keep in step with the names in mp3_layer3.c. */
enum {
  MP3_T_V1,
  MP3_T_V2,
  MP3_T_V25,
  MP3_T_L1,
  MP3_T_L2,
  MP3_T_L3,
  MP3_T_CRC,
  MP3_T_PADDED,
  MP3_T_RESYNC,
  MP3_T_BLOCK_LONG,
  MP3_T_BLOCK_START,
  MP3_T_BLOCK_SHORT,
  MP3_T_BLOCK_STOP,
  MP3_T_MIXED,
  MP3_T_SUBBLOCK_GAIN,
  MP3_T_PREFLAG,
  MP3_T_SCALEFAC_SCALE,
  MP3_T_SCFSI,
  MP3_T_MS_STEREO,
  MP3_T_INTENSITY,
  MP3_T_INTENSITY_ILLEGAL,
  MP3_T_LINBITS,
  MP3_T_COUNT1_A,
  MP3_T_COUNT1_B,
  MP3_T_RESERVOIR,
  MP3_T_UNGROUNDED,
  MP3_T_SATURATED,
  MP3_T_L2_GROUPED,
  MP3_T_L2_UNGROUPED,
  MP3_T_L2_INTENSITY,
  MP3_T_L2_SCFSI_0,
  MP3_T_L2_SCFSI_1,
  MP3_T_L2_SCFSI_2,
  MP3_T_L2_SCFSI_3,
  MP3_T_L1_ALLOCATED,
  MP3_T_ALLOC_TABLE, /* five, one per Layer II allocation table */
  MP3_T_HUFF = MP3_T_ALLOC_TABLE + 5, /* thirty-two, one per table */
  MP3_T_COUNT = MP3_T_HUFF + 32
};
extern unsigned long gaud_mp3_trace_counts[MP3_T_COUNT];
/** @brief Count one visit to arm @p which. */
#define MP3_TRACE(which) (gaud_mp3_trace_counts[which]++)
#else
/** @brief Nothing, in an ordinary build. */
#define MP3_TRACE(which) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3_INTERNAL_H
