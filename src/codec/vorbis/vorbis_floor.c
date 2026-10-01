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
 * The floor: the spectral envelope a packet states, and the curve it
 * renders to.
 *
 * **Floor 1 is entirely integer, by design rather than by this library's
 * choice.** Its output is a piecewise linear curve in a logarithmic
 * domain, and the specification defines it with Bresenham's line
 * algorithm over integer Y values that index a 256-entry table. So there
 * is nothing to approximate here: a correct decoder's floor is exactly
 * the specification's, bit for bit, and any disagreement with a reference
 * decoder is a defect rather than a rounding difference. That makes this
 * the one part of the Vorbis audio path with an exact answer, which is
 * worth knowing when a comparison against a reference starts to differ.
 *
 * Three things about it are easy to get wrong, and all three are invisible
 * on a quiet passage:
 *
 * **The Y values are differentially coded against a predicted value**,
 * where the prediction interpolates between the two already-decoded
 * neighbours - the *nearest* ones in X, which are not the adjacent ones
 * in the order the stream states them. A decoder that used the
 * neighbours by index decodes a curve that is right wherever the stream
 * happened to state its points in increasing X.
 *
 * **The "room" calculation is asymmetric.** The coded value is folded
 * into the space between the prediction and the ends of the range, and
 * which end gets the odd value depends on which side has more room. Get
 * it backwards and the curve is right whenever the prediction sits below
 * the middle.
 *
 * **A Y value of zero means "no correction", not "zero amplitude".** It
 * marks the point as one the curve passes through by prediction, and -
 * this is the part with no analogue elsewhere - such a point is then
 * *skipped* when the curve is rendered. A decoder that rendered through
 * it draws a curve with the right values at the wrong X positions.
 *
 * Floor 0 is here too, and it is refused. See the comment on it.
 */

#include "vorbis_internal.h"
#include "vorbis_tables.h"
#include <string.h>

/** The Y range each multiplier gives, from the specification. */
static const uint32_t floor1_range[4] = {256u, 128u, 86u, 64u};

/**
 * The already-decoded point with the largest X below @p x_list[at].
 *
 * **Over the points already decoded, which is indices below @p at**, and
 * compared by X. The specification calls this `low_neighbor` and spells
 * it out for a reason: the points are *not* stated in increasing X, so
 * the nearest neighbour in X and the previous one in the list are
 * different points, and a curve built from the wrong one is right only
 * where the encoder happened to state them in order.
 */
static uint32_t low_neighbor(const uint32_t * x_list, uint32_t at) {
  uint32_t best = 0;
  uint32_t best_x = 0;
  bool found = false;
  for (uint32_t i = 0; i < at; ++i) {
    if (x_list[i] < x_list[at] && (!found || x_list[i] > best_x)) {
      best = i;
      best_x = x_list[i];
      found = true;
    }
  }
  return best;
}

/** The already-decoded point with the smallest X above @p x_list[at]. */
static uint32_t high_neighbor(const uint32_t * x_list, uint32_t at) {
  uint32_t best = 0;
  uint32_t best_x = 0;
  bool found = false;
  for (uint32_t i = 0; i < at; ++i) {
    if (x_list[i] > x_list[at] && (!found || x_list[i] < best_x)) {
      best = i;
      best_x = x_list[i];
      found = true;
    }
  }
  return best;
}

/**
 * The Y of the line through (@p x0, @p y0) and (@p x1, @p y1) at @p x.
 *
 * The specification's `render_point`, and **integer division truncating
 * toward zero is part of the answer** rather than an approximation of
 * one: the prediction this computes is subtracted from a coded value at
 * the encoder and added back here, so the two have to agree exactly. A
 * version that rounded would decode every differentially coded point off
 * by one wherever the division was not exact.
 */
static int32_t render_point(
    int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x) {
  int32_t dy = y1 - y0;
  int32_t adx = x1 - x0;
  int32_t ady = dy < 0 ? -dy : dy;
  int32_t error = ady * (x - x0);
  int32_t offset = adx ? error / adx : 0;
  return dy < 0 ? y0 - offset : y0 + offset;
}

/**
 * Draw the line from (@p x0, @p y0) to (@p x1, @p y1) into @p out.
 *
 * The specification's `render_line`, which is Bresenham's algorithm with
 * one unusual detail: the step is split into a whole part `base` and a
 * fractional part carried in `error`, and **`base` is a truncating
 * division of a possibly negative number**. C's `/` truncates toward
 * zero, which is what the specification's pseudocode means, so the
 * expression is written plainly - but the `sy` below is `base - 1` for a
 * descending line and `base + 1` for an ascending one, and that asymmetry
 * is what makes the truncation come out right in both directions.
 *
 * Stops at @p limit, because a floor may state an X position beyond the
 * block's own spectrum: a stream's block sizes are per packet and the
 * floor is per stream, so a curve written for the long block runs past
 * the end of a short one.
 */
static void render_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1,
    unsigned char * out, int32_t limit) {
  int32_t dy = y1 - y0;
  int32_t adx = x1 - x0;
  int32_t ady = dy < 0 ? -dy : dy;
  if (adx <= 0) {
    return;
  }
  int32_t base = dy / adx;
  int32_t step = dy < 0 ? base - 1 : base + 1;
  int32_t error = 0;
  int32_t y = y0;
  int32_t abs_base = base < 0 ? -base : base;
  ady -= abs_base * adx;
  if (x0 < limit) {
    out[x0] = (unsigned char)(y < 0 ? 0 : (y > 255 ? 255 : y));
  }
  for (int32_t x = x0 + 1; x < x1; ++x) {
    error += ady;
    if (error >= adx) {
      error -= adx;
      y += step;
    }
    else {
      y += base;
    }
    if (x >= limit) {
      return;
    }
    out[x] = (unsigned char)(y < 0 ? 0 : (y > 255 ? 255 : y));
  }
}

/** Floor 1: decode the Y values, then render the curve. */
static GAUD_Result floor1_decode(const VORBIS_Floor1 * floor,
    const VORBIS_Setup * setup, VORBIS_Bits * bits, uint32_t lines,
    unsigned char * out_curve, bool * out_used) {
  if (gaud_vorbis_bits_read(bits, 1) == 0) {
    /* The channel carries nothing in this packet. Not an error and not
     * silence either: the residue for it is still read if a coupling
     * step pairs it with a channel that does carry something. */
    *out_used = false;
    return GAUD_OK;
  }
  *out_used = true;

  uint32_t range = floor1_range[floor->multiplier - 1u];
  int32_t y[VORBIS_FLOOR1_VALUES];
  unsigned width = gaud_vorbis_ilog(range - 1u);
  y[0] = (int32_t)gaud_vorbis_bits_read(bits, width);
  y[1] = (int32_t)gaud_vorbis_bits_read(bits, width);

  uint32_t at = 2;
  for (uint32_t i = 0; i < floor->partitions; ++i) {
    unsigned class_number = floor->partition_class[i];
    unsigned dimensions = floor->class_dimensions[class_number];
    unsigned subclass_bits = floor->class_subclasses[class_number];
    uint32_t selector = 0;
    if (subclass_bits) {
      selector = gaud_vorbis_codebook_decode(
          &setup->codebooks[floor->class_masterbook[class_number]], bits);
      if (selector == UINT32_MAX) {
        return GAUD_ERR_CORRUPT;
      }
    }
    for (unsigned j = 0; j < dimensions; ++j) {
      /* The masterbook's entry is a packed set of subclass numbers, one
       * per dimension, `subclass_bits` wide and lowest first. */
      unsigned mask = (1u << subclass_bits) - 1u;
      int16_t book = floor->subclass_book[class_number][selector & mask];
      selector >>= subclass_bits;
      if (at >= floor->values) {
        return GAUD_ERR_CORRUPT;
      }
      if (book < 0) {
        y[at++] = 0; /* This subclass codes nothing. */
        continue;
      }
      uint32_t entry
          = gaud_vorbis_codebook_decode(&setup->codebooks[book], bits);
      if (entry == UINT32_MAX) {
        return GAUD_ERR_CORRUPT;
      }
      y[at++] = (int32_t)entry;
    }
  }
  if (at != floor->values || bits->past_end) {
    return GAUD_ERR_CORRUPT;
  }

  /*
   * Undo the differential coding.
   *
   * Each point above the first two is coded as an offset from a value
   * predicted by interpolating between its two nearest already-decoded
   * neighbours in X. The offset is folded into whichever side of the
   * prediction has room, with the odd values going to the larger side -
   * which is the asymmetry the file comment names.
   *
   * `step2` records which points the curve actually passes through. A
   * coded zero means "no correction", and such a point is skipped when
   * the curve is rendered rather than drawn to; a renderer that drew to
   * it would put the right Y values at the wrong X positions. Decoding a
   * nonzero value also marks *both its neighbours* as points the curve
   * passes through, which is the detail that makes the skipping work.
   */
  bool step2[VORBIS_FLOOR1_VALUES];
  int32_t final_y[VORBIS_FLOOR1_VALUES];
  memset(step2, 0, sizeof(step2));
  step2[0] = true;
  step2[1] = true;
  final_y[0] = y[0];
  final_y[1] = y[1];

  for (uint32_t i = 2; i < floor->values; ++i) {
    uint32_t low = low_neighbor(floor->x_list, i);
    uint32_t high = high_neighbor(floor->x_list, i);
    int32_t predicted = render_point((int32_t)floor->x_list[low],
        final_y[low], (int32_t)floor->x_list[high], final_y[high],
        (int32_t)floor->x_list[i]);
    int32_t value = y[i];
    int32_t high_room = (int32_t)range - predicted;
    int32_t low_room = predicted;
    int32_t room = 2 * (high_room < low_room ? high_room : low_room);
    if (value == 0) {
      step2[i] = false;
      final_y[i] = predicted;
      continue;
    }
    step2[low] = true;
    step2[high] = true;
    step2[i] = true;
    if (value >= room) {
      final_y[i] = high_room > low_room ? value - low_room + predicted
                                        : predicted - value + high_room - 1;
    }
    else {
      final_y[i] = (value & 1) ? predicted - ((value + 1) >> 1)
                               : predicted + (value >> 1);
    }
  }

  /*
   * Render, walking the points in increasing X.
   *
   * The curve starts at the first point and is drawn from each point the
   * curve passes through to the next one; past the last, it is flat. The
   * Y values are multiplied by the multiplier here and nowhere else,
   * which is what turns a range of 64, 86 or 128 into an index into the
   * 256-entry table.
   */
  int32_t last_x = 0;
  int32_t last_y = final_y[0] * (int32_t)floor->multiplier;
  memset(out_curve, 0, lines);
  for (uint32_t k = 1; k < floor->values; ++k) {
    uint32_t i = floor->sorted[k];
    if (!step2[i]) {
      continue;
    }
    int32_t this_x = (int32_t)floor->x_list[i];
    int32_t this_y = final_y[i] * (int32_t)floor->multiplier;
    if (last_x < this_x) {
      render_line(last_x, last_y, this_x, this_y, out_curve, (int32_t)lines);
    }
    last_x = this_x;
    last_y = this_y;
    if (last_x >= (int32_t)lines) {
      break;
    }
  }
  /* Flat from the last point to the end of the spectrum. A floor may
   * stop short of the block's own width - its X positions are per stream
   * and the block size is per packet - and what the specification says
   * about the rest is that it holds the last value. */
  if (last_x < (int32_t)lines) {
    unsigned char value
        = (unsigned char)(last_y < 0 ? 0 : (last_y > 255 ? 255 : last_y));
    for (int32_t x = last_x; x < (int32_t)lines; ++x) {
      out_curve[x] = value;
    }
  }
  return GAUD_OK;
}

GAUD_Result gaud_vorbis_floor_decode(const VORBIS_Floor * floor,
    const VORBIS_Setup * setup, VORBIS_Bits * bits, uint32_t lines,
    unsigned char * out_curve, bool * out_used) {
  if (floor->type == 1u) {
    return floor1_decode(&floor->u.one, setup, bits, lines, out_curve,
        out_used);
  }
  /*
   * **Floor 0 is identified and refused, and the refusal is honest about
   * why.** It is a line spectral pair curve: the packet states an
   * amplitude and a set of LSP coefficients, and the curve is the
   * magnitude response of the polynomial they describe - which needs a
   * cosine and a square root per spectral line, in a decoder this
   * library promises to keep integer and byte-identical everywhere
   * (planning/audio.md section 11.1).
   *
   * That is doable and is not the reason it is not done. The reason is
   * that **nothing can score it.** No encoder in the oracle image emits
   * floor 0 at any setting - `make vorbis-coverage` says so - and the
   * fixed-point approximation of a transcendental curve is exactly the
   * kind of code whose error nobody notices without a reference to
   * compare against. A refusal that says so is better than an
   * approximation nobody can check.
   *
   * A stream using it opens and reports its length; asking it to decode
   * answers ::GAUD_ERR_UNSUPPORTED, which is a per-track answer a
   * capability bit cannot give.
   */
  (void)lines;
  (void)out_curve;
  *out_used = false;
  return GAUD_ERR_UNSUPPORTED;
}
