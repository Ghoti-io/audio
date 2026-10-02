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
 * The pyramid vector quantiser's index: RFC 6716 section 4.3.4.2.
 *
 * A band's shape is one point on the surface of an L1 ball - a vector of
 * `N` integers whose absolute values sum to `K` - and the bitstream
 * carries it as a single uniformly distributed integer between zero and
 * `V(N,K)-1`, where `V(N,K)` counts those vectors. So the whole of shape
 * decoding is an enumeration: turn one integer back into the vector it
 * names.
 *
 * **This is the one piece of CELT the prose specifies completely.**
 * Section 4.3.4.2 gives the five steps, gives the recurrence
 * `V(N,K) = V(N-1,K) + V(N,K-1) + V(N-1,K-1)` with `V(N,0) = 1` and
 * `V(0,K) = 0`, and then says in as many words that implementations
 * "MAY use any methods they like, as long as they are equivalent to the
 * mathematical definition". That invitation is taken up twice over: the
 * code below walks the enumeration with a helper function `U`, and the
 * tests walk it with `V` computed straight from the recurrence, and the
 * two must agree on every vector.
 *
 * **Why `U` and not `V`.** Writing `U(N,K)` for the number of those
 * vectors whose first element is positive, the reference's own
 * commentary gives `U(N,K) = (V(N-1,K-1) + V(N,K-1))/2` and
 * `V(N,K) = U(N,K) + U(N,K+1)` - so the half-sum the prose's step 1
 * computes, `(V(N-j-1,k) + V(N-j,k))/2`, is exactly `U(N-j,k+1)`. `U`
 * obeys the same three-term recurrence as `V`, which means one row of
 * it can be stepped down a dimension in place. That is the difference
 * between `K` words of working memory and `N*K` of them, and it is why
 * the enumeration is written around `U`.
 *
 * **The sizes stay small for a reason.** `V(N,K)` must fit in 32 bits,
 * because the index is read by ::gaud_opus_dec_uint and that is its
 * limit; a band too wide for its pulse count is split in two instead
 * (section 4.3.4.4). So `N` and `K` are never both large: measured over
 * the conformance vectors, `N` reaches 96 and `K` reaches 128, and the
 * largest `N` arrives with `K` of 5 while the largest `K` arrives with
 * `N` of 2. The 32-bit arithmetic below therefore does not overflow on
 * any input the format can present, and it is deliberately unsigned so
 * that the subtractions in ::row_down are defined even where an
 * intermediate would go negative.
 */

#include "opus_celt.h"
#include <string.h>

/**
 * Step a row of `U` up one dimension, in place.
 *
 * Any sequence obeying `u[i][j] = u[i-1][j] + u[i][j-1] + u[i-1][j-1]`
 * can be advanced this way; @p base is the new row's first entry.
 */
static void row_up(uint32_t * u, unsigned len, uint32_t base) {
  unsigned j = 1;
  do {
    uint32_t next = u[j] + u[j - 1u] + base;
    u[j - 1u] = base;
    base = next;
  } while (++j < len);
  u[j - 1u] = base;
}

/**
 * Step a row of `U` down one dimension, in place.
 *
 * The same recurrence read backwards: `u[i-1][j] = u[i][j] - u[i][j-1]
 * - u[i-1][j-1]`. Unsigned on purpose - an intermediate here can pass
 * below zero and wrap, and the final values are correct regardless,
 * which signed arithmetic would make undefined rather than merely ugly.
 */
static void row_down(uint32_t * u, unsigned len, uint32_t base) {
  unsigned j = 1;
  do {
    uint32_t next = u[j] - u[j - 1u] - base;
    u[j - 1u] = base;
    base = next;
  } while (++j < len);
  u[j - 1u] = base;
}

uint32_t gaud_celt_pvq_urow(unsigned n, unsigned k, uint32_t * u) {
  unsigned len = k + 2u;
  u[0] = 0;
  u[1] = 1;
  /*
   * Row two in closed form: `U(2,k)` is `2k-1` for `k` of one or more,
   * which seeds the recurrence. Rows zero and one are degenerate and
   * the callers below never ask for them - a band of fewer than two
   * dimensions is not shape-coded at all.
   */
  for (unsigned j = 2; j < len; ++j) {
    u[j] = (j << 1) - 1u;
  }
  for (unsigned j = 2; j < n; ++j) {
    row_up(u + 1, k + 1u, 1u);
  }
  return u[k] + u[k + 1u];
}

uint32_t gaud_celt_pvq_v(unsigned n, unsigned k) {
  uint32_t u[CELT_MAX_PULSES + 2u];
  /*
   * The three degenerate rows, which ::gaud_celt_pvq_urow does not
   * cover because its recurrence is seeded at two dimensions. They are
   * here so that this function matches the definition over its whole
   * domain and can be checked against the table of V(N,K) for N and K
   * below ten that the reference prints: no pulses is one vector (the
   * empty one), no dimensions cannot hold a pulse, and one dimension
   * holds any number of them in exactly two ways, one per sign.
   */
  if (k == 0) {
    return 1u;
  }
  if (n == 0) {
    return 0u;
  }
  if (n == 1) {
    return 2u;
  }
  return gaud_celt_pvq_urow(n, k, u);
}

void gaud_celt_pulses_from_index(
    int * y, unsigned n, unsigned k, uint32_t index) {
  uint32_t u[CELT_MAX_PULSES + 2u];
  (void)gaud_celt_pvq_urow(n, k, u);
  /*
   * Section 4.3.4.2's five steps, one dimension at a time. `u[k+1]` is
   * the prose's `p`: the count of vectors whose first element is
   * positive, so comparing the index against it recovers the sign and
   * leaves an index into the half it belongs to.
   */
  for (unsigned j = 0; j < n; ++j) {
    uint32_t p = u[k + 1u];
    /* All-ones when the index is in the negative half, zero otherwise:
     * the sign is then applied without a branch, and - more to the
     * point - without the index having to be signed. */
    uint32_t sign = index >= p ? 0xFFFFFFFFu : 0u;
    index -= p & sign;
    unsigned k0 = k;
    p = u[k];
    /* Walk down until the remaining count no longer exceeds the index;
     * how far it walked is how many pulses this dimension took. */
    while (p > index) {
      p = u[--k];
    }
    index -= p;
    int magnitude = (int)(k0 - k);
    y[j] = (int)(((uint32_t)magnitude + sign) ^ sign);
    row_down(u, k + 2u, 0u);
  }
}

void gaud_celt_decode_pulses(
    OPUS_Range * range, int * y, unsigned n, unsigned k) {
  uint32_t u[CELT_MAX_PULSES + 2u];
  uint32_t total = gaud_celt_pvq_urow(n, k, u);
  gaud_celt_pulses_from_index(y, n, k, gaud_opus_dec_uint(range, total));
}
