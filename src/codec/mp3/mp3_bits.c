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
 * The bit reader the main data is read through, and the reservoir under it.
 *
 * **This is the file that exists because of the bit reservoir**, which is
 * the one thing in MPEG audio that makes a frame not self-contained. A
 * Layer III frame's main data does not begin after its own header: it
 * begins `main_data_begin` bytes *before* it, inside frames already gone
 * past, and a frame's own bytes may carry the tail of the previous frame's
 * data and the head of the next one's. So the reader is over an
 * accumulating buffer rather than over a frame, and the position in it is
 * tracked in bits.
 *
 * The reader is deliberately a reader over **memory** and not over a
 * ::GAUD_Stream, for the same reason FLAC's is: the bytes have to be in
 * hand before the arithmetic starts, and here they have to be in hand from
 * several frames at once.
 */

#include "mp3_internal.h"
#include <string.h>

void gaud_mp3_bits_init(
    MP3_Bits * bits, const unsigned char * data, size_t size) {
  bits->data = data;
  bits->size = size;
  bits->at = 0;
  bits->overrun = false;
}

uint32_t gaud_mp3_bits_read(MP3_Bits * bits, unsigned count) {
  /* Zero is a legal request and the only answer to it is zero. It happens
   * for real: a scalefactor partition whose slen is 0 asks for no bits per
   * scalefactor, and a reader that shifted by 32 instead would be
   * undefined behaviour on a value nothing had asked about. */
  if (count == 0) {
    return 0;
  }
  uint32_t value = 0;
  for (unsigned taken = 0; taken < count; ++taken) {
    size_t byte = (bits->at + taken) >> 3;
    if (byte >= bits->size) {
      /* Past the end. The remaining bits read as zero and the overrun is
       * latched, so a granule can be decoded to its end and asked about
       * once - which is what keeps the Huffman loop readable, exactly as
       * in the FLAC reader. Nothing downstream of an overrun is trusted. */
      bits->overrun = true;
      value <<= (count - taken);
      bits->at += count;
      return value;
    }
    unsigned bit = 7u - (unsigned)((bits->at + taken) & 7u);
    value = (value << 1) | ((bits->data[byte] >> bit) & 1u);
  }
  bits->at += count;
  return value;
}

void gaud_mp3_bits_seek(MP3_Bits * bits, size_t position) {
  bits->at = position;
  if (position > bits->size * 8u) {
    bits->overrun = true;
  }
}

int32_t gaud_mp3_bits_signed(MP3_Bits * bits, unsigned count) {
  /* Two's complement over `count` bits, which is how the linbits
   * magnitude's sign and the Layer I and II samples are stored. */
  uint32_t raw = gaud_mp3_bits_read(bits, count);
  if (count == 0 || count >= 32u) {
    return (int32_t)raw;
  }
  uint32_t sign = 1u << (count - 1u);
  if (raw & sign) {
    return (int32_t)raw - (int32_t)(sign << 1);
  }
  return (int32_t)raw;
}

void gaud_mp3_reservoir_reset(MP3_Reservoir * reservoir) {
  reservoir->length = 0;
}

bool gaud_mp3_reservoir_push(MP3_Reservoir * reservoir,
    uint32_t main_data_begin, const unsigned char * data, size_t size) {
  if (main_data_begin > reservoir->length) {
    /* The frame points further back than there is history. Two things
     * cause this and neither is corruption: the first frames of a stream,
     * whose back-pointers reach before the file began, and the first frame
     * after a seek. The caller is told so it can count the granule as
     * undecodable rather than decode whatever happens to be in the
     * buffer - which would be the previous track's audio after a seek. */
    reservoir->length = 0;
    if (size <= sizeof(reservoir->data)) {
      memcpy(reservoir->data, data, size);
      reservoir->length = size;
    }
    return false;
  }
  /* Keep exactly the bytes this frame reaches back for, so that bit zero
   * of the buffer is bit zero of this frame's main data. */
  memmove(reservoir->data,
      reservoir->data + reservoir->length - main_data_begin, main_data_begin);
  reservoir->length = main_data_begin;
  if (reservoir->length + size > sizeof(reservoir->data)) {
    /* Cannot happen for a frame this library accepted - the cap is a
     * maximum frame plus the largest back-pointer the format can state -
     * and is checked rather than assumed, because the alternative is a
     * buffer overrun driven by a nine-bit field in a file. */
    reservoir->length = 0;
    return false;
  }
  memcpy(reservoir->data + reservoir->length, data, size);
  reservoir->length += size;
  return true;
}

void gaud_mp3_reservoir_trim(MP3_Reservoir * reservoir) {
  /* Keep the most a back-pointer can ask for and no more. Without this
   * the buffer grows by a frame every frame. */
  if (reservoir->length > MP3_RESERVOIR_KEEP) {
    memmove(reservoir->data,
        reservoir->data + reservoir->length - MP3_RESERVOIR_KEEP,
        MP3_RESERVOIR_KEEP);
    reservoir->length = MP3_RESERVOIR_KEEP;
  }
}
