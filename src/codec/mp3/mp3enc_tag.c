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
 * The Info and Xing tag frame: the first frame of the file, which is not
 * audio.
 *
 * Players read three things from it that nothing else in an MP3 states:
 * how many frames there are and how many bytes (so a duration without
 * walking the file), a table of contents that turns a position in time into
 * a position in bytes for a variable-rate file, and - in the 36 bytes the
 * LAME encoder added after it - how many samples of delay and padding the
 * encoder put around the audio, which is what makes playback gapless.
 *
 * The frame is a real frame: it has a header and a side-information block
 * of zeros, and it stands where the first audio frame would, in a slot of
 * the same length as its neighbours, so a decoder that does not know the tag
 * decodes it as one granule pair of silence. A tag for a constant-rate file
 * says `Info`, one for any other `Xing`; the layout is the same.
 *
 * **The encoder's name starts `LAME`, and says no more of LAME than that.**
 * ffmpeg takes the delay and padding from this extension only when the
 * name's first four bytes are `LAME`, `Lavf` or `Lavc` - case-sensitive, at
 * the start, and nothing after them is read (measured against the pinned
 * ffmpeg in `check-mp3-encode`: `LAME` and spaces gives the recording's exact
 * length, `lame` or `XLAME` gives the delay as silence). Without it a file
 * from this encoder plays 1057 samples late and ends with 609 of padding.
 * So the field says `LAME(G1)`: the four letters the readers test, then what
 * did write it and its major generation, in the nine bytes the format
 * allows. It is a prefix a reader requires and not a claim to be LAME's
 * code; nothing here parses a version, and the digits are this encoder's.
 * This library's own decoder honours any printable name. libmpg123's rule
 * has not been tested here.
 */

#include "mp3enc_internal.h"

static void put32(unsigned char * at, uint32_t value) {
  at[0] = (unsigned char)(value >> 24);
  at[1] = (unsigned char)(value >> 16);
  at[2] = (unsigned char)(value >> 8);
  at[3] = (unsigned char)value;
}

void gaud_mp3e_tag_build(const MP3E_Tag * tag, unsigned char * frame) {
  MP3E_Frame_Header h = {
      .version = tag->version,
      .bitrate_index = tag->bitrate_index,
      .rate_index = tag->rate_index,
      .mode = tag->channels == 1u ? MP3_MODE_SINGLE_CHANNEL : MP3_MODE_STEREO,
      .channels = tag->channels,
  };
  memset(frame, 0, tag->frame_bytes);
  gaud_mp3e_header_write(frame, &h);
  unsigned char * at = frame + 4u + gaud_mp3e_side_bytes(tag->version, tag->channels);
  memcpy(at, tag->vbr ? "Xing" : "Info", 4);
  uint32_t flags = 0x03u | (tag->toc ? 0x04u : 0u) | (tag->vbr ? 0x08u : 0u);
  put32(at + 4, flags);
  unsigned char * field = at + 8u;
  put32(field, tag->frames);
  field += 4;
  put32(field, tag->bytes);
  field += 4;
  if (tag->toc) {
    memcpy(field, tag->toc, 100u);
    field += 100;
  }
  if (tag->vbr) {
    put32(field, tag->quality);
    field += 4;
  }
  unsigned char * lame = field;
  memcpy(lame, MP3E_TAG_ENCODER, 9);
  lame[9] = (unsigned char)((0u << 4) | (tag->vbr_method & 0xFu));
  lame[10] = (unsigned char)tag->lowpass_100hz;
  lame[20] = (unsigned char)(tag->bitrate_byte > 255u ? 255u : tag->bitrate_byte);
  lame[21] = (unsigned char)(tag->delay >> 4);
  lame[22] = (unsigned char)(((tag->delay & 0xFu) << 4) | ((tag->padding >> 8) & 0xFu));
  lame[23] = (unsigned char)tag->padding;
  /* Stereo mode in bits 2 to 4: 0 mono, 1 stereo, 3 joint. */
  lame[24] = (unsigned char)((tag->channels == 1u ? 0u : 3u) << 2);
  put32(lame + 28, tag->music_length);
  lame[32] = (unsigned char)(tag->music_crc >> 8);
  lame[33] = (unsigned char)tag->music_crc;
  /* The checksum covers every byte of the frame before its own field. In
   * the layout LAME writes - side information, 120 bytes of Xing fields and
   * the 36 of the extension - that is the first 190, which is where the
   * rule "the first 190 bytes" comes from; the layouts here differ by
   * channel count and MPEG version, and the rule that survives is the
   * general one. (An earlier draft summed 190 bytes whatever the frame's
   * length, which read past the end of a short frame into whatever was
   * there, and the cross-architecture gate found it: the bytes differed.) */
  uint16_t tag_crc = gaud_mp3e_crc16(frame, (size_t)(lame + 34 - frame), 0);
  lame[34] = (unsigned char)(tag_crc >> 8);
  lame[35] = (unsigned char)tag_crc;
}
