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
 * Every band's shape: RFC 6716 sections 4.3.4.4 and 4.3.4.5, around the
 * single-band coder in opus_celt_shape.c. Never installed.
 *
 * This is the largest function in CELT and it does four things at once,
 * which is why it is one function rather than four.
 *
 * **It splits.** A PVQ codebook has to fit in 32 bits, so a band with
 * enough bits to exceed that is halved, a gain parameter is coded
 * saying how the energy divides between the halves, and the whole
 * process recurses. Up to `LM+1` levels deep.
 *
 * **It does stereo with the same machinery.** Two channels are the two
 * halves of a split whose gain parameter happens to be a stereo angle.
 * That is the whole of mid-side here; intensity stereo is the case
 * where the angle's resolution collapses to one value and only the mid
 * is coded, and dual stereo is the case where the split is skipped and
 * the channels are coded independently.
 *
 * **It changes the time-frequency resolution.** Section 4.3.4.5's flag
 * per band is applied here, as Hadamard rotations that trade frequency
 * resolution for time resolution before the split and are undone after
 * it. The samples are reordered into time order for the split and back
 * afterwards, so that a split divides the band in time rather than in
 * frequency when that is what the flag asked for.
 *
 * **It folds.** A band the allocator gave nothing to is not left empty:
 * it is filled with a copy of lower-frequency content, or with noise
 * from an agreed generator when there is no lower content to copy. The
 * `collapse` mask threads through all of the above so that section
 * 4.3.5 can tell which time blocks ended up with nothing real in them.
 *
 * **The encoder's half is left out, and one branch with it.** Everything
 * guarded by `encode` in RFC 6716's `quant_band` is absent, which takes
 * `intensity_stereo`, `stereo_split` and `stereo_itheta` with it - the
 * decoder reaches intensity stereo through ::gaud_celt_quant_all_bands's
 * averaging of the two normalised channels and through `stereo_merge`,
 * never through those. The band energies are therefore not a parameter
 * here, because their only use was in `intensity_stereo`.
 *
 * The reference also has `if (i >= m->effEBands) X = norm;`, which is
 * for Opus Custom modes whose effective band count is below their band
 * count. Opus itself defines one mode and it has 21 of each, so that
 * branch cannot be taken and is not written.
 */

#include "opus_celt.h"
#include "opus_celt_math.h"
#include <string.h>

/** 1/sqrt(2) in Q15, which every Hadamard step multiplies by. */
#define CELT_SQRT_HALF 23170

/**
 * @brief One Hadamard step: sum and difference of neighbouring pairs.
 *
 * Trades one bin of frequency resolution for one of time resolution,
 * or back again - it is its own inverse up to the scaling.
 *
 * @param x The data.
 * @param n How many entries each of the @p stride interleaved sets has.
 * @param stride How far apart one set's entries are.
 */
static void haar1(int16_t * x, int n, int stride) {
  n >>= 1;
  for (int i = 0; i < stride; ++i) {
    for (int j = 0; j < n; ++j) {
      int16_t a = (int16_t)gaud_celt_mult16_16_q15(
          CELT_SQRT_HALF, x[stride * 2 * j + i]);
      int16_t b = (int16_t)gaud_celt_mult16_16_q15(
          CELT_SQRT_HALF, x[stride * (2 * j + 1) + i]);
      x[stride * 2 * j + i] = (int16_t)(a + b);
      x[stride * (2 * j + 1) + i] = (int16_t)(a - b);
    }
  }
}

/**
 * The order the Hadamard interleave visits its sets in.
 *
 * Four rows, for strides of 2, 4, 8 and 16, laid end to end and indexed
 * by `stride - 2`. The order is not the natural one because the
 * Hadamard transform's outputs come out in bit-reversed sequency.
 */
static const int kOrdery[] = {
  1, 0,
  3, 0, 2, 1,
  7, 0, 4, 3, 6, 1, 5, 2,
  15, 0, 8, 7, 12, 3, 11, 4, 14, 1, 9, 6, 13, 2, 10, 5,
};

/**
 * @brief Gather the @p stride interleaved sets into contiguous runs.
 *
 * @param x The band.
 * @param n How many entries each set has.
 * @param stride How many sets.
 * @param hadamard Whether to use the sequency order rather than 0..n.
 * @param tmp Scratch of at least `n * stride` entries.
 */
static void deinterleave_hadamard(
    int16_t * x, int n, int stride, bool hadamard, int16_t * tmp) {
  int total = n * stride;
  const int * order = kOrdery + stride - 2;
  for (int i = 0; i < stride; ++i) {
    int target = hadamard ? order[i] : i;
    for (int j = 0; j < n; ++j) {
      tmp[target * n + j] = x[j * stride + i];
    }
  }
  memcpy(x, tmp, (size_t)total * sizeof *x);
}

/**
 * @brief Undo ::deinterleave_hadamard.
 *
 * @param x The band.
 * @param n How many entries each set has.
 * @param stride How many sets.
 * @param hadamard Whether the sequency order was used.
 * @param tmp Scratch of at least `n * stride` entries.
 */
static void interleave_hadamard(
    int16_t * x, int n, int stride, bool hadamard, int16_t * tmp) {
  int total = n * stride;
  const int * order = kOrdery + stride - 2;
  for (int i = 0; i < stride; ++i) {
    int source = hadamard ? order[i] : i;
    for (int j = 0; j < n; ++j) {
      tmp[j * stride + i] = x[source * n + j];
    }
  }
  memcpy(x, tmp, (size_t)total * sizeof *x);
}

/**
 * @brief Turn a decoded mid and side back into left and right.
 *
 * The side was coded against a normalised mid, so undoing the split
 * means rescaling by the norm of each of the sum and the difference -
 * which is computed from the two vectors rather than transmitted.
 *
 * @param x The mid, which becomes the left.
 * @param y The side, which becomes the right.
 * @param mid The mid's gain in Q15.
 * @param n How many bins.
 */
static void stereo_merge(int16_t * x, int16_t * y, int16_t mid, int n) {
  int32_t cross = 0;
  int32_t side_energy = 0;
  int32_t left_energy;
  int32_t right_energy;
  int16_t half_mid;
  int left_shift;
  int right_shift;
  int16_t left_gain;
  int16_t right_gain;
  // |X+Y|^2 and |X-Y|^2 as |X|^2 + |Y|^2 -/+ 2*sum(xy). The mid is
  // normalised, so |X|^2 is its gain squared and is not summed.
  for (int j = 0; j < n; ++j) {
    cross += gaud_celt_mult16_16(x[j], y[j]);
    side_energy += gaud_celt_mult16_16(y[j], y[j]);
  }
  cross = gaud_celt_shl32(gaud_celt_mult16_16(mid, cross >> 16), 1)
      + (((int32_t)(int16_t)mid * (int32_t)(uint16_t)(uint32_t)cross) >> 15);
  // mid and side are Q15 where x and y are Q14, hence the halving.
  half_mid = (int16_t)(mid >> 1);
  left_energy = gaud_celt_mult16_16(half_mid, half_mid) + side_energy
      - 2 * cross;
  right_energy = gaud_celt_mult16_16(half_mid, half_mid) + side_energy
      + 2 * cross;
  // Too close to degenerate to invert: the reference gives up and
  // copies, which keeps a near-zero denominator out of the gains.
  if (right_energy < 161061 || left_energy < 161061) {
    for (int j = 0; j < n; ++j) {
      y[j] = x[j];
    }
    return;
  }
  left_shift = gaud_celt_ilog2(left_energy) >> 1;
  right_shift = gaud_celt_ilog2(right_energy) >> 1;
  left_gain = gaud_celt_rsqrt_norm(
      gaud_celt_vshr32(left_energy, (left_shift - 7) << 1));
  right_gain = gaud_celt_rsqrt_norm(
      gaud_celt_vshr32(right_energy, (right_shift - 7) << 1));
  if (left_shift < 7) {
    left_shift = 7;
  }
  if (right_shift < 7) {
    right_shift = 7;
  }
  for (int j = 0; j < n; ++j) {
    int16_t left = (int16_t)gaud_celt_mult16_16_q15(mid, x[j]);
    int16_t right = y[j];
    x[j] = (int16_t)gaud_celt_pshr32(
        gaud_celt_mult16_16(left_gain, gaud_celt_sub16(left, right)),
        (unsigned)(left_shift + 1));
    y[j] = (int16_t)gaud_celt_pshr32(
        gaud_celt_mult16_16(right_gain, gaud_celt_add16(left, right)),
        (unsigned)(right_shift + 1));
  }
}

/**
 * @brief How many values the split's angle is quantised to.
 *
 * Section 4.3.4.4's "precision derived from the current allocation":
 * the more bits the band has over what one pulse costs, the finer the
 * angle, up to 256 steps.
 *
 * @param n How many bins each half has.
 * @param bits The band's budget in eighths of a bit.
 * @param offset The bias, which differs for two-bin stereo.
 * @param pulse_cap What one pulse in this band costs.
 * @param stereo Whether the split is the stereo one.
 * @return 1 when no angle is coded at all, otherwise an even count.
 */
static int compute_qn(
    int n, int32_t bits, int offset, int pulse_cap, bool stereo) {
  static const int16_t kExp2Table8[8] = {
    16384, 17866, 19483, 21247, 23170, 25267, 27554, 30048
  };
  int n2 = 2 * n - 1;
  int32_t qb;
  int qn;
  if (stereo && n == 2) {
    --n2;
  }
  // The cap keeps enough back that a stereo split at full side can
  // still afford one pulse there; without it the side would collapse,
  // and the side is not folded.
  qb = bits - pulse_cap - (4 << CELT_BITRES);
  if (qb > (bits + n2 * offset) / n2) {
    qb = (bits + n2 * offset) / n2;
  }
  if (qb > (8 << CELT_BITRES)) {
    qb = 8 << CELT_BITRES;
  }
  if (qb < (1 << CELT_BITRES >> 1)) {
    return 1;
  }
  qn = kExp2Table8[qb & 7] >> (14 - (qb >> CELT_BITRES));
  return (qn + 1) >> 1 << 1;
}

/** Everything ::quant_band needs that does not change between calls. */
typedef struct {
  OPUS_Range * range;        ///< The decoder.
  unsigned spread;           ///< Which of Table 59's four values.
  uint32_t intensity;        ///< First band coded as intensity stereo.
  int32_t * remaining_bits;  ///< The frame's budget, spent as we go.
  uint32_t * seed;           ///< The folding generator's state.
  int16_t * hadamard_tmp;    ///< Scratch for the two reorderings.
  int16_t * lowband_scratch; ///< A copy of the fold source, when needed.
} CELT_BandContext;

/**
 * @brief One band, recursively: split it, or code it, then undo.
 *
 * Transcribed from RFC 6716's `quant_band` with the encoder removed.
 * The parameter list is long for the same reason the reference's is:
 * the recursion passes state down that the top level set up, and
 * flattening it would mean recomputing it at every level.
 *
 * @param ctx What does not change.
 * @param band Which band, for the cache and the log-width.
 * @param x The first half, or the only one.
 * @param y The second half for a stereo split, else NULL.
 * @param n How many bins each of @p x and @p y has.
 * @param bits The budget for this band, in eighths of a bit.
 * @param blocks How many time blocks the band spans.
 * @param tf_change This band's time-frequency flag.
 * @param lowband Where to fold from, or NULL to fold from noise.
 * @param lm The frame size, decremented by one at each split.
 * @param lowband_out Receives this band scaled for a later band to fold
 *   from, or NULL when the caller does not need it.
 * @param level How deep the recursion is; zero is the band itself.
 * @param gain The length to give the result, in Q15.
 * @param fill One bit per time block, saying which may be folded into.
 * @return The collapse mask.
 */
static unsigned quant_band(CELT_BandContext * ctx, uint32_t band, int16_t * x,
    int16_t * y, int n, int32_t bits, int blocks, int tf_change,
    int16_t * lowband, int lm, int16_t * lowband_out, int level, int16_t gain,
    int fill) {
  OPUS_Range * range = ctx->range;
  int n0 = n;
  int block_size = n;
  int block_size0;
  int blocks0 = blocks;
  bool long_blocks = blocks0 == 1;
  int time_divide = 0;
  int recombine = 0;
  bool inverted = false;
  int16_t mid = 0;
  int16_t side = 0;
  unsigned mask = 0;
  bool stereo = y != NULL;
  bool split = stereo;

  block_size /= blocks;
  block_size0 = block_size;

  // One bin: there is no shape, only a sign, and only if it is afforded.
  if (n == 1) {
    int16_t * channel = x;
    for (int c = 0; c < 1 + (stereo ? 1 : 0); ++c) {
      int sign = 0;
      if (*ctx->remaining_bits >= 1 << CELT_BITRES) {
        sign = (int)gaud_opus_dec_bits(range, 1);
        *ctx->remaining_bits -= 1 << CELT_BITRES;
        bits -= 1 << CELT_BITRES;
      }
      channel[0] = sign ? (int16_t)-16384 : (int16_t)16384;
      channel = y;
    }
    if (lowband_out != NULL) {
      lowband_out[0] = (int16_t)(x[0] >> 4);
    }
    return 1u;
  }

  if (!stereo && level == 0) {
    // Section 4.3.4.5. Raising the frequency resolution first, by
    // folding pairs of blocks together, then lowering it if the flag
    // asked for the other direction.
    if (tf_change > 0) {
      recombine = tf_change;
    }
    if (lowband != NULL
        && (recombine != 0 || ((block_size & 1) == 0 && tf_change < 0)
            || blocks0 > 1)) {
      // The fold source is about to be transformed, and it belongs to
      // whichever band it came from; work on a copy.
      memcpy(ctx->lowband_scratch, lowband, (size_t)n * sizeof *lowband);
      lowband = ctx->lowband_scratch;
    }
    for (int k = 0; k < recombine; ++k) {
      // Two blocks become one, so two mask bits become one.
      static const unsigned char kInterleave[16] = {
        0, 1, 1, 1, 2, 3, 3, 3, 2, 3, 3, 3, 2, 3, 3, 3
      };
      if (lowband != NULL) {
        haar1(lowband, n >> k, 1 << k);
      }
      fill = kInterleave[fill & 0xF] | kInterleave[fill >> 4] << 2;
    }
    blocks >>= recombine;
    block_size <<= recombine;

    while ((block_size & 1) == 0 && tf_change < 0) {
      if (lowband != NULL) {
        haar1(lowband, block_size, blocks);
      }
      fill |= fill << blocks;
      blocks <<= 1;
      block_size >>= 1;
      ++time_divide;
      ++tf_change;
    }
    blocks0 = blocks;
    block_size0 = block_size;

    // Put the samples in time order, so that a split divides the band
    // in time rather than in frequency.
    if (blocks0 > 1 && lowband != NULL) {
      deinterleave_hadamard(lowband, block_size >> recombine,
          blocks0 << recombine, long_blocks, ctx->hadamard_tmp);
    }
  }

  // Section 4.3.4.4: if the codebook would need more than 32 bits, and
  // the cache says this band's largest entry plus 1.5 bits is under
  // budget, halve it.
  if (lm != -1) {
    // Only asked for when lm is not -1, which is also the only case in
    // which the row is guaranteed to exist.
    const unsigned char * cache = gaud_celt_pulse_cache(lm, band);
    if (!stereo && bits > (int32_t)cache[cache[0]] + 12 && n > 2) {
      n >>= 1;
      y = x + n;
      split = true;
      lm -= 1;
      if (blocks == 1) {
        fill = (fill & 1) | (fill << 1);
      }
      blocks = (blocks + 1) >> 1;
    }
  }

  if (split) {
    int qn;
    int32_t itheta = 0;
    int32_t mid_bits;
    int32_t side_bits;
    int32_t delta;
    int32_t qalloc;
    int pulse_cap;
    int offset;
    int orig_fill;
    uint32_t tell;
    int32_t imid;
    int32_t iside;

    pulse_cap = (int)gaud_opus_logN400[band] + (int)lm * (1 << CELT_BITRES);
    offset = (pulse_cap >> 1)
        - (stereo && n == 2 ? CELT_QTHETA_OFFSET_TWOPHASE : CELT_QTHETA_OFFSET);
    qn = compute_qn(n, bits, offset, pulse_cap, stereo);
    if (stereo && band >= ctx->intensity) {
      qn = 1;
    }
    tell = gaud_opus_tell_frac(range);
    if (qn != 1) {
      // Three distributions for the one parameter: a step for stereo, a
      // uniform one when the band is already divided in time, and a
      // triangular one otherwise - the last because a mono split is
      // usually near the middle and a stereo one usually is not.
      if (stereo && n > 2) {
        int p0 = 3;
        int x0 = qn / 2;
        uint32_t ft = (uint32_t)(p0 * (x0 + 1) + x0);
        uint32_t fs = gaud_opus_decode(range, ft);
        int value;
        if (fs < (uint32_t)((x0 + 1) * p0)) {
          value = (int)fs / p0;
        } else {
          value = x0 + 1 + (int)(fs - (uint32_t)((x0 + 1) * p0));
        }
        gaud_opus_dec_update(range,
            (uint32_t)(value <= x0 ? p0 * value : (value - 1 - x0) + (x0 + 1) * p0),
            (uint32_t)(value <= x0 ? p0 * (value + 1) : (value - x0) + (x0 + 1) * p0),
            ft);
        itheta = value;
      } else if (blocks0 > 1 || stereo) {
        itheta = (int32_t)gaud_opus_dec_uint(range, (uint32_t)qn + 1u);
      } else {
        uint32_t ft = (uint32_t)(((qn >> 1) + 1) * ((qn >> 1) + 1));
        uint32_t fm = gaud_opus_decode(range, ft);
        uint32_t fl;
        uint32_t fs;
        // Inverting a triangular number, which is where the exact
        // integer square root earns its keep.
        if (fm < (uint32_t)((qn >> 1) * ((qn >> 1) + 1) >> 1)) {
          itheta = (int32_t)((gaud_celt_isqrt32(8u * fm + 1u) - 1u) >> 1);
          fs = (uint32_t)itheta + 1u;
          fl = (uint32_t)(itheta * (itheta + 1) >> 1);
        } else {
          itheta = (int32_t)((2u * ((uint32_t)qn + 1u)
                                 - gaud_celt_isqrt32(8u * (ft - fm - 1u) + 1u))
              >> 1);
          fs = (uint32_t)(qn + 1) - (uint32_t)itheta;
          fl = ft - (uint32_t)((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
        }
        gaud_opus_dec_update(range, fl, fl + fs, ft);
      }
      itheta = itheta * 16384 / qn;
    } else if (stereo) {
      // No angle: the side is not coded at all, only whether it was
      // negated before the encoder folded it into the mid.
      if (bits > 2 << CELT_BITRES
          && *ctx->remaining_bits > 2 << CELT_BITRES) {
        inverted = gaud_opus_dec_bit_logp(range, 2) != 0;
      }
      itheta = 0;
    }
    qalloc = (int32_t)(gaud_opus_tell_frac(range) - tell);
    bits -= qalloc;

    orig_fill = fill;
    if (itheta == 0) {
      imid = 32767;
      iside = 0;
      fill &= (1 << blocks) - 1;
      delta = -16384;
    } else if (itheta == 16384) {
      imid = 0;
      iside = 32767;
      fill &= ((1 << blocks) - 1) << blocks;
      delta = 16384;
    } else {
      imid = gaud_celt_bitexact_cos((int16_t)itheta);
      iside = gaud_celt_bitexact_cos((int16_t)(16384 - itheta));
      // The division of bits that minimises the squared error.
      delta = gaud_celt_frac_mul16((n - 1) << 7,
          gaud_celt_bitexact_log2tan(iside, imid));
    }
    mid = (int16_t)imid;
    side = (int16_t)iside;

    if (n == 2 && stereo) {
      // Two bins in stereo: mid and side are orthogonal, so the side is
      // fully determined by the mid up to one sign bit.
      int sign = 0;
      int16_t * first;
      int16_t * second;
      mid_bits = bits;
      side_bits = 0;
      if (itheta != 0 && itheta != 16384) {
        side_bits = 1 << CELT_BITRES;
      }
      mid_bits -= side_bits;
      *ctx->remaining_bits -= qalloc + side_bits;

      first = itheta > 8192 ? y : x;
      second = itheta > 8192 ? x : y;
      if (side_bits != 0) {
        sign = (int)gaud_opus_dec_bits(range, 1);
      }
      sign = 1 - 2 * sign;
      // orig_fill, not fill: the side should still be folded even where
      // itheta of 16384 cleared fill's low bits.
      mask = quant_band(ctx, band, first, NULL, n, mid_bits, blocks, tf_change,
          lowband, lm, lowband_out, level, gain, orig_fill);
      second[0] = (int16_t)(-sign * first[1]);
      second[1] = (int16_t)(sign * first[0]);
      x[0] = (int16_t)gaud_celt_mult16_16_q15(mid, x[0]);
      x[1] = (int16_t)gaud_celt_mult16_16_q15(mid, x[1]);
      y[0] = (int16_t)gaud_celt_mult16_16_q15(side, y[0]);
      y[1] = (int16_t)gaud_celt_mult16_16_q15(side, y[1]);
      for (int j = 0; j < 2; ++j) {
        int16_t sum = x[j];
        x[j] = gaud_celt_sub16(sum, y[j]);
        y[j] = gaud_celt_add16(sum, y[j]);
      }
    } else {
      int16_t * next_lowband2 = NULL;
      int16_t * next_lowband_out1 = NULL;
      int next_level = 0;
      int32_t rebalance;

      // A split in time rather than frequency: give the quieter block
      // more than its share, because pre-echo is what is audible.
      if (blocks0 > 1 && !stereo && (itheta & 0x3FFF) != 0) {
        if (itheta > 8192) {
          delta -= delta >> (4 - lm);
        } else {
          int32_t raised = delta + (n << CELT_BITRES >> (5 - lm));
          delta = raised < 0 ? raised : 0;
        }
      }
      mid_bits = (bits - delta) / 2;
      if (mid_bits > bits) {
        mid_bits = bits;
      }
      if (mid_bits < 0) {
        mid_bits = 0;
      }
      side_bits = bits - mid_bits;
      *ctx->remaining_bits -= qalloc;

      if (lowband != NULL && !stereo) {
        next_lowband2 = lowband + n;
      }
      // Only stereo hands the fold source outward from here; a mono
      // split's halves are rejoined below and handed on together.
      if (stereo) {
        next_lowband_out1 = lowband_out;
      } else {
        next_level = level + 1;
      }

      rebalance = *ctx->remaining_bits;
      if (mid_bits >= side_bits) {
        // The mid keeps its own normalisation in stereo, because a
        // later band folds from it and needs it unit-norm.
        mask = quant_band(ctx, band, x, NULL, n, mid_bits, blocks, tf_change,
            lowband, lm, next_lowband_out1, next_level,
            stereo ? (int16_t)CELT_Q15ONE
                   : (int16_t)gaud_celt_mult16_16_p15(gain, mid),
            fill);
        rebalance = mid_bits - (rebalance - *ctx->remaining_bits);
        if (rebalance > 3 << CELT_BITRES && itheta != 0) {
          side_bits += rebalance - (3 << CELT_BITRES);
        }
        mask |= (unsigned)quant_band(ctx, band, y, NULL, n, side_bits, blocks,
                    tf_change, next_lowband2, lm, NULL, next_level,
                    (int16_t)gaud_celt_mult16_16_p15(gain, side), fill >> blocks)
            << ((blocks0 >> 1) & (stereo ? 0 : -1));
      } else {
        mask = (unsigned)quant_band(ctx, band, y, NULL, n, side_bits, blocks,
                   tf_change, next_lowband2, lm, NULL, next_level,
                   (int16_t)gaud_celt_mult16_16_p15(gain, side), fill >> blocks)
            << ((blocks0 >> 1) & (stereo ? 0 : -1));
        rebalance = side_bits - (rebalance - *ctx->remaining_bits);
        if (rebalance > 3 << CELT_BITRES && itheta != 16384) {
          mid_bits += rebalance - (3 << CELT_BITRES);
        }
        mask |= quant_band(ctx, band, x, NULL, n, mid_bits, blocks, tf_change,
            lowband, lm, next_lowband_out1, next_level,
            stereo ? (int16_t)CELT_Q15ONE
                   : (int16_t)gaud_celt_mult16_16_p15(gain, mid),
            fill);
      }
    }
  } else {
    // No split: spend what is left on pulses, or fold.
    int q = gaud_celt_bits_to_pulses(lm, band, bits);
    int32_t cost = gaud_celt_pulses_to_bits(lm, band, q);
    *ctx->remaining_bits -= cost;
    // The allocator aims not to overspend but can, so back off until it
    // fits. The encoder does the same, so both agree on where it lands.
    while (*ctx->remaining_bits < 0 && q > 0) {
      *ctx->remaining_bits += cost;
      --q;
      cost = gaud_celt_pulses_to_bits(lm, band, q);
      *ctx->remaining_bits -= cost;
    }

    if (q != 0) {
      mask = gaud_celt_alg_unquant(x, (uint32_t)n,
          (uint32_t)gaud_celt_get_pulses(q), ctx->spread, (uint32_t)blocks,
          range, gain);
    } else {
      unsigned cm_mask = (unsigned)((1UL << blocks) - 1UL);
      fill &= (int)cm_mask;
      if (fill == 0) {
        memset(x, 0, (size_t)n * sizeof *x);
      } else {
        if (lowband == NULL) {
          // Nothing below to copy: agreed noise instead.
          for (int j = 0; j < n; ++j) {
            *ctx->seed = gaud_celt_lcg_rand(*ctx->seed);
            x[j] = (int16_t)((int32_t)*ctx->seed >> 20);
          }
          mask = cm_mask;
        } else {
          // Folded spectrum: a lower band, dithered about 48 dB down so
          // that a repeated copy does not sound like one.
          for (int j = 0; j < n; ++j) {
            int16_t dither;
            *ctx->seed = gaud_celt_lcg_rand(*ctx->seed);
            dither = (*ctx->seed & 0x8000u) ? (int16_t)4 : (int16_t)-4;
            x[j] = (int16_t)(lowband[j] + dither);
          }
          mask = (unsigned)fill;
        }
        gaud_celt_renormalise_vector(x, (uint32_t)n, gain);
      }
    }
  }

  if (stereo) {
    if (n != 2) {
      stereo_merge(x, y, mid, n);
    }
    if (inverted) {
      for (int j = 0; j < n; ++j) {
        y[j] = (int16_t)-y[j];
      }
    }
  } else if (level == 0) {
    // Undo everything section 4.3.4.5 did on the way in, in reverse.
    if (blocks0 > 1) {
      interleave_hadamard(x, block_size >> recombine, blocks0 << recombine,
          long_blocks, ctx->hadamard_tmp);
    }
    block_size = block_size0;
    blocks = blocks0;
    for (int k = 0; k < time_divide; ++k) {
      blocks >>= 1;
      block_size <<= 1;
      mask |= mask >> blocks;
      haar1(x, block_size, blocks);
    }
    for (int k = 0; k < recombine; ++k) {
      // One mask bit becomes two, which is the inverse of kInterleave.
      static const unsigned char kDeinterleave[16] = {
        0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33, 0x3C, 0x3F,
        0xC0, 0xC3, 0xCC, 0xCF, 0xF0, 0xF3, 0xFC, 0xFF
      };
      mask = kDeinterleave[mask & 0xF];
      haar1(x, n0 >> k, 1 << k);
    }
    blocks <<= recombine;

    if (lowband_out != NULL) {
      // Scaled by sqrt(N) so that a later band folding from this one
      // gets something of the right size whatever width it is.
      int16_t scale = (int16_t)gaud_celt_sqrt(
          gaud_celt_shl32((int32_t)n0, 22));
      for (int j = 0; j < n0; ++j) {
        lowband_out[j] = (int16_t)gaud_celt_mult16_16_q15(scale, x[j]);
      }
    }
    mask &= (unsigned)((1 << blocks) - 1);
  }
  return mask;
}

void gaud_celt_quant_all_bands(OPUS_Range * range, const CELT_Mode * mode,
    uint32_t start, uint32_t end, int16_t * x, int16_t * y,
    unsigned char * collapse, const int32_t * pulses, bool short_blocks,
    unsigned spread, bool dual_stereo, uint32_t intensity, const int * tf_res,
    int32_t total_bits, int32_t balance, uint32_t coded_bands, uint32_t * seed,
    int16_t * scratch) {
  uint32_t channels = y != NULL ? 2u : 1u;
  uint32_t m = 1u << mode->lm;
  int blocks = short_blocks ? (int)m : 1;
  uint32_t frame = m * (uint32_t)mode->edges[CELT_BANDS];
  // The normalised spectrum, which later bands fold from, then the two
  // scratch areas quant_band needs.
  int16_t * norm = scratch;
  int16_t * norm2 = norm + frame;
  int16_t * hadamard_tmp = norm + channels * frame;
  int16_t * lowband_scratch = hadamard_tmp + CELT_MAX_BAND_BINS;
  uint32_t lowband_offset = 0;
  bool update_lowband = true;
  int32_t remaining_bits = 0;
  CELT_BandContext ctx;

  ctx.range = range;
  ctx.spread = spread;
  ctx.intensity = intensity;
  ctx.remaining_bits = &remaining_bits;
  ctx.seed = seed;
  ctx.hadamard_tmp = hadamard_tmp;
  ctx.lowband_scratch = lowband_scratch;

  for (uint32_t i = start; i < end; ++i) {
    uint32_t band_start = m * (uint32_t)mode->edges[i];
    int n = (int)(m * (uint32_t)mode->edges[i + 1] - band_start);
    int16_t * band_x = x + band_start;
    int16_t * band_y = y != NULL ? y + band_start : NULL;
    int32_t tell = (int32_t)gaud_opus_tell_frac(range);
    int32_t curr_balance;
    int32_t bits;
    int tf_change = tf_res[i];
    int32_t effective_lowband = -1;
    unsigned x_mask;
    unsigned y_mask;

    if (i != start) {
      balance -= tell;
    }
    remaining_bits = total_bits - tell - 1;
    if (i <= coded_bands - 1u) {
      uint32_t spread_over = coded_bands - i;
      curr_balance = balance / (int32_t)(spread_over < 3u ? spread_over : 3u);
      bits = pulses[i] + curr_balance;
      if (bits > remaining_bits + 1) {
        bits = remaining_bits + 1;
      }
      if (bits > 16383) {
        bits = 16383;
      }
      if (bits < 0) {
        bits = 0;
      }
    } else {
      bits = 0;
    }

    // RFC 8251 section 9. In a hybrid frame at a low bit rate only one
    // CELT band may be coded, and the second is wider than it, so it had
    // nothing to fold from and fell back to noise - audible as pre-echo
    // on a transient. The second band is therefore always allowed to
    // fold, from the first band repeated far enough to cover it. For a
    // CELT-only frame the first two bands are the same width and nothing
    // is copied.
    if (((int32_t)band_start - n >= (int32_t)(m * (uint32_t)mode->edges[start])
            || i == start + 1u)
        && (update_lowband || lowband_offset == 0)) {
      lowband_offset = i;
    }
    if (i == start + 1u) {
      int32_t n1 = (int32_t)(m
          * (uint32_t)(mode->edges[start + 1u] - mode->edges[start]));
      int32_t n2 = (int32_t)(m
          * (uint32_t)(mode->edges[start + 2u] - mode->edges[start + 1u]));
      int32_t offset = (int32_t)(m * (uint32_t)mode->edges[start]);
      if (n2 > n1) {
        memmove(&norm[offset + n1], &norm[offset + 2 * n1 - n2],
            (size_t)(n2 - n1) * sizeof *norm);
        if (channels == 2u) {
          memmove(&norm2[offset + n1], &norm2[offset + 2 * n1 - n2],
              (size_t)(n2 - n1) * sizeof *norm2);
        }
      }
    }

    // What the bands being folded from can offer. Over-estimating is
    // safe here and under-estimating is not, so the masks are OR-ed.
    if (lowband_offset != 0
        && (spread != CELT_SPREAD_AGGRESSIVE || blocks > 1 || tf_change < 0)) {
      uint32_t fold_start;
      uint32_t fold_end;
      int32_t low = (int32_t)(m * (uint32_t)mode->edges[lowband_offset]) - n;
      int32_t floor_bin = (int32_t)(m * (uint32_t)mode->edges[start]);
      effective_lowband = low > floor_bin ? low : floor_bin;
      fold_start = lowband_offset;
      while ((int32_t)(m * (uint32_t)mode->edges[--fold_start])
          > effective_lowband) {
      }
      fold_end = lowband_offset - 1u;
      while (++fold_end < i
          && (int32_t)(m * (uint32_t)mode->edges[fold_end])
              < effective_lowband + n) {
      }
      x_mask = 0;
      y_mask = 0;
      for (uint32_t f = fold_start; f < fold_end; ++f) {
        x_mask |= collapse[f * channels];
        y_mask |= collapse[f * channels + channels - 1u];
      }
    } else {
      // Folding from the generator instead, which fills every block.
      x_mask = y_mask = (unsigned)((1 << blocks) - 1);
    }

    if (dual_stereo && i == intensity) {
      // Intensity stereo starts here, so the two normalised channels
      // become one for everything above.
      dual_stereo = false;
      for (uint32_t j = m * (uint32_t)mode->edges[start]; j < band_start; ++j) {
        norm[j] = (int16_t)((norm[j] + norm2[j]) >> 1);
      }
    }
    if (dual_stereo) {
      x_mask = quant_band(&ctx, i, band_x, NULL, n, bits / 2, blocks, tf_change,
          effective_lowband != -1 ? norm + effective_lowband : NULL, (int)mode->lm,
          norm + band_start, 0, (int16_t)CELT_Q15ONE, (int)x_mask);
      y_mask = quant_band(&ctx, i, band_y, NULL, n, bits / 2, blocks, tf_change,
          effective_lowband != -1 ? norm2 + effective_lowband : NULL,
          (int)mode->lm, norm2 + band_start, 0, (int16_t)CELT_Q15ONE,
          (int)y_mask);
    } else {
      x_mask = quant_band(&ctx, i, band_x, band_y, n, bits, blocks, tf_change,
          effective_lowband != -1 ? norm + effective_lowband : NULL,
          (int)mode->lm, norm + band_start, 0, (int16_t)CELT_Q15ONE,
          (int)(x_mask | y_mask));
      y_mask = x_mask;
    }
    collapse[i * channels] = (unsigned char)x_mask;
    collapse[i * channels + channels - 1u] = (unsigned char)y_mask;
    balance += pulses[i] + tell;

    // Stop moving the fold source up once the bands stop being dense.
    update_lowband = bits > (n << CELT_BITRES);
  }
}
