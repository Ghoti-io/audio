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
 * ITU-T G.711 companding: µ-law and A-law, both directions.
 *
 * Shared between WAV (`WAVE_FORMAT_MULAW`, `WAVE_FORMAT_ALAW`) and AIFF-C
 * (`ulaw`, `alaw`). One byte in, one 16-bit sample out, with no state
 * carried between samples - which is what makes these the easy half of
 * phase 2 and the right place to establish the coded-sample path before
 * ADPCM's blocks arrive.
 *
 * The decode is a 256-entry table generated at build time by the same
 * arithmetic the encoder inverts, not a transcribed constant. A transcribed
 * table is a page of magic numbers nothing checks; this way the two
 * directions are derived from one definition and the round-trip test over
 * all 256 codes is a real check on it. (memory: a remembered constant needs
 * calibration.)
 *
 * **Range.** µ-law is defined on a 14-bit magnitude and A-law on 13, both
 * carried left-aligned in 16 bits, which is why the decoded values are
 * multiples of 4 and 8 respectively and why round-tripping arbitrary 16-bit
 * PCM through either is lossy in a way round-tripping their own output is
 * not.
 */

#ifndef GHOTI_IO_GAUD_SRC_CODEC_SHARED_G711_H
#define GHOTI_IO_GAUD_SRC_CODEC_SHARED_G711_H

#include <ghoti.io/audio/macros.h>
#include <stdint.h>

/* The fuzz harnesses are C++ and link these objects directly, so the
 * declarations need C linkage or the harness asks the linker for a
 * mangled name nothing defines. */
#ifdef __cplusplus
extern "C" {
#endif


/** @brief Decode one µ-law byte to a signed 16-bit sample. */
int16_t gaud_g711_ulaw_decode(uint8_t code);

/** @brief Encode one signed 16-bit sample as a µ-law byte. */
uint8_t gaud_g711_ulaw_encode(int16_t sample);

/** @brief Decode one A-law byte to a signed 16-bit sample. */
int16_t gaud_g711_alaw_decode(uint8_t code);

/** @brief Encode one signed 16-bit sample as an A-law byte. */
uint8_t gaud_g711_alaw_encode(int16_t sample);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_SRC_CODEC_SHARED_G711_H
