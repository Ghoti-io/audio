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
 * The four bytes of an MPEG audio frame header.
 *
 * ```text
 *   AAAAAAAA AAABBCCD EEEEFFGH IIJJKLMM
 *   A sync (11 bits, all one)   F sample rate index
 *   B version                   G padding
 *   C layer                     H private
 *   D protection absent         I channel mode
 *   E bitrate index             J mode extension
 *                               K copyright  L original  M emphasis
 * ```
 *
 * Three of those fields are inverted, offset or reserved in ways that are
 * easy to get subtly wrong, and all three are the kind of mistake that
 * produces a file that decodes but is the wrong length:
 *
 * - **The layer field counts down.** `11` is Layer I and `01` is Layer III,
 *   so the layer is `4 - field` and the field's `00` is reserved.
 * - **The version field's reserved value is in the middle.** `00` is the
 *   unofficial MPEG-2.5, `01` is reserved, `10` is MPEG-2 and `11` is
 *   MPEG-1 - so a "version" that is read as a number rather than mapped
 *   puts 2.5 and 1 at opposite ends and 2.5 next to the reserved code.
 * - **The protection bit is negative logic.** It is set when there is *no*
 *   CRC, which is the common case, so a reader that treats it as "has CRC"
 *   is wrong about almost every file rather than about an unusual one, and
 *   is wrong by two bytes at the front of the data it then reads.
 */

#include "mp3_internal.h"
#include <string.h>

/**
 * Bitrates in kbit/s, indexed by the header's four-bit field.
 *
 * Index 0 is the free format, which states no rate; index 15 is reserved
 * and is refused before this is read. The two version groups are MPEG-1 and
 * everything below it: MPEG-2 and MPEG-2.5 share one table, because 13818-3
 * defines the low sampling frequency extension's rates once and 2.5 extends
 * the sampling frequencies without touching them.
 */
static const uint16_t mp3_bitrates[2][3][16] = {
    /* MPEG-1, Layers I, II, III. */
    {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0},
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0},
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}},
    /* MPEG-2 and MPEG-2.5. Layers II and III share a table. */
    {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}},
};

/** Sampling frequencies, by version and by the header's two-bit index. */
static const uint32_t mp3_sample_rates[3][4] = {
    {44100u, 48000u, 32000u, 0u}, /* MPEG-1 */
    {22050u, 24000u, 16000u, 0u}, /* MPEG-2 */
    {11025u, 12000u, 8000u, 0u},  /* MPEG-2.5 */
};

bool gaud_mp3_header_parse(
    const unsigned char * bytes, MP3_Header * out_header) {
  if (!bytes || !out_header) {
    return false;
  }
  /* Eleven bits of sync, not eight. The twelfth bit is the top of the
   * version field, and a stream written for MPEG-2.5 has it clear - so a
   * reader that insists on 0xFFF rejects every 2.5 file, and one that
   * accepts 0xFF alone accepts a byte pair that occurs constantly in
   * compressed data. */
  if (bytes[0] != 0xFFu || (bytes[1] & 0xE0u) != 0xE0u) {
    return false;
  }

  unsigned version_field = (bytes[1] >> 3) & 0x03u;
  if (version_field == 0x01u) {
    return false;
  }
  unsigned layer_field = (bytes[1] >> 1) & 0x03u;
  if (layer_field == 0u) {
    return false;
  }
  unsigned bitrate_index = (bytes[2] >> 4) & 0x0Fu;
  if (bitrate_index == 0x0Fu) {
    return false;
  }
  unsigned rate_index = (bytes[2] >> 2) & 0x03u;
  if (rate_index == 0x03u) {
    return false;
  }

  /* Zeroed before a field is written, so that the bytes of the result are
   * a function of the input and nothing else. Without it the struct's
   * padding carries whatever was on the stack, two parses of one header
   * are not byte-identical, and anything that compares or hashes a header
   * - the fuzz harness does both - reports a difference that is not
   * there. The fields would all be correct; it is the holes between them
   * that would not be. */
  MP3_Header header;
  memset(&header, 0, sizeof(header));
  switch (version_field) {
  case 0x00u: header.version = MP3_MPEG25; break;
  case 0x02u: header.version = MP3_MPEG2; break;
  default: header.version = MP3_MPEG1; break;
  }
  header.layer = 4u - layer_field;
  header.crc_present = (bytes[1] & 0x01u) == 0u;
  header.bitrate_index = bitrate_index;
  header.rate_index = rate_index;
  header.padding = (bytes[2] & 0x02u) != 0u;
  header.private_bit = (bytes[2] & 0x01u) != 0u;
  header.mode = (MP3_Mode)((bytes[3] >> 6) & 0x03u);
  header.mode_extension = (bytes[3] >> 4) & 0x03u;
  header.copyright = (bytes[3] & 0x08u) != 0u;
  header.original = (bytes[3] & 0x04u) != 0u;
  header.emphasis = bytes[3] & 0x03u;

  header.sample_rate = mp3_sample_rates[header.version][rate_index];
  unsigned group = header.version == MP3_MPEG1 ? 0u : 1u;
  header.bitrate
      = (uint32_t)mp3_bitrates[group][header.layer - 1u][bitrate_index] * 1000u;
  header.channels = header.mode == MP3_MODE_SINGLE_CHANNEL ? 1u : 2u;

  if (header.layer == 1u) {
    header.samples = 384u;
  }
  else if (header.layer == 2u) {
    header.samples = 1152u;
  }
  else {
    /* Layer III has two granules of 576 in MPEG-1 and one in the low
     * sampling frequency versions. This is the field that makes a 2.5 file
     * decode to twice its length if it is assumed. */
    header.samples = header.version == MP3_MPEG1 ? 1152u : 576u;
  }

  if (header.bitrate == 0u) {
    /* Free format. The header states no rate, so the frame's length is the
     * distance to the next sync word and cannot be computed here. */
    header.frame_size = 0u;
  }
  else if (header.layer == 1u) {
    /* **The division comes before the multiplication.** Layer I is
     * measured in four-byte slots, so the slot count is floored and then
     * scaled, and a reader that folds the 4 into the numerator gets a
     * length up to three bytes too long for most rates. */
    header.frame_size = (12u * header.bitrate / header.sample_rate
                            + (header.padding ? 1u : 0u))
        * 4u;
  }
  else {
    header.frame_size
        = header.samples / 8u * header.bitrate / header.sample_rate
        + (header.padding ? 1u : 0u);
  }
  if (header.frame_size > MP3_MAX_FRAME_SIZE) {
    return false;
  }
  if (header.frame_size != 0u
      && header.frame_size < MP3_HEADER_SIZE + (header.crc_present ? 2u : 0u)
              + gaud_mp3_side_info_size(&header)) {
    /* A combination whose stated length does not reach the end of its own
     * side information. MPEG-2.5 at 8 kbit/s and 8 kHz is the real case:
     * 72 x 8000 / 8000 = 72 bytes for a frame that needs 4 + 9 of
     * structure before any data, which is legal, and the ones that fail
     * this are the ones where it is not. */
    return false;
  }

  *out_header = header;
  return true;
}

uint32_t gaud_mp3_min_frame_size(const MP3_Header * header) {
  if (!header || header->sample_rate == 0u) {
    return 0u;
  }
  unsigned group = header->version == MP3_MPEG1 ? 0u : 1u;
  /* Index 1 is the lowest rate the version and layer have, index 0 being
   * the free format. No frame of this stream can be shorter than this, and
   * a padding byte only makes one longer. */
  uint32_t bitrate
      = (uint32_t)mp3_bitrates[group][header->layer - 1u][1] * 1000u;
  if (bitrate == 0u) {
    return 0u;
  }
  if (header->layer == 1u) {
    return 12u * bitrate / header->sample_rate * 4u;
  }
  return header->samples / 8u * bitrate / header->sample_rate;
}

uint32_t gaud_mp3_side_info_size(const MP3_Header * header) {
  if (!header || header->layer != 3u) {
    return 0u;
  }
  if (header->version == MP3_MPEG1) {
    return header->channels == 1u ? 17u : 32u;
  }
  return header->channels == 1u ? 9u : 17u;
}

bool gaud_mp3_headers_compatible(
    const MP3_Header * first, const MP3_Header * second) {
  if (!first || !second) {
    return false;
  }
  return first->version == second->version && first->layer == second->layer
      && first->rate_index == second->rate_index
      && first->channels == second->channels;
}
