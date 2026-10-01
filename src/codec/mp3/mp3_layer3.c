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
 * Layer III: side information, scalefactors, Huffman, requantisation,
 * stereo, and the hybrid filterbank.
 *
 * The order below is the order of 11172-3 Figure 3-A.3, and every step is
 * where the standard puts it. What is worth knowing before reading it:
 *
 * **A granule is 576 lines and a frame is one or two of them.** MPEG-1 has
 * two; the low sampling frequency versions have one, which is why the side
 * information is half the size and carries no scalefactor reuse.
 *
 * **The spectrum is ordered three different ways in the course of one
 * granule.** The Huffman data arrives in the order the encoder wrote it -
 * which for short blocks is band by band and then window by window - the
 * requantisation needs the scalefactor of the band it is in, and the
 * filterbank needs 18 consecutive lines per subband. The reordering
 * between the second and the third is a named step in the standard and a
 * named function here, and getting it wrong produces a spectrum that is
 * *almost* right, which is the kind of wrong that sounds like a bad
 * encoder rather than like a bug.
 *
 * **Everything is Q28 integer arithmetic.** planning/audio.md 11.1
 * promises byte-identical output on every architecture, and that promise
 * is kept by there being no float here to round differently.
 */

#include "mp3_internal.h"
#include "mp3_tables.h"
#include <string.h>

#ifdef GAUD_MP3_TRACE
#include <stdio.h>
unsigned long gaud_mp3_trace_counts[MP3_T_COUNT];
static const char * const trace_name[] = {
    "MPEG-1",
    "MPEG-2",
    "MPEG-2.5",
    "Layer I",
    "Layer II",
    "Layer III",
    "a frame with a CRC",
    "a padded frame",
    "a resynchronisation",
    "block type 0, long",
    "block type 1, start",
    "block type 2, three short",
    "block type 3, stop",
    "a mixed block",
    "a nonzero subblock gain",
    "preflag",
    "scalefac_scale set",
    "a scalefactor band reused from granule 0",
    "middle/side stereo",
    "intensity stereo",
    "an illegal intensity position",
    "linbits used",
    "count1 table A",
    "count1 table B",
    "the bit reservoir reached back",
    "a granule with no reservoir",
    "a sample that saturated",
    "Layer II grouped samples",
    "Layer II ungrouped samples",
    "Layer II intensity stereo",
    "Layer II scfsi 0, three scalefactors",
    "Layer II scfsi 1, two scalefactors",
    "Layer II scfsi 2, one scalefactor",
    "Layer II scfsi 3, two scalefactors",
    "a Layer I subband with bits allocated",
};
__attribute__((destructor)) static void trace_dump(void) {
  for (int i = 0; i < MP3_T_ALLOC_TABLE; ++i) {
    fprintf(stderr, "MPEGTRACE\t%s\t%lu\n", trace_name[i],
        gaud_mp3_trace_counts[i]);
  }
  for (int i = 0; i < 5; ++i) {
    fprintf(stderr, "MPEGTRACE\tLayer II allocation table %d\t%lu\n", i,
        gaud_mp3_trace_counts[MP3_T_ALLOC_TABLE + i]);
  }
  for (int i = 0; i < 32; ++i) {
    fprintf(stderr, "MPEGTRACE\tHuffman table %d\t%lu\n", i,
        gaud_mp3_trace_counts[MP3_T_HUFF + i]);
  }
}
#endif

/** The constant the requantisation formula subtracts from global_gain.
 *
 * 11172-3 2.4.3.4 calls it a system constant and prints 64, saying in the
 * same paragraph that it depends on the power transfer characteristic of
 * the filterbank the implementation chose. For the filterbank the standard
 * itself specifies - the matrixing and window of Figure 3-A.2, which is
 * what mp3_synth.c implements - the value is 210, which is what every
 * decoder in use subtracts and what makes this library's output agree in
 * level with the reference decoders. The gate that would catch it being
 * wrong is the absolute RMS check in `make check-mpeg`, not the
 * difference: being 2^36.5 out is not a subtle error. */
#define MP3_GAIN_BIAS 210

/** Multiply two Q28 values, rounding to nearest. */
static int32_t mul(int32_t a, int32_t b) {
  int64_t product = (int64_t)a * b;
  return (int32_t)((product + (1 << (MP3_Q - 1))) >> MP3_Q);
}

/** Clamp a Q28 accumulator into an int32. */
static int32_t saturate(MP3_Layer3 * state, int64_t value) {
  if (value > INT32_MAX) {
    ++state->saturated;
    MP3_TRACE(MP3_T_SATURATED);
    return INT32_MAX;
  }
  if (value < INT32_MIN) {
    ++state->saturated;
    MP3_TRACE(MP3_T_SATURATED);
    return INT32_MIN;
  }
  return (int32_t)value;
}

bool gaud_mp3_band_row(const MP3_Header * header, unsigned * out_row) {
  if (header->version == MP3_MPEG1) {
    *out_row = header->rate_index;
    return true;
  }
  if (header->version == MP3_MPEG2) {
    *out_row = 3u + header->rate_index;
    return true;
  }
  return false;
}

void gaud_mp3_layer3_reset(MP3_Layer3 * state) {
  gaud_mp3_reservoir_reset(&state->reservoir);
  for (unsigned ch = 0; ch < 2u; ++ch) {
    gaud_mp3_synth_reset(&state->channel[ch].synth);
    memset(state->channel[ch].overlap, 0, sizeof(state->channel[ch].overlap));
    memset(state->channel[ch].scalefac_long, 0,
        sizeof(state->channel[ch].scalefac_long));
    memset(state->channel[ch].scalefac_short, 0,
        sizeof(state->channel[ch].scalefac_short));
  }
}

/* ------------------------------------------------- side information */

/**
 * Read the side information, which is the fixed-size part after the header.
 *
 * MPEG-1 and the low sampling frequency versions differ in four ways and
 * all four are here: the back-pointer is nine bits rather than eight,
 * there are two granules rather than one, there is scalefactor reuse
 * information, and there is a preflag bit. MPEG-2 pays for its single
 * granule with a nine-bit scalefac_compress instead of four, which is
 * what carries the preflag it has no bit for.
 */
static GAUD_Result parse_side(
    MP3_Bits * bits, const MP3_Header * header, MP3_Side_Info * side) {
  bool lsf = header->version != MP3_MPEG1;
  unsigned channels = header->channels;
  unsigned granules = lsf ? 1u : 2u;

  memset(side, 0, sizeof(*side));
  side->main_data_begin = gaud_mp3_bits_read(bits, lsf ? 8u : 9u);
  /* The private bits are not data this library interprets; they are read
   * so that what follows is at the right offset. */
  (void)gaud_mp3_bits_read(
      bits, lsf ? (channels == 1u ? 1u : 2u) : (channels == 1u ? 5u : 3u));
  if (!lsf) {
    for (unsigned ch = 0; ch < channels; ++ch) {
      side->scfsi[ch] = (uint8_t)gaud_mp3_bits_read(bits, 4u);
    }
  }

  for (unsigned gr = 0; gr < granules; ++gr) {
    for (unsigned ch = 0; ch < channels; ++ch) {
      MP3_Granule * granule = &side->granule[gr][ch];
      granule->part2_3_length = gaud_mp3_bits_read(bits, 12u);
      granule->big_values = gaud_mp3_bits_read(bits, 9u);
      granule->global_gain = gaud_mp3_bits_read(bits, 8u);
      granule->scalefac_compress = gaud_mp3_bits_read(bits, lsf ? 9u : 4u);
      granule->window_switching = gaud_mp3_bits_read(bits, 1u) != 0;
      if (granule->window_switching) {
        granule->block_type = (uint8_t)gaud_mp3_bits_read(bits, 2u);
        granule->mixed_block = gaud_mp3_bits_read(bits, 1u) != 0;
        for (unsigned region = 0; region < 2u; ++region) {
          granule->table_select[region] = (uint8_t)gaud_mp3_bits_read(bits, 5u);
        }
        granule->table_select[2] = 0;
        for (unsigned window = 0; window < 3u; ++window) {
          granule->subblock_gain[window]
              = (uint8_t)gaud_mp3_bits_read(bits, 3u);
        }
        /* Not transmitted for a switched window; the standard states the
         * values these take. 13818-3's spelling of them is used, which
         * counts one less than the number of bands in the region - and
         * 11172-3's prose, which counts the bands themselves, gives the
         * same two boundaries by a different route. */
        granule->region0_count
            = granule->block_type == 2u && !granule->mixed_block ? 8u : 7u;
        granule->region1_count = 36u;
        if (granule->block_type == 0u) {
          /* Window switching with a normal window is the one combination
           * the format has no meaning for. */
          return GAUD_ERR_CORRUPT;
        }
      }
      else {
        for (unsigned region = 0; region < 3u; ++region) {
          granule->table_select[region] = (uint8_t)gaud_mp3_bits_read(bits, 5u);
        }
        granule->region0_count = (uint8_t)gaud_mp3_bits_read(bits, 4u);
        granule->region1_count = (uint8_t)gaud_mp3_bits_read(bits, 3u);
        granule->block_type = 0u;
        granule->mixed_block = false;
      }
      if (!lsf) {
        granule->preflag = gaud_mp3_bits_read(bits, 1u) != 0;
      }
      granule->scalefac_scale = gaud_mp3_bits_read(bits, 1u) != 0;
      granule->count1table_select = gaud_mp3_bits_read(bits, 1u) != 0;
      MP3_TRACE(MP3_T_BLOCK_LONG + granule->block_type);
      if (granule->mixed_block) {
        MP3_TRACE(MP3_T_MIXED);
      }
      if (granule->preflag) {
        MP3_TRACE(MP3_T_PREFLAG);
      }
      if (granule->scalefac_scale) {
        MP3_TRACE(MP3_T_SCALEFAC_SCALE);
      }
      MP3_TRACE(granule->count1table_select ? MP3_T_COUNT1_B : MP3_T_COUNT1_A);
      for (unsigned window = 0; window < 3u; ++window) {
        if (granule->subblock_gain[window]) {
          MP3_TRACE(MP3_T_SUBBLOCK_GAIN);
        }
      }
      for (unsigned region = 0; region < 3u; ++region) {
        MP3_TRACE(MP3_T_HUFF + granule->table_select[region]);
      }

      /* Two refusals, both of things the format forbids rather than
       * things this library cannot do. A table a frame may not select is
       * a frame that cannot be decoded at all, and a pair count past the
       * 576 lines a granule has would write off the end of the
       * spectrum. */
      for (unsigned region = 0; region < 3u; ++region) {
        if (gaud_mp3_huff[granule->table_select[region]].unused) {
          return GAUD_ERR_CORRUPT;
        }
      }
      if (granule->big_values > 288u) {
        return GAUD_ERR_CORRUPT;
      }
    }
  }
  return bits->overrun ? GAUD_ERR_CORRUPT : GAUD_OK;
}

/* --------------------------------------------------- scalefactors */

/** The four groups of bands scfsi lets the second granule reuse. */
static const unsigned scfsi_band[5] = {0u, 6u, 11u, 16u, 21u};

/** Read one granule's scalefactors, MPEG-1's way. */
static void scalefactors_mpeg1(MP3_Bits * bits, const MP3_Side_Info * side,
    unsigned gr, unsigned ch, MP3_Layer3_Channel * channel) {
  const MP3_Granule * granule = &side->granule[gr][ch];
  unsigned slen1 = gaud_mp3_slen[granule->scalefac_compress][0];
  unsigned slen2 = gaud_mp3_slen[granule->scalefac_compress][1];

  if (granule->block_type == 2u) {
    /* Short blocks have no reuse: the standard says scfsi is zero for a
     * frame in which either granule is short. */
    unsigned first = granule->mixed_block ? 3u : 0u;
    if (granule->mixed_block) {
      for (unsigned sfb = 0; sfb < 8u; ++sfb) {
        channel->scalefac_long[sfb] = (int32_t)gaud_mp3_bits_read(bits, slen1);
      }
    }
    for (unsigned sfb = first; sfb < 12u; ++sfb) {
      unsigned width = sfb < 6u ? slen1 : slen2;
      for (unsigned window = 0; window < 3u; ++window) {
        channel->scalefac_short[sfb][window]
            = (int32_t)gaud_mp3_bits_read(bits, width);
      }
    }
    /* Band 12 is not transmitted and is not coded; zeroing it keeps the
     * requantiser's band walk from reading a stale value if a file ever
     * puts data there. */
    for (unsigned window = 0; window < 3u; ++window) {
      channel->scalefac_short[12][window] = 0;
    }
    return;
  }

  for (unsigned group = 0; group < 4u; ++group) {
    bool reuse = gr == 1u && (side->scfsi[ch] & (8u >> group)) != 0u;
    for (unsigned sfb = scfsi_band[group]; sfb < scfsi_band[group + 1u];
        ++sfb) {
      if (reuse) {
        MP3_TRACE(MP3_T_SCFSI);
        continue; /* Granule 0's value stands. */
      }
      channel->scalefac_long[sfb]
          = (int32_t)gaud_mp3_bits_read(bits, sfb < 11u ? slen1 : slen2);
    }
  }
  channel->scalefac_long[21] = 0;
  channel->scalefac_long[22] = 0;
}

/**
 * Read one granule's scalefactors, MPEG-2's way.
 *
 * Arithmetic rather than a table lookup: 13818-3 derives four field
 * widths and a preflag from the nine bits of scalefac_compress, in three
 * ranges for an ordinary channel and three more for the right channel of
 * an intensity-coded pair. The partition sizes are the only table, and the
 * generator checked that each of the six adds to the number of
 * scalefactors the block shape has.
 */
static void scalefactors_lsf(MP3_Bits * bits, MP3_Side_Info * side, unsigned ch,
    bool intensity_right, MP3_Layer3_Channel * channel) {
  MP3_Granule * granule = &side->granule[0][ch];
  uint32_t compress = granule->scalefac_compress;
  unsigned slen[4] = {0, 0, 0, 0};
  unsigned group;

  if (!intensity_right) {
    if (compress < 400u) {
      slen[0] = (compress >> 4) / 5u;
      slen[1] = (compress >> 4) % 5u;
      slen[2] = (compress % 16u) >> 2;
      slen[3] = compress % 4u;
      group = 0u;
      granule->preflag = false;
    }
    else if (compress < 500u) {
      slen[0] = ((compress - 400u) >> 2) / 5u;
      slen[1] = ((compress - 400u) >> 2) % 5u;
      slen[2] = (compress - 400u) % 4u;
      slen[3] = 0u;
      group = 1u;
      granule->preflag = false;
    }
    else {
      slen[0] = (compress - 500u) / 3u;
      slen[1] = (compress - 500u) % 3u;
      slen[2] = 0u;
      slen[3] = 0u;
      group = 2u;
      granule->preflag = true;
    }
  }
  else {
    uint32_t scaled = compress >> 1;
    if (scaled < 180u) {
      slen[0] = scaled / 36u;
      slen[1] = (scaled % 36u) / 6u;
      slen[2] = (scaled % 36u) % 6u;
      slen[3] = 0u;
      group = 3u;
    }
    else if (scaled < 244u) {
      slen[0] = ((scaled - 180u) % 64u) >> 4;
      slen[1] = ((scaled - 180u) % 16u) >> 2;
      slen[2] = (scaled - 180u) % 4u;
      slen[3] = 0u;
      group = 4u;
    }
    else {
      slen[0] = (scaled - 244u) / 3u;
      slen[1] = (scaled - 244u) % 3u;
      slen[2] = 0u;
      slen[3] = 0u;
      group = 5u;
    }
    granule->preflag = false;
  }

  unsigned shape
      = granule->block_type != 2u ? 0u : (granule->mixed_block ? 2u : 1u);
  const uint8_t * counts = gaud_mp3_lsf_nsfb[group][shape];

  /* The scalefactors arrive as one run, and where they land depends on
   * the block shape: a long granule fills the long array, a short one
   * fills the short array window by window, and a mixed one fills the
   * first six long bands and then the short array from band 3. */
  unsigned index = 0;
  unsigned total = 0;
  for (unsigned partition = 0; partition < 4u; ++partition) {
    total += counts[partition];
  }
  int32_t values[40];
  for (unsigned i = 0; i < total && i < 40u; ++i) {
    values[i] = 0;
  }
  for (unsigned partition = 0; partition < 4u; ++partition) {
    for (unsigned n = 0; n < counts[partition]; ++n) {
      if (index >= 40u) {
        return;
      }
      values[index++] = (int32_t)gaud_mp3_bits_read(bits, slen[partition]);
    }
  }

  memset(channel->scalefac_long, 0, sizeof(channel->scalefac_long));
  memset(channel->scalefac_short, 0, sizeof(channel->scalefac_short));
  index = 0;
  if (granule->block_type != 2u) {
    for (unsigned sfb = 0; sfb < 21u && index < total; ++sfb) {
      channel->scalefac_long[sfb] = values[index++];
    }
    return;
  }
  unsigned first = 0u;
  if (granule->mixed_block) {
    for (unsigned sfb = 0; sfb < 6u && index < total; ++sfb) {
      channel->scalefac_long[sfb] = values[index++];
    }
    first = 3u;
  }
  for (unsigned sfb = first; sfb < 12u; ++sfb) {
    for (unsigned window = 0; window < 3u; ++window) {
      if (index >= total) {
        return;
      }
      channel->scalefac_short[sfb][window] = values[index++];
    }
  }
}

/* ------------------------------------------------ Huffman decoding */

/** Walk one tree and return the leaf's payload, or -1 past the end. */
static int walk(MP3_Bits * bits, const int16_t * nodes, unsigned offset) {
  unsigned at = offset;
  for (unsigned step = 0; step < 32u; ++step) {
    unsigned bit = gaud_mp3_bits_read(bits, 1u);
    int16_t entry = nodes[at + bit];
    if (entry < 0) {
      return -(int)entry - 1;
    }
    at = offset + 2u * (unsigned)entry;
  }
  return -1;
}

/** Read one value's magnitude extension and sign. */
static int32_t finish_value(MP3_Bits * bits, int32_t value, unsigned linbits) {
  if (value == 15 && linbits) {
    MP3_TRACE(MP3_T_LINBITS);
    value += (int32_t)gaud_mp3_bits_read(bits, linbits);
  }
  if (value == 0) {
    return 0;
  }
  return gaud_mp3_bits_read(bits, 1u) ? -value : value;
}

/**
 * Decode one granule's spectral values.
 *
 * Three regions of pairs, each with its own Huffman table, then a region
 * of quadruples whose values are -1, 0 or 1, then zeros to 576. Stops at
 * whichever comes first of 576 values and the end of the granule's bits,
 * which is what the standard says and is not the same thing: a granule
 * may carry stuffing bits after its data, and a granule may also run out
 * of bits before 576 values, in which case the rest of the spectrum is
 * zero.
 *
 * @return how many values were decoded, which the stereo step needs as
 *   the bound of the non-zero part.
 */
static unsigned decode_spectrum(MP3_Bits * bits, const MP3_Granule * granule,
    size_t end_bit, unsigned row, int32_t is[576]) {
  memset(is, 0, 576u * sizeof(is[0]));
  unsigned big = granule->big_values * 2u;
  if (big > 576u) {
    big = 576u;
  }

  const uint16_t * boundaries = gaud_mp3_sfb_long[row];
  unsigned bands = gaud_mp3_sfb_long_bands[row];
  unsigned region[3];
  if (granule->window_switching) {
    /* The standard states these rather than transmitting them. For three
     * short windows the first region is the first 36 lines, which is the
     * first three short bands counted once per window; otherwise it is
     * the first eight long bands. */
    region[0] = granule->block_type == 2u && !granule->mixed_block
        ? 36u
        : boundaries[8];
    region[1] = 576u;
  }
  else {
    unsigned first = granule->region0_count + 1u;
    unsigned second = first + granule->region1_count + 1u;
    if (first > bands) {
      first = bands;
    }
    if (second > bands) {
      second = bands;
    }
    region[0] = boundaries[first];
    region[1] = boundaries[second];
  }
  region[2] = big;
  for (unsigned i = 0; i < 2u; ++i) {
    if (region[i] > big) {
      region[i] = big;
    }
  }
  if (region[1] < region[0]) {
    region[1] = region[0];
  }

  unsigned at = 0;
  for (unsigned part = 0; part < 3u; ++part) {
    const MP3_Huff * table = &gaud_mp3_huff[granule->table_select[part]];
    while (at < region[part]) {
      if (bits->at >= end_bit || bits->overrun) {
        return at;
      }
      int32_t x = 0;
      int32_t y = 0;
      if (table->width != 0u) {
        int payload = walk(bits, gaud_mp3_huff_nodes, table->offset);
        if (payload < 0) {
          return at;
        }
        x = finish_value(bits, (payload >> 4) & 15, table->linbits);
        y = finish_value(bits, payload & 15, table->linbits);
      }
      is[at] = x;
      is[at + 1u] = y;
      at += 2u;
    }
  }

  /* The quadruple region. Its values are -1, 0 or 1, so the code carries
   * one bit per value and a sign follows each non-zero one. */
  unsigned quad = gaud_mp3_quad_offset[granule->count1table_select ? 1u : 0u];
  while (at + 4u <= 576u) {
    if (bits->at >= end_bit || bits->overrun) {
      break;
    }
    int payload = walk(bits, gaud_mp3_quad_nodes, quad);
    if (payload < 0) {
      break;
    }
    for (unsigned i = 0; i < 4u; ++i) {
      int32_t value = (payload >> (3u - i)) & 1;
      if (value) {
        value = gaud_mp3_bits_read(bits, 1u) ? -1 : 1;
      }
      is[at + i] = value;
    }
    at += 4u;
  }
  return at;
}

/* ------------------------------------------------- requantisation */

/**
 * One value, requantised.
 *
 * `|is|^(4/3) * 2^(gain/4)`, with the table's mantissa and exponent doing
 * the first half and a shift and one of four multipliers doing the
 * second. The exponent arrives in quarters because that is the resolution
 * global_gain has, and the quarter that is left after the whole part is
 * taken out as a shift is what gaud_mp3_gain_frac is for.
 */
static int32_t requantize_one(MP3_Layer3 * state, int32_t value, int gain4) {
  if (value == 0) {
    return 0;
  }
  int32_t magnitude = value < 0 ? -value : value;
  if (magnitude > 8206) {
    magnitude = 8206; /* linbits cannot reach past the table. */
  }
  uint32_t packed = gaud_mp3_pow43[magnitude];
  int64_t mantissa = packed & 0xFFFFFFu;
  int exponent = (int)(packed >> 24);

  /* The quarter, floored, so that the remainder is always 0 to 3 and the
   * multiplier below is never applied with the wrong sign. */
  int whole = gain4 >> 2;
  unsigned rest = (unsigned)(gain4 & 3);
  /* Non-negative throughout: the table's mantissa and the multiplier are
   * both positive and the sign is applied at the end. That is what makes
   * the shift below defined rather than undefined behaviour. */
  int64_t scaled = mantissa * gaud_mp3_gain_frac[rest];
  /* mantissa is 24 bits and the multiplier is Q28, so the product is
   * Q(23 + 28) relative to 2^exponent. Bringing it to Q28 is a shift of
   * 23 - exponent - whole, which can go either way. */
  int shift = 23 - exponent - whole;
  int64_t result;
  if (shift >= 0) {
    if (shift > 62) {
      return 0; /* Scaled so far down that nothing survives. */
    }
    result = scaled >> shift;
  }
  else {
    if (-shift > 24) {
      /* Scaled so far up that it cannot be represented. Saturating
       * rather than wrapping: see the note on saturate(). */
      result = value < 0 ? INT64_MIN : INT64_MAX;
    }
    else {
      result = scaled << -shift;
    }
  }
  if (value < 0) {
    result = -result;
  }
  return saturate(state, result);
}

/** The gain exponent, in quarters, for one band's worth of lines. */
static int band_gain(const MP3_Granule * granule, int32_t scalefac,
    unsigned pretab, unsigned subblock_gain) {
  int gain = (int)granule->global_gain - MP3_GAIN_BIAS - 8 * (int)subblock_gain;
  int step = granule->scalefac_scale ? 4 : 2;
  gain -= step * ((int)scalefac + (int)pretab);
  return gain;
}

/**
 * Requantise one granule, and put its lines where the filterbank wants
 * them.
 *
 * For a long block the two orders are the same and this is a walk over
 * the bands. For three short windows they are not: the values arrive band
 * by band and then window by window within each band, and the filterbank
 * wants eighteen consecutive lines per subband, six from each window. The
 * scatter at the end of the short path is the standard's "reordering"
 * step.
 */
static void requantize(MP3_Layer3 * state, const MP3_Header * header,
    const MP3_Granule * granule, const MP3_Layer3_Channel * channel,
    unsigned row, const int32_t is[576], unsigned count, int32_t xr[576]) {
  (void)header;
  memset(xr, 0, 576u * sizeof(xr[0]));
  const uint16_t * longs = gaud_mp3_sfb_long[row];
  unsigned long_bands = gaud_mp3_sfb_long_bands[row];
  const uint16_t * shorts = gaud_mp3_sfb_short[row];
  unsigned short_bands = gaud_mp3_sfb_short_bands[row];

  if (granule->block_type != 2u) {
    /* The band tables reach the top of the spectrum because the
     * generator padded them there: 11172-3's own long tables stop at
     * about 16 kHz, the lines above have no transmitted scalefactor, and
     * the padding band's scalefactor is zero. See the comment on
     * gaud_mp3_sfb_long; the measurement that forced it is recorded in
     * notes/audio/mpeg.md. */
    for (unsigned band = 0; band < long_bands; ++band) {
      unsigned from = longs[band];
      unsigned to = longs[band + 1u];
      int gain = band_gain(granule, channel->scalefac_long[band],
          granule->preflag ? gaud_mp3_pretab[band] : 0u, 0u);
      for (unsigned i = from; i < to && i < count; ++i) {
        xr[i] = requantize_one(state, is[i], gain);
      }
    }
    return;
  }

  /* Three short windows. A mixed block keeps its lowest 36 lines long,
   * which is two polyphase subbands; the standard allows the long and
   * short band tables to disagree about where that boundary falls and
   * every rate in both documents puts the first three short bands at
   * exactly 36 lines, so 36 is what is used. */
  unsigned at = 0;
  unsigned first_short = 0u;
  if (granule->mixed_block) {
    for (unsigned band = 0; band < 8u && longs[band] < 36u; ++band) {
      unsigned from = longs[band];
      unsigned to = longs[band + 1u] < 36u ? longs[band + 1u] : 36u;
      int gain = band_gain(granule, channel->scalefac_long[band], 0u, 0u);
      for (unsigned i = from; i < to; ++i) {
        if (i < count) {
          xr[i] = requantize_one(state, is[i], gain);
        }
        ++at;
      }
    }
    first_short = 3u;
  }

  for (unsigned band = first_short; band < short_bands; ++band) {
    unsigned from = shorts[band];
    unsigned width = shorts[band + 1u] - from;
    for (unsigned window = 0; window < 3u; ++window) {
      int gain = band_gain(granule, channel->scalefac_short[band][window], 0u,
          granule->subblock_gain[window]);
      for (unsigned k = 0; k < width; ++k, ++at) {
        if (at >= count) {
          continue;
        }
        int32_t value = requantize_one(state, is[at], gain);
        /* The reordering, in one line: line `from + k` of window
         * `window` belongs to subband (from + k) / 6, at offset
         * `window * 6 + (from + k) % 6` inside it. */
        unsigned line = from + k;
        unsigned target = (line / 6u) * 18u + window * 6u + (line % 6u);
        if (target < 576u) {
          xr[target] = value;
        }
      }
    }
  }
}

/* ------------------------------------------------------- stereo */

/** Which band table a granule's bands come from, and how many there are. */
static void band_extent(const MP3_Granule * granule, unsigned row,
    const uint16_t ** out_bands, unsigned * out_count, bool * out_short) {
  if (granule->block_type == 2u) {
    *out_bands = gaud_mp3_sfb_short[row];
    *out_count = gaud_mp3_sfb_short_bands[row];
    *out_short = true;
  }
  else {
    *out_bands = gaud_mp3_sfb_long[row];
    *out_count = gaud_mp3_sfb_long_bands[row];
    *out_short = false;
  }
}

/**
 * Middle/side and intensity stereo, per 11172-3 2.4.3.4.
 *
 * The two are independent switches in the header's mode extension and
 * both can be on. Where they meet is the part worth stating: intensity
 * stereo applies from the first band above the *right* channel's
 * non-zero part, the middle/side matrix applies below that, and a band
 * whose intensity position is the illegal one is handled as though
 * intensity stereo were off - which means by the matrix if it is on.
 */
static void stereo(MP3_Layer3 * state, const MP3_Header * header,
    const MP3_Granule * granule, const MP3_Layer3_Channel * right, unsigned row,
    bool intensity, bool middle_side) {
  int32_t * left = state->xr[0];
  int32_t * side = state->xr[1];
  bool lsf = header->version != MP3_MPEG1;
  unsigned illegal = lsf ? 15u : 7u;

  const uint16_t * bands;
  unsigned count;
  bool is_short;
  band_extent(granule, row, &bands, &count, &is_short);

  /* Where the right channel stops carrying its own spectrum. Everything
   * above it is intensity stereo if intensity stereo is on. */
  unsigned bound = intensity ? state->nonzero[1] : 576u;

  for (unsigned band = 0; band < count; ++band) {
    for (unsigned window = 0; window < (is_short ? 3u : 1u); ++window) {
      unsigned from;
      unsigned to;
      if (is_short) {
        /* After the reordering, a short band's lines are not contiguous:
         * each of its six-line groups sits inside a subband. The band is
         * walked in the reordered layout for that reason. */
        from = bands[band];
        to = bands[band + 1u];
      }
      else {
        from = bands[band];
        to = bands[band + 1u];
      }
      int32_t position = is_short ? right->scalefac_short[band][window]
                                  : right->scalefac_long[band];
      bool is_intensity = intensity && from * (is_short ? 3u : 1u) >= bound;
      if (is_intensity && (uint32_t)position == illegal) {
        MP3_TRACE(MP3_T_INTENSITY_ILLEGAL);
        is_intensity = false;
      }

      for (unsigned line = from; line < to; ++line) {
        unsigned index;
        if (is_short) {
          index = (line / 6u) * 18u + window * 6u + (line % 6u);
        }
        else {
          index = line;
        }
        if (index >= 576u) {
          continue;
        }
        if (is_intensity) {
          const int32_t * weight = lsf
              ? gaud_mp3_is_weight_lsf[granule->scalefac_compress & 1u]
                                      [position]
              : gaud_mp3_is_weight[position];
          /* The right channel is computed from the *original* left, then
           * the left is overwritten. 13818-3 spells the order out where
           * 11172-3's sequential assignment could be read either way,
           * and the other reading loses energy. */
          int32_t value = left[index];
          side[index] = mul(value, weight[1]);
          left[index] = mul(value, weight[0]);
        }
        else if (middle_side) {
          int32_t middle = left[index];
          int32_t difference = side[index];
          left[index] = mul(middle + difference, gaud_mp3_inv_sqrt2);
          side[index] = mul(middle - difference, gaud_mp3_inv_sqrt2);
        }
      }
    }
  }
}

/* -------------------------------------------- the hybrid filterbank */

/** The eight butterflies at each subband boundary, 11172-3 Figure 3-A.5. */
static void antialias(int32_t xr[576], const MP3_Granule * granule) {
  unsigned boundaries = 31u;
  if (granule->block_type == 2u) {
    /* Three short windows have no aliasing to reduce - the butterflies
     * exist to undo the overlap of long transforms - except across the
     * long part of a mixed block, which is its first two subbands. */
    boundaries = granule->mixed_block ? 1u : 0u;
  }
  for (unsigned boundary = 1u; boundary <= boundaries; ++boundary) {
    int32_t * lower = xr + 18u * boundary;
    for (unsigned i = 0; i < 8u; ++i) {
      int32_t above = lower[i];
      int32_t below = lower[-1 - (int)i];
      lower[-1 - (int)i]
          = mul(below, gaud_mp3_cs[i]) - mul(above, gaud_mp3_ca[i]);
      lower[i] = mul(above, gaud_mp3_cs[i]) + mul(below, gaud_mp3_ca[i]);
    }
  }
}

/**
 * One subband's inverse transform, windowing and overlap.
 *
 * @param in The subband's 18 frequency lines.
 * @param overlap The 18 values stored from the previous granule, replaced
 *   with this one's second half.
 * @param out The 18 time samples this granule contributes.
 */
static void hybrid(const int32_t in[18], const MP3_Granule * granule,
    bool force_long, int32_t overlap[18], int32_t out[18]) {
  int32_t block[36];
  if (granule->block_type == 2u && !force_long) {
    memset(block, 0, sizeof(block));
    for (unsigned window = 0; window < 3u; ++window) {
      for (unsigned i = 0; i < 12u; ++i) {
        int64_t sum = 0;
        for (unsigned k = 0; k < 6u; ++k) {
          sum += (int64_t)in[window * 6u + k] * gaud_mp3_imdct12[i][k];
        }
        int32_t value = (int32_t)(sum >> MP3_Q);
        /* Each short block is windowed on its own and then the three are
         * overlapped six values apart, which is what spreads twelve
         * samples over the middle twenty-four of the thirty-six. */
        block[6u + window * 6u + i] += mul(value, gaud_mp3_short_window[i]);
      }
    }
  }
  else {
    const int32_t * window
        = gaud_mp3_block_window[force_long && granule->block_type == 2u
                ? 0u
                : granule->block_type];
    for (unsigned i = 0; i < 36u; ++i) {
      int64_t sum = 0;
      for (unsigned k = 0; k < 18u; ++k) {
        sum += (int64_t)in[k] * gaud_mp3_imdct36[i][k];
      }
      block[i] = mul((int32_t)(sum >> MP3_Q), window[i]);
    }
  }
  for (unsigned i = 0; i < 18u; ++i) {
    out[i] = block[i] + overlap[i];
    overlap[i] = block[18u + i];
  }
}

/* ------------------------------------------------------ the frame */

GAUD_Result gaud_mp3_layer3_frame(MP3_Layer3 * state, const MP3_Header * header,
    const unsigned char * frame, size_t size, int32_t * pcm) {
  unsigned row = 0;
  if (!gaud_mp3_band_row(header, &row)) {
    return GAUD_ERR_UNSUPPORTED;
  }
  unsigned channels = header->channels;
  unsigned granules = header->version == MP3_MPEG1 ? 2u : 1u;
  uint32_t side_size = gaud_mp3_side_info_size(header);
  size_t head = MP3_HEADER_SIZE + (header->crc_present ? 2u : 0u);
  if (size < head + side_size) {
    return GAUD_ERR_CORRUPT;
  }

  MP3_Bits side_bits;
  gaud_mp3_bits_init(&side_bits, frame + head, side_size);
  GAUD_Result result = parse_side(&side_bits, header, &state->side);
  if (result != GAUD_OK) {
    return result;
  }

  MP3_TRACE(MP3_T_V1 + (int)header->version);
  MP3_TRACE(MP3_T_L3);
  if (header->crc_present) {
    MP3_TRACE(MP3_T_CRC);
  }
  if (header->padding) {
    MP3_TRACE(MP3_T_PADDED);
  }
  if (state->side.main_data_begin) {
    MP3_TRACE(MP3_T_RESERVOIR);
  }
  bool grounded
      = gaud_mp3_reservoir_push(&state->reservoir, state->side.main_data_begin,
          frame + head + side_size, size - head - side_size);

  memset(pcm, 0, (size_t)header->samples * channels * sizeof(pcm[0]));
  if (!grounded) {
    /* The frame's main data starts before anything this decoder has
     * seen: the stream's first frames, or the first frame after a seek.
     * Its granules are silence, which is what every decoder does and
     * what keeps the frame count exact - and it is counted, so that a
     * caller can tell this from a file that contains silence. */
    state->ungrounded += granules;
    for (unsigned gr = 0; gr < granules; ++gr) {
      MP3_TRACE(MP3_T_UNGROUNDED);
    }
    gaud_mp3_reservoir_trim(&state->reservoir);
    return GAUD_OK;
  }

  MP3_Bits main_bits;
  gaud_mp3_bits_init(
      &main_bits, state->reservoir.data, state->reservoir.length);
  size_t at = 0;

  bool intensity = header->mode == MP3_MODE_JOINT_STEREO
      && (header->mode_extension & 1u) != 0u;
  bool middle_side = header->mode == MP3_MODE_JOINT_STEREO
      && (header->mode_extension & 2u) != 0u;

  for (unsigned gr = 0; gr < granules; ++gr) {
    for (unsigned ch = 0; ch < channels; ++ch) {
      const MP3_Granule * granule = &state->side.granule[gr][ch];
      size_t end = at + granule->part2_3_length;
      gaud_mp3_bits_seek(&main_bits, at);
      if (header->version == MP3_MPEG1) {
        scalefactors_mpeg1(
            &main_bits, &state->side, gr, ch, &state->channel[ch]);
      }
      else {
        scalefactors_lsf(&main_bits, &state->side, ch, intensity && ch == 1u,
            &state->channel[ch]);
      }
      int32_t is[576];
      state->nonzero[ch] = decode_spectrum(&main_bits, granule, end, row, is);
      requantize(state, header, granule, &state->channel[ch], row, is,
          state->nonzero[ch], state->xr[ch]);
      at = end;
    }

    if (intensity) {
      MP3_TRACE(MP3_T_INTENSITY);
    }
    if (middle_side) {
      MP3_TRACE(MP3_T_MS_STEREO);
    }
    if (channels == 2u && (intensity || middle_side)) {
      stereo(state, header, &state->side.granule[gr][1], &state->channel[1],
          row, intensity, middle_side);
    }

    for (unsigned ch = 0; ch < channels; ++ch) {
      const MP3_Granule * granule = &state->side.granule[gr][ch];
      antialias(state->xr[ch], granule);
      for (unsigned sb = 0; sb < 32u; ++sb) {
        bool force_long = granule->mixed_block && sb < 2u;
        int32_t out[18];
        hybrid(state->xr[ch] + sb * 18u, granule, force_long,
            state->channel[ch].overlap[sb], out);
        /* Frequency inversion: every second value of every second
         * subband, to undo the polyphase filterbank's own inversion. */
        for (unsigned i = 0; i < 18u; ++i) {
          state->scratch[sb * 18u + i]
              = (sb & 1u) && (i & 1u) ? -out[i] : out[i];
        }
      }
      /* Eighteen passes of the polyphase filterbank, one per time slot,
       * each turning 32 subband values into 32 consecutive samples. */
      for (unsigned slot = 0; slot < 18u; ++slot) {
        int32_t subband[32];
        for (unsigned sb = 0; sb < 32u; ++sb) {
          subband[sb] = state->scratch[sb * 18u + slot];
        }
        gaud_mp3_synth_run(&state->channel[ch].synth, subband,
            pcm + (size_t)(gr * 576u + slot * 32u) * channels + ch, channels);
      }
    }
  }
  gaud_mp3_reservoir_trim(&state->reservoir);
  return GAUD_OK;
}
