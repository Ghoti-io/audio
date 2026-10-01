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
 * Xing, Info, VBRI and the LAME extension: a length bolted onto a format
 * that has none.
 *
 * All three live in the **first frame**, which is a real frame with a real
 * header that decodes to silence, and they occupy the space where its
 * samples would be. That is what makes them compatible with every decoder
 * ever written: a decoder that knows nothing about them decodes a frame of
 * silence and carries on.
 *
 * **Xing and VBRI are different tags in different places, not two names for
 * one thing.** Xing's sits at the start of the main data, so finding it
 * means knowing how long the side information is, which depends on the
 * version and the channel count. VBRI's sits at a fixed offset of 36 bytes
 * from the frame's own start and does not. A reader that looks for one and
 * not the other silently loses the length of every file from the encoder it
 * ignored.
 *
 * **The CRC is the trap here.** When the protection bit says a frame
 * carries a 16-bit CRC, the main data starts two bytes later, and most
 * writers of these tags put the tag where the main data *would* be without
 * one. So both offsets are tried, and the one that matches a magic wins -
 * which is the only way to be right about both the files that are
 * consistent and the files that are not.
 */

#include "mp3_internal.h"
#include <string.h>

/** Read a big-endian 32-bit field. The format is MSB first throughout. */
static uint32_t rd_u32be(const unsigned char * at) {
  return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16)
      | ((uint32_t)at[2] << 8) | (uint32_t)at[3];
}

/** How far into the frame a Xing tag can be, given the side information. */
#define MP3_XING_AT(crc, side) (MP3_HEADER_SIZE + (crc) + (side))

/** Where VBRI always is, measured from the frame's first byte. */
#define MP3_VBRI_AT 36u

/**
 * Read the 36-byte LAME extension that follows a Xing tag's fields.
 *
 * @param at The nine-byte encoder string's first byte.
 * @param available Bytes from @p at to the end of the frame.
 *
 * **The encoder string is checked before the delay is believed, and the
 * check is "printable" rather than a list of names.** The first draft had a
 * list - `LAME` and `Lavc`, the two spellings this format's own encoder and
 * libavcodec write - and measuring it found a third: ffmpeg writes
 * `Lavf lame` when it is asked for bit-exact output, which is precisely the
 * spelling every fixture in this library's corpus carries. Both reference
 * decoders read the delay out of all three, and a file whose delay this
 * library ignored would decode 1,105 frames longer than they do.
 *
 * What the check is actually for is the other case: a writer that filled in
 * the Xing fields and stopped. The bytes after them are then the frame's
 * own data, which for a silent first frame is zeros - so an unprintable
 * first byte is the signal that there is no extension here, and reading a
 * delay out of it would trim a track by an arbitrary number of frames.
 */
static void parse_lame(
    const unsigned char * at, size_t available, MP3_Vbr_Tag * tag) {
  if (available < 36u) {
    return;
  }
  for (unsigned i = 0; i < 9u; ++i) {
    if (at[i] < 0x20u || at[i] > 0x7Eu) {
      return;
    }
  }
  memcpy(tag->encoder, at, 9);
  tag->encoder[9] = '\0';
  /* Twelve bits each, packed across three bytes: the delay's low nibble
   * and the padding's high nibble share byte 22. */
  tag->lame_delay = ((uint32_t)at[21] << 4) | ((uint32_t)at[22] >> 4);
  tag->lame_padding = (((uint32_t)at[22] & 0x0Fu) << 8) | (uint32_t)at[23];
  tag->music_length = rd_u32be(at + 28);
  tag->lame_present = true;
}

/** Read a Xing or Info tag whose magic has already been matched. */
static bool parse_xing(
    const unsigned char * frame, size_t size, size_t at, MP3_Vbr_Tag * tag) {
  /* The magic, then four flag bits deciding which of four fields follow.
   * Nothing states the tag's length, so the only way to know where the
   * LAME extension starts is to add up the fields that are present. */
  if (at + 8u > size) {
    return false;
  }
  tag->is_info = memcmp(frame + at, "Info", 4) == 0;
  uint32_t flags = rd_u32be(frame + at + 4u);
  size_t cursor = at + 8u;

  if (flags & 0x0001u) {
    if (cursor + 4u > size) {
      return false;
    }
    tag->frames = rd_u32be(frame + cursor);
    tag->has_frames = true;
    cursor += 4u;
  }
  if (flags & 0x0002u) {
    if (cursor + 4u > size) {
      return false;
    }
    tag->bytes = rd_u32be(frame + cursor);
    tag->has_bytes = true;
    cursor += 4u;
  }
  if (flags & 0x0004u) {
    if (cursor + 100u > size) {
      return false;
    }
    memcpy(tag->toc, frame + cursor, 100);
    tag->has_toc = true;
    cursor += 100u;
  }
  if (flags & 0x0008u) {
    if (cursor + 4u > size) {
      return false;
    }
    tag->quality = rd_u32be(frame + cursor);
    tag->has_quality = true;
    cursor += 4u;
  }

  tag->present = true;
  parse_lame(frame + cursor, size - cursor, tag);
  return true;
}

/**
 * Read a VBRI tag.
 *
 * Fraunhofer's, and its own shape: a stated table geometry rather than
 * Xing's fixed hundred entries, and the frame count and byte count always
 * present rather than flagged. There is no LAME extension here; the
 * encoder that writes VBRI is not the encoder that writes that.
 */
static bool parse_vbri(
    const unsigned char * frame, size_t size, MP3_Vbr_Tag * tag) {
  /* 4 magic, 2 version, 2 delay, 2 quality, 4 bytes, 4 frames, and then
   * the table's geometry. Only the first 22 are needed to answer the
   * question this library asks of it. */
  if (MP3_VBRI_AT + 22u > size) {
    return false;
  }
  const unsigned char * at = frame + MP3_VBRI_AT;
  tag->vbri = true;
  tag->present = true;
  tag->quality = ((uint32_t)at[8] << 8) | (uint32_t)at[9];
  tag->has_quality = true;
  tag->bytes = rd_u32be(at + 10);
  tag->has_bytes = true;
  tag->frames = rd_u32be(at + 14);
  tag->has_frames = true;
  return true;
}

bool gaud_mp3_tag_parse(const unsigned char * frame, size_t size,
    const MP3_Header * header, MP3_Vbr_Tag * out_tag) {
  if (!frame || !header || !out_tag) {
    return false;
  }
  memset(out_tag, 0, sizeof(*out_tag));

  if (MP3_VBRI_AT + 4u <= size && memcmp(frame + MP3_VBRI_AT, "VBRI", 4) == 0) {
    return parse_vbri(frame, size, out_tag);
  }

  uint32_t side = gaud_mp3_side_info_size(header);
  /* Without the CRC first: that is where the great majority of tags are,
   * including every one written by an encoder that sets no CRC at all. */
  size_t candidates[2] = {
      MP3_XING_AT(0u, side),
      MP3_XING_AT(2u, side),
  };
  size_t tries = header->crc_present ? 2u : 1u;
  for (size_t i = 0; i < tries; ++i) {
    size_t at = candidates[i];
    if (at + 4u > size) {
      continue;
    }
    if (memcmp(frame + at, "Xing", 4) == 0
        || memcmp(frame + at, "Info", 4) == 0) {
      return parse_xing(frame, size, at, out_tag);
    }
  }
  return false;
}

GAUD_Trim gaud_mp3_tag_trim(const MP3_Vbr_Tag * tag) {
  GAUD_Trim trim;
  memset(&trim, 0, sizeof(trim));
  if (!tag || !tag->lame_present) {
    return trim;
  }
  trim.encoder_delay = (uint64_t)tag->lame_delay + MP3_DECODER_DELAY;
  trim.padding = tag->lame_padding > MP3_DECODER_DELAY
      ? (uint64_t)tag->lame_padding - MP3_DECODER_DELAY
      : 0u;
  trim.stated = true;
  return trim;
}
