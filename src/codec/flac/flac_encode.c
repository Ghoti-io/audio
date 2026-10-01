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
 * Writing FLAC: fixed predictors, Rice coding, and nothing in floating
 * point.
 *
 * **The predictors are the format's fixed set and not LPC, and that is a
 * decision rather than a stage not yet reached.** An LPC encoder computes
 * an autocorrelation, runs Levinson-Durbin over it and quantises the
 * result, and every step of that is floating point. Floating point in the
 * encoder would make this library's output depend on the host's rounding,
 * its instruction selection and whether the compiler contracted a multiply
 * and an add into one - and planning/audio.md 11.1 promises that our output
 * is byte-identical across architectures, which `check-golden` tests. The
 * fixed predictors are four integer differences, so that promise holds for
 * what we write as well as for what we read.
 *
 * What it costs is a few percent of compression against `flac -8`, which
 * buys LPC with an architecture-dependent encoder. What it does not cost is
 * correctness: every file this writes is a FLAC file that libFLAC decodes
 * to the samples that went in, which is the whole of what lossless means.
 * Section 11.3's rule applies - the gap is measured and labelled, in
 * documentation/flac.md, rather than left for a reader to discover.
 *
 * Four things the encoder does do, each because it exercises a decoder path
 * that would otherwise be reachable only from someone else's files:
 *
 *   - **all four stereo decorrelations**, chosen per frame by estimated cost;
 *   - **escaped Rice partitions**, when a partition of constant or nearly
 *     constant residuals is cheaper raw than Rice-coded - a silent passage
 *     is the common case;
 *   - **wasted bits**, when every sample in a subframe shares low zeros,
 *     which is what 8-bit material carried in a 16-bit file looks like;
 *   - **CONSTANT and VERBATIM subframes**, where they win.
 */

#include "../../container/ogg/ogg.h"
#include "../../meta/scheme.h"
#include "../shared/bytes.h"
#include "flac_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/security/md5.h>
#include <string.h>

/** Frames per block. libFLAC's default above 44.1 kHz, and ours always. */
#define ENC_BLOCK_SIZE 4096u

/**
 * Bytes reserved after the metadata blocks for a seek table.
 *
 * A seek table has to sit in front of the audio and its contents are not
 * known until the audio has been written, so the space is reserved as a
 * PADDING block and overwritten at finish - which is what PADDING is for.
 * 1 KiB holds 56 points and leaves room for a padding block after them;
 * libFLAC's own command line reserves eight times this by default, so it
 * is modest rather than generous.
 */
#define ENC_RESERVE 1024u

/** The largest partition order the encoder will consider. */
#define ENC_MAX_PARTITION_ORDER 6u

/** Bytes of the Ogg mapping's head before the `fLaC` it also carries. */
#define OGG_FLAC_HEAD_SIZE 13u

/** The whole first packet: the mapping head and STREAMINFO. */
#define OGG_FLAC_PACKET \
  (OGG_FLAC_HEAD_SIZE + FLAC_BLOCK_HEADER + FLAC_STREAMINFO_SIZE)

/**
 * The logical stream number Ogg FLAC files this library writes carry.
 *
 * Ogg's serial is meant to be random so that two streams concatenated
 * into one file do not collide. A fixed value makes our output
 * reproducible, which planning/audio.md 11.1 requires and `check-golden`
 * tests, and the collision it risks only matters for chained streams -
 * which this writer does not produce. A caller who needs a distinct
 * serial is chaining files together and is choosing them anyway.
 */
#define OGG_FLAC_SERIAL 0x47414f46u

/** What one encoder holds between calls. */
typedef struct {
  GAUD_Stream * stream;             ///< Borrowed; where the bytes go.
  const GAUD_Allocator * allocator; ///< Everything here comes from it.
  const GAUD_Meta * meta;           ///< Borrowed; may be NULL.
  GAUD_Meta_Policy meta_policy;     ///< What of it to write.
  GAUD_Sample_Format format;        ///< What the caller's buffers hold.
  uint32_t channels;                ///< 1 to 8.
  uint32_t sample_rate;             ///< Hertz.
  uint32_t bits;          ///< Bit depth, from @p format.
  size_t sample_bytes;    ///< Bytes per sample in the caller's buffer.

  int64_t * block;        ///< channels * ENC_BLOCK_SIZE, planar.
  /** The mid and side channels a stereo frame might be coded as. */
  int64_t * work;
  /**
   * Three blocks in one allocation, because a subframe should cost the
   * allocator nothing: the residuals, the samples with their wasted bits
   * shifted out, and the zig-zagged residuals the parameter search reads.
   * Named rather than reached by arithmetic at each call site - the first
   * draft did the latter and the overlap between the second and third was
   * a line of pointer casts nobody could check by eye.
   */
  int64_t * residual;     ///< The predictor's residuals.
  int64_t * shifted;      ///< Samples with their wasted bits removed.
  uint64_t * zigzagged;   ///< @p residual folded, for the parameter search.
  uint32_t fill;          ///< Frames buffered in @p block.
  /**
   * One seek point is kept per @p point_stride frames, and the stride
   * doubles whenever the array fills - halving what is held and keeping
   * the points spread evenly over however long the stream turns out to
   * be. A fixed array that simply stopped collecting would describe the
   * opening minutes of a long file and nothing after, which is precisely
   * the case a seek table exists for.
   */
  uint32_t point_stride;  ///< Frames between kept seek points.
  uint32_t point_since;   ///< Frames since the last one was kept.

  FLAC_Bit_Writer bw;      ///< Where a frame is assembled, bit by bit.
  unsigned char * md5_row; ///< One frame's bytes, for the running digest.
  GSEC_Md5 md5;            ///< Of the unencoded audio, for STREAMINFO.
  bool md5_live;           ///< False once finished, or if it ever failed.

  uint64_t total_frames;  ///< Sample frames written so far.
  uint64_t frame_number;  ///< The next frame's number, for its header.
  /**
   * The block sizes STREAMINFO will state, **excluding the last block**.
   *
   * RFC 9639 section 8.2 says the minimum excludes it, and the reason is
   * visible the moment it is got wrong: the last block of a stream is
   * almost always short, so folding it in makes the minimum differ from
   * the maximum, and a reader takes "min != max" to mean the stream is
   * variable-blocksize. libFLAC then expects every frame header to carry
   * a sample number where ours carries a frame number, and warns that the
   * file might not be seekable.
   *
   * That warning is the whole story of why `flac -t` is run with
   * `--warnings-as-errors` in `check-writer`: without it libFLAC exits
   * zero and the gate reported a pass for every file this encoder had
   * ever written.
   *
   * @p pending_block holds the block just written, which is folded in
   * when the *next* one arrives - so the last never is, unless it is also
   * the first.
   */
  uint32_t min_block;     ///< @see the comment above.
  uint32_t max_block;     ///< Likewise.
  uint32_t pending_block; ///< The last block written, not yet folded in.
  bool have_pending_block; ///< Whether @p pending_block holds one.
  uint32_t min_frame_bytes; ///< Smallest frame written, for STREAMINFO.
  uint32_t max_frame_bytes; ///< Largest.

  FLAC_Seek_Point * points; ///< Collected while encoding. Owned.
  size_t point_count;       ///< How many.
  size_t point_capacity;    ///< Room in @p points; fixed.

  uint64_t streaminfo_offset;  ///< Where to patch STREAMINFO at finish.
  uint64_t reserve_offset;     ///< Where the reserved padding begins.
  uint64_t first_frame_offset; ///< What a seek point's offset is from.

  /**
   * Ogg, if this is the Ogg FLAC encoder.
   *
   * The two share everything from the block buffer down and differ only
   * in where a finished frame goes, so this is a branch at one point in
   * emit_block() and another in the header, rather than a second encoder
   * carrying a second copy of the Rice coder.
   */
  bool ogg;                   ///< Whether to packetise rather than append.
  OGG_Writer ogg_writer;      ///< The page builder, when it does.
  uint64_t first_page_offset; ///< Where that page begins, to rewrite it.
  /** The first page, kept so that finish() can patch STREAMINFO into it. */
  unsigned char first_page[OGG_HEADER_FIXED + 1u + OGG_FLAC_PACKET];
  size_t first_page_size;     ///< How many bytes of it there are.
} FLAC_Encoder;

/* ------------------------------------------------------------- helpers */

/** Zig-zag an int64 residual into the unsigned value Rice codes. */
static uint64_t zigzag(int64_t value) {
  return ((uint64_t)value << 1) ^ (uint64_t)(value >> 63);
}

/** How many bits a signed value needs, including its sign bit. */
static unsigned signed_width(int64_t value) {
  uint64_t magnitude = value < 0 ? ~(uint64_t)value : (uint64_t)value;
  unsigned bits = 1u;
  while (magnitude) {
    ++bits;
    magnitude >>= 1;
  }
  return bits;
}

/* ------------------------------------------------- residual estimation */

/** The cost in bits of Rice-coding @p count values at parameter @p k. */
static uint64_t rice_bits(const uint64_t * zigzagged, uint32_t count,
    unsigned k) {
  uint64_t total = (uint64_t)count * (uint64_t)(k + 1u);
  for (uint32_t i = 0; i < count; ++i) {
    total += zigzagged[i] >> k;
  }
  return total;
}

/**
 * Pick the cheapest coding for one partition.
 *
 * @param out_escape Receives the raw width when escaping wins, else zero -
 *   except that a width of zero is itself a legal escape, so @p out_is_escape
 *   is what distinguishes them. This is the null-means-two-things trap in
 *   miniature and it is worth two parameters to avoid.
 */
static uint64_t best_partition(const uint64_t * zigzagged,
    const int64_t * raw, uint32_t count, unsigned parameter_bits,
    unsigned * out_parameter, bool * out_is_escape, unsigned * out_escape) {
  unsigned limit = parameter_bits == 4u ? 14u : 30u;
  uint64_t best = UINT64_MAX;
  unsigned best_k = 0;
  /* Start from the mean rather than sweeping every parameter: the optimum
   * is within a bit or two of log2 of the mean, and sweeping 31 of them
   * per partition per candidate predictor is most of an encoder's time. */
  uint64_t sum = 0;
  for (uint32_t i = 0; i < count; ++i) {
    sum += zigzagged[i];
  }
  unsigned estimate = 0;
  if (count) {
    uint64_t mean = sum / count;
    while ((mean >> estimate) > 0 && estimate < limit) {
      ++estimate;
    }
  }
  unsigned from = estimate > 2u ? estimate - 2u : 0;
  unsigned to = estimate + 2u < limit ? estimate + 2u : limit;
  for (unsigned k = from; k <= to; ++k) {
    uint64_t bits = rice_bits(zigzagged, count, k);
    if (bits < best) {
      best = bits;
      best_k = k;
    }
  }

  /* The escape: state a raw width once and write every residual at it.
   * For a partition of identical residuals - silence, a held tone - this
   * is a handful of bits against one per sample. */
  /* An all-zero partition is written at a width of ZERO, which the
   * format allows and which says "every residual here is zero" in five
   * bits. The maximum over signed_width() cannot find it: a value of 0
   * and a value of -1 both have a magnitude of zero after the sign fold,
   * and -1 genuinely needs one bit, so the width function must answer 1
   * for both and the all-zero case has to be recognised here. Silence
   * and a held tone are what reach it, and without this the escape of
   * width 0 is an arm of the decoder nothing in the suite exercises. */
  bool all_zero = true;
  for (uint32_t i = 0; i < count; ++i) {
    if (raw[i] != 0) {
      all_zero = false;
      break;
    }
  }
  unsigned width = 0;
  if (!all_zero) {
    for (uint32_t i = 0; i < count; ++i) {
      unsigned need = signed_width(raw[i]);
      if (need > width) {
        width = need;
      }
    }
  }
  if (width <= 32u) {
    uint64_t escaped = 5u + (uint64_t)count * width;
    if (escaped < best) {
      *out_parameter = 0;
      *out_is_escape = true;
      *out_escape = width;
      return parameter_bits + escaped;
    }
  }
  *out_parameter = best_k;
  *out_is_escape = false;
  *out_escape = 0;
  return parameter_bits + best;
}

/** One residual block's chosen partitioning. */
typedef struct {
  unsigned method;      ///< 0 for 4-bit parameters, 1 for 5-bit.
  uint32_t order;       ///< Partition order.
  uint64_t bits;        ///< Total cost including the 6-bit header.
  /** The Rice parameter for each partition, where it is Rice-coded. */
  unsigned parameter[1u << ENC_MAX_PARTITION_ORDER];
  /** Whether each partition escapes to a raw width instead. */
  bool escape[1u << ENC_MAX_PARTITION_ORDER];
  /** That raw width, where it does. Zero is legal and means all zeroes. */
  unsigned width[1u << ENC_MAX_PARTITION_ORDER];
} Residual_Plan;

/**
 * Choose a partition order and a parameter for each partition.
 *
 * @param zigzagged The residuals, already folded, @p count of them.
 * @param raw The same residuals, signed, for the escape width.
 * @param block_size The frame's block size, which the partitions tile.
 * @param order The predictor order the first partition is short by.
 */
static void plan_residual(const uint64_t * zigzagged, const int64_t * raw,
    uint32_t block_size, uint32_t order, Residual_Plan * out) {
  out->bits = UINT64_MAX;
  for (uint32_t partition_order = 0;
       partition_order <= ENC_MAX_PARTITION_ORDER; ++partition_order) {
    uint32_t partitions = 1u << partition_order;
    if (block_size % partitions != 0) {
      continue;
    }
    uint32_t per = block_size >> partition_order;
    if (per <= order) {
      continue;
    }
    for (unsigned method = 0; method < 2u; ++method) {
      unsigned parameter_bits = method == 0 ? 4u : 5u;
      Residual_Plan candidate;
      candidate.method = method;
      candidate.order = partition_order;
      candidate.bits = 6u; /* the method and the order */
      uint32_t at = 0;
      for (uint32_t p = 0; p < partitions; ++p) {
        uint32_t count = p == 0 ? per - order : per;
        candidate.bits += best_partition(zigzagged + at, raw + at, count,
            parameter_bits, &candidate.parameter[p], &candidate.escape[p],
            &candidate.width[p]);
        at += count;
      }
      if (candidate.bits < out->bits) {
        *out = candidate;
      }
    }
  }
}

/* -------------------------------------------------------- the subframe */

/** How one subframe will be written. */
typedef struct {
  unsigned type;     ///< 0 constant, 1 verbatim, 8+n fixed of order n.
  uint32_t order;    ///< The fixed order, for type 8+n.
  uint32_t wasted;   ///< Low zero bits shifted out of every sample.
  uint64_t bits;     ///< Estimated cost.
  Residual_Plan plan; ///< How the residuals are partitioned, for 8+n.
} Subframe_Plan;

/**
 * Residuals of the order-@p order fixed predictor over @p samples.
 *
 * Integer differences, which is the whole reason the encoder has no
 * floating point in it. The accumulation is 64-bit for the reason the
 * decoder's is: order four over 32-bit samples overflows anything
 * narrower, and that is undefined behaviour rather than a wrong number.
 */
static void fixed_residual(const int64_t * samples, uint32_t count,
    uint32_t order, int64_t * out) {
  for (uint32_t i = order; i < count; ++i) {
    int64_t predicted = 0;
    switch (order) {
    case 0:
      predicted = 0;
      break;
    case 1:
      predicted = samples[i - 1];
      break;
    case 2:
      predicted = 2 * samples[i - 1] - samples[i - 2];
      break;
    case 3:
      predicted = 3 * samples[i - 1] - 3 * samples[i - 2] + samples[i - 3];
      break;
    default:
      predicted = 4 * samples[i - 1] - 6 * samples[i - 2]
          + 4 * samples[i - 3] - samples[i - 4];
      break;
    }
    out[i - order] = samples[i] - predicted;
  }
}

/**
 * Choose how to code one channel's samples.
 *
 * @param scratch Two buffers of @p count values, reused across calls.
 */
static void plan_subframe(const int64_t * samples, uint32_t count,
    uint32_t depth, int64_t * residual, int64_t * shifted,
    uint64_t * zigzagged, Subframe_Plan * out) {
  memset(out, 0, sizeof(*out));

  /* Wasted bits: low zeros every sample shares. 8-bit material carried in
   * a 16-bit file is the common case, and so is anything that has been
   * through a gain of exactly 2. */
  uint64_t combined = 0;
  bool all_zero = true;
  for (uint32_t i = 0; i < count; ++i) {
    combined |= (uint64_t)samples[i];
    if (samples[i] != 0) {
      all_zero = false;
    }
  }
  uint32_t wasted = 0;
  if (!all_zero) {
    while ((combined & 1u) == 0) {
      combined >>= 1;
      ++wasted;
    }
  }
  if (wasted >= depth) {
    wasted = 0;
  }
  out->wasted = wasted;
  uint32_t effective = depth - wasted;

  /* Everything below works on the shifted samples, in scratch of their
   * own: shifting in place would corrupt the channel for the next
   * candidate decorrelation to consider. */
  if (wasted) {
    for (uint32_t i = 0; i < count; ++i) {
      shifted[i] = samples[i] >> wasted;
    }
  }
  else {
    memcpy(shifted, samples, (size_t)count * sizeof(int64_t));
  }

  /* CONSTANT, where it applies. */
  bool constant = true;
  for (uint32_t i = 1; i < count; ++i) {
    if (shifted[i] != shifted[0]) {
      constant = false;
      break;
    }
  }
  uint64_t header = 8u + (wasted ? wasted : 0u);
  if (constant) {
    out->type = 0;
    out->bits = header + effective;
    return;
  }

  /* VERBATIM is the ceiling every predictor has to beat. */
  out->type = 1u;
  out->bits = header + (uint64_t)count * effective;
  out->order = 0;

  uint32_t max_order = count < FLAC_MAX_FIXED_ORDER
      ? count
      : FLAC_MAX_FIXED_ORDER;
  for (uint32_t order = 0; order <= max_order; ++order) {
    fixed_residual(shifted, count, order, residual);
    uint32_t residuals = count - order;
    for (uint32_t i = 0; i < residuals; ++i) {
      zigzagged[i] = zigzag(residual[i]);
    }
    Residual_Plan plan;
    plan_residual(zigzagged, residual, count, order, &plan);
    if (plan.bits == UINT64_MAX) {
      continue;
    }
    uint64_t total = header + (uint64_t)order * effective + plan.bits;
    if (total < out->bits) {
      out->type = 8u + order;
      out->order = order;
      out->bits = total;
      out->plan = plan;
    }
  }
}

/** Write one subframe according to @p plan. */
static void write_subframe(FLAC_Bit_Writer * bw, const int64_t * samples,
    uint32_t count, uint32_t depth, const Subframe_Plan * plan,
    int64_t * residual, int64_t * shifted, uint64_t * zigzagged) {
  gaud_flac_bitw_write(bw, 0, 1u);
  gaud_flac_bitw_write(bw, plan->type, 6u);
  if (plan->wasted) {
    gaud_flac_bitw_write(bw, 1u, 1u);
    gaud_flac_bitw_write_unary(bw, plan->wasted - 1u);
  }
  else {
    gaud_flac_bitw_write(bw, 0, 1u);
  }
  uint32_t effective = depth - plan->wasted;

  if (plan->wasted) {
    for (uint32_t i = 0; i < count; ++i) {
      shifted[i] = samples[i] >> plan->wasted;
    }
  }
  else {
    memcpy(shifted, samples, (size_t)count * sizeof(int64_t));
  }

  if (plan->type == 0) {
    gaud_flac_bitw_write(bw, (uint64_t)shifted[0], effective);
    return;
  }
  if (plan->type == 1u) {
    for (uint32_t i = 0; i < count; ++i) {
      gaud_flac_bitw_write(bw, (uint64_t)shifted[i], effective);
    }
    return;
  }

  uint32_t order = plan->order;
  for (uint32_t i = 0; i < order; ++i) {
    gaud_flac_bitw_write(bw, (uint64_t)shifted[i], effective);
  }
  fixed_residual(shifted, count, order, residual);
  uint32_t residuals = count - order;
  for (uint32_t i = 0; i < residuals; ++i) {
    zigzagged[i] = zigzag(residual[i]);
  }

  const Residual_Plan * rp = &plan->plan;
  gaud_flac_bitw_write(bw, rp->method, 2u);
  gaud_flac_bitw_write(bw, rp->order, 4u);
  unsigned parameter_bits = rp->method == 0 ? 4u : 5u;
  unsigned escape_code = rp->method == 0 ? 0xFu : 0x1Fu;
  uint32_t partitions = 1u << rp->order;
  uint32_t per = count >> rp->order;
  uint32_t at = 0;
  for (uint32_t p = 0; p < partitions; ++p) {
    uint32_t n = p == 0 ? per - order : per;
    if (rp->escape[p]) {
      gaud_flac_bitw_write(bw, escape_code, parameter_bits);
      gaud_flac_bitw_write(bw, rp->width[p], 5u);
      for (uint32_t i = 0; i < n; ++i) {
        gaud_flac_bitw_write(bw, (uint64_t)residual[at + i], rp->width[p]);
      }
    }
    else {
      unsigned k = rp->parameter[p];
      gaud_flac_bitw_write(bw, k, parameter_bits);
      for (uint32_t i = 0; i < n; ++i) {
        uint64_t value = zigzagged[at + i];
        gaud_flac_bitw_write_unary(bw, (uint32_t)(value >> k));
        gaud_flac_bitw_write(bw, value, k);
      }
    }
    at += n;
  }
}

/* --------------------------------------------------------- frame header */

/** The block-size code for @p size, or 0 when none of the constants fit. */
static uint32_t block_size_code(uint32_t size) {
  if (size == 192u) {
    return 1u;
  }
  for (uint32_t i = 2; i <= 5u; ++i) {
    if (size == (576u << (i - 2u))) {
      return i;
    }
  }
  for (uint32_t i = 8; i <= 15u; ++i) {
    if (size == (256u << (i - 8u))) {
      return i;
    }
  }
  return 0;
}

/** The sample-rate code for @p rate, or 0 to defer to STREAMINFO. */
static uint32_t rate_code(uint32_t rate) {
  static const uint32_t rates[12] = {
      0, 88200u, 176400u, 192000u, 8000u, 16000u, 22050u, 24000u, 32000u,
      44100u, 48000u, 96000u};
  for (uint32_t i = 1; i < 12u; ++i) {
    if (rate == rates[i]) {
      return i;
    }
  }
  return 0;
}

/** The bit-depth code for @p bits, or 0 to defer to STREAMINFO. */
static uint32_t depth_code(uint32_t bits) {
  switch (bits) {
  case 8u:
    return 1u;
  case 12u:
    return 2u;
  case 16u:
    return 4u;
  case 20u:
    return 5u;
  case 24u:
    return 6u;
  case 32u:
    return 7u;
  default:
    return 0;
  }
}

/* ------------------------------------------------------- the frame body */

/**
 * Decide the stereo decorrelation and lay the channels out for coding.
 *
 * @param out_assignment The channel assignment the header will carry.
 * @return The planned subframes, one per coded channel.
 */
static void plan_frame(FLAC_Encoder * enc, uint32_t count,
    Subframe_Plan plans[FLAC_MAX_CHANNELS], uint32_t * out_assignment,
    int64_t * coded[FLAC_MAX_CHANNELS]) {
  uint32_t channels = enc->channels;
  for (uint32_t ch = 0; ch < channels; ++ch) {
    coded[ch] = enc->block + (size_t)ch * ENC_BLOCK_SIZE;
  }
  *out_assignment = channels - 1u;

  if (channels != 2u) {
    for (uint32_t ch = 0; ch < channels; ++ch) {
      plan_subframe(coded[ch], count, enc->bits, enc->residual,
          enc->shifted, enc->zigzagged, &plans[ch]);
    }
    return;
  }

  /* Stereo: four ways to carry two channels, and the cheapest wins. The
   * side channel is one bit wider in each of the three decorrelated
   * forms, which is accounted for by planning it at depth + 1 rather than
   * by hoping it fits. */
  int64_t * left = enc->block;
  int64_t * right = enc->block + ENC_BLOCK_SIZE;
  int64_t * side = enc->work;
  int64_t * mid = enc->work + ENC_BLOCK_SIZE;
  for (uint32_t i = 0; i < count; ++i) {
    side[i] = left[i] - right[i];
    /* An arithmetic shift, so that the halving rounds towards negative
     * infinity - which is what the decoder's reconstruction undoes. A
     * division by two would round towards zero and lose the sample. */
    mid[i] = (left[i] + right[i]) >> 1;
  }

  Subframe_Plan plan_left;
  Subframe_Plan plan_right;
  Subframe_Plan plan_side;
  Subframe_Plan plan_mid;
  plan_subframe(left, count, enc->bits, enc->residual, enc->shifted,
      enc->zigzagged, &plan_left);
  plan_subframe(right, count, enc->bits, enc->residual, enc->shifted,
      enc->zigzagged, &plan_right);
  plan_subframe(side, count, enc->bits + 1u, enc->residual, enc->shifted,
      enc->zigzagged, &plan_side);
  plan_subframe(mid, count, enc->bits, enc->residual, enc->shifted,
      enc->zigzagged, &plan_mid);

  uint64_t independent = plan_left.bits + plan_right.bits;
  uint64_t left_side = plan_left.bits + plan_side.bits;
  uint64_t right_side = plan_side.bits + plan_right.bits;
  uint64_t mid_side = plan_mid.bits + plan_side.bits;

  uint64_t best = independent;
  *out_assignment = 1u;
  plans[0] = plan_left;
  plans[1] = plan_right;
  coded[0] = left;
  coded[1] = right;
  if (left_side < best) {
    best = left_side;
    *out_assignment = 8u;
    plans[0] = plan_left;
    plans[1] = plan_side;
    coded[0] = left;
    coded[1] = side;
  }
  if (right_side < best) {
    best = right_side;
    *out_assignment = 9u;
    plans[0] = plan_side;
    plans[1] = plan_right;
    coded[0] = side;
    coded[1] = right;
  }
  if (mid_side < best) {
    *out_assignment = 0xAu;
    plans[0] = plan_mid;
    plans[1] = plan_side;
    coded[0] = mid;
    coded[1] = side;
  }
}

/**
 * Fold a block's size into STREAMINFO's minimum and maximum, one behind.
 *
 * One behind, so that the last block of the stream never contributes. See
 * ::FLAC_Encoder::min_block for why that matters.
 */
static void note_block_size(FLAC_Encoder * enc, uint32_t count) {
  if (enc->have_pending_block) {
    uint32_t previous = enc->pending_block;
    if (previous < enc->min_block || enc->min_block == 0) {
      enc->min_block = previous;
    }
    if (previous > enc->max_block) {
      enc->max_block = previous;
    }
  }
  enc->pending_block = count;
  enc->have_pending_block = true;
}

/** Encode and emit the @p count frames buffered in @p enc. */
static GAUD_Result emit_block(FLAC_Encoder * enc, uint32_t count) {
  if (count == 0) {
    return GAUD_OK;
  }

  Subframe_Plan plans[FLAC_MAX_CHANNELS];
  int64_t * coded[FLAC_MAX_CHANNELS];
  uint32_t assignment = 0;
  plan_frame(enc, count, plans, &assignment, coded);

  gaud_flac_bitw_reset(&enc->bw);
  FLAC_Bit_Writer * bw = &enc->bw;
  gaud_flac_bitw_write(bw, 0x3FFEu, 14u);
  gaud_flac_bitw_write(bw, 0, 1u);
  /* Fixed block size, so the coded number is the frame number. Every
   * frame but the last is ENC_BLOCK_SIZE, which is exactly what the fixed
   * strategy means - a shorter final frame does not make a stream
   * variable-blocksize. */
  gaud_flac_bitw_write(bw, 0, 1u);

  uint32_t block_code = block_size_code(count);
  uint32_t extra_block = 0;
  if (block_code == 0) {
    if (count <= 256u) {
      block_code = 6u;
      extra_block = 1u;
    }
    else {
      block_code = 7u;
      extra_block = 2u;
    }
  }
  gaud_flac_bitw_write(bw, block_code, 4u);
  gaud_flac_bitw_write(bw, rate_code(enc->sample_rate), 4u);
  gaud_flac_bitw_write(bw, assignment, 4u);
  gaud_flac_bitw_write(bw, depth_code(enc->bits), 3u);
  gaud_flac_bitw_write(bw, 0, 1u);
  gaud_flac_bitw_write_coded_number(bw, enc->frame_number);
  if (extra_block == 1u) {
    gaud_flac_bitw_write(bw, count - 1u, 8u);
  }
  else if (extra_block == 2u) {
    gaud_flac_bitw_write(bw, count - 1u, 16u);
  }
  if (bw->failed) {
    return GAUD_ERR_OOM;
  }
  /* The header is byte-aligned by construction, and the CRC-8 covers all
   * of it. Asserting rather than assuming, because a field written at the
   * wrong width would otherwise checksum a span offset by a few bits and
   * produce a file whose every frame is rejected for the wrong reason. */
  if (bw->bits != 0) {
    return GAUD_ERR_INTERNAL;
  }
  gaud_flac_bitw_write(bw, gaud_flac_crc8(bw->data, bw->size), 8u);

  uint32_t coded_channels = enc->channels == 2u && assignment >= 8u
      ? 2u
      : enc->channels;
  for (uint32_t ch = 0; ch < coded_channels; ++ch) {
    uint32_t depth = enc->bits;
    if ((assignment == 8u || assignment == 0xAu) && ch == 1u) {
      ++depth;
    }
    else if (assignment == 9u && ch == 0) {
      ++depth;
    }
    write_subframe(bw, coded[ch], count, depth, &plans[ch], enc->residual,
        enc->shifted, enc->zigzagged);
  }
  gaud_flac_bitw_align(bw);
  if (bw->failed) {
    return GAUD_ERR_OOM;
  }
  gaud_flac_bitw_write(bw, gaud_flac_crc16(bw->data, bw->size), 16u);
  if (bw->failed) {
    return GAUD_ERR_OOM;
  }

  if (enc->ogg) {
    /* One frame to a page, flushed, which makes the granule position the
     * sample number of the last sample on the page with no bookkeeping -
     * and that is the number a seeker reads. The page overhead is about
     * thirty bytes against a frame of several kilobytes. */
    GAUD_Result paged = gaud_ogg_writer_packet(&enc->ogg_writer, bw->data,
        bw->size, enc->total_frames + count, true, false);
    if (paged != GAUD_OK) {
      return paged;
    }
    if (bw->size < enc->min_frame_bytes || enc->min_frame_bytes == 0) {
      enc->min_frame_bytes = (uint32_t)bw->size;
    }
    if (bw->size > enc->max_frame_bytes) {
      enc->max_frame_bytes = (uint32_t)bw->size;
    }
    note_block_size(enc, count);
    enc->total_frames += count;
    ++enc->frame_number;
    return GAUD_OK;
  }

  uint64_t at = gaud_stream_tell(enc->stream);
  GAUD_Result written = gaud_stream_write(enc->stream, bw->data, bw->size);
  if (written != GAUD_OK) {
    return written;
  }

  if (++enc->point_since >= enc->point_stride) {
    enc->point_since = 0;
    if (enc->point_count == enc->point_capacity) {
      /* Full: keep every second point and halve the rate from here. The
       * points that remain are still evenly spaced, which is the only
       * property the table needs, and the memory is fixed however long
       * the stream runs. */
      for (size_t i = 0; i * 2u < enc->point_count; ++i) {
        enc->points[i] = enc->points[i * 2u];
      }
      enc->point_count = (enc->point_count + 1u) / 2u;
      enc->point_stride *= 2u;
    }
    enc->points[enc->point_count].sample = enc->total_frames;
    enc->points[enc->point_count].offset = at - enc->first_frame_offset;
    enc->points[enc->point_count].frame_samples = (uint16_t)count;
    ++enc->point_count;
  }

  if (bw->size < enc->min_frame_bytes || enc->min_frame_bytes == 0) {
    enc->min_frame_bytes = (uint32_t)bw->size;
  }
  if (bw->size > enc->max_frame_bytes) {
    enc->max_frame_bytes = (uint32_t)bw->size;
  }
  note_block_size(enc, count);
  enc->total_frames += count;
  ++enc->frame_number;
  return GAUD_OK;
}

/* ------------------------------------------------------ metadata blocks */

/** Fill in a metadata block header. */
static void fill_block_header(
    unsigned char header[FLAC_BLOCK_HEADER], bool last, unsigned type,
    uint32_t size) {
  header[0] = (unsigned char)((last ? 0x80u : 0) | (type & 0x7Fu));
  header[1] = (unsigned char)((size >> 16) & 0xFFu);
  header[2] = (unsigned char)((size >> 8) & 0xFFu);
  header[3] = (unsigned char)(size & 0xFFu);
}

/** Write a metadata block header straight to a native stream. */
static GAUD_Result put_block_header(
    GAUD_Stream * stream, bool last, unsigned type, uint32_t size) {
  unsigned char header[FLAC_BLOCK_HEADER];
  fill_block_header(header, last, type, size);
  return gaud_stream_write(stream, header, sizeof(header));
}

/**
 * Write one complete metadata block, header and body.
 *
 * **The one place the two containers diverge in the header**, which is why
 * every block below goes through it: natively a block is four bytes and a
 * body appended to the stream, and in Ogg it is one packet on one page.
 */
static GAUD_Result put_block(FLAC_Encoder * enc, bool last, unsigned type,
    const unsigned char * body, size_t size) {
  if (size > 0xFFFFFFu) {
    return GAUD_ERR_LIMIT;
  }
  if (!enc->ogg) {
    GAUD_Result result
        = put_block_header(enc->stream, last, type, (uint32_t)size);
    if (result != GAUD_OK || size == 0) {
      return result;
    }
    return gaud_stream_write(enc->stream, body, size);
  }
  unsigned char * packet
      = gcu_allocator_malloc(enc->allocator, FLAC_BLOCK_HEADER + size);
  if (!packet) {
    return GAUD_ERR_OOM;
  }
  fill_block_header(packet, last, type, (uint32_t)size);
  if (size) {
    memcpy(packet + FLAC_BLOCK_HEADER, body, size);
  }
  /* Header packets carry a granule position of zero: no sample has been
   * finished yet, and a non-zero one here would tell a seeker that the
   * stream already has audio in it. */
  GAUD_Result result = gaud_ogg_writer_packet(
      &enc->ogg_writer, packet, FLAC_BLOCK_HEADER + size, 0, true, false);
  gcu_allocator_free(enc->allocator, packet);
  return result;
}

/** Build and write a PICTURE block for one of @p meta's pictures. */
static GAUD_Result write_picture(
    FLAC_Encoder * enc, const GAUD_Picture * picture) {
  const char * mime = picture->mime_type ? picture->mime_type : "";
  const char * description
      = picture->description ? picture->description : "";
  size_t mime_length = strlen(mime);
  size_t description_length = strlen(description);
  size_t size = 32u + mime_length + description_length + picture->size;
  if (size > 0xFFFFFFu) {
    /* A metadata block's length is 24 bits. A picture that does not fit is
     * dropped with no diagnostic channel to say so here, which is why the
     * limit is also checked when reading. */
    return GAUD_OK;
  }
  unsigned char * body = gcu_allocator_malloc(enc->allocator, size);
  if (!body) {
    return GAUD_ERR_OOM;
  }
  size_t at = 0;
  gaud_wr_u32be(body + at, gaud_picture_kind_to_apic(picture->kind));
  at += 4;
  gaud_wr_u32be(body + at, (uint32_t)mime_length);
  at += 4;
  memcpy(body + at, mime, mime_length);
  at += mime_length;
  gaud_wr_u32be(body + at, (uint32_t)description_length);
  at += 4;
  memcpy(body + at, description, description_length);
  at += description_length;
  /* The stated dimensions are written back as they were stated, not as
   * this build measured them. A verified picture whose claim was wrong
   * would otherwise be silently corrected on a round trip, which changes
   * a file the caller did not ask to change and hides the defect from the
   * next reader. */
  gaud_wr_u32be(body + at, picture->stated_width);
  gaud_wr_u32be(body + at + 4, picture->stated_height);
  gaud_wr_u32be(body + at + 8, picture->stated_depth);
  gaud_wr_u32be(body + at + 12, picture->stated_colors);
  gaud_wr_u32be(body + at + 16, (uint32_t)picture->size);
  at += 20;
  if (picture->size) {
    memcpy(body + at, picture->data, picture->size);
  }

  GAUD_Result result = put_block(enc, false, FLAC_BLOCK_PICTURE, body, size);
  gcu_allocator_free(enc->allocator, body);
  return result;
}

/** Write every metadata block that goes in front of the audio. */
static GAUD_Result write_metadata(FLAC_Encoder * enc) {
  GAUD_Result result = GAUD_OK;
  bool keep_common = enc->meta_policy == GAUD_META_PRESERVE_ALL
      || enc->meta_policy == GAUD_META_KEEP_COMMON_ONLY;
  bool keep_raw = enc->meta_policy == GAUD_META_PRESERVE_ALL
      || enc->meta_policy == GAUD_META_KEEP_RAW_ONLY;

  /* VORBIS_COMMENT is written whether or not there are tags, because the
   * vendor string is a tag in its own right and every FLAC file in the
   * wild carries one. */
  unsigned char * comment = NULL;
  size_t comment_size = 0;
  result = gaud_vorbis_comment_build(keep_common ? enc->meta : NULL,
      "Ghoti.io Audio", enc->allocator, &comment, &comment_size);
  if (result != GAUD_OK) {
    return result;
  }
  result = put_block(
      enc, false, FLAC_BLOCK_VORBIS_COMMENT, comment, comment_size);
  gcu_allocator_free(enc->allocator, comment);
  if (result != GAUD_OK) {
    return result;
  }

  if (keep_common && enc->meta) {
    size_t pictures = gaud_meta_picture_count(enc->meta);
    for (size_t i = 0; i < pictures; ++i) {
      const GAUD_Picture * picture = gaud_meta_picture(enc->meta, i);
      if (!picture) {
        continue;
      }
      result = write_picture(enc, picture);
      if (result != GAUD_OK) {
        return result;
      }
    }
  }

  if (keep_raw && enc->meta) {
    size_t blocks = gaud_meta_raw_count(enc->meta);
    for (size_t i = 0; i < blocks; ++i) {
      const char * scheme = NULL;
      const char * id = NULL;
      const void * data = NULL;
      size_t size = 0;
      if (gaud_meta_raw(enc->meta, i, &scheme, &id, &data, &size) != GAUD_OK) {
        continue;
      }
      if (!scheme || strcmp(scheme, "flac") != 0 || !id) {
        /* Another container's raw bytes. An ID3 frame has no place in a
         * FLAC metadata block and writing it under a made-up type would
         * produce a file whose own reader could not put it back. */
        continue;
      }
      unsigned type = 0;
      if (strcmp(id, "APPLICATION") == 0) {
        type = FLAC_BLOCK_APPLICATION;
      }
      else if (strcmp(id, "CUESHEET") == 0) {
        type = FLAC_BLOCK_CUESHEET;
      }
      else if (strncmp(id, "BLOCK_", 6) == 0 && id[6] && id[7] && !id[8]) {
        type = (unsigned)((id[6] - '0') * 10 + (id[7] - '0'));
        if (type > 126u) {
          continue;
        }
      }
      else {
        continue;
      }
      if (size > 0xFFFFFFu) {
        continue;
      }
      result = put_block(enc, false, type, data, size);
      if (result != GAUD_OK) {
        return result;
      }
    }
  }
  return GAUD_OK;
}

/* ------------------------------------------------------------ STREAMINFO */

/** Build the 34 bytes of STREAMINFO, taking the digest as it goes. */
static GAUD_Result build_streaminfo(
    FLAC_Encoder * enc, unsigned char block[FLAC_STREAMINFO_SIZE]) {
  /* A stream of exactly one block has no "excluding the last" to apply:
   * the only block there is is the answer, and leaving the fields at zero
   * would state a minimum block size the format forbids. */
  if (enc->min_block == 0 && enc->have_pending_block) {
    enc->min_block = enc->pending_block;
    enc->max_block = enc->pending_block;
  }
  FLAC_Bit_Writer bw;
  gaud_flac_bitw_init(&bw, enc->allocator);
  gaud_flac_bitw_write(&bw, enc->min_block ? enc->min_block : ENC_BLOCK_SIZE,
      16u);
  gaud_flac_bitw_write(&bw, enc->max_block ? enc->max_block : ENC_BLOCK_SIZE,
      16u);
  gaud_flac_bitw_write(&bw, enc->min_frame_bytes, 24u);
  gaud_flac_bitw_write(&bw, enc->max_frame_bytes, 24u);
  gaud_flac_bitw_write(&bw, enc->sample_rate, 20u);
  gaud_flac_bitw_write(&bw, enc->channels - 1u, 3u);
  gaud_flac_bitw_write(&bw, enc->bits - 1u, 5u);
  gaud_flac_bitw_write(&bw, enc->total_frames, 36u);
  if (bw.failed || bw.size != 18u || bw.bits != 0) {
    gaud_flac_bitw_free(&bw);
    return bw.failed ? GAUD_ERR_OOM : GAUD_ERR_INTERNAL;
  }

  memcpy(block, bw.data, 18u);
  gaud_flac_bitw_free(&bw);

  unsigned char digest[GSEC_MD5_DIGEST_LEN];
  if (enc->md5_live && gsec_md5_final(&enc->md5, digest) == GSEC_OK) {
    memcpy(block + 18, digest, sizeof(digest));
  }
  else {
    /* All zero is the format's spelling of "not computed". It is legal and
     * it is a real loss - `flac -t` says it cannot verify - so it happens
     * only when the digest itself failed, never as a shortcut. */
    memset(block + 18, 0, 16u);
  }
  enc->md5_live = false;
  return GAUD_OK;
}

/** Overwrite the reserved padding with a seek table and smaller padding. */
static GAUD_Result write_seektable(FLAC_Encoder * enc) {
  if (gaud_stream_seek(enc->stream, (int64_t)enc->reserve_offset,
          GAUD_SEEK_SET)
      != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  /* The reservation holds a seek table's header and points, then a
   * padding block's header and whatever is left. Both headers are four
   * bytes, so the points get what remains of the kilobyte after eight. */
  size_t room = ENC_RESERVE >= 8u ? ENC_RESERVE - 8u : 0;
  size_t points = room / 18u;
  if (points > enc->point_count) {
    points = enc->point_count;
  }

  GAUD_Result result = put_block_header(
      enc->stream, false, FLAC_BLOCK_SEEKTABLE, (uint32_t)(points * 18u));
  if (result != GAUD_OK) {
    return result;
  }
  /* The points held are already spread evenly over the whole stream - the
   * decimation in emit_block is what makes that true - so these are taken
   * in order rather than resampled again. */
  for (size_t pick = 0; pick < points; ++pick) {
    unsigned char entry[18];
    gaud_wr_u32be(entry, (uint32_t)(enc->points[pick].sample >> 32));
    gaud_wr_u32be(entry + 4, (uint32_t)enc->points[pick].sample);
    gaud_wr_u32be(entry + 8, (uint32_t)(enc->points[pick].offset >> 32));
    gaud_wr_u32be(entry + 12, (uint32_t)enc->points[pick].offset);
    gaud_wr_u16be(entry + 16, enc->points[pick].frame_samples);
    result = gaud_stream_write(enc->stream, entry, sizeof(entry));
    if (result != GAUD_OK) {
      return result;
    }
  }

  size_t written = 4u + points * 18u;
  size_t remaining = ENC_RESERVE - written;
  /* The last metadata block before the audio, whatever its size. */
  result = put_block_header(
      enc->stream, true, FLAC_BLOCK_PADDING, (uint32_t)(remaining - 4u));
  if (result != GAUD_OK) {
    return result;
  }
  unsigned char zeros[64];
  memset(zeros, 0, sizeof(zeros));
  size_t left = remaining - 4u;
  while (left) {
    size_t chunk = left < sizeof(zeros) ? left : sizeof(zeros);
    result = gaud_stream_write(enc->stream, zeros, chunk);
    if (result != GAUD_OK) {
      return result;
    }
    left -= chunk;
  }
  return GAUD_OK;
}

/* ------------------------------------------------------------- the vtable */

/** Append one buffer's frames, emitting blocks as they fill. */
static GAUD_Result encoder_write(
    GAUD_Encoder * encoder, const GAUD_Buffer * buffer) {
  FLAC_Encoder * enc = gaud_encoder_private(encoder);
  size_t frames = gaud_buffer_frames(buffer);
  const unsigned char * data = gaud_buffer_data_const(buffer);
  size_t frame_size = gaud_buffer_frame_size(buffer);
  size_t width = enc->sample_bytes;
  bool little = gaud_host_is_little_endian();

  for (size_t f = 0; f < frames; ++f) {
    for (uint32_t ch = 0; ch < enc->channels; ++ch) {
      const unsigned char * at = data + f * frame_size + (size_t)ch * width;
      uint64_t raw = 0;
      for (size_t b = 0; b < width; ++b) {
        /* Read the sample in the host's order and assemble it as a value,
         * so that everything downstream - the predictors, the digest and
         * the bits written - is the same on either endianness. */
        raw |= (uint64_t)at[little ? b : width - 1u - b] << (8u * b);
      }
      /* Sign-extend from the format's width. */
      uint64_t sign = (uint64_t)1 << (enc->bits - 1u);
      int64_t value = (raw & sign)
          ? -(int64_t)((((uint64_t)1 << enc->bits) - raw) - 1u) - 1
          : (int64_t)raw;
      enc->block[(size_t)ch * ENC_BLOCK_SIZE + enc->fill] = value;
    }
    ++enc->fill;
    if (enc->fill == ENC_BLOCK_SIZE) {
      GAUD_Result result = emit_block(enc, enc->fill);
      if (result != GAUD_OK) {
        return result;
      }
      enc->fill = 0;
    }
  }

  /* The digest is over the samples as the format defines them: signed,
   * little-endian, packed to the bit depth's byte width, interleaved. It
   * is built here rather than taken from the caller's buffer, because the
   * caller's buffer is in the host's byte order and the digest must not
   * be. */
  for (size_t f = 0; f < frames; ++f) {
    for (uint32_t ch = 0; ch < enc->channels; ++ch) {
      const unsigned char * at = data + f * frame_size + (size_t)ch * width;
      unsigned char * to = enc->md5_row + ((size_t)ch * width);
      for (size_t b = 0; b < width; ++b) {
        to[b] = at[little ? b : width - 1u - b];
      }
    }
    if (enc->md5_live
        && gsec_md5_update(&enc->md5, enc->md5_row, width * enc->channels)
            != GSEC_OK) {
      enc->md5_live = false;
    }
  }
  return GAUD_OK;
}

static GAUD_Result encoder_finish(GAUD_Encoder * encoder) {
  FLAC_Encoder * enc = gaud_encoder_private(encoder);
  GAUD_Result result = emit_block(enc, enc->fill);
  if (result != GAUD_OK) {
    return result;
  }
  enc->fill = 0;

  unsigned char info[FLAC_STREAMINFO_SIZE];
  if (enc->ogg) {
    /* The stream has to be closed before the first page is patched,
     * because flushing writes at the end and patching writes at the
     * start. */
    result = gaud_ogg_writer_flush(&enc->ogg_writer, true);
    if (result != GAUD_OK) {
      return result;
    }
    result = build_streaminfo(enc, info);
    if (result != GAUD_OK) {
      return result;
    }
    /* Patching STREAMINFO changes the first page's body, so its checksum
     * has to be recomputed and the whole page rewritten. Its length does
     * not change - STREAMINFO is a fixed 34 bytes - which is the only
     * reason an in-place patch is possible at all. */
    size_t at = OGG_HEADER_FIXED + 1u + OGG_FLAC_HEAD_SIZE
        + FLAC_BLOCK_HEADER;
    memcpy(enc->first_page + at, info, sizeof(info));
    memset(enc->first_page + 22, 0, 4);
    gaud_wr_u32le(enc->first_page + 22,
        gaud_ogg_crc32(enc->first_page, enc->first_page_size));
    if (gaud_stream_seek(enc->stream, (int64_t)enc->first_page_offset,
            GAUD_SEEK_SET)
        != GAUD_OK) {
      return GAUD_ERR_IO;
    }
    result = gaud_stream_write(
        enc->stream, enc->first_page, enc->first_page_size);
    if (result != GAUD_OK) {
      return result;
    }
    gaud_encoder_add_frames(encoder, 0);
    return gaud_stream_seek(enc->stream, 0, GAUD_SEEK_END);
  }

  result = write_seektable(enc);
  if (result != GAUD_OK) {
    return result;
  }
  result = build_streaminfo(enc, info);
  if (result != GAUD_OK) {
    return result;
  }
  if (gaud_stream_seek(
          enc->stream, (int64_t)enc->streaminfo_offset, GAUD_SEEK_SET)
      != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  result = gaud_stream_write(enc->stream, info, sizeof(info));
  if (result != GAUD_OK) {
    return result;
  }
  gaud_encoder_add_frames(encoder, 0);
  return gaud_stream_seek(enc->stream, 0, GAUD_SEEK_END);
}

static void encoder_close(GAUD_Encoder * encoder) {
  FLAC_Encoder * enc = gaud_encoder_private(encoder);
  if (!enc) {
    return;
  }
  gaud_flac_bitw_free(&enc->bw);
  gcu_allocator_free(enc->allocator, enc->block);
  gcu_allocator_free(enc->allocator, enc->work);
  gcu_allocator_free(enc->allocator, enc->residual);
  gcu_allocator_free(enc->allocator, enc->points);
  gcu_allocator_free(enc->allocator, enc->md5_row);
  gcu_allocator_free(enc->allocator, enc);
}

static const GAUD_Encoder_Vtable flac_encoder_vtable = {
    .write = encoder_write,
    .finish = encoder_finish,
    .close = encoder_close,
};

/**
 * Write the header a native FLAC stream begins with.
 *
 * `fLaC`, a STREAMINFO of zeroes to be patched at finish, the metadata
 * blocks, and a kilobyte of padding the seek table is written into.
 */
static GAUD_Result open_native(FLAC_Encoder * enc) {
  GAUD_Stream * stream = enc->stream;
  GAUD_Result result = gaud_stream_write(stream, FLAC_MAGIC, 4u);
  if (result != GAUD_OK) {
    return result;
  }
  enc->streaminfo_offset = gaud_stream_tell(stream) + FLAC_BLOCK_HEADER;
  /* STREAMINFO is written twice: zeroes now, because the frame sizes and
   * the digest are not known until the audio has been written, and the
   * real thing from gaud_encoder_finish(). This is why the encoder needs
   * a seekable sink and says so. */
  unsigned char placeholder[FLAC_STREAMINFO_SIZE];
  memset(placeholder, 0, sizeof(placeholder));
  result = put_block(
      enc, false, FLAC_BLOCK_STREAMINFO, placeholder, sizeof(placeholder));
  if (result != GAUD_OK) {
    return result;
  }
  result = write_metadata(enc);
  if (result != GAUD_OK) {
    return result;
  }

  enc->reserve_offset = gaud_stream_tell(stream);
  result = put_block_header(stream, true, FLAC_BLOCK_PADDING,
      ENC_RESERVE - FLAC_BLOCK_HEADER);
  if (result != GAUD_OK) {
    return result;
  }
  unsigned char zeros[64];
  memset(zeros, 0, sizeof(zeros));
  size_t left = ENC_RESERVE - FLAC_BLOCK_HEADER;
  while (left) {
    size_t chunk = left < sizeof(zeros) ? left : sizeof(zeros);
    result = gaud_stream_write(stream, zeros, chunk);
    if (result != GAUD_OK) {
      return result;
    }
    left -= chunk;
  }
  enc->first_frame_offset = gaud_stream_tell(stream);
  return GAUD_OK;
}

/**
 * Write the header an Ogg FLAC stream begins with.
 *
 * The mapping's first packet, alone on the first page, then every other
 * metadata block as a packet of its own, then an empty PADDING block to
 * carry the last-block flag. No seek table: its offsets describe a native
 * stream's bytes and mean nothing here, and Ogg's granule positions are
 * what a seeker reads instead.
 */
static GAUD_Result open_ogg(FLAC_Encoder * enc) {
  gaud_ogg_writer_init(&enc->ogg_writer, enc->stream, OGG_FLAC_SERIAL);
  enc->first_page_offset = gaud_stream_tell(enc->stream);

  unsigned char packet[OGG_FLAC_PACKET];
  memset(packet, 0, sizeof(packet));
  packet[0] = 0x7Fu;
  memcpy(packet + 1, "FLAC", 4);
  packet[5] = 1u; /* mapping major version */
  packet[6] = 0;  /* mapping minor version */
  /* The number of header packets that follow, big-endian - and the one
   * big-endian field in a structure whose other multi-byte numbers (the
   * Vorbis comment's lengths) are little-endian. Zero means "unknown",
   * which is legal and is what this writes: the count is not known until
   * the metadata has been laid out, and a reader that needed it would be
   * unable to read a stream written to a pipe. */
  packet[7] = 0;
  packet[8] = 0;
  memcpy(packet + 9, FLAC_MAGIC, 4);
  fill_block_header(packet + OGG_FLAC_HEAD_SIZE, false,
      FLAC_BLOCK_STREAMINFO, FLAC_STREAMINFO_SIZE);
  /* The STREAMINFO body stays zero until finish patches it in place. */

  GAUD_Result result = gaud_ogg_writer_packet(
      &enc->ogg_writer, packet, sizeof(packet), 0, true, false);
  if (result != GAUD_OK) {
    return result;
  }
  /* The page just written, reconstructed, so that finish can patch
   * STREAMINFO into it and recompute its checksum without reading the
   * file back. One segment, because the packet is 51 bytes. */
  enc->first_page_size = OGG_HEADER_FIXED + 1u + sizeof(packet);
  if (gaud_stream_seek(enc->stream, (int64_t)enc->first_page_offset,
          GAUD_SEEK_SET)
      != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  if (gaud_stream_read(enc->stream, enc->first_page, enc->first_page_size)
      != enc->first_page_size) {
    return GAUD_ERR_IO;
  }
  if (gaud_stream_seek(enc->stream, 0, GAUD_SEEK_END) != GAUD_OK) {
    return GAUD_ERR_IO;
  }

  result = write_metadata(enc);
  if (result != GAUD_OK) {
    return result;
  }
  /* An empty PADDING block, purely to carry the last-block flag. Making
   * the last metadata block carry it would mean knowing which one that
   * is before writing any of them, and four bytes is cheaper than that
   * bookkeeping being wrong when a file has no pictures. */
  return put_block(enc, true, FLAC_BLOCK_PADDING, NULL, 0);
}

/** Both encoders, which differ only in their header and their framing. */
static GAUD_Result encoder_open(bool ogg, GAUD_Stream * stream,
    const GAUD_Encode_Params * params, GAUD_Encoder ** out_encoder) {
  if (params->coding != GAUD_CODING_PCM
      && params->coding != GAUD_CODING_FLAC) {
    return GAUD_ERR_UNSUPPORTED;
  }
  uint32_t bits = 0;
  switch (params->format) {
  case GAUD_SAMPLE_S8:
    bits = 8u;
    break;
  case GAUD_SAMPLE_S16:
    bits = 16u;
    break;
  case GAUD_SAMPLE_S24:
    bits = 24u;
    break;
  case GAUD_SAMPLE_S32:
    bits = 32u;
    break;
  default:
    /* U8 is the one that is tempting and wrong: FLAC's 8-bit samples are
     * signed, and writing WAV's unsigned bytes into one would move every
     * sample by half the scale. A caller with U8 converts first, which
     * ops.h does and this must not do silently. */
    return GAUD_ERR_UNSUPPORTED;
  }
  if (params->layout.channels == 0
      || params->layout.channels > FLAC_MAX_CHANNELS) {
    return GAUD_ERR_UNSUPPORTED;
  }
  if (params->sample_rate == 0 || params->sample_rate >= (1u << 20)) {
    return GAUD_ERR_INVALID;
  }

  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);
  GAUD_Result failure = GAUD_ERR_INTERNAL;
  FLAC_Encoder * enc = gcu_allocator_malloc(allocator, sizeof(*enc));
  if (!enc) {
    return GAUD_ERR_OOM;
  }
  memset(enc, 0, sizeof(*enc));
  enc->ogg = ogg;
  enc->stream = stream;
  enc->allocator = allocator;
  enc->meta = params->meta;
  enc->meta_policy = params->meta_policy;
  enc->format = params->format;
  enc->channels = params->layout.channels;
  enc->sample_rate = params->sample_rate;
  enc->bits = bits;
  enc->sample_bytes = bits / 8u;
  gaud_flac_bitw_init(&enc->bw, allocator);

  size_t block_values = (size_t)enc->channels * ENC_BLOCK_SIZE;
  enc->block = gcu_allocator_malloc(allocator, block_values * sizeof(int64_t));
  enc->work
      = gcu_allocator_malloc(allocator, 2u * ENC_BLOCK_SIZE * sizeof(int64_t));
  /* Three blocks: the residuals, the shifted samples the planner works
   * on, and the zig-zagged copy the parameter search reads. One
   * allocation rather than three so a subframe costs no allocator traffic
   * at all. */
  enc->residual
      = gcu_allocator_malloc(allocator, 3u * ENC_BLOCK_SIZE * sizeof(int64_t));
  enc->md5_row
      = gcu_allocator_malloc(allocator, (size_t)enc->channels * 4u);
  enc->point_capacity = (ENC_RESERVE - 8u) / 18u;
  enc->points = gcu_allocator_malloc(
      allocator, enc->point_capacity * sizeof(*enc->points));
  if (!enc->block || !enc->work || !enc->residual || !enc->md5_row
      || !enc->points) {
    failure = GAUD_ERR_OOM;
    goto Fail;
  }
  enc->shifted = enc->residual + ENC_BLOCK_SIZE;
  /* The zig-zagged copy aliases the third block. It is `uint64_t` where
   * the others are `int64_t`, and the two have the same size and
   * alignment, so this is a reinterpretation of one buffer rather than
   * two objects overlapping - nothing reads it as both. */
  enc->zigzagged = (uint64_t *)(enc->residual + 2u * ENC_BLOCK_SIZE);
  enc->point_stride = 1u;
  enc->md5_live = gsec_md5_init(&enc->md5) == GSEC_OK;

  failure = enc->ogg ? open_ogg(enc) : open_native(enc);
  if (failure != GAUD_OK) {
    goto Fail;
  }

  failure = gaud_encoder_create_internal(
      stream, params, allocator, &flac_encoder_vtable, enc, out_encoder);
  if (failure != GAUD_OK) {
    goto Fail;
  }
  return GAUD_OK;

Fail:
  gaud_flac_bitw_free(&enc->bw);
  gcu_allocator_free(allocator, enc->block);
  gcu_allocator_free(allocator, enc->work);
  gcu_allocator_free(allocator, enc->residual);
  gcu_allocator_free(allocator, enc->md5_row);
  gcu_allocator_free(allocator, enc->points);
  gcu_allocator_free(allocator, enc);
  return failure;
}

/** @brief ::GAUD_Codec::encoder_open: write a native FLAC stream. */
GAUD_Result gaud_flac_encoder_open(const GAUD_Codec * codec,
    GAUD_Stream * stream, const GAUD_Encode_Params * params,
    GAUD_Encoder ** out_encoder) {
  (void)codec;
  return encoder_open(false, stream, params, out_encoder);
}

/** @brief ::GAUD_Codec::encoder_open: write the same frames into Ogg. */
GAUD_Result gaud_flac_ogg_encoder_open(const GAUD_Codec * codec,
    GAUD_Stream * stream, const GAUD_Encode_Params * params,
    GAUD_Encoder ** out_encoder) {
  (void)codec;
  return encoder_open(true, stream, params, out_encoder);
}
