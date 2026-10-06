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
 * What goes in a frame besides the spectrum: the header, the side
 * information, the scalefactors and the field widths they are written in.
 *
 * Each writer here is the other half of a reader in `mp3_header.c` or
 * `mp3_layer3.c`, and each reuses that reader's types and tables so that
 * the two cannot drift: the side information is the decoder's own
 * ::MP3_Granule, the scalefactor field widths are looked up in the
 * decoder's own `gaud_mp3_slen`, and the MPEG-2 partition sizes come from
 * the same `gaud_mp3_lsf_nsfb`. What the encoder owns is the *choice* -
 * which of the sixteen width pairs is cheapest for these scalefactors -
 * and the bit order.
 */

#include "mp3enc_internal.h"

/* ------------------------------------------------------------ the tables */

static const uint16_t bitrates_v1[15] = {
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
static const uint16_t bitrates_v2[15] = {
    0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
static const uint32_t rates[3][3] = {
    {44100u, 48000u, 32000u},
    {22050u, 24000u, 16000u},
    {11025u, 12000u, 8000u},
};

int gaud_mp3e_bitrate_index(MP3_Version version, unsigned kbps) {
  const uint16_t * table = version == MP3_MPEG1 ? bitrates_v1 : bitrates_v2;
  for (unsigned i = 1; i < 15u; ++i) {
    if (table[i] == kbps) {
      return (int)i;
    }
  }
  return -1;
}

unsigned gaud_mp3e_bitrate_kbps(MP3_Version version, unsigned index) {
  if (index == 0u || index > 14u) {
    return 0;
  }
  return (version == MP3_MPEG1 ? bitrates_v1 : bitrates_v2)[index];
}

bool gaud_mp3e_rate_lookup(
    uint32_t sample_rate, MP3_Version * version, unsigned * rate_index) {
  for (unsigned v = 0; v < 3u; ++v) {
    for (unsigned r = 0; r < 3u; ++r) {
      if (rates[v][r] == sample_rate) {
        *version = (MP3_Version)v;
        *rate_index = r;
        return true;
      }
    }
  }
  return false;
}

uint32_t gaud_mp3e_frame_bytes(const MP3E_Frame_Header * header) {
  uint32_t slots = header->version == MP3_MPEG1 ? 144u : 72u;
  return (uint32_t)((uint64_t)slots * header->bitrate / header->sample_rate);
}

uint32_t gaud_mp3e_side_bytes(MP3_Version version, unsigned channels) {
  if (version == MP3_MPEG1) {
    return channels == 1u ? 17u : 32u;
  }
  return channels == 1u ? 9u : 17u;
}

/* ------------------------------------------------------------ the header */

void gaud_mp3e_header_write(unsigned char out[4], const MP3E_Frame_Header * h) {
  static const unsigned char version_bits[3] = {3u, 2u, 0u};
  out[0] = 0xFFu;
  out[1] = (unsigned char)(0xE0u | (version_bits[h->version] << 3) | (1u << 1)
      | (h->crc ? 0u : 1u));
  out[2] = (unsigned char)((h->bitrate_index << 4) | (h->rate_index << 2)
      | (h->padding ? 2u : 0u));
  /* The "original" bit is set, as every encoder does. */
  out[3] = (unsigned char)(((unsigned)h->mode << 6) | (h->mode_extension << 4)
      | 4u);
}

uint16_t gaud_mp3e_crc16(const unsigned char * data, size_t size, uint16_t crc) {
  for (size_t i = 0; i < size; ++i) {
    for (unsigned bit = 0x80u; bit != 0; bit >>= 1) {
      bool top = (crc & 0x8000u) != 0;
      bool in = (data[i] & bit) != 0;
      crc = (uint16_t)(crc << 1);
      if (top != in) {
        crc ^= 0x8005u;
      }
    }
  }
  return crc;
}

/* ---------------------------------------------------------- side information */

void gaud_mp3e_side_write(unsigned char * out, const MP3E_Frame_Header * h,
    uint32_t main_data_begin, const uint8_t scfsi[2],
    const MP3E_Granule granule[2][2]) {
  bool lsf = h->version != MP3_MPEG1;
  unsigned channels = h->channels;
  unsigned granules = lsf ? 1u : 2u;
  uint32_t size = gaud_mp3e_side_bytes(h->version, channels);
  memset(out, 0, size);
  MP3E_Bits bits;
  gaud_mp3e_bits_init(&bits, out, size);

  gaud_mp3e_bits_put(&bits, main_data_begin, lsf ? 8u : 9u);
  gaud_mp3e_bits_put(&bits, 0, lsf ? (channels == 1u ? 1u : 2u)
                                   : (channels == 1u ? 5u : 3u));
  if (!lsf) {
    for (unsigned ch = 0; ch < channels; ++ch) {
      gaud_mp3e_bits_put(&bits, scfsi[ch], 4u);
    }
  }
  for (unsigned gr = 0; gr < granules; ++gr) {
    for (unsigned ch = 0; ch < channels; ++ch) {
      const MP3_Granule * g = &granule[gr][ch].side;
      gaud_mp3e_bits_put(&bits, g->part2_3_length, 12u);
      gaud_mp3e_bits_put(&bits, g->big_values, 9u);
      gaud_mp3e_bits_put(&bits, g->global_gain, 8u);
      gaud_mp3e_bits_put(&bits, g->scalefac_compress, lsf ? 9u : 4u);
      gaud_mp3e_bits_put(&bits, g->window_switching ? 1u : 0u, 1u);
      if (g->window_switching) {
        gaud_mp3e_bits_put(&bits, g->block_type, 2u);
        gaud_mp3e_bits_put(&bits, g->mixed_block ? 1u : 0u, 1u);
        for (unsigned r = 0; r < 2u; ++r) {
          gaud_mp3e_bits_put(&bits, g->table_select[r], 5u);
        }
        for (unsigned w = 0; w < 3u; ++w) {
          gaud_mp3e_bits_put(&bits, g->subblock_gain[w], 3u);
        }
      }
      else {
        for (unsigned r = 0; r < 3u; ++r) {
          gaud_mp3e_bits_put(&bits, g->table_select[r], 5u);
        }
        gaud_mp3e_bits_put(&bits, g->region0_count, 4u);
        gaud_mp3e_bits_put(&bits, g->region1_count, 3u);
      }
      if (!lsf) {
        gaud_mp3e_bits_put(&bits, g->preflag ? 1u : 0u, 1u);
      }
      gaud_mp3e_bits_put(&bits, g->scalefac_scale ? 1u : 0u, 1u);
      gaud_mp3e_bits_put(&bits, g->count1table_select ? 1u : 0u, 1u);
    }
  }
}

/* ------------------------------------------------------------ scalefactors */

/** Bits needed to carry values up to @p maximum. */
static unsigned width_for(unsigned maximum) {
  unsigned bits = 0;
  while (maximum != 0u) {
    ++bits;
    maximum >>= 1;
  }
  return bits;
}

/** The four groups scfsi can reuse. */
static const unsigned group_start[5] = {0u, 6u, 11u, 16u, 21u};

/** MPEG-1: the cheapest of the sixteen (slen1, slen2) pairs. */
static bool plan_v1(MP3E_Granule * granule) {
  MP3_Granule * side = &granule->side;
  unsigned max1 = 0;
  unsigned max2 = 0;
  unsigned count1 = 0;
  unsigned count2 = 0;
  if (side->block_type == 2u) {
    for (unsigned sfb = 0; sfb < 12u; ++sfb) {
      for (unsigned w = 0; w < 3u; ++w) {
        unsigned v = granule->sf_short[sfb][w];
        if (sfb < 6u) {
          max1 = v > max1 ? v : max1;
        }
        else {
          max2 = v > max2 ? v : max2;
        }
      }
    }
    count1 = 18u;
    count2 = 18u;
  }
  else {
    for (unsigned group = 0; group < 4u; ++group) {
      if (granule->scfsi_reused[group]) {
        continue;
      }
      for (unsigned sfb = group_start[group]; sfb < group_start[group + 1u];
          ++sfb) {
        unsigned v = granule->sf_long[sfb];
        if (sfb < 11u) {
          max1 = v > max1 ? v : max1;
          ++count1;
        }
        else {
          max2 = v > max2 ? v : max2;
          ++count2;
        }
      }
    }
  }
  unsigned need1 = width_for(max1);
  unsigned need2 = width_for(max2);
  unsigned best = 16u;
  unsigned best_bits = 0;
  for (unsigned c = 0; c < 16u; ++c) {
    unsigned s1 = gaud_mp3_slen[c][0];
    unsigned s2 = gaud_mp3_slen[c][1];
    if (s1 < need1 || s2 < need2) {
      continue;
    }
    unsigned bits = count1 * s1 + count2 * s2;
    if (best == 16u || bits < best_bits) {
      best = c;
      best_bits = bits;
    }
  }
  if (best == 16u) {
    return false;
  }
  side->scalefac_compress = best;
  granule->part2_bits = best_bits;
  return true;
}

/** MPEG-2 and 2.5, group 0 of 13818-3's field-width scheme. */
static bool plan_lsf(MP3E_Granule * granule) {
  MP3_Granule * side = &granule->side;
  bool is_short = side->block_type == 2u;
  const uint8_t * counts = gaud_mp3_lsf_nsfb[0][is_short ? 1u : 0u];
  unsigned need[4] = {0, 0, 0, 0};
  unsigned index = 0;
  for (unsigned p = 0; p < 4u; ++p) {
    unsigned maximum = 0;
    for (unsigned n = 0; n < counts[p]; ++n, ++index) {
      unsigned v = is_short ? granule->sf_short[index / 3u][index % 3u]
                            : granule->sf_long[index];
      maximum = v > maximum ? v : maximum;
    }
    need[p] = width_for(maximum);
  }
  if (need[0] > 4u || need[1] > 4u || need[2] > 3u || need[3] > 3u) {
    return false;
  }
  side->scalefac_compress
      = ((need[0] * 5u + need[1]) << 4) | (need[2] << 2) | need[3];
  side->preflag = false;
  granule->part2_bits = 0;
  for (unsigned p = 0; p < 4u; ++p) {
    granule->part2_bits += counts[p] * need[p];
  }
  return true;
}

bool gaud_mp3e_scalefactor_plan(MP3E_Granule * granule, MP3_Version version) {
  return version == MP3_MPEG1 ? plan_v1(granule) : plan_lsf(granule);
}

void gaud_mp3e_scalefactor_write(MP3E_Bits * bits,
    const MP3E_Granule * granule, MP3_Version version) {
  const MP3_Granule * side = &granule->side;
  if (version == MP3_MPEG1) {
    unsigned slen1 = gaud_mp3_slen[side->scalefac_compress][0];
    unsigned slen2 = gaud_mp3_slen[side->scalefac_compress][1];
    if (side->block_type == 2u) {
      for (unsigned sfb = 0; sfb < 12u; ++sfb) {
        for (unsigned w = 0; w < 3u; ++w) {
          gaud_mp3e_bits_put(
              bits, granule->sf_short[sfb][w], sfb < 6u ? slen1 : slen2);
        }
      }
      return;
    }
    for (unsigned group = 0; group < 4u; ++group) {
      if (granule->scfsi_reused[group]) {
        continue;
      }
      for (unsigned sfb = group_start[group]; sfb < group_start[group + 1u];
          ++sfb) {
        gaud_mp3e_bits_put(
            bits, granule->sf_long[sfb], sfb < 11u ? slen1 : slen2);
      }
    }
    return;
  }
  uint32_t c = side->scalefac_compress;
  unsigned slen[4] = {(c >> 4) / 5u, (c >> 4) % 5u, (c % 16u) >> 2, c % 4u};
  bool is_short = side->block_type == 2u;
  const uint8_t * counts = gaud_mp3_lsf_nsfb[0][is_short ? 1u : 0u];
  unsigned index = 0;
  for (unsigned p = 0; p < 4u; ++p) {
    for (unsigned n = 0; n < counts[p]; ++n, ++index) {
      unsigned v = is_short ? granule->sf_short[index / 3u][index % 3u]
                            : granule->sf_long[index];
      gaud_mp3e_bits_put(bits, v, slen[p]);
    }
  }
}

/* ------------------------------------------------------------- the layout */

void gaud_mp3e_layout(MP3E_Layout * layout, unsigned row, bool is_short) {
  memset(layout, 0, sizeof(*layout));
  layout->is_short = is_short;
  if (!is_short) {
    const uint16_t * bounds = gaud_mp3_sfb_long[row];
    unsigned bands = gaud_mp3_sfb_long_bands[row];
    layout->sfb_count = bands;
    for (unsigned b = 0; b < bands; ++b) {
      layout->band[b] = (MP3E_Band){bounds[b], (uint16_t)(bounds[b + 1u] - bounds[b]),
          (uint8_t)b, 0};
    }
    layout->count = bands;
    for (unsigned i = 0; i < MP3E_LINES; ++i) {
      layout->order[i] = (uint16_t)i;
    }
    return;
  }
  const uint16_t * bounds = gaud_mp3_sfb_short[row];
  unsigned bands = gaud_mp3_sfb_short_bands[row];
  layout->sfb_count = bands;
  unsigned run = 0;
  for (unsigned b = 0; b < bands; ++b) {
    unsigned width = bounds[b + 1u] - bounds[b];
    for (unsigned w = 0; w < 3u; ++w, ++run) {
      unsigned start = 3u * bounds[b] + w * width;
      layout->band[run] = (MP3E_Band){(uint16_t)start, (uint16_t)width,
          (uint8_t)b, (uint8_t)w};
      for (unsigned k = 0; k < width; ++k) {
        unsigned line = bounds[b] + k;
        layout->order[start + k]
            = (uint16_t)((line / 6u) * 18u + w * 6u + line % 6u);
      }
    }
  }
  layout->count = run;
}
