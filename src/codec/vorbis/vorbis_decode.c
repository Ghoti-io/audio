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
 * One audio packet, and the lapping between packets.
 *
 * The order is the specification's and is not negotiable, because two of
 * the steps do not commute:
 *
 *   1. the mode, which chooses the block size and - for a long block -
 *      the two flags that say what the neighbouring blocks are;
 *   2. a floor per channel, which may say the channel carries nothing;
 *   3. **propagate that**: a coupling step whose two channels disagree
 *      about carrying something makes both carry something, because the
 *      inverse coupling below reads both;
 *   4. the residue, per submap, for the channels that carry something;
 *   5. **inverse coupling, on the residue and before the floor.** This
 *      is the step whose order is easy to get wrong: the coupled pair is
 *      a magnitude and an angle in the *residue* domain, and applying
 *      the floor first would couple two channels whose envelopes differ;
 *   6. the dot product of the floor and the residue, which is the
 *      spectrum;
 *   7. the inverse transform, the window, and the overlap-add.
 *
 * ## The lapping, and why a packet's output is not its own
 *
 * A packet's transform produces `n` samples and the packet is worth
 * `n/2`, because consecutive blocks overlap by half. **The first packet
 * of a stream therefore produces nothing at all**: there is no previous
 * block to add it to, and the specification says so in as many words. A
 * decoder that emitted it emits half a block of a window's rising edge,
 * which sounds like a click and measures as a length one block too long.
 *
 * The overlap is held as `tail`: the previous block's windowed samples
 * from where its right slope began. The current block's left slope lands
 * on exactly that span - the window flags guarantee the two slopes have
 * the same length, which is what the flags are *for* - so the add is a
 * straight one and the samples before the current right slope are final.
 *
 * ## The two trims
 *
 * Vorbis has no field for its length, so both ends of a stream are given
 * by granule positions, and they work in opposite directions:
 *
 *   - **the last page's position is the total**, so anything decoded
 *     past it is padding the encoder added to fill a block;
 *   - **the first page's position may be less than what the packets up
 *     to it decode to**, and the difference is trimmed from the
 *     *beginning*. That is how a stream starts part way into a block,
 *     and it is the arm that nothing in this corpus exercises - every
 *     fixture here starts at zero. It is implemented rather than
 *     refused, because the alternative is being wrong rather than
 *     saying so, and a diagnostic records when it fires.
 */

#include "../../container/ogg/ogg.h"
#include "../shared/bytes.h"
#include "vorbis_internal.h"
#include "vorbis_tables.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/** What one Vorbis decoder holds between calls. */
typedef struct {
  VORBIS_File * doc_state; ///< Borrowed from the document.
  const GAUD_Allocator * allocator; ///< Everything here comes from it.
  OGG_Reader reader;       ///< Positioned in the audio pages.
  uint32_t channels;       ///< From the identification header.
  uint32_t long_block;     ///< The larger block size.
  uint32_t short_block;    ///< The smaller one.

  int32_t * residue;  ///< channels by long_block/2, in Q#VORBIS_Q.
  unsigned char * curves; ///< channels by long_block/2, floor indices.
  int32_t * spectrum; ///< long_block/2, in Q#VORBIS_SPECTRUM_Q.
  int32_t * scratch;  ///< long_block, for the transform.
  int32_t * block;    ///< long_block, the transform's output.
  int32_t * tail;     ///< channels by long_block/2, the lap.
  uint32_t tail_length;  ///< How much of it is live.
  bool have_previous;    ///< Whether a block has been decoded at all.
  /**
   * The next block is the first after a seek landed mid-stream: it has no
   * block before it to lap with, so its output cannot be handed out, but
   * its length still counts and its right half is the lap for the next.
   */
  bool landing;
  /**
   * What to add to a page's granule position, plus the shape of the last
   * block on it, to get the frame number this decoder gives the same
   * place. They differ because the decoder hands frames out up to the
   * start of the *next* overlap and the granule position counts to the
   * block's centre. See ::land_near. Worked out on the first seek.
   */
  int64_t bias;
  bool bias_known; ///< Whether @p bias has been worked out.
  /** The last decoded block's n/4 - right_n/4: how far past its centre it went. */
  uint32_t last_slack;
  uint32_t previous_right_n; ///< The previous block's right slope length.

  /**
   * Decoded frames not yet handed to the caller.
   *
   * **Sized at a whole block per channel and not half of one**, which is
   * the subtlety: a packet is worth half its block only when its
   * neighbours are the same size. A long block whose *next* neighbour is
   * short has its right slope early - at `(3n - n0)/4` rather than
   * `n/2` - so it finalises 1,472 samples of a 2,048-sample block rather
   * than 1,024. Half a block is the common case and the wrong bound, and
   * the fixture that overran it is the only one in the corpus with a
   * transient in it.
   */
  int32_t * pending;
  uint32_t pending_stride;  ///< Frames per channel in @p pending.
  uint32_t pending_frames; ///< How many frames it holds.
  /**
   * The absolute frame number of @p pending's first frame.
   *
   * **Tracked rather than derived from the position**, because the two
   * come apart: `position` is how many frames the caller has been given
   * and is clamped by the tail trim, while this is where the decoded
   * block sits in the stream. A seek that computed the one from the
   * other landed on the frame it had most recently handed out rather
   * than the frame it was asked for, which looks like a seek that
   * works for forward targets and ignores backward ones.
   */
  uint64_t pending_base;
  uint32_t pending_used;   ///< How many have been handed out.

  /** Which decoded channel goes in each output slot; see the table. */
  unsigned char order[8];
  uint64_t position;    ///< The next frame the caller will receive.
  uint64_t decoded;     ///< Frames produced before any trim.
  bool first_page_seen; ///< Whether the head trim has been considered.
  bool ended;           ///< Whether the packet stream is exhausted.
} VORBIS_Decoder;

/**
 * The 16-bit sample a Q#VORBIS_TIME_Q value rounds to.
 *
 * Exactly mp3_decode.c's shape: a rounding term of half an output step,
 * then a shift of `Q - 15`, then a clip. A first draft had an extra
 * halving after the shift and a compensating bit added to the
 * transform's scale, which cancelled in the output and did not cancel
 * in between - the block values were carried twice as large as they
 * needed to be, and the loudest fixture in the corpus saturated.
 */
static int16_t to_s16(int32_t value) {
  int64_t scaled = ((int64_t)value + (1 << (VORBIS_TIME_Q - 16)))
      >> (VORBIS_TIME_Q - 15);
  if (scaled > 32767) {
    return 32767;
  }
  if (scaled < -32768) {
    return -32768;
  }
  return (int16_t)scaled;
}

/**
 * Vorbis's channel order mapped onto this library's, per channel count.
 *
 * **`buffer.h` says the order is WAV's `dwChannelMask`** and that every
 * other container's is mapped onto it on the way in. Vorbis's own order
 * is not that one: from three channels up it puts the centre channel
 * second and the low-frequency channel last, where WAV puts the centre
 * third and the low-frequency fourth.
 *
 * Entry `[channels][slot]` is which Vorbis channel belongs in that
 * output slot. One and two channels are the identity, which is why this
 * is invisible until a file has three - and why it was found by the one
 * six-channel fixture in the corpus disagreeing with ffmpeg while
 * agreeing with libsndfile. ffmpeg reorders to WAV and libsndfile hands
 * back the stream's own order; this library promises the former.
 *
 * Above eight the specification declines to define an order, so the
 * identity is the honest answer and gaud_channel_layout_default() says
 * as much by reporting no mask at all.
 */
static const unsigned char vorbis_channel_order[9][8] = {
    [1] = {0},
    [2] = {0, 1},
    /* L C R -> FL FR FC */
    [3] = {0, 2, 1},
    /* FL FR BL BR, which is already WAV's order. */
    [4] = {0, 1, 2, 3},
    /* FL C FR BL BR -> FL FR FC BL BR */
    [5] = {0, 2, 1, 3, 4},
    /* FL C FR BL BR LFE -> FL FR FC LFE BL BR */
    [6] = {0, 2, 1, 5, 3, 4},
    /* FL C FR SL SR BC LFE -> FL FR FC LFE BC SL SR */
    [7] = {0, 2, 1, 6, 5, 3, 4},
    /* FL C FR SL SR BL BR LFE -> FL FR FC LFE BL BR SL SR */
    [8] = {0, 2, 1, 7, 5, 6, 3, 4},
};

unsigned gaud_vorbis_channel_slot(uint32_t channels, unsigned slot) {
  if (channels == 0 || channels > 8u || slot >= channels) {
    return slot;
  }
  return vorbis_channel_order[channels][slot];
}

/** Saturating addition. */
static int32_t add_sat(int32_t a, int32_t b) {
  int32_t sum;
  if (!__builtin_add_overflow(a, b, &sum)) {
    return sum;
  }
  return b < 0 ? INT32_MIN : INT32_MAX;
}

/** @p a times a Q::VORBIS_TABLE_Q factor, rounded and saturated. */
static int32_t mul_q(int32_t a, int32_t factor) {
  int64_t product = (int64_t)a * factor;
  int64_t half = (int64_t)1 << (VORBIS_TABLE_Q - 1);
  product = product >= 0 ? product + half : product - half;
  product >>= VORBIS_TABLE_Q;
  if (product > INT32_MAX) {
    return INT32_MAX;
  }
  if (product < INT32_MIN) {
    return INT32_MIN;
  }
  return (int32_t)product;
}

/** log2 of @p n, which the caller has already checked is a power of two. */
static unsigned log2_of(uint32_t n) {
  unsigned bits = 0;
  while ((1u << bits) < n) {
    ++bits;
  }
  return bits;
}

static void decoder_close(GAUD_Decoder * decoder) {
  VORBIS_Decoder * state = gaud_decoder_private(decoder);
  if (!state) {
    return;
  }
  const GAUD_Allocator * allocator = state->allocator;
  gaud_ogg_reader_free(&state->reader);
  gcu_allocator_free(allocator, state->residue);
  gcu_allocator_free(allocator, state->curves);
  gcu_allocator_free(allocator, state->spectrum);
  gcu_allocator_free(allocator, state->scratch);
  gcu_allocator_free(allocator, state->block);
  gcu_allocator_free(allocator, state->tail);
  gcu_allocator_free(allocator, state->pending);
  gcu_allocator_free(allocator, state);
}

/**
 * Where the window's rising slope begins, and where its falling one does.
 *
 * **A quarter and not a half**, which is the whole of a defect worth
 * recording: the specification puts the left slope at `n/4 - left_n/4`
 * and the right at `3n/4 - right_n/4`, and a first draft used `n/2` and
 * `n/2` - which is right when the slope is the block's own size, and is
 * every fixture in the corpus whose neighbours match. It overruns the
 * lap buffer the moment a long block sits beside a short one, and that
 * is how it was found rather than by listening.
 *
 * The slope itself is half as long as the size it is named for: a
 * `left_n` slope spans `left_n/2` samples. So the two halves of a
 * window of size n span `n/2` each and meet in the middle, which is the
 * ordinary case these two functions reduce to.
 */
static uint32_t window_left_start(uint32_t n, uint32_t left_n) {
  return (n - left_n) / 4u;
}

/** Where the window's falling slope begins. See window_left_start(). */
static uint32_t window_right_start(uint32_t n, uint32_t right_n) {
  return (3u * n - right_n) / 4u;
}

/**
 * Apply the window to @p block, in place.
 *
 * The shape is four regions: zero, a rising slope, one, a falling slope,
 * zero. Which is to say the window of the block's own size only when both
 * neighbours are the same size - and **the slopes are the length of the
 * smaller neighbour**, which is what the two flags in a long block's
 * packet state. Without that a long block beside a short one would
 * overlap it over a span the short block does not cover.
 */
static void apply_window(int32_t * block, uint32_t n, uint32_t left_n,
    uint32_t right_n) {
  uint32_t left_start = window_left_start(n, left_n);
  uint32_t right_start = window_right_start(n, right_n);
  const int32_t * left
      = gaud_vorbis_windows[log2_of(left_n) - VORBIS_LOG2_MIN_BLOCK];
  const int32_t * right
      = gaud_vorbis_windows[log2_of(right_n) - VORBIS_LOG2_MIN_BLOCK];

  for (uint32_t i = 0; i < left_start; ++i) {
    block[i] = 0;
  }
  for (uint32_t i = 0; i < left_n / 2u; ++i) {
    block[left_start + i] = mul_q(block[left_start + i], left[i]);
  }
  /* The flat middle is already itself. */
  for (uint32_t i = 0; i < right_n / 2u; ++i) {
    /* The window's falling half is its rising half backwards, which is
     * why the table holds only one of them. */
    block[right_start + i]
        = mul_q(block[right_start + i], right[right_n / 2u - 1u - i]);
  }
  for (uint32_t i = right_start + right_n / 2u; i < n; ++i) {
    block[i] = 0;
  }
}

/**
 * A packet's block size and its two window slopes, without decoding it.
 *
 * The same reading decode_packet() starts with: the mode number, and for a
 * long block the two flags that say whether its neighbours are long.
 *
 * @return ::GAUD_OK; ::GAUD_ERR_FORMAT for a header packet, which has no
 *   shape; ::GAUD_ERR_CORRUPT for a mode number the setup does not have.
 */
static GAUD_Result packet_shape(const VORBIS_Decoder * state,
    const unsigned char * data, size_t size, uint32_t * n, uint32_t * left_n,
    uint32_t * right_n) {
  const VORBIS_Setup * setup = &state->doc_state->setup;
  VORBIS_Bits bits;
  gaud_vorbis_bits_init(&bits, data, size);
  if (gaud_vorbis_bits_read(&bits, 1) != 0) {
    return GAUD_ERR_FORMAT;
  }
  uint32_t mode_number = gaud_vorbis_bits_read(&bits, setup->mode_bits);
  if (mode_number >= setup->mode_count || bits.past_end) {
    return GAUD_ERR_CORRUPT;
  }
  const VORBIS_Mode * mode = &setup->modes[mode_number];
  *n = mode->block_flag ? state->long_block : state->short_block;
  *left_n = *n;
  *right_n = *n;
  if (mode->block_flag) {
    if (gaud_vorbis_bits_read(&bits, 1) == 0) {
      *left_n = state->short_block;
    }
    if (gaud_vorbis_bits_read(&bits, 1) == 0) {
      *right_n = state->short_block;
    }
  }
  return GAUD_OK;
}

/** Decode one audio packet; produces frames into @p state->pending. */
static GAUD_Result decode_packet(VORBIS_Decoder * state,
    const unsigned char * data, size_t size) {
  const VORBIS_Setup * setup = &state->doc_state->setup;
  VORBIS_Bits bits;
  gaud_vorbis_bits_init(&bits, data, size);

  if (gaud_vorbis_bits_read(&bits, 1) != 0) {
    /* A header packet where audio was due. The specification says a
     * decoder must ignore it rather than fail, because a chained
     * stream's next link begins this way. */
    state->pending_frames = 0;
    state->pending_used = 0;
    return GAUD_OK;
  }
  uint32_t mode_number = gaud_vorbis_bits_read(&bits, setup->mode_bits);
  if (mode_number >= setup->mode_count || bits.past_end) {
    return GAUD_ERR_CORRUPT;
  }
  const VORBIS_Mode * mode = &setup->modes[mode_number];
  uint32_t n = mode->block_flag ? state->long_block : state->short_block;
  uint32_t lines = n / 2u;
  uint32_t left_n = n;
  uint32_t right_n = n;
  if (mode->block_flag) {
    if (gaud_vorbis_bits_read(&bits, 1) == 0) {
      left_n = state->short_block;
    }
    if (gaud_vorbis_bits_read(&bits, 1) == 0) {
      right_n = state->short_block;
    }
  }
  const VORBIS_Mapping * mapping = &setup->mappings[mode->mapping];

  /* The floors, and whether each channel carries anything. */
  bool used[256];
  // Whether the channel stated a floor at all. A channel can be *used*
  // without one, when coupling pairs it with a channel that has one, and
  // then it takes part in the residue and in the coupling but its
  // spectrum is zero, because there is no envelope to scale it by. Its
  // curve was never written.
  bool has_floor[256];
  if (state->channels > 256u) {
    return GAUD_ERR_UNSUPPORTED;
  }
  for (uint32_t ch = 0; ch < state->channels; ++ch) {
    const VORBIS_Floor * floor
        = &setup->floors[mapping->floor[mapping->mux[ch]]];
    bool one = false;
    GAUD_Result result = gaud_vorbis_floor_decode(floor, setup, &bits,
        lines, state->curves + (size_t)ch * (state->long_block / 2u), &one);
    if (result != GAUD_OK) {
      return result;
    }
    used[ch] = one;
    has_floor[ch] = one;
  }

  /*
   * Propagate. A coupling step reads both of its channels, so if either
   * carries something both must be decoded - and a decoder that skipped
   * the silent one would read the pair's residue out of step and get
   * every channel after it wrong too.
   */
  for (uint32_t i = 0; i < mapping->coupling_steps; ++i) {
    if (used[mapping->magnitude[i]] || used[mapping->angle[i]]) {
      used[mapping->magnitude[i]] = true;
      used[mapping->angle[i]] = true;
    }
  }

  memset(state->residue, 0,
      (size_t)state->channels * (state->long_block / 2u)
          * sizeof(*state->residue));

  /* The residue, one call per submap over the channels in it. */
  for (uint32_t submap = 0; submap < mapping->submaps; ++submap) {
    uint32_t count = 0;
    bool wanted[256];
    uint32_t which[256];
    for (uint32_t ch = 0; ch < state->channels; ++ch) {
      if (mapping->mux[ch] != submap) {
        continue;
      }
      wanted[count] = used[ch];
      which[count] = ch;
      ++count;
    }
    if (count == 0) {
      continue;
    }
    /*
     * The submap's channels are gathered into a contiguous block,
     * because a residue of type 2 reads them as one interleaved vector
     * and the interleaving is over the submap's channels in order - not
     * over the stream's. A mapping with one submap makes the two the
     * same, which is every fixture here but one.
     */
    int32_t * gathered = state->residue;
    bool contiguous = true;
    for (uint32_t i = 0; i < count; ++i) {
      if (which[i] != i) {
        contiguous = false;
      }
    }
    GAUD_Result result;
    if (contiguous) {
      result = gaud_vorbis_residue_decode(
          &setup->residues[mapping->residue[submap]], setup, &bits, count,
          state->long_block / 2u, wanted, gathered);
    }
    else {
      /* Decode into the scratch and scatter. The scratch is sized for
       * one block of the longest size, which is channels times lines
       * only when every channel is in this submap - and then the branch
       * above took it. */
      size_t span = (size_t)count * (state->long_block / 2u);
      int32_t * temporary
          = gcu_allocator_malloc(state->allocator, span * sizeof(int32_t));
      if (!temporary) {
        return GAUD_ERR_OOM;
      }
      memset(temporary, 0, span * sizeof(int32_t));
      result = gaud_vorbis_residue_decode(
          &setup->residues[mapping->residue[submap]], setup, &bits, count,
          state->long_block / 2u, wanted, temporary);
      if (result == GAUD_OK) {
        for (uint32_t i = 0; i < count; ++i) {
          memcpy(state->residue
                  + (size_t)which[i] * (state->long_block / 2u),
              temporary + (size_t)i * (state->long_block / 2u),
              (size_t)(state->long_block / 2u) * sizeof(int32_t));
        }
      }
      gcu_allocator_free(state->allocator, temporary);
    }
    if (result != GAUD_OK) {
      return result;
    }
  }

  /*
   * Inverse coupling, in reverse order and before the floor.
   *
   * The pair is a magnitude and an angle; the inverse recovers two
   * channels from them, and which of the four cases applies depends on
   * the signs of both. In *reverse* step order, because the steps were
   * applied forwards at the encoder and each one's input is the previous
   * one's output.
   */
  for (uint32_t step = mapping->coupling_steps; step-- > 0;) {
    int32_t * magnitude = state->residue
        + (size_t)mapping->magnitude[step] * (state->long_block / 2u);
    int32_t * angle = state->residue
        + (size_t)mapping->angle[step] * (state->long_block / 2u);
    for (uint32_t j = 0; j < lines; ++j) {
      int32_t m = magnitude[j];
      int32_t a = angle[j];
      int32_t new_m;
      int32_t new_a;
      if (m > 0) {
        if (a > 0) {
          new_m = m;
          new_a = m - a;
        }
        else {
          new_a = m;
          new_m = m + a;
        }
      }
      else {
        if (a > 0) {
          new_m = m;
          new_a = m + a;
        }
        else {
          new_a = m;
          new_m = m - a;
        }
      }
      magnitude[j] = new_m;
      angle[j] = new_a;
    }
  }

  /* The dot product, the transform, the window and the lap. */
  uint32_t left_start = window_left_start(n, left_n);
  uint32_t right_start = window_right_start(n, right_n);
  if (state->have_previous && state->previous_right_n != left_n) {
    /* The previous block's right slope and this block's left slope must
     * be the same length: that is what the window flags are for, and a
     * stream where they disagree cannot be lapped. */
    return GAUD_ERR_CORRUPT;
  }
  bool emit = state->have_previous;
  // The stream's first block has no block before it to lap with, and
  // hands out only what is left of it before the next overlap: from its
  // centre, which is where a granule position starts counting, to where
  // that begins. Nothing at all when the next block is its own size, and a
  // quarter of a long block less a quarter of a short one when it is not.
  bool first = !state->have_previous && !state->landing;
  uint32_t produced = emit || state->landing
      ? right_start - left_start
      : (first ? right_start - n / 2u : 0);
  if (produced > state->pending_stride) {
    return GAUD_ERR_CORRUPT;
  }
  /*
   * How far the transform's output has to move to reach
   * ::VORBIS_TIME_Q, and **it goes both ways**. The transform halves
   * its state once per two stages, so a long block loses more than a
   * short one: the move is a right shift of one bit for a 64- or
   * 128-sample block, nothing at 256 or 512, and a *left* shift of one
   * or two above that. A first draft held this in an `unsigned` and
   * every block of 1,024 or more shifted right by 31 - which decoded
   * the two fixtures whose blocks are small and nothing else, and is
   * the shape of a defect a corpus of one sample rate would have
   * missed.
   */
  unsigned shift = gaud_vorbis_imdct_shift(n);
  int move = (int)VORBIS_SPECTRUM_Q - (int)shift - (int)VORBIS_TIME_Q
      - (int)VORBIS_TDAC_GAIN_BITS;

  for (uint32_t ch = 0; ch < state->channels; ++ch) {
    const int32_t * residue
        = state->residue + (size_t)ch * (state->long_block / 2u);
    const unsigned char * curve
        = state->curves + (size_t)ch * (state->long_block / 2u);
    if (!has_floor[ch]) {
      memset(state->block, 0, (size_t)n * sizeof(*state->block));
    }
    else {
      for (uint32_t j = 0; j < lines; ++j) {
        int32_t mantissa = gaud_vorbis_floor_db[curve[j]][0];
        int32_t floor_shift = gaud_vorbis_floor_db[curve[j]][1];
        /* residue is Q#VORBIS_Q and the floor is mantissa >> shift, so
         * the product reaches Q#VORBIS_SPECTRUM_Q with a shift of
         * `floor_shift + VORBIS_Q - VORBIS_SPECTRUM_Q`. */
        int64_t product = (int64_t)residue[j] * mantissa;
        int right = floor_shift + (int)VORBIS_Q - (int)VORBIS_SPECTRUM_Q;
        if (right > 0) {
          int64_t half = (int64_t)1 << (right - 1);
          product = product >= 0 ? product + half : product - half;
          product >>= right;
        }
        else if (right < 0) {
          product <<= -right;
        }
        if (product > INT32_MAX) {
          product = INT32_MAX;
        }
        else if (product < INT32_MIN) {
          product = INT32_MIN;
        }
        state->spectrum[j] = (int32_t)product;
      }
      gaud_vorbis_imdct(state->spectrum, n, state->scratch, state->block);
      /* The transform's own scale depends on the block size; everything
       * downstream is Q#VORBIS_TIME_Q. A left shift saturates, because
       * it is the one of the two that can overflow. */
      if (move > 0) {
        int32_t half = (int32_t)1 << (move - 1);
        for (uint32_t i = 0; i < n; ++i) {
          state->block[i] = (state->block[i] + half) >> move;
        }
      }
      else if (move < 0) {
        /* A multiply and not a shift: shifting a *negative* value left
         * is undefined in C, and half the samples of any real block are
         * negative. UBSan reports it as "left shift of negative value",
         * which is how this was found. */
        int64_t factor = (int64_t)1 << -move;
        for (uint32_t i = 0; i < n; ++i) {
          int64_t scaled = (int64_t)state->block[i] * factor;
          if (scaled > INT32_MAX) {
            scaled = INT32_MAX;
          }
          else if (scaled < INT32_MIN) {
            scaled = INT32_MIN;
          }
          state->block[i] = (int32_t)scaled;
        }
      }
      apply_window(state->block, n, left_n, right_n);
    }
    if (!used[ch]) {
      /* A silent channel still has to be windowed to zero, which it
       * already is, and still has to lap - its tail is zero. */
    }

    int32_t * tail = state->tail + (size_t)ch * (state->long_block / 2u);
    int32_t * out = state->pending + (size_t)ch * state->pending_stride;
    if (state->have_previous) {
      for (uint32_t j = 0; j < state->tail_length; ++j) {
        state->block[left_start + j]
            = add_sat(state->block[left_start + j], tail[j]);
      }
      for (uint32_t j = 0; j < produced; ++j) {
        out[j] = state->block[left_start + j];
      }
    }
    else if (first) {
      for (uint32_t j = 0; j < produced; ++j) {
        out[j] = state->block[n / 2u + j];
      }
    }
    for (uint32_t j = 0; j < right_n / 2u; ++j) {
      tail[j] = state->block[right_start + j];
    }
  }
  state->tail_length = right_n / 2u;
  state->previous_right_n = right_n;
  state->have_previous = true;
  state->landing = false;
  state->last_slack = n / 4u - right_n / 4u;
  state->pending_base = state->decoded;
  state->pending_frames = emit || first ? produced : 0;
  state->pending_used = 0;
  state->decoded += produced;
  return GAUD_OK;
}

/** Pull the next audio packet and decode it. */
static GAUD_Result decode_next(VORBIS_Decoder * state) {
  for (;;) {
    const unsigned char * data = NULL;
    size_t size = 0;
    uint64_t granule = 0;
    GAUD_Result result = gaud_ogg_reader_packet(
        &state->reader, &data, &size, &granule, NULL);
    if (result != GAUD_OK) {
      state->ended = true;
      return result;
    }
    if (size == 0) {
      continue;
    }
    if ((data[0] & 1u) != 0) {
      /* A header packet: its first bit is one and an audio packet's is
       * zero, which is why every header type is odd. Skipped, which is
       * how a decoder resuming from a page boundary gets past the
       * setup header that may share it. */
      continue;
    }
    result = decode_packet(state, data, size);
    if (result != GAUD_OK) {
      return result;
    }

    /*
     * The head trim. The first page's granule position is where the
     * stream starts; if the packets up to it decode to more than that,
     * the difference belongs to the part of the first block the encoder
     * did not mean to include.
     */
    if (!state->first_page_seen && granule != OGG_NO_GRANULE) {
      state->first_page_seen = true;
      // In the granule position's terms, which count to a block's centre
      // and not to the start of the next overlap.
      uint64_t centre = state->decoded - state->last_slack;
      if (centre > granule) {
        uint64_t drop = centre - granule;
        if (drop >= state->pending_frames) {
          state->decoded -= state->pending_frames;
          state->pending_frames = 0;
          state->pending_base = state->decoded;
        }
        else {
          state->pending_used = (uint32_t)drop;
          state->decoded -= drop;
          state->pending_base = 0;
        }
      }
    }
    return GAUD_OK;
  }
}

static GAUD_Result decoder_read(GAUD_Decoder * decoder, GAUD_Buffer * buffer) {
  VORBIS_Decoder * state = gaud_decoder_private(decoder);
  size_t capacity = gaud_buffer_capacity(buffer);
  uint32_t channels = state->channels;
  unsigned char * data = gaud_buffer_data(buffer);
  size_t filled = 0;

  while (filled < capacity) {
    if (state->pending_used >= state->pending_frames) {
      if (state->ended) {
        break;
      }
      GAUD_Result result = decode_next(state);
      if (result == GAUD_ERR_FORMAT) {
        break; /* The end of the audio. */
      }
      if (result != GAUD_OK) {
        return result;
      }
      continue;
    }
    uint32_t available = state->pending_frames - state->pending_used;
    size_t room = capacity - filled;
    uint32_t take = available < room ? available : (uint32_t)room;
    int16_t * out
        = (int16_t *)(void *)(data + filled * channels * sizeof(int16_t));
    for (uint32_t frame = 0; frame < take; ++frame) {
      for (uint32_t slot = 0; slot < channels; ++slot) {
        uint32_t ch = state->order[slot];
        const int32_t * from = state->pending
            + (size_t)ch * state->pending_stride
            + state->pending_used + frame;
        out[(size_t)frame * channels + slot] = to_s16(*from);
      }
    }
    state->pending_used += take;
    filled += take;
    state->position += take;
  }

  /* The tail trim: the last page's granule position is the length, and
   * anything past it is the padding the encoder added to fill a block. */
  uint64_t total = gaud_track_frames(gaud_decoder_track(decoder));
  if (total != UINT64_MAX && state->position > total) {
    uint64_t excess = state->position - total;
    filled = excess >= filled ? 0 : filled - (size_t)excess;
    state->position = total;
  }
  gaud_decoder_set_position(decoder, state->position);
  return gaud_buffer_set_frames(buffer, filled);
}

/** Position at the start of the stream, before any packet has been read. */
static GAUD_Result rewind_to_audio(VORBIS_Decoder * state) {
  state->position = 0;
  state->decoded = 0;
  state->pending_frames = 0;
  state->pending_used = 0;
  state->pending_base = 0;
  state->have_previous = false;
  state->landing = false;
  state->tail_length = 0;
  state->previous_right_n = 0;
  state->first_page_seen = false;
  state->ended = false;
  return gaud_ogg_reader_seek(&state->reader, state->doc_state->audio_offset);
}

/** What reading packets up to the end of a page that states a position found. */
typedef struct {
  uint64_t produced;       ///< Frames the packets would hand out, in total.
  uint64_t produced_first; ///< The same at the first packet to state one.
  uint32_t slack_first;    ///< That packet's n/4 - right_n/4.
  uint32_t last_slack;     ///< The last block's n/4 - right_n/4.
  uint64_t granule;        ///< The page's granule position.
  bool end_of_stream;      ///< The page is the stream's last.
} VORBIS_Scan;

/**
 * Read packets from the reader's place to the end of the next page that
 * states a granule position, adding up what they would produce.
 *
 * @param reader A reader of its own, so that the decoder's is not moved.
 * @param first_has_no_previous Whether the first packet read is the
 *   stream's first, which produces nothing; a landing's is not, since
 *   the block before it exists, only unread.
 */
static GAUD_Result scan_to_anchor(const VORBIS_Decoder * state,
    OGG_Reader * reader, bool first_has_no_previous, VORBIS_Scan * scan) {
  bool first = first_has_no_previous;
  bool stated = false;
  memset(scan, 0, sizeof *scan);
  for (;;) {
    const unsigned char * data = NULL;
    size_t size = 0;
    uint64_t granule = 0;
    GAUD_Result result
        = gaud_ogg_reader_packet(reader, &data, &size, &granule, NULL);
    if (result != GAUD_OK) {
      return result;
    }
    uint32_t n = 0;
    uint32_t left_n = 0;
    uint32_t right_n = 0;
    result = size == 0 ? GAUD_ERR_FORMAT
                       : packet_shape(state, data, size, &n, &left_n, &right_n);
    if (result == GAUD_ERR_FORMAT) {
      continue; // A header, or nothing.
    }
    if (result != GAUD_OK) {
      return result;
    }
    if (first) {
      scan->produced += window_right_start(n, right_n) - n / 2u;
    }
    else {
      scan->produced
          += window_right_start(n, right_n) - window_left_start(n, left_n);
    }
    first = false;
    scan->last_slack = n / 4u - right_n / 4u;
    if (!stated && granule != OGG_NO_GRANULE) {
      stated = true;
      scan->produced_first = scan->produced;
      scan->slack_first = scan->last_slack;
    }
    bool page_done = reader->next_segment >= reader->segments;
    if (page_done && granule != OGG_NO_GRANULE) {
      scan->granule = granule;
      scan->end_of_stream = (reader->flags & OGG_FLAG_EOS) != 0;
      return GAUD_OK;
    }
  }
}

/**
 * Work out ::VORBIS_Decoder::bias from the first page that states a
 * position, decoding nothing: what the decoder's own numbering is there
 * is the sum of what its packets produce, less any head trim.
 */
static GAUD_Result find_bias(VORBIS_Decoder * state) {
  OGG_Reader probe;
  gaud_ogg_reader_init(&probe, state->reader.stream, state->allocator);
  probe.serial = state->reader.serial;
  probe.have_serial = true;
  GAUD_Result result
      = gaud_ogg_reader_seek(&probe, state->doc_state->audio_offset);
  VORBIS_Scan scan;
  if (result == GAUD_OK) {
    result = scan_to_anchor(state, &probe, true, &scan);
  }
  gaud_ogg_reader_free(&probe);
  if (result != GAUD_OK) {
    return result == GAUD_ERR_FORMAT ? GAUD_ERR_CORRUPT : result;
  }
  // The head trim decode_next() applies, at the first packet that states
  // a position: what it decoded beyond that position is not the stream's.
  int64_t trim = 0;
  if (scan.produced_first - scan.slack_first > scan.granule) {
    trim = (int64_t)(scan.produced_first - scan.slack_first - scan.granule);
  }
  state->bias = (int64_t)scan.produced - trim - (int64_t)scan.granule
      - (int64_t)scan.last_slack;
  state->bias_known = true;
  return GAUD_OK;
}

/** The most earlier pages a landing that cannot be numbered is moved back by. */
#define VORBIS_LAND_TRIES 4u

/**
 * Put the decoder on a page from which decoding forward reaches @p frame
 * with nothing wrong, or at the start where that is no better.
 *
 * **Nothing about a Vorbis block depends on the ones before it except
 * the lap**, so there is no pre-roll to run and no state to converge: the
 * first block decoded on the landing page produces no output, because the
 * block it laps with is not there, but its right half is exactly the lap
 * the next block wants, and from the second block on the output is
 * bit-identical to a straight read's. What the first block does have to do
 * is count: it advances the position by what it would have produced, so
 * that the next one is numbered as the granule positions number it.
 *
 * The landing is two blocks' worth of frames early, because the first
 * block's own output is the part that cannot be handed out, and it is
 * never longer than the longest block.
 *
 * **The place is numbered from the end of the landing page and not from
 * its beginning**, which is why a page that begins with the tail of a
 * packet needs no special case here as it does for Opus: the page's own
 * granule position says where its last block ends, the blocks on it add
 * up to how long they are, and that is the number the first of them
 * starts at, whatever came before. The decoder hands frames out up to the
 * start of the next overlap and a granule position counts to a block's
 * centre, so the end of the last block is its granule position plus the
 * difference, `n/4 - right_n/4`, plus whatever constant the stream's
 * first page showed (::find_bias). A last page cannot be the anchor: its
 * position is the length, which may be less than what it decodes to.
 *
 * Does nothing when the landing is no nearer than where the decoder is.
 */
static GAUD_Result land_near(VORBIS_Decoder * state, uint64_t frame) {
  if (!state->bias_known) {
    GAUD_Result found = find_bias(state);
    if (found != GAUD_OK) {
      return found;
    }
  }
  uint64_t margin = 2u * (uint64_t)state->long_block;
  uint64_t target = frame > margin ? frame - margin : 0;
  uint64_t offset = 0;
  uint64_t granule = 0;
  for (unsigned tries = 0; tries < VORBIS_LAND_TRIES && target > 0; ++tries) {
    if (gaud_ogg_bisect(&state->reader, target, state->doc_state->audio_offset,
            &offset, &granule)
            != GAUD_OK
        || granule == 0) {
      break;
    }
    // Number the place backwards from the end of the page, where a
    // position is stated: what the blocks on it add up to is how far
    // before that the page's first block starts. A page that begins with
    // the tail of a packet needs no special case, because the reader
    // drops that fragment here and in the decoder alike, and what is
    // counted back from the end is only what both go on to read.
    OGG_Reader probe;
    gaud_ogg_reader_init(&probe, state->reader.stream, state->allocator);
    probe.serial = state->reader.serial;
    probe.have_serial = true;
    VORBIS_Scan scan;
    GAUD_Result scanned = gaud_ogg_reader_seek(&probe, offset);
    if (scanned == GAUD_OK) {
      scanned = scan_to_anchor(state, &probe, false, &scan);
    }
    gaud_ogg_reader_free(&probe);
    if (scanned != GAUD_OK || scan.end_of_stream) {
      // The last page's position is the length, which may be less than
      // what its blocks decode to, so it cannot number anything.
      target = granule - 1u;
      continue;
    }
    int64_t at = (int64_t)scan.granule + (int64_t)scan.last_slack
        + state->bias - (int64_t)scan.produced;
    if (at < 0 || (uint64_t)at + state->long_block > frame) {
      target = granule - 1u;
      continue;
    }
    if (frame >= state->pending_base && (uint64_t)at <= state->decoded) {
      return GAUD_OK; // Decoding on is already as near and is exact.
    }
    state->position = (uint64_t)at;
    state->decoded = (uint64_t)at;
    state->pending_frames = 0;
    state->pending_used = 0;
    state->pending_base = (uint64_t)at;
    state->have_previous = false;
    state->landing = true;
    state->tail_length = 0;
    state->previous_right_n = 0;
    state->first_page_seen = true;
    state->ended = false;
    return gaud_ogg_reader_seek(&state->reader, offset);
  }
  if (frame < state->pending_base) {
    return rewind_to_audio(state);
  }
  return GAUD_OK; // Nothing nearer was found; decode on from here.
}

/**
 * Seek by bisecting the file to a page and decoding forward from it.
 *
 * Exact: what is read after a seek is what a straight read gives, to the
 * bit, because the only thing a block carries to the next is its lap and
 * the landing decodes the block that provides it. See ::land_near.
 */
static GAUD_Result decoder_seek(
    GAUD_Decoder * decoder, uint64_t frame, uint64_t * out_landed) {
  VORBIS_Decoder * state = gaud_decoder_private(decoder);
  uint64_t total = gaud_track_frames(gaud_decoder_track(decoder));
  if (total != UINT64_MAX && frame > total) {
    frame = total;
  }
  // A target behind the decoder, or a long way ahead of it, is reached by
  // landing on a page near it rather than by decoding from the start or
  // through everything between.
  if (frame < state->pending_base
      || (frame > state->decoded
          && frame - state->decoded > 4u * state->long_block)) {
    GAUD_Result jump = land_near(state, frame);
    if (jump != GAUD_OK) {
      return jump;
    }
  }
  /*
   * Three cases, and the first draft had only two. The packet already
   * decoded covers `[pending_base, pending_base + pending_frames)`; a
   * target inside it is a move within the buffer, a target below it
   * needs a rewind, and a target above it needs more packets. The draft
   * tested the target against `position - pending_used` - the frame most
   * recently handed out rather than where the buffer starts - so a seek
   * backwards into the current block did nothing at all and reported
   * the position it was already at.
   */
  for (;;) {
    if (state->pending_frames
        && frame >= state->pending_base
        && frame < state->pending_base + state->pending_frames) {
      state->pending_used = (uint32_t)(frame - state->pending_base);
      state->position = frame;
      break;
    }
    if (frame < state->pending_base) {
      GAUD_Result result = rewind_to_audio(state);
      if (result != GAUD_OK) {
        return result;
      }
      continue;
    }
    GAUD_Result result = decode_next(state);
    if (result == GAUD_ERR_FORMAT) {
      /* Past the end: land where the audio stopped. */
      state->position = state->pending_base + state->pending_frames;
      state->pending_used = state->pending_frames;
      break;
    }
    if (result != GAUD_OK) {
      return result;
    }
  }
  if (total != UINT64_MAX && state->position > total) {
    state->position = total;
  }
  gaud_decoder_set_position(decoder, state->position);
  *out_landed = state->position;
  return GAUD_OK;
}

static const GAUD_Decoder_Vtable vorbis_decoder_vtable = {
    .read = decoder_read,
    .seek = decoder_seek,
    .close = decoder_close,
};

GAUD_Result gaud_vorbis_decoder_open(
    const GAUD_Codec * codec, GAUD_Track * track, GAUD_Decoder ** out) {
  (void)codec;
  VORBIS_File * doc_state = gaud_track_private(track);
  if (!doc_state) {
    return GAUD_ERR_INTERNAL;
  }
  /* Floor 0 is identified and refused; see vorbis_floor.c for why. A
   * per-track answer, which is what a capability bit cannot give. */
  for (uint32_t i = 0; i < doc_state->setup.floor_count; ++i) {
    if (doc_state->setup.floors[i].type == 0) {
      return GAUD_ERR_UNSUPPORTED;
    }
  }

  const GAUD_Allocator * allocator = doc_state->allocator;
  VORBIS_Decoder * state = gcu_allocator_malloc(allocator, sizeof(*state));
  if (!state) {
    return GAUD_ERR_OOM;
  }
  memset(state, 0, sizeof(*state));
  state->doc_state = doc_state;
  state->allocator = allocator;
  state->channels = doc_state->info.channels;
  state->long_block = doc_state->info.blocksize_long;
  state->short_block = doc_state->info.blocksize_short;
  for (uint32_t i = 0; i < 8u && i < state->channels; ++i) {
    state->order[i] = (unsigned char)gaud_vorbis_channel_slot(
        state->channels, i);
  }

  size_t half = state->long_block / 2u;
  size_t per_channel = (size_t)state->channels * half;
  state->residue
      = gcu_allocator_malloc(allocator, per_channel * sizeof(int32_t));
  state->curves = gcu_allocator_malloc(allocator, per_channel);
  state->spectrum = gcu_allocator_malloc(allocator, half * sizeof(int32_t));
  state->scratch = gcu_allocator_malloc(
      allocator, (size_t)state->long_block * sizeof(int32_t));
  state->block = gcu_allocator_malloc(
      allocator, (size_t)state->long_block * sizeof(int32_t));
  state->tail
      = gcu_allocator_malloc(allocator, per_channel * sizeof(int32_t));
  state->pending_stride = state->long_block;
  state->pending = gcu_allocator_malloc(allocator,
      (size_t)state->channels * state->pending_stride * sizeof(int32_t));
  if (!state->residue || !state->curves || !state->spectrum
      || !state->scratch || !state->block || !state->tail
      || !state->pending) {
    goto Fail;
  }
  memset(state->tail, 0, per_channel * sizeof(int32_t));

  gaud_ogg_reader_init(
      &state->reader, gaud_doc_stream(gaud_track_doc(track)), allocator);
  state->reader.serial = doc_state->serial;
  state->reader.have_serial = true;
  if (rewind_to_audio(state) != GAUD_OK) {
    goto Fail;
  }

  GAUD_Result result = gaud_decoder_create_internal(
      track, &vorbis_decoder_vtable, state, out);
  if (result != GAUD_OK) {
    goto Fail;
  }
  return GAUD_OK;

Fail:
  gaud_ogg_reader_free(&state->reader);
  gcu_allocator_free(allocator, state->residue);
  gcu_allocator_free(allocator, state->curves);
  gcu_allocator_free(allocator, state->spectrum);
  gcu_allocator_free(allocator, state->scratch);
  gcu_allocator_free(allocator, state->block);
  gcu_allocator_free(allocator, state->tail);
  gcu_allocator_free(allocator, state->pending);
  gcu_allocator_free(allocator, state);
  return GAUD_ERR_OOM;
}
