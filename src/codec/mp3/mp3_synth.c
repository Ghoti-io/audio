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
 * The polyphase synthesis filterbank: 32 subbands in, 32 samples out.
 *
 * **Every layer ends here.** Layers I and II hand it requantised subband
 * samples directly; Layer III hands it the output of its own hybrid
 * filterbank. It is the one piece of arithmetic the three share, and it is
 * what the 512 coefficients of Table 3-B.3 are for.
 *
 * The implementation is the flow chart of 11172-3 Figure 3-A.2, in the
 * order the standard draws it, with one substitution: the shift of the
 * 1024-value state vector that the chart begins with is a *rotation* here.
 * Moving 960 values with every 32 samples produced would be 138 KiB of
 * copying per frame per channel to express an offset that can be
 * subtracted instead. V[n] below is `state[(at + n) & 1023]`, and `at`
 * steps back by 64 each call - which is the same vector, read from a
 * moving origin.
 *
 * Everything is Q28 with 64-bit accumulation, and the two places a sum can
 * grow are both bounded by the standard: the matrixing sums 32 products of
 * values below 1.0, and the windowing sums 16. Both are saturated rather
 * than wrapped, because a wrap turns a loud passage into a click and a
 * saturation turns it into a loud passage.
 */

#include "mp3_internal.h"
#include "mp3_tables.h"

/** Clamp a 64-bit Q28 accumulator into the Q28 range an int32 can hold. */
static int32_t saturate(int64_t value) {
  if (value > INT32_MAX) {
    return INT32_MAX;
  }
  if (value < INT32_MIN) {
    return INT32_MIN;
  }
  return (int32_t)value;
}

void gaud_mp3_synth_reset(MP3_Synth * synth) {
  memset(synth->state, 0, sizeof(synth->state));
  synth->at = 0;
}

void gaud_mp3_synth_run(MP3_Synth * synth, const int32_t subband[32],
    int32_t * out, size_t stride) {
  /* The rotation. `at` is where V[0] lives; stepping it back by 64 is the
   * chart's "for i=1023 downto 64 do V[i]=V[i-64]". */
  synth->at = (synth->at + 1024u - 64u) & 1023u;
  int32_t * state = synth->state;
  unsigned at = synth->at;

  /* Matrixing: V[i] = sum N[i][k] * S[k], for the 64 new values. */
  for (unsigned i = 0; i < 64u; ++i) {
    int64_t sum = 0;
    const int32_t * row = gaud_mp3_synth_cos[i];
    for (unsigned k = 0; k < 32u; ++k) {
      sum += (int64_t)row[k] * subband[k];
    }
    state[(at + i) & 1023u] = saturate(sum >> MP3_Q);
  }

  /* The window is applied to a vector built out of two halves of each
   * 128-value stretch of V, which is the chart's U, and the output is a
   * sum down the columns of W. The two loops are fused here: nothing
   * needs U or W as a whole, and keeping them would be 4 KiB of copying
   * to hold values each used once. */
  for (unsigned j = 0; j < 32u; ++j) {
    int64_t sum = 0;
    for (unsigned i = 0; i < 16u; ++i) {
      /* W[j + 32i] = U[j + 32i] * D[j + 32i], and U's index maps back
       * into V by the chart's build: the even 32-value blocks of U come
       * from the starts of V's 128-value stretches and the odd ones from
       * 96 values into them. */
      unsigned u = j + 32u * i;
      unsigned block = u >> 6;   /* which 64-value block of U */
      unsigned inside = u & 63u; /* where in it */
      unsigned v = block * 128u + (inside < 32u ? inside : inside - 32u + 96u);
      sum += (int64_t)state[(at + v) & 1023u] * gaud_mp3_window[u];
    }
    out[j * stride] = saturate(sum >> MP3_Q);
  }
}
