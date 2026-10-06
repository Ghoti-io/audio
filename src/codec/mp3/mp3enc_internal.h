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
 * The MPEG audio Layer III encoder: private declarations. Never installed.
 *
 * **Everything here is integer arithmetic**, which is a promise and not a
 * style: planning/audio.md section 11.12 says an encoder's output is
 * byte-identical on every architecture, and a psychoacoustic model written
 * in floating point could not keep it. The tables are generated
 * (`tools/tables/gen_mp3enc_tables.py`, and `gen_mp3enc_psy.py` for the
 * model's), the transforms are matrices in Q28 with 64-bit accumulation,
 * and where a logarithm, a root or a power is wanted there is an integer
 * routine for it in `mp3enc_math.c`.
 *
 * The files follow the order a granule travels:
 *
 * - `mp3enc_filter.c` - the polyphase analysis and the hybrid MDCT: the
 *   decoder's synthesis run backwards, scaled so that the pair is unity.
 * - `mp3enc_psy.c` - what the ear would not notice: allowed noise per
 *   scalefactor band, and whether the block should be short.
 * - `mp3enc_quant.c` - the two nested loops: the quantiser's step, and the
 *   per-band amplification that shapes the noise under the threshold.
 * - `mp3enc_huff.c` - the cheapest Huffman coding of what was chosen.
 * - `mp3enc_frame.c` - header, side information, scalefactors and the
 *   bit reservoir.
 * - `mp3enc_rate.c` - how many bits each granule may have.
 * - `mp3enc_encoder.c` - the object a caller holds, and the tag frame.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3ENC_INTERNAL_H
#define GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3ENC_INTERNAL_H

#include <ghoti.io/audio/macros.h>
#include "mp3_internal.h"
#include "mp3_tables.h"
#include "mp3enc_psy_tables.h"
#include "mp3enc_tables.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Lines in a granule, and the granule's length in samples. */
#define MP3E_LINES 576u

/** The most bits one granule's main data may occupy: part2_3_length is
 *  twelve bits wide. */
#define MP3E_MAX_PART23 4095u

/** The most bits a frame's main data can occupy, two granules of two
 *  channels, with room for byte alignment. */
#define MP3E_MAX_FRAME_DATA ((4u * MP3E_MAX_PART23) / 8u + 16u)

/** The back-pointer's range: nine bits for MPEG-1, eight below it. */
#define MP3E_RESERVOIR_MAX_V1 511u
#define MP3E_RESERVOIR_MAX_V2 255u

/** Bands in a granule's layout, long (22) or three windows of short (39). */
#define MP3E_MAX_BANDS 40u

/* ------------------------------------------------------- integer maths */

/** floor(sqrt(value)). */
uint32_t gaud_mp3e_isqrt(uint64_t value);

/**
 * log2(value) in Q8, rounded down, for value > 0; 0 for 0.
 *
 * Eight fractional bits are 0.4% of an octave, which is under a hundredth
 * of a decibel of energy - far finer than anything the model decides on.
 */
int32_t gaud_mp3e_log2_q8(uint64_t value);

/** 2^(exponent / 256) as a Q16 value, for exponent in a few hundred. */
uint32_t gaud_mp3e_exp2_q8(int32_t exponent);

/* ------------------------------------------------------------ bit writer */

typedef struct {
  unsigned char * data; ///< Borrowed; zeroed by the caller.
  size_t capacity;      ///< Bytes in @p data.
  size_t bits;          ///< Bits written.
  bool overflow;        ///< A write went past the capacity.
} MP3E_Bits;

void gaud_mp3e_bits_init(MP3E_Bits * bits, unsigned char * data, size_t capacity);

/** Append the low @p count bits of @p value, most significant first. */
void gaud_mp3e_bits_put(MP3E_Bits * bits, uint32_t value, unsigned count);

/* ---------------------------------------------------------- band layout */

/** One run of lines that share a scalefactor, in bitstream order. */
typedef struct {
  uint16_t start;  ///< First line, in bitstream order.
  uint16_t width;  ///< Lines.
  uint8_t sfb;     ///< Scalefactor band.
  uint8_t window;  ///< 0 for a long block; 0 to 2 for a short one.
} MP3E_Band;

/**
 * The bands of one block shape at one sampling frequency.
 *
 * **Bitstream order is not spectrum order for short blocks**, and the
 * quantiser works in bitstream order throughout so that a band is always
 * one contiguous run. @p order maps a bitstream position to where that
 * line lives in the spectrum the filterbank produced - the decoder's
 * reordering step, run the other way.
 */
typedef struct {
  MP3E_Band band[MP3E_MAX_BANDS];
  unsigned count;           ///< Entries in @p band.
  unsigned sfb_count;       ///< Scalefactor bands per window.
  bool is_short;            ///< Three windows.
  uint16_t order[MP3E_LINES];
} MP3E_Layout;

/** Build the layout for @p row of ::gaud_mp3_sfb_long, long or short. */
void gaud_mp3e_layout(MP3E_Layout * layout, unsigned row, bool is_short);

/* --------------------------------------------------- a quantised granule */

/**
 * Everything one channel of one granule is coded as.
 *
 * The side information is the decoder's own ::MP3_Granule so that what the
 * encoder writes and what the decoder reads are one type, and a field
 * cannot be added to one and forgotten in the other.
 */
typedef struct {
  MP3_Granule side;      ///< As it will be written; part2_3_length last.
  int32_t is[MP3E_LINES]; ///< Quantised values, in bitstream order.
  uint8_t sf_long[22];   ///< Scalefactors of a long block; 21 is always 0.
  uint8_t sf_short[13][3]; ///< Of a short block, [band][window].
  uint32_t part2_bits;   ///< Bits of scalefactors.
  uint32_t part3_bits;   ///< Bits of Huffman data.
  uint16_t big_end;      ///< Lines coded as pairs.
  uint16_t count1_end;   ///< Lines through the end of the quadruples.
  bool scfsi_reused[4];  ///< Groups this granule leaves to the first one.
} MP3E_Granule;

/* ------------------------------------------------------------- huffman */

/**
 * Choose the regions and tables for @p granule->is, fill in the side
 * information that says so, and set @c part3_bits.
 *
 * The search is exhaustive over every split the format can state; see
 * `mp3enc_huff.c` for why that is affordable.
 */
void gaud_mp3e_huffman_plan(MP3E_Granule * granule,
    const MP3E_Layout * layout, unsigned row);

/** The bits of main data part 3 for the plan in @p granule. */
uint32_t gaud_mp3e_huffman_bits(const MP3E_Granule * granule, unsigned row);

/** Write part 3 of @p granule. */
void gaud_mp3e_huffman_write(
    MP3E_Bits * bits, const MP3E_Granule * granule, unsigned row);

/* --------------------------------------------------------- scalefactors */

/**
 * Choose scalefac_compress for @p granule's scalefactors, and set
 * @c part2_bits. @p version and @p granule_index (0 or 1) matter because
 * MPEG-1 may leave the second granule's scalefactors to the first.
 *
 * @return false when the scalefactors do not fit any field width, which
 *   the quantiser answers by amplifying less.
 */
bool gaud_mp3e_scalefactor_plan(MP3E_Granule * granule, MP3_Version version);

/** Write the scalefactors of @p granule, honouring its reused groups. */
void gaud_mp3e_scalefactor_write(MP3E_Bits * bits,
    const MP3E_Granule * granule, MP3_Version version);

/* ---------------------------------------------------------- quantiser */

/** Energy of a spectral line, squared and scaled; see ::MP3E_ENERGY_SHIFT. */
#define MP3E_ENERGY_SHIFT 6

/**
 * Quantise one value: the integer whose requantisation is nearest, with
 * the ISO rounding offset, given the step @p e4 in quarter-octaves.
 */
int32_t gaud_mp3e_quantize_one(int32_t value, int e4);

/** What the decoder makes of @p quantized at step @p e4, in Q28. */
int64_t gaud_mp3e_dequantize_one(int32_t quantized, int e4);

/** Everything the quantiser needs to know about one channel-granule. */
typedef struct {
  const int32_t * spectrum;   ///< 576 lines in spectrum order, Q28.
  uint8_t block_type;         ///< 0 long, 1 start, 2 short, 3 stop.
  uint64_t allowed[MP3E_MAX_BANDS]; ///< Noise each band may carry.
  uint32_t target_bits;       ///< What to aim for; the loop never exceeds it.
  uint32_t hard_bits;         ///< What it must not exceed.
  bool fit_to_noise;          ///< VBR: stop at the coarsest that is masked.
} MP3E_Quant_Input;

/**
 * Run the two loops and fill in @p out.
 *
 * @return how many bands are still louder than allowed, which is 0 when
 *   the granule is masked everywhere and tells the rate controller whether
 *   the bits it gave were enough.
 */
unsigned gaud_mp3e_quantize_granule(const MP3E_Quant_Input * input,
    const MP3E_Layout * layout, unsigned row, MP3_Version version,
    MP3E_Granule * out);

/**
 * What a spectrum would cost to code with noise held to @p allowed: the
 * perceptual entropy, sum of width * log2(1 + sqrt(energy / allowed)) over
 * the bands that need any, in bits. Used to compare ways of coding the
 * same granule and to decide how many bits it deserves.
 */
uint32_t gaud_mp3e_estimate_bits(const MP3E_Layout * layout,
    const int32_t * spectrum, const uint64_t * allowed);

/* -------------------------------------------------------- filterbank */

typedef struct {
  int32_t x[512];          ///< The last 512 input samples, newest at 0.
  int32_t slot[2][18][32]; ///< This and the previous granule's subbands, Q28.
  unsigned current;        ///< Which of @p slot is the newer one.
} MP3E_Filter;

void gaud_mp3e_filter_reset(MP3E_Filter * filter);

/**
 * Take 576 samples (stride @p stride apart) through the polyphase
 * analysis, leaving the granule's 18 time slots of 32 subband samples in
 * the filter. Does not transform.
 */
void gaud_mp3e_filter_analyze(
    MP3E_Filter * filter, const int16_t * samples, size_t stride);

/**
 * The hybrid transform of the granule just analysed: 36 slots through an
 * MDCT of the block type, then alias reduction, into @p out in the
 * spectrum order the decoder's requantiser fills.
 */
void gaud_mp3e_filter_transform(
    const MP3E_Filter * filter, unsigned block_type, int32_t out[MP3E_LINES]);


/* ------------------------------------------------ the psychoacoustic model */

/** Per-sampling-frequency tables the model derives once at open. */
typedef struct {
  unsigned psy_row;    ///< Version * 3 + rate index: the generated tables' row.
  unsigned band_row;   ///< The scalefactor band tables' row.
  unsigned long_count; ///< Partitions of the 1024-point spectrum.
  unsigned short_count;
  /** Spreading of masking from partition i to j, Q16; and each j's norm. */
  uint32_t long_spread[MP3E_MAX_LONG_PARTS][MP3E_MAX_LONG_PARTS];
  uint32_t long_norm[MP3E_MAX_LONG_PARTS];
  uint32_t short_spread[MP3E_MAX_SHORT_PARTS][MP3E_MAX_SHORT_PARTS];
  uint32_t short_norm[MP3E_MAX_SHORT_PARTS];
  /** Which partition each MDCT line's frequency falls in. */
  uint8_t long_line_part[MP3E_LINES];
  uint8_t short_line_part[192];
  /** Bins in each partition. */
  uint16_t long_bins[MP3E_MAX_LONG_PARTS];
  uint16_t short_bins[MP3E_MAX_SHORT_PARTS];
} MP3E_Psy_Rate;

/** What the model concludes about one channel of one granule. */
typedef struct {
  /** Noise each scalefactor band may carry, in the quantiser's units. */
  uint64_t allowed_long[23];
  uint64_t allowed_short[3][14];
  /** Perceptual entropy, in bits, of coding it long or as three short. */
  uint32_t pe_long;
  uint32_t pe_short;
  /** The noise each line of the long transform may carry, which is what a
   *  transient detector compares the short windows' energies against. */
  uint64_t allowed_line[MP3E_LINES];
} MP3E_Psy_Result;

/** Everything the model remembers of one channel between granules. */
typedef struct {
  int16_t history[3u * MP3E_LINES]; ///< The last three granules' samples.
  int32_t long_re[2][513];          ///< The last two long spectra.
  int32_t long_im[2][513];
  uint32_t long_mag[2][513];
  int32_t short_re[2][129];         ///< The last two short spectra.
  int32_t short_im[2][129];
  uint32_t short_mag[2][129];
  uint64_t long_previous[MP3E_MAX_LONG_PARTS]; ///< The last thresholds.
} MP3E_Psy_Channel;

void gaud_mp3e_psy_rate_init(
    MP3E_Psy_Rate * rate, unsigned psy_row, unsigned band_row);

void gaud_mp3e_psy_channel_reset(MP3E_Psy_Channel * channel);

/**
 * Take in a granule's 576 samples and say what its neighbourhood permits.
 *
 * @param snr_offset_q8 Decibels (Q8) added to the signal-to-noise ratio
 *   the model demands under each band: positive is stricter and costs bits.
 */
void gaud_mp3e_psy_analyze(const MP3E_Psy_Rate * rate,
    MP3E_Psy_Channel * channel, const int16_t * samples, int snr_offset_q8,
    MP3E_Psy_Result * out);

/* ----------------------------------------------------------- the frame */

/** A frame's header fields, as the encoder chooses them. */
typedef struct {
  MP3_Version version;
  unsigned bitrate_index;
  unsigned rate_index;
  bool padding;
  MP3_Mode mode;
  unsigned mode_extension;
  bool crc;
  unsigned channels;
  uint32_t bitrate;     ///< bits per second
  uint32_t sample_rate;
} MP3E_Frame_Header;

/** The bitrate index for @p kbps in @p version's table, or -1. */
int gaud_mp3e_bitrate_index(MP3_Version version, unsigned kbps);

/** The kbps of @p index in @p version's table. */
unsigned gaud_mp3e_bitrate_kbps(MP3_Version version, unsigned index);

/** The version and rate index of @p sample_rate, or false. */
bool gaud_mp3e_rate_lookup(
    uint32_t sample_rate, MP3_Version * version, unsigned * rate_index);

/** Bytes in the frame, without padding: 144 (72) x bitrate / rate. */
uint32_t gaud_mp3e_frame_bytes(const MP3E_Frame_Header * header);

/** Bytes of side information. */
uint32_t gaud_mp3e_side_bytes(MP3_Version version, unsigned channels);

/** Write the four header bytes. */
void gaud_mp3e_header_write(unsigned char out[4], const MP3E_Frame_Header * h);

/**
 * Write the side information of a frame. @p granule is [granule][channel];
 * @p scfsi is the reuse flags per channel (bit 3 = first group).
 */
void gaud_mp3e_side_write(unsigned char * out, const MP3E_Frame_Header * h,
    uint32_t main_data_begin, const uint8_t scfsi[2],
    const MP3E_Granule granule[2][2]);

/** CRC-16 of the Layer III protected bits (header bytes 2-3 and side info). */
uint16_t gaud_mp3e_crc16(const unsigned char * data, size_t size, uint16_t crc);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_MP3_MP3ENC_INTERNAL_H
