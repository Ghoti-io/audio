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
 * FLAC's two checksums.
 *
 * A frame header ends in a CRC-8 over itself and a frame ends in a CRC-16
 * over the whole frame, which is what makes resynchronising into the middle
 * of a stream possible at all: a fourteen-bit sync code appears in ordinary
 * audio data about once every sixteen kilobytes, and without a checksum to
 * reject those a seek would land on noise and decode it.
 *
 * **Both are computed bit-serially here rather than from a table**, which
 * is a deliberate trade and not an oversight. A table is faster per byte,
 * but these run over a frame header (a dozen bytes) and over a frame that
 * has already been read into memory, and the decode of that frame's
 * subframes costs orders of magnitude more than checksumming it. What a
 * table costs instead is 256 entries per polynomial that nothing checks: a
 * single wrong entry is a checksum that is right for every input a test
 * happens to use and wrong for one byte value in 256, which is the shape of
 * defect this suite has the hardest time finding. The loop has no table to
 * be wrong.
 *
 * The parameters, which every CRC gets wrong somewhere:
 *
 *   CRC-8   polynomial 0x07, initial value 0, no reflection, no final xor.
 *   CRC-16  polynomial 0x8005, initial value 0, no reflection, no final xor.
 *
 * Neither is reflected, which is the unusual half - most deployed CRC-16s
 * are. RFC 9639 section 9.1.7 and 9.3 state both.
 */

#include "flac_internal.h"

uint8_t gaud_flac_crc8(const unsigned char * data, size_t size) {
  uint8_t crc = 0;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8u; ++bit) {
      crc = (uint8_t)((crc & 0x80u) ? ((uint8_t)(crc << 1) ^ 0x07u)
                                    : (uint8_t)(crc << 1));
    }
  }
  return crc;
}

uint16_t gaud_flac_crc16(const unsigned char * data, size_t size) {
  uint16_t crc = 0;
  for (size_t i = 0; i < size; ++i) {
    crc ^= (uint16_t)((uint16_t)data[i] << 8);
    for (unsigned bit = 0; bit < 8u; ++bit) {
      crc = (uint16_t)((crc & 0x8000u) ? ((uint16_t)(crc << 1) ^ 0x8005u)
                                       : (uint16_t)(crc << 1));
    }
  }
  return crc;
}
