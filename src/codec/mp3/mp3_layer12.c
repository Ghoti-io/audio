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
 * Layers I and II: subband samples, quantised directly.
 *
 * **These are the layers without an MDCT**, and that is the whole
 * difference. A Layer I or II frame carries quantised samples of the 32
 * polyphase subbands and nothing else: no Huffman coding, no bit
 * reservoir, no window switching, no overlap. So this file has no state
 * but the filterbank's, every frame is self-contained, and the two layers
 * differ from each other only in how many samples a frame holds and how
 * the quantiser is chosen.
 *
 * Layer I: a four-bit allocation per subband says how many bits each of
 * its twelve samples takes. Layer II: a two-to-four-bit allocation
 * indexes a table, the table says how many levels, and a second table
 * says how many bits that is and whether three samples share one codeword.
 *
 * The requantisation formula is one formula for both, and that is the
 * standard's doing rather than a simplification here: 11172-3 2.4.3.2
 * gives Layer I's as `2^nb/(2^nb - 1) * (s + 2^(-nb+1))` and 2.4.3.3
 * gives Layer II's as `C * (s + D)`, and the constants in Table 3-B.4 for
 * a class of `2^nb - 1` levels are exactly that C and that D.
 */

#include "mp3_internal.h"
#include "mp3_tables.h"
#include <string.h>

/** Multiply two Q28 values, rounding to nearest. */
static int32_t mul(int32_t a, int32_t b) {
  int64_t product = (int64_t)a * b;
  return (int32_t)((product + (1 << (MP3_Q - 1))) >> MP3_Q);
}

/**
 * Which Layer II allocation table a frame uses.
 *
 * Read off the headings of 11172-3 Tables 3-B.2a to 3-B.2d, which state
 * the sampling frequencies and per-channel bitrates each covers, plus
 * 13818-3's single table for the three lower frequencies. The per-channel
 * bitrate is the frame's divided by its channel count, which is what
 * makes a 128 kbit/s stereo frame and a 64 kbit/s mono one use the same
 * table - they carry the same number of bits per subband.
 */
static unsigned layer2_table(const MP3_Header * header) {
  if (header->version != MP3_MPEG1) {
    return 4u;
  }
  uint32_t per_channel = header->bitrate / header->channels / 1000u;
  uint32_t rate = header->sample_rate;
  if ((rate == 48000u && per_channel >= 56u)
      || (per_channel >= 56u && per_channel <= 80u)) {
    return 0u;
  }
  if (rate != 48000u && per_channel >= 96u) {
    return 1u;
  }
  if (rate != 32000u && per_channel <= 48u) {
    return 2u;
  }
  return 3u;
}

/** The lowest subband coded in intensity stereo, or 32 for none. */
static unsigned intensity_bound(const MP3_Header * header, unsigned limit) {
  if (header->mode != MP3_MODE_JOINT_STEREO) {
    return limit;
  }
  /* Layers I and II spell joint stereo as intensity stereo over the
   * subbands from a bound the mode extension names: 4, 8, 12 or 16. */
  unsigned bound = 4u << header->mode_extension;
  return bound < limit ? bound : limit;
}

/**
 * One sample, requantised and rescaled.
 *
 * @param raw The sample's own code, @p class->sample_bits wide.
 * @param factor The subband's scalefactor, Q28.
 *
 * The first bit is inverted and what remains is a two's complement
 * fraction whose most significant bit is worth -1, which is the
 * standard's wording and not an implementation choice: the quantiser is
 * symmetric about zero and its codes are offset so that the all-zeros
 * code is the most negative value rather than silence.
 */
static int32_t requantize(
    const MP3_Quant_Class * class, uint32_t raw, int32_t factor) {
  unsigned width = class->sample_bits;
  uint32_t flipped = raw ^ (1u << (width - 1u));
  int32_t value = (int32_t)flipped;
  if (flipped & (1u << (width - 1u))) {
    value -= (int32_t)(1u << width);
  }
  /* The fraction, in Q28: a width-bit two's complement value divided by
   * 2^(width-1).
   *
   * A multiplication and not a shift, because `value` is signed and may
   * be negative, and shifting a negative value left is undefined
   * behaviour - which UBSan says out loud and every compiler in
   * existence does the obvious thing with. The product cannot overflow:
   * the value is at most 2^(width-1) in magnitude and the multiplier is
   * 2^(29-width), so the result is at most 2^28, which is 1.0. */
  int32_t fraction = value * (int32_t)(1u << (MP3_Q - width + 1u));
  int32_t offset = fraction + class->d;
  return mul(mul(offset, class->c), factor);
}

void gaud_mp3_layer12_reset(MP3_Layer12 * state) {
  gaud_mp3_synth_reset(&state->synth[0]);
  gaud_mp3_synth_reset(&state->synth[1]);
}

/** Layer I: twelve samples per subband, one allocation field each. */
static GAUD_Result layer1_frame(MP3_Layer12 * state, MP3_Bits * bits,
    const MP3_Header * header, int32_t * pcm) {
  unsigned channels = header->channels;
  unsigned bound = intensity_bound(header, 32u);
  unsigned allocation[2][32];
  int32_t factor[2][32];

  for (unsigned sb = 0; sb < 32u; ++sb) {
    unsigned shared = sb >= bound ? 1u : channels;
    for (unsigned ch = 0; ch < shared; ++ch) {
      allocation[ch][sb] = gaud_mp3_bits_read(bits, 4u);
      if (allocation[ch][sb] == 15u) {
        /* The one allocation value the format forbids. */
        return GAUD_ERR_CORRUPT;
      }
    }
    for (unsigned ch = shared; ch < channels; ++ch) {
      allocation[ch][sb] = allocation[0][sb];
    }
  }
  for (unsigned sb = 0; sb < 32u; ++sb) {
    for (unsigned ch = 0; ch < channels; ++ch) {
      factor[ch][sb] = allocation[ch][sb]
          ? gaud_mp3_scalefactor[gaud_mp3_bits_read(bits, 6u)]
          : 0;
    }
  }

  MP3_TRACE(MP3_T_V1 + (int)header->version);
  MP3_TRACE(MP3_T_L1);
  for (unsigned slot = 0; slot < 12u; ++slot) {
    int32_t subband[2][32];
    memset(subband, 0, sizeof(subband));
    for (unsigned sb = 0; sb < 32u; ++sb) {
      unsigned shared = sb >= bound ? 1u : channels;
      for (unsigned ch = 0; ch < shared; ++ch) {
        if (allocation[ch][sb] == 0u) {
          continue;
        }
        /* An allocation of n means n+1 bits, so a class of 2^(n+1) - 1
         * levels. **Not the class at that index**: Table 3-B.4's first
         * four rows are 3, 5, 7 and 9 levels, so only the rows from 15
         * upwards line up with a power of two minus one. The generated
         * map is the correspondence, searched for rather than assumed. */
        MP3_TRACE(MP3_T_L1_ALLOCATED);
        unsigned width = allocation[ch][sb] + 1u;
        const MP3_Quant_Class * class = &gaud_mp3_classes
                                            [gaud_mp3_class_for_bits[width]];
        uint32_t raw = gaud_mp3_bits_read(bits, width);
        for (unsigned target = ch; target < (sb >= bound ? channels : ch + 1u);
            ++target) {
          subband[target][sb] = requantize(class, raw, factor[target][sb]);
        }
      }
    }
    for (unsigned ch = 0; ch < channels; ++ch) {
      gaud_mp3_synth_run(&state->synth[ch], subband[ch],
          pcm + (size_t)slot * 32u * channels + ch, channels);
    }
  }
  return bits->overrun ? GAUD_ERR_CORRUPT : GAUD_OK;
}

/** Layer II: thirty-six samples per subband, in three scalefactor parts. */
static GAUD_Result layer2_frame(MP3_Layer12 * state, MP3_Bits * bits,
    const MP3_Header * header, int32_t * pcm) {
  unsigned channels = header->channels;
  unsigned table = layer2_table(header);
  unsigned limit = gaud_mp3_alloc_sblimit[table];
  unsigned bound = intensity_bound(header, limit);
  int8_t class_index[2][32];
  unsigned scfsi[2][32];
  int32_t factor[2][32][3];

  MP3_TRACE(MP3_T_V1 + (int)header->version);
  MP3_TRACE(MP3_T_L2);
  MP3_TRACE(MP3_T_ALLOC_TABLE + table);
  if (bound < limit) {
    MP3_TRACE(MP3_T_L2_INTENSITY);
  }
  memset(class_index, -1, sizeof(class_index));
  for (unsigned sb = 0; sb < limit; ++sb) {
    unsigned shared = sb >= bound ? 1u : channels;
    unsigned nbal = gaud_mp3_alloc_nbal[table][sb];
    for (unsigned ch = 0; ch < shared; ++ch) {
      unsigned value = gaud_mp3_bits_read(bits, nbal);
      class_index[ch][sb] = gaud_mp3_alloc[table][sb][value];
    }
    for (unsigned ch = shared; ch < channels; ++ch) {
      class_index[ch][sb] = class_index[0][sb];
    }
  }
  for (unsigned sb = 0; sb < limit; ++sb) {
    for (unsigned ch = 0; ch < channels; ++ch) {
      scfsi[ch][sb]
          = class_index[ch][sb] >= 0 ? gaud_mp3_bits_read(bits, 2u) : 0u;
    }
  }
  for (unsigned sb = 0; sb < limit; ++sb) {
    for (unsigned ch = 0; ch < channels; ++ch) {
      factor[ch][sb][0] = 0;
      factor[ch][sb][1] = 0;
      factor[ch][sb][2] = 0;
      if (class_index[ch][sb] < 0) {
        continue;
      }
      /* One, two or three scalefactors for the frame's three parts, by
       * 11172-3 2.4.3.3: '00' is three, '01' is two with the first
       * covering parts 0 and 1, '10' is one for all three, and '11' is
       * two with the second covering parts 1 and 2. */
      int32_t first = gaud_mp3_scalefactor[gaud_mp3_bits_read(bits, 6u)];
      MP3_TRACE(MP3_T_L2_SCFSI_0 + scfsi[ch][sb]);
      switch (scfsi[ch][sb]) {
      case 0u: {
        factor[ch][sb][0] = first;
        factor[ch][sb][1] = gaud_mp3_scalefactor[gaud_mp3_bits_read(bits, 6u)];
        factor[ch][sb][2] = gaud_mp3_scalefactor[gaud_mp3_bits_read(bits, 6u)];
        break;
      }
      case 1u: {
        factor[ch][sb][0] = first;
        factor[ch][sb][1] = first;
        factor[ch][sb][2] = gaud_mp3_scalefactor[gaud_mp3_bits_read(bits, 6u)];
        break;
      }
      case 2u: {
        factor[ch][sb][0] = first;
        factor[ch][sb][1] = first;
        factor[ch][sb][2] = first;
        break;
      }
      default: {
        factor[ch][sb][0] = first;
        int32_t second = gaud_mp3_scalefactor[gaud_mp3_bits_read(bits, 6u)];
        factor[ch][sb][1] = second;
        factor[ch][sb][2] = second;
        break;
      }
      }
    }
  }

  for (unsigned group = 0; group < 12u; ++group) {
    unsigned part = group / 4u;
    int32_t subband[2][3][32];
    memset(subband, 0, sizeof(subband));
    for (unsigned sb = 0; sb < limit; ++sb) {
      unsigned shared = sb >= bound ? 1u : channels;
      for (unsigned ch = 0; ch < shared; ++ch) {
        if (class_index[ch][sb] < 0) {
          continue;
        }
        const MP3_Quant_Class * class = &gaud_mp3_classes[class_index[ch][sb]];
        uint32_t raw[3];
        MP3_TRACE(class->grouping ? MP3_T_L2_GROUPED : MP3_T_L2_UNGROUPED);
        if (class->grouping) {
          /* Three samples in one codeword, taken apart by repeated
           * division: the standard's own degrouping algorithm. */
          uint32_t code = gaud_mp3_bits_read(bits, class->bits);
          for (unsigned i = 0; i < 3u; ++i) {
            raw[i] = code % class->steps;
            code /= class->steps;
          }
        }
        else {
          for (unsigned i = 0; i < 3u; ++i) {
            raw[i] = gaud_mp3_bits_read(bits, class->bits);
          }
        }
        unsigned last = sb >= bound ? channels : ch + 1u;
        for (unsigned target = ch; target < last; ++target) {
          for (unsigned i = 0; i < 3u; ++i) {
            subband[target][i][sb]
                = requantize(class, raw[i], factor[target][sb][part]);
          }
        }
      }
    }
    for (unsigned i = 0; i < 3u; ++i) {
      for (unsigned ch = 0; ch < channels; ++ch) {
        gaud_mp3_synth_run(&state->synth[ch], subband[ch][i],
            pcm + (size_t)(group * 3u + i) * 32u * channels + ch, channels);
      }
    }
  }
  return bits->overrun ? GAUD_ERR_CORRUPT : GAUD_OK;
}

GAUD_Result gaud_mp3_layer12_frame(MP3_Layer12 * state,
    const MP3_Header * header, const unsigned char * frame, size_t size,
    int32_t * pcm) {
  size_t head = MP3_HEADER_SIZE + (header->crc_present ? 2u : 0u);
  if (size <= head) {
    return GAUD_ERR_CORRUPT;
  }
  memset(pcm, 0, (size_t)header->samples * header->channels * sizeof(pcm[0]));
  MP3_Bits bits;
  gaud_mp3_bits_init(&bits, frame + head, size - head);
  if (header->layer == 1u) {
    return layer1_frame(state, &bits, header, pcm);
  }
  return layer2_frame(state, &bits, header, pcm);
}
