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
 * Decoding one FLAC frame, as RFC 9639 section 9 specifies it.
 *
 * A frame is a header, one subframe per channel, byte padding and a
 * CRC-16. Each subframe independently chooses how it is coded - a constant
 * run, raw samples, a fixed-order predictor or an LPC one - and all but the
 * first two end in Rice-coded residuals.
 *
 * Three things here are wider than they look, and each is a defect class
 * rather than a nicety:
 *
 * **Everything accumulates in `int64_t`.** A 32-bit FLAC stream's samples
 * already fill an `int32_t`, and the fourth-order fixed predictor computes
 * `4*a - 6*b + 4*c - d` over them. Doing that in 32 bits is signed
 * overflow - undefined behaviour, not merely a wrong answer - for input a
 * fuzzer reaches in seconds and for real 32-bit recordings. The same
 * applies to an LPC sum of up to 32 coefficients of up to 15 bits each.
 *
 * **A side channel carries one bit more than the frame's stated depth.**
 * Left-side, right-side and mid-side each code a difference, whose range
 * is twice the samples'. A decoder that reads the side subframe at the
 * stated depth loses the sign of every large difference, which sounds like
 * intermittent crackle and passes any test that compares RMS.
 *
 * **Wasted bits are per subframe, not per frame.** A channel whose samples
 * are all even carries one fewer bit and shifts back up on the way out, and
 * it is entirely normal for one channel of a stereo pair to have them and
 * the other not.
 */

#include "flac_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/*
 * Which arms of this file a corpus actually reaches, counted.
 *
 * **Compiled out of every ordinary build, and committed anyway.** A
 * decoder like this is a tree of choices the *encoder* made - which
 * subframe type, which predictor order, which stereo decorrelation - so
 * "the corpus decodes correctly" says nothing about how much of the
 * decoder ran. Measuring it the first time found eight arms that no
 * fixture reached, three of which no encoder in the oracle image will
 * ever produce an input for.
 *
 * A figure like that is worthless where it cannot be re-derived, so the
 * instrument lives here rather than in a scratch patch:
 *
 *     make flac-coverage
 *
 * See notes/audio/flac-coverage.md in the workspace for what the numbers
 * were and what each one cost to reach.
 */
#ifdef GAUD_FLAC_TRACE
#include <stdio.h>
#include <stdlib.h>
enum {
  T_CONST, T_VERB, T_FIXED0, T_FIXED1, T_FIXED2, T_FIXED3, T_FIXED4, T_LPC,
  T_RICE1, T_RICE2, T_ESCAPE, T_ESCAPE0, T_WASTED, T_INDEP, T_LS, T_SR,
  T_MS, T_VARBLK, T_RATE_EXTRA, T_BS_EXTRA, T_DEPTH_INFO, T_RATE_INFO,
  T_LPC_HIGH, T_PART_DEEP, T_COUNT
};
static unsigned long gaud_flac_trace[T_COUNT];
static const char * const gaud_flac_trace_name[T_COUNT] = {
    "CONSTANT subframe", "VERBATIM subframe", "FIXED order 0",
    "FIXED order 1", "FIXED order 2", "FIXED order 3", "FIXED order 4",
    "LPC subframe", "Rice, 4-bit parameters", "Rice, 5-bit parameters",
    "escaped partition", "escaped partition of width 0", "wasted bits",
    "independent channels", "left/side", "side/right", "mid/side",
    "variable blocksize", "sample rate after the coded number",
    "block size after the coded number",
    "bit depth deferred to STREAMINFO",
    "sample rate deferred to STREAMINFO", "LPC order above 12",
    "partition order above 4"};
/** @brief Count one visit to arm @p which. */
#define TRACE(which) (gaud_flac_trace[which]++)
__attribute__((destructor)) static void gaud_flac_trace_dump(void) {
  for (int i = 0; i < T_COUNT; ++i) {
    fprintf(stderr, "FLACTRACE\t%s\t%lu\n", gaud_flac_trace_name[i],
        gaud_flac_trace[i]);
  }
}
#else
/** @brief Nothing, in an ordinary build. */
#define TRACE(which) ((void)0)
#endif

/** The 14-bit value a frame begins with. */
#define FRAME_SYNC 0x3FFEu

/** What the four block-size bits mean, where the answer is a constant. */
static uint32_t block_size_from_code(uint32_t code) {
  switch (code) {
  case 0x1u:
    return 192u;
  case 0x2u:
  case 0x3u:
  case 0x4u:
  case 0x5u:
    return 576u << (code - 2u);
  case 0x8u:
  case 0x9u:
  case 0xAu:
  case 0xBu:
  case 0xCu:
  case 0xDu:
  case 0xEu:
  case 0xFu:
    return 256u << (code - 8u);
  default:
    /* 0 is reserved; 6 and 7 are read from the end of the header. */
    return 0;
  }
}

/** What the four sample-rate bits mean, where the answer is a constant. */
static uint32_t sample_rate_from_code(uint32_t code) {
  static const uint32_t rates[12] = {
      0, 88200u, 176400u, 192000u, 8000u, 16000u, 22050u, 24000u, 32000u,
      44100u, 48000u, 96000u};
  return code < 12u ? rates[code] : 0;
}

/** What the three bit-depth bits mean; zero means "STREAMINFO says". */
static uint32_t depth_from_code(uint32_t code) {
  static const uint32_t depths[8] = {0, 8u, 12u, 0, 16u, 20u, 24u, 32u};
  /* Index 3 is reserved and shares its zero with index 0, so the caller
   * distinguishes them by the code rather than by the answer. */
  return depths[code & 7u];
}

/**
 * The header, parsed.
 *
 * `header_bytes` is what the CRC-8 covers, which is everything from the
 * sync code up to but not including the CRC itself. It is recorded rather
 * than recomputed because the header's length is variable - the block size
 * and sample rate can each append one or two bytes - and a second pass that
 * re-derived it would be the same arithmetic written twice.
 */
typedef struct {
  uint32_t block_size;        ///< Sample frames this frame holds.
  uint32_t sample_rate;       ///< Hertz, resolved against STREAMINFO.
  uint32_t channels;          ///< 1 to 8, from the assignment.
  uint32_t bits_per_sample;   ///< Resolved against STREAMINFO.
  uint32_t channel_assignment; ///< 0-7 independent, 8-10 decorrelated.
  bool variable_block_size;   ///< Whether @p number is a sample number.
  uint64_t number;      ///< Frame number, or first sample number.
  size_t header_bytes;  ///< What the CRC-8 covers; see above.
} Frame_Header;

/**
 * Read and check a frame header.
 *
 * @return ::GAUD_OK, ::GAUD_ERR_CORRUPT for anything the format forbids,
 *   or ::GAUD_ERR_IO when the buffer ran out mid-header.
 */
static GAUD_Result read_header(FLAC_Bits * br, const FLAC_Streaminfo * info,
    Frame_Header * out) {
  if (gaud_flac_bits_read(br, 14u) != FRAME_SYNC) {
    return GAUD_ERR_CORRUPT;
  }
  /* The bit after the sync is reserved and must be zero. It is the one that
   * makes the sync code fifteen bits in practice, and checking it is most
   * of what makes a false sync rare. */
  if (gaud_flac_bits_read(br, 1u) != 0) {
    return GAUD_ERR_CORRUPT;
  }
  out->variable_block_size = gaud_flac_bits_read(br, 1u) != 0;

  uint32_t block_code = gaud_flac_bits_read(br, 4u);
  uint32_t rate_code = gaud_flac_bits_read(br, 4u);
  out->channel_assignment = gaud_flac_bits_read(br, 4u);
  uint32_t depth_code = gaud_flac_bits_read(br, 3u);
  /* A second reserved bit, also zero. */
  if (gaud_flac_bits_read(br, 1u) != 0) {
    return GAUD_ERR_CORRUPT;
  }
  if (br->overrun) {
    return GAUD_ERR_IO;
  }

  if (rate_code == 0xFu) {
    /* Forbidden precisely so that the byte after a header cannot complete a
     * second sync code. */
    return GAUD_ERR_CORRUPT;
  }
  if (block_code == 0 || depth_code == 3u || out->channel_assignment > 0xAu) {
    return GAUD_ERR_CORRUPT;
  }

  if (!gaud_flac_bits_read_coded_number(br, &out->number)) {
    return br->overrun ? GAUD_ERR_IO : GAUD_ERR_CORRUPT;
  }

  out->block_size = block_size_from_code(block_code);
  if (block_code == 6u) {
    out->block_size = gaud_flac_bits_read(br, 8u) + 1u;
  }
  else if (block_code == 7u) {
    out->block_size = gaud_flac_bits_read(br, 16u) + 1u;
  }

  out->sample_rate = sample_rate_from_code(rate_code);
  if (rate_code == 0) {
    out->sample_rate = info->sample_rate;
  }
  else if (rate_code == 0xCu) {
    out->sample_rate = gaud_flac_bits_read(br, 8u) * 1000u;
  }
  else if (rate_code == 0xDu) {
    out->sample_rate = gaud_flac_bits_read(br, 16u);
  }
  else if (rate_code == 0xEu) {
    out->sample_rate = gaud_flac_bits_read(br, 16u) * 10u;
  }

  out->bits_per_sample = depth_from_code(depth_code);
  if (depth_code == 0) {
    out->bits_per_sample = info->bits_per_sample;
  }

  TRACE(out->channel_assignment < 8u
          ? T_INDEP
          : (out->channel_assignment == 8u
                  ? T_LS
                  : (out->channel_assignment == 9u ? T_SR : T_MS)));
  if (out->variable_block_size) {
    TRACE(T_VARBLK);
  }
  if (rate_code >= 0xCu) {
    TRACE(T_RATE_EXTRA);
  }
  if (block_code == 6u || block_code == 7u) {
    TRACE(T_BS_EXTRA);
  }
  if (depth_code == 0) {
    TRACE(T_DEPTH_INFO);
  }
  if (rate_code == 0) {
    TRACE(T_RATE_INFO);
  }
  out->channels = out->channel_assignment < 8u
      ? out->channel_assignment + 1u
      : 2u;

  if (br->overrun) {
    return GAUD_ERR_IO;
  }
  if (out->block_size == 0 || out->bits_per_sample == 0
      || out->bits_per_sample > 32u) {
    return GAUD_ERR_CORRUPT;
  }

  /* The header is byte-aligned here by construction: every field above is a
   * whole number of bytes once the fixed 32 bits and the coded number are
   * counted. If it is not, the coded number was malformed in a way its own
   * check let through, and continuing would checksum the wrong span. */
  if (br->bits % 8u != 0) {
    return GAUD_ERR_CORRUPT;
  }
  out->header_bytes = (size_t)(gaud_flac_bits_consumed(br) / 8u);

  uint8_t stated = (uint8_t)gaud_flac_bits_read(br, 8u);
  if (br->overrun) {
    return GAUD_ERR_IO;
  }
  if (gaud_flac_crc8(br->data, out->header_bytes) != stated) {
    return GAUD_ERR_CORRUPT;
  }
  return GAUD_OK;
}

/**
 * Read one partitioned-Rice residual block into @p residual.
 *
 * @param order The predictor order, whose warm-up samples the first
 *   partition is short by.
 */
static GAUD_Result read_residual(FLAC_Bits * br, uint32_t block_size,
    uint32_t order, int64_t * residual) {
  uint32_t method = gaud_flac_bits_read(br, 2u);
  if (method > 1u) {
    /* 2 and 3 are reserved for a later revision of the format, so this is
     * a file we do not understand rather than a file that is wrong. */
    return GAUD_ERR_UNSUPPORTED;
  }
  TRACE(method == 0 ? T_RICE1 : T_RICE2);
  unsigned parameter_bits = method == 0 ? 4u : 5u;
  uint32_t escape = method == 0 ? 0xFu : 0x1Fu;

  uint32_t partition_order = gaud_flac_bits_read(br, 4u);
  if (br->overrun) {
    return GAUD_ERR_IO;
  }
  if (partition_order > 4u) {
    TRACE(T_PART_DEEP);
  }
  uint32_t partitions = 1u << partition_order;
  /* Every partition but the first holds block_size >> partition_order
   * samples, so a block size that is not a multiple of that is a frame
   * whose partitions cannot tile it. */
  if ((block_size >> partition_order) << partition_order != block_size) {
    return GAUD_ERR_CORRUPT;
  }
  uint32_t per_partition = block_size >> partition_order;
  /* The first partition is short by the predictor's warm-up samples, so a
   * partition smaller than the order would need a negative count. Equal is
   * allowed and means an empty first partition, which libFLAC does not
   * write but the format does not forbid. */
  if (per_partition < order) {
    return GAUD_ERR_CORRUPT;
  }

  size_t at = 0;
  for (uint32_t p = 0; p < partitions; ++p) {
    uint32_t count = p == 0 ? per_partition - order : per_partition;
    uint32_t parameter = gaud_flac_bits_read(br, parameter_bits);
    if (parameter == escape) {
      /* An escaped partition abandons Rice coding and states a raw width,
       * which may be zero - and a zero width means every residual in the
       * partition is zero, not that the partition is malformed. */
      TRACE(T_ESCAPE);
      uint32_t raw_bits = gaud_flac_bits_read(br, 5u);
      if (raw_bits == 0) {
        TRACE(T_ESCAPE0);
      }
      if (raw_bits > 32u) {
        return GAUD_ERR_CORRUPT;
      }
      for (uint32_t i = 0; i < count; ++i) {
        residual[at++] = gaud_flac_bits_read_signed(br, raw_bits);
      }
    }
    else {
      for (uint32_t i = 0; i < count; ++i) {
        uint32_t quotient = gaud_flac_bits_read_unary(br);
        /* The reader caps a unary run at 2^28, and the parameter is at
         * most 30, so this product is under 2^58 and the residual it
         * becomes is under 2^57. That is what lets the predictor sum -
         * itself under 2^51 - be added to it in an int64 without
         * overflowing, which is the only reason this arithmetic is
         * defined rather than merely usually right. */
        uint64_t value = ((uint64_t)quotient << parameter)
            | gaud_flac_bits_read64(br, parameter);
        /* Zig-zag: the low bit is the sign, so odd values are negative and
         * the magnitude is the rest. Folding it this way rather than with a
         * branch keeps the sign of the most common value - zero - free. */
        residual[at++] = (int64_t)(value >> 1) ^ -(int64_t)(value & 1u);
      }
      if (br->overrun) {
        return GAUD_ERR_IO;
      }
    }
    if (br->overrun) {
      return GAUD_ERR_IO;
    }
  }
  return GAUD_OK;
}

/** The fixed predictors' coefficients, orders 0 through 4. */
static const int fixed_coefficients[5][4] = {
    {0, 0, 0, 0},
    {1, 0, 0, 0},
    {2, -1, 0, 0},
    {3, -3, 1, 0},
    {4, -6, 4, -1},
};

/** Shift every sample back up by the bits the subframe declined to code. */
static void apply_wasted(int64_t * out, uint32_t block_size, uint32_t wasted) {
  if (wasted == 0) {
    return;
  }
  for (uint32_t i = 0; i < block_size; ++i) {
    /* Through unsigned: `x << n` on a negative value is undefined, and
     * half the samples in a signal are negative. */
    out[i] = (int64_t)((uint64_t)out[i] << wasted);
  }
}

/**
 * Decode one subframe into @p out, which holds @p block_size samples.
 *
 * @param depth The subframe's own bit depth: the frame's, plus one when
 *   this is the side channel of a decorrelated pair.
 * @param residual Scratch for @p block_size values, supplied by the caller
 *   so that one allocation serves every subframe of every frame.
 */
static GAUD_Result read_subframe(FLAC_Bits * br, uint32_t block_size,
    uint32_t depth, int64_t * residual, int64_t * out) {
  if (gaud_flac_bits_read(br, 1u) != 0) {
    /* The subframe's leading bit is reserved and zero. */
    return GAUD_ERR_CORRUPT;
  }
  uint32_t type = gaud_flac_bits_read(br, 6u);
  uint32_t wasted = 0;
  if (gaud_flac_bits_read(br, 1u)) {
    /* The count is unary and one less than the answer, so a flagged
     * subframe always has at least one wasted bit. */
    wasted = gaud_flac_bits_read_unary(br) + 1u;
  }
  if (br->overrun) {
    return GAUD_ERR_IO;
  }
  if (wasted >= depth) {
    /* Every bit wasted leaves nothing to code. RFC 9639 allows wasted
     * bits equal to the depth only for a constant zero subframe, and
     * libFLAC does not write it; refusing is safe and keeps the shift
     * below in range. */
    return GAUD_ERR_CORRUPT;
  }
  if (wasted) {
    TRACE(T_WASTED);
  }
  uint32_t effective = depth - wasted;

  uint32_t order = 0;
  bool is_lpc = false;
  if (type == 0) {
    TRACE(T_CONST);
    /* CONSTANT. */
    int64_t value = gaud_flac_bits_read_signed64(br, effective);
    for (uint32_t i = 0; i < block_size; ++i) {
      out[i] = value;
    }
    if (br->overrun) {
      return GAUD_ERR_IO;
    }
    apply_wasted(out, block_size, wasted);
    return GAUD_OK;
  }
  if (type == 1u) {
    TRACE(T_VERB);
    /* VERBATIM. */
    for (uint32_t i = 0; i < block_size; ++i) {
      out[i] = gaud_flac_bits_read_signed64(br, effective);
    }
    if (br->overrun) {
      return GAUD_ERR_IO;
    }
    apply_wasted(out, block_size, wasted);
    return GAUD_OK;
  }
  if (type >= 8u && type <= 12u) {
    order = type - 8u;
    TRACE(T_FIXED0 + order);
  }
  else if (type >= 32u) {
    order = type - 31u;
    is_lpc = true;
    TRACE(T_LPC);
    if (order > 12u) {
      TRACE(T_LPC_HIGH);
    }
  }
  else {
    /* Reserved: 2 to 7, 13 to 31. A later revision may define them. */
    return GAUD_ERR_UNSUPPORTED;
  }
  if (order > block_size) {
    return GAUD_ERR_CORRUPT;
  }

  for (uint32_t i = 0; i < order; ++i) {
    out[i] = gaud_flac_bits_read_signed64(br, effective);
  }
  if (br->overrun) {
    return GAUD_ERR_IO;
  }

  int32_t coefficients[FLAC_MAX_LPC_ORDER];
  uint32_t shift = 0;
  if (is_lpc) {
    uint32_t precision = gaud_flac_bits_read(br, 4u) + 1u;
    if (precision == 16u) {
      /* 0b1111 + 1. The all-ones code is forbidden, which is what keeps a
       * precision of zero out of the shift below. */
      return GAUD_ERR_CORRUPT;
    }
    int32_t signed_shift = gaud_flac_bits_read_signed(br, 5u);
    if (signed_shift < 0) {
      /* The field is signed and a negative shift has never been legal;
       * RFC 9639 states it, and libFLAC has never written one. */
      return GAUD_ERR_CORRUPT;
    }
    shift = (uint32_t)signed_shift;
    for (uint32_t i = 0; i < order; ++i) {
      coefficients[i] = gaud_flac_bits_read_signed(br, precision);
    }
    if (br->overrun) {
      return GAUD_ERR_IO;
    }
  }

  GAUD_Result result = read_residual(br, block_size, order, residual);
  if (result != GAUD_OK) {
    return result;
  }

  /* `sum >> shift` on a negative sum is an arithmetic shift on every
   * compiler this builds with, and the format requires one: the predictor
   * is a fixed-point filter and rounding its output towards zero instead
   * of towards negative infinity changes the samples. C leaves it
   * implementation-defined rather than undefined, so this is a portability
   * note and not a latent fault. */
  if (is_lpc) {
    for (uint32_t i = order; i < block_size; ++i) {
      int64_t sum = 0;
      for (uint32_t c = 0; c < order; ++c) {
        sum += (int64_t)coefficients[c] * out[i - 1u - c];
      }
      out[i] = (sum >> shift) + residual[i - order];
    }
  }
  else {
    const int * coefficient = fixed_coefficients[order];
    for (uint32_t i = order; i < block_size; ++i) {
      int64_t sum = 0;
      for (uint32_t c = 0; c < order; ++c) {
        sum += (int64_t)coefficient[c] * out[i - 1u - c];
      }
      out[i] = sum + residual[i - order];
    }
  }

  apply_wasted(out, block_size, wasted);
  return GAUD_OK;
}

/**
 * Undo whichever stereo decorrelation the channel assignment names.
 *
 * Every arithmetic step is 64-bit, and mid-side is the case that makes it
 * necessary rather than tidy. Reconstructing the left channel computes
 * `mid + side`, which is twice a sample: for a 32-bit stream that is 33
 * bits, and doing it in 32 would drop the bit that the following shift is
 * about to bring back down.
 */
static void undo_stereo(uint32_t assignment, uint32_t block_size,
    int64_t * left, int64_t * right) {
  if (assignment == 8u) {
    /* Left and side: the second channel is left - right. */
    for (uint32_t i = 0; i < block_size; ++i) {
      right[i] = left[i] - right[i];
    }
  }
  else if (assignment == 9u) {
    /* Side and right: the first channel is left - right. */
    for (uint32_t i = 0; i < block_size; ++i) {
      left[i] = left[i] + right[i];
    }
  }
  else if (assignment == 0xAu) {
    /* Mid and side. The mid channel dropped the sum's low bit, and the
     * side channel's low bit is what puts it back - the two together are
     * the sum exactly, which is why this is lossless and an average of the
     * two would not be. */
    for (uint32_t i = 0; i < block_size; ++i) {
      int64_t side = right[i];
      int64_t mid = (int64_t)((uint64_t)left[i] << 1) | (side & 1);
      left[i] = (mid + side) >> 1;
      right[i] = (mid - side) >> 1;
    }
  }
}

void gaud_flac_frame_free(FLAC_Frame * frame) {
  if (!frame || !frame->samples) {
    return;
  }
  gcu_allocator_free(frame->allocator, frame->samples);
  frame->samples = NULL;
  frame->capacity_frames = 0;
}

GAUD_Result gaud_flac_frame_reserve(
    FLAC_Frame * frame, uint32_t channels, uint32_t block_size) {
  if (channels == 0 || channels > FLAC_MAX_CHANNELS
      || block_size > FLAC_MAX_BLOCK_SIZE) {
    return GAUD_ERR_INVALID;
  }
  if (frame->samples && frame->capacity_frames >= block_size
      && frame->capacity_channels >= channels) {
    return GAUD_OK;
  }
  uint32_t want_frames = block_size > frame->capacity_frames
      ? block_size
      : frame->capacity_frames;
  uint32_t want_channels = channels > frame->capacity_channels
      ? channels
      : frame->capacity_channels;
  size_t count = (size_t)want_frames * (size_t)want_channels;
  int64_t * grown
      = gcu_allocator_malloc(frame->allocator, count * sizeof(int64_t));
  if (!grown) {
    return GAUD_ERR_OOM;
  }
  gaud_flac_frame_free(frame);
  frame->samples = grown;
  frame->capacity_frames = want_frames;
  frame->capacity_channels = want_channels;
  return GAUD_OK;
}

GAUD_Result gaud_flac_frame_decode(const unsigned char * data, size_t size,
    const FLAC_Streaminfo * info, FLAC_Frame * frame, size_t * out_used) {
  FLAC_Bits br;
  gaud_flac_bits_init(&br, data, size);

  Frame_Header header;
  memset(&header, 0, sizeof(header));
  GAUD_Result result = read_header(&br, info, &header);
  if (result != GAUD_OK) {
    return result;
  }
  if (header.block_size > FLAC_MAX_BLOCK_SIZE
      || header.channels > FLAC_MAX_CHANNELS) {
    return GAUD_ERR_CORRUPT;
  }
  /* A frame that disagrees with STREAMINFO about the shape of the stream is
   * not a frame of this stream. Checking it here rather than trusting the
   * header is what stops a resync landing on a plausible-looking false sync
   * inside the audio and decoding thousands of samples of noise. */
  if (info->channels && header.channels != info->channels) {
    return GAUD_ERR_CORRUPT;
  }
  if (info->bits_per_sample
      && header.bits_per_sample != info->bits_per_sample) {
    return GAUD_ERR_CORRUPT;
  }

  result = gaud_flac_frame_reserve(frame, header.channels, header.block_size);
  if (result != GAUD_OK) {
    return result;
  }

  int64_t * residual = gcu_allocator_malloc(
      frame->allocator, (size_t)header.block_size * sizeof(int64_t));
  if (!residual) {
    return GAUD_ERR_OOM;
  }

  for (uint32_t ch = 0; ch < header.channels; ++ch) {
    /* The one channel of a decorrelated pair that carries a difference
     * needs an extra bit, and which one it is depends on the assignment:
     * left-side and mid-side put it second, right-side first. */
    uint32_t depth = header.bits_per_sample;
    if ((header.channel_assignment == 8u || header.channel_assignment == 0xAu)
        && ch == 1u) {
      ++depth;
    }
    else if (header.channel_assignment == 9u && ch == 0) {
      ++depth;
    }
    int64_t * channel = frame->samples + (size_t)ch * frame->capacity_frames;
    result = read_subframe(
        &br, header.block_size, depth, residual, channel);
    if (result != GAUD_OK) {
      gcu_allocator_free(frame->allocator, residual);
      return result;
    }
  }
  gcu_allocator_free(frame->allocator, residual);

  gaud_flac_bits_align(&br);
  size_t body_bytes = (size_t)(gaud_flac_bits_consumed(&br) / 8u);
  uint16_t stated = (uint16_t)gaud_flac_bits_read(&br, 16u);
  if (br.overrun) {
    return GAUD_ERR_IO;
  }
  if (gaud_flac_crc16(data, body_bytes) != stated) {
    return GAUD_ERR_CORRUPT;
  }

  if (header.channels == 2u && header.channel_assignment >= 8u) {
    undo_stereo(header.channel_assignment, header.block_size,
        frame->samples, frame->samples + frame->capacity_frames);
  }

  frame->block_size = header.block_size;
  frame->sample_rate = header.sample_rate;
  frame->channels = header.channels;
  frame->bits_per_sample = header.bits_per_sample;
  frame->variable_block_size = header.variable_block_size;
  frame->number = header.number;
  *out_used = body_bytes + 2u;
  return GAUD_OK;
}

bool gaud_flac_frame_peek(const unsigned char * data, size_t size,
    const FLAC_Streaminfo * info, uint32_t * out_block_size,
    uint64_t * out_number, bool * out_is_sample_number) {
  FLAC_Bits br;
  gaud_flac_bits_init(&br, data, size);
  Frame_Header header;
  memset(&header, 0, sizeof(header));
  if (read_header(&br, info, &header) != GAUD_OK) {
    return false;
  }
  if (info->channels && header.channels != info->channels) {
    return false;
  }
  if (info->bits_per_sample
      && header.bits_per_sample != info->bits_per_sample) {
    return false;
  }
  if (out_block_size) {
    *out_block_size = header.block_size;
  }
  if (out_number) {
    *out_number = header.number;
  }
  if (out_is_sample_number) {
    *out_is_sample_number = header.variable_block_size;
  }
  return true;
}
