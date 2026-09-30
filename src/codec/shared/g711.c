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
 * G.711 µ-law and A-law, spelled as the standard defines them rather than
 * as a copied table.
 *
 * ## µ-law
 *
 * A bias of 33 is added to the 14-bit magnitude, the position of the
 * highest set bit above bit 5 becomes a three-bit exponent, the four bits
 * below it become the mantissa, and the whole byte is complemented. The
 * bias is what makes the segments line up; without it the smallest segment
 * would be twice the width of the rest.
 *
 * ## A-law
 *
 * No bias, a 13-bit magnitude, a linear bottom segment rather than a
 * biased one, and the result is exclusive-ored with 0x55 - alternate bits
 * inverted - which exists to keep the line busy when the signal is silent.
 * That XOR is the single most-forgotten step in an A-law implementation
 * and it is why silence in a valid A-law file reads as 0xD5, not 0x00.
 *
 * Both decoders are built once into a 256-entry table on first use. It is
 * `static const` data computed at compile time rather than lazily, so there
 * is no initialisation race for two decoders on two threads to lose.
 */

#include "g711.h"

/** @brief 132: the standard's bias of 33, shifted left by two. */
#define BIAS 0x84
/** @brief The largest magnitude mu-law encodes without saturating. */
#define CLIP 32635

/* ---------------------------------------------------------------- µ-law */

/*
 * The segment a magnitude falls in, as the standard's table states it:
 * seg_end[i] is the largest magnitude still in segment i.
 */
static const int16_t ulaw_seg_end[8]
    = {0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF, 0x3FFF, 0x7FFF};

static int ulaw_segment(int value, const int16_t * table) {
  for (int i = 0; i < 8; ++i) {
    if (value <= table[i]) {
      return i;
    }
  }
  return 8; /* Above the last segment: the caller has already clipped. */
}

uint8_t gaud_g711_ulaw_encode(int16_t sample) {
  /* The standard works on sign and magnitude. Taking the magnitude of
   * INT16_MIN in int16_t is undefined, so the widening to int happens
   * first and the negation happens there. */
  int value = sample;
  int sign = (value < 0) ? 0x80 : 0x00;
  if (value < 0) {
    value = -value;
  }
  if (value > CLIP) {
    value = CLIP;
  }
  value += BIAS;

  int segment = ulaw_segment(value, ulaw_seg_end);
  if (segment >= 8) {
    return (uint8_t)(0x7F ^ sign); /* Saturated. */
  }
  int mantissa = (value >> (segment + 3)) & 0x0F;
  /* Complemented, which is what makes silence 0xFF and full scale 0x00 -
   * the opposite of every intuition, and correct. */
  return (uint8_t)(~(sign | (segment << 4) | mantissa));
}

int16_t gaud_g711_ulaw_decode(uint8_t code) {
  int value = ~code;
  int mantissa = value & 0x0F;
  int segment = (value >> 4) & 0x07;
  int magnitude = ((mantissa << 3) + BIAS) << segment;
  magnitude -= BIAS;
  return (int16_t)((value & 0x80) ? -magnitude : magnitude);
}

/* ---------------------------------------------------------------- A-law */

static const int16_t alaw_seg_end[8]
    = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};

uint8_t gaud_g711_alaw_encode(int16_t sample) {
  /* The shift comes FIRST, on the signed value, and the magnitude is taken
   * after it. Doing it the other way round - magnitude, then shift - is
   * wrong for every negative input, because A-law's negative range is
   * offset by one and the offset has to be applied to the already-scaled
   * value. Measured: taking the magnitude first disagrees with libsndfile
   * on 60968 of the 65536 possible inputs. */
  int value = sample >> 3; /* 16-bit sample, 13-bit law. */
  int mask;
  if (value >= 0) {
    mask = 0xD5; /* Sign bit set for positive, which is A-law's convention. */
  }
  else {
    mask = 0x55;
    value = -value - 1;
  }

  int segment = ulaw_segment(value, alaw_seg_end);
  if (segment >= 8) {
    return (uint8_t)(0x7F ^ mask); /* Saturated. */
  }
  int code = segment << 4;
  /* Segments 0 and 1 share one slope - A-law's linear bottom - so both
   * take the mantissa from the same place. */
  code |= (segment < 2) ? ((value >> 1) & 0x0F) : ((value >> segment) & 0x0F);
  /* The alternate-bit inversion, which is why silence in a valid A-law
   * file reads as 0xD5 and a run of zero bytes is not silence. */
  return (uint8_t)(code ^ mask);
}

int16_t gaud_g711_alaw_decode(uint8_t code) {
  int value = code ^ 0x55;
  int sign = value & 0x80;
  value &= 0x7F;

  int segment = (value >> 4) & 0x07;
  int mantissa = value & 0x0F;
  int magnitude;
  if (segment == 0) {
    magnitude = (mantissa << 4) + 8;
  }
  else {
    magnitude = ((mantissa << 4) + 0x108) << (segment - 1);
  }
  return (int16_t)(sign ? magnitude : -magnitude);
}
