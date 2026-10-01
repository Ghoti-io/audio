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
 * Vorbis codebooks: the prefix code, and the vector each entry may stand
 * for.
 *
 * **A codebook states lengths and not codewords.** That is the single
 * thing about this part of the format that has to be got right, because
 * everything else follows from it. The stream gives one code length per
 * entry; the codewords are then assigned by a procedure, and a decoder
 * that assigned them differently reads every entry of every codebook
 * wrong - which looks like noise rather than like a parse failure, so
 * there is no partial credit.
 *
 * The procedure: each entry in turn takes the numerically smallest
 * codeword of its own length that is neither a prefix of an already
 * assigned codeword nor prefixed by one. Entry *order*, not length order,
 * which is what separates this from a canonical Huffman code - two
 * codebooks with the same multiset of lengths in different orders have
 * different codewords.
 *
 * And building the code is where a malformed one is detected. A set of
 * lengths may describe a tree with no room for the next entry
 * (overpopulated), which is refused here; or one with nodes left over
 * (underpopulated), which is accepted, because real streams contain them
 * and the leftover nodes simply match no codeword at decode time. A
 * decoder that skipped the construction would read a well-formed stream
 * correctly and a malformed one into whichever entry it happened to
 * reach.
 */

#include "vorbis_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/** The codebook sync pattern, read as a 24-bit field. */
#define VORBIS_CODEBOOK_SYNC 0x564342u

/** The deepest a codeword may be; the format's lengths are five bits. */
#define VORBIS_MAX_LENGTH 32u

/**
 * The specification's `float32_unpack`, scaled into Q#VORBIS_Q.
 *
 * A 21-bit magnitude, a sign, and a ten-bit exponent biased by 788, which
 * together can name 2^-788 and 2^235. Neither is representable and
 * neither occurs; what this has to do is be exact for the range that does
 * occur and saturate rather than wrap outside it.
 *
 * The shift is applied to the magnitude and the sign put back afterwards,
 * because a right shift of a negative value is implementation-defined in
 * C and this library promises the same bytes on every platform.
 */
static int64_t float32_unpack_q(uint32_t packed) {
  int64_t magnitude = (int64_t)(packed & 0x1FFFFFu);
  int exponent = (int)((packed & 0x7FE00000u) >> 21);
  bool negative = (packed & 0x80000000u) != 0;
  int shift = exponent - 788 + (int)VORBIS_Q;

  int64_t value;
  if (shift >= 0) {
    /* 2^42 times a 21-bit magnitude is the most an int64 holds. */
    if (shift > 42) {
      value = INT64_MAX;
    }
    else {
      value = magnitude << shift;
    }
  }
  else if (-shift >= 62) {
    value = 0;
  }
  else {
    /* Rounded to nearest rather than truncated, so that the value does
     * not depend on which way a shift happens to round. */
    int64_t half = (int64_t)1 << (-shift - 1);
    value = (magnitude + half) >> -shift;
  }
  return negative ? -value : value;
}

/** Saturating narrowing to the spectrum's word, as the MPEG decoder does. */
static int32_t saturate(int64_t value) {
  if (value > INT32_MAX) {
    return INT32_MAX;
  }
  if (value < INT32_MIN) {
    return INT32_MIN;
  }
  return (int32_t)value;
}

/**
 * Assign a codeword to every entry whose length is nonzero.
 *
 * `next[length]` is the smallest codeword of that length still available,
 * held as a `length`-bit path from the root. Taking one has two effects,
 * and both are needed:
 *
 *   - **Above it**, every node on the path to the root is now interior.
 *     The next free node at this depth is the sibling, unless the node
 *     taken *was* a right child - in which case the pair is exhausted and
 *     the next free node at this depth is the left child of the next free
 *     node one level up. That is what the downward loop computes, and it
 *     carries, which is why it is a loop rather than a step.
 *   - **Below it**, any free node that was a descendant of the node taken
 *     is no longer free, and is re-pointed the same way. The upward loop
 *     stops at the first depth whose free node was not a descendant,
 *     because the assignment's own invariant says the deeper ones would
 *     already have moved if they were on the same path.
 *
 * @return ::GAUD_OK, or ::GAUD_ERR_CORRUPT for lengths that describe a
 *   tree with no room for an entry they name.
 */
static GAUD_Result assign_codewords(VORBIS_Codebook * book) {
  uint32_t next[VORBIS_MAX_LENGTH + 1];
  memset(next, 0, sizeof(next));
  book->used = 0;

  for (uint32_t i = 0; i < book->entries; ++i) {
    unsigned length = book->lengths[i];
    if (length == 0) {
      continue;
    }
    uint32_t codeword = next[length];
    /* A codeword wider than its own length means the counter has run
     * past the last node at that depth: the lengths describe a tree
     * with more leaves than it has room for. At 32 the test cannot be
     * written this way - the shift would be undefined - and cannot be
     * needed either, because a 32-bit counter cannot exceed 32 bits. */
    if (length < VORBIS_MAX_LENGTH && (codeword >> length) != 0) {
      return GAUD_ERR_CORRUPT;
    }
    book->codewords[book->used] = codeword;
    book->entry_of[book->used] = i;
    ++book->used;

    for (unsigned depth = length; depth > 0; --depth) {
      if (next[depth] & 1u) {
        /* A right child: this pair is finished, so move up. */
        next[depth] = depth == 1u ? next[1] + 1u : next[depth - 1u] << 1;
        break;
      }
      ++next[depth];
    }
    for (unsigned depth = length + 1u; depth <= VORBIS_MAX_LENGTH; ++depth) {
      if ((next[depth] >> 1) != codeword) {
        break;
      }
      codeword = next[depth];
      next[depth] = next[depth - 1u] << 1;
    }
  }
  return GAUD_OK;
}

/**
 * Build the walk-one-bit-at-a-time tree the decoder uses.
 *
 * A tree rather than a flat table, because a codeword may be 32 bits long
 * and a flat table would be four billion entries. The nodes are grown
 * rather than sized up front: the count is bounded by the sum of the
 * lengths, which for a pathological codebook is far larger than the
 * number that actually appear.
 */
static GAUD_Result build_tree(
    const GAUD_Allocator * allocator, VORBIS_Codebook * book) {
  size_t capacity = 64;
  int32_t * child = gcu_allocator_malloc(allocator, capacity * 2u
      * sizeof(*child));
  if (!child) {
    return GAUD_ERR_OOM;
  }
  child[0] = 0;
  child[1] = 0;
  size_t nodes = 1;

  for (uint32_t k = 0; k < book->used; ++k) {
    uint32_t codeword = book->codewords[k];
    unsigned length = book->lengths[book->entry_of[k]];
    size_t node = 0;
    for (unsigned depth = 0; depth < length; ++depth) {
      unsigned bit = (codeword >> (length - 1u - depth)) & 1u;
      int32_t * slot = &child[node * 2u + bit];
      bool last = depth + 1u == length;
      if (last) {
        if (*slot != 0) {
          /*
           * Something already occupies this node, so one codeword is a
           * prefix of another - which assign_codewords() is supposed to
           * have made impossible. Refused rather than asserted, because
           * the lengths came out of a file.
           *
           * **The second of two checks that catch the same thing**, and
           * the redundancy is deliberate rather than left over: either
           * one alone refuses every overpopulated codebook the unit
           * tests contain, which was established by disabling them one
           * at a time and finding the test still passed. The cheap one
           * above catches it without building anything; this one is
           * what makes the tree's own invariant self-evident at the
           * point where it would be violated.
           */
          gcu_allocator_free(allocator, child);
          return GAUD_ERR_CORRUPT;
        }
        /* Leaves are stored as the negated entry number minus one, so
         * that entry zero is distinguishable from an empty slot. */
        *slot = -(int32_t)book->entry_of[k] - 1;
        break;
      }
      if (*slot < 0) {
        gcu_allocator_free(allocator, child);
        return GAUD_ERR_CORRUPT; /* A leaf where an interior node is due. */
      }
      if (*slot == 0) {
        if (nodes == capacity) {
          size_t grown = capacity * 2u;
          int32_t * bigger = gcu_allocator_malloc(allocator, grown * 2u
              * sizeof(*child));
          if (!bigger) {
            gcu_allocator_free(allocator, child);
            return GAUD_ERR_OOM;
          }
          memcpy(bigger, child, capacity * 2u * sizeof(*child));
          gcu_allocator_free(allocator, child);
          child = bigger;
          capacity = grown;
          slot = &child[node * 2u + bit];
        }
        child[nodes * 2u] = 0;
        child[nodes * 2u + 1u] = 0;
        *slot = (int32_t)nodes;
        ++nodes;
      }
      node = (size_t)*slot;
    }
  }
  book->tree = child;
  book->tree_nodes = nodes;
  return GAUD_OK;
}

uint32_t gaud_vorbis_codebook_decode(
    const VORBIS_Codebook * book, VORBIS_Bits * bits) {
  if (!book->tree) {
    return UINT32_MAX;
  }
  size_t node = 0;
  for (unsigned depth = 0; depth <= VORBIS_MAX_LENGTH; ++depth) {
    unsigned bit = (unsigned)gaud_vorbis_bits_read(bits, 1);
    if (bits->past_end) {
      return UINT32_MAX;
    }
    int32_t step = book->tree[node * 2u + bit];
    if (step < 0) {
      return (uint32_t)(-step - 1);
    }
    if (step == 0) {
      /* An unpopulated node: these lengths describe a tree with room
       * left over, and this is a bit pattern no entry stands for. Legal
       * in a file and an error in the stream. */
      return UINT32_MAX;
    }
    node = (size_t)step;
  }
  return UINT32_MAX;
}

/** Compute the vector values a codebook with a lookup stands for. */
static GAUD_Result build_values(const GAUD_Allocator * allocator,
    VORBIS_Codebook * book, const uint32_t * multiplicands,
    uint32_t lookup_values, int64_t minimum, int64_t delta) {
  size_t count = (size_t)book->entries * book->dimensions;
  book->values = gcu_allocator_malloc(allocator, count * sizeof(int32_t));
  if (!book->values) {
    return GAUD_ERR_OOM;
  }
  for (uint32_t entry = 0; entry < book->entries; ++entry) {
    int64_t last = 0;
    if (book->lookup_type == 1u) {
      /*
       * A lattice: the entry number is read as a number in base
       * `lookup_values`, one digit per dimension, lowest digit first.
       * So the whole book is `lookup_values` multiplicands rather than
       * `entries * dimensions` of them, which is why a codebook of
       * millions of entries fits in a few hundred bytes.
       */
      uint32_t divisor = 1;
      for (uint32_t j = 0; j < book->dimensions; ++j) {
        uint32_t offset = (entry / divisor) % lookup_values;
        int64_t value
            = minimum + (int64_t)multiplicands[offset] * delta + last;
        book->values[(size_t)entry * book->dimensions + j] = saturate(value);
        if (book->sequence_p) {
          last = value;
        }
        divisor *= lookup_values;
      }
    }
    else {
      size_t offset = (size_t)entry * book->dimensions;
      for (uint32_t j = 0; j < book->dimensions; ++j) {
        int64_t value
            = minimum + (int64_t)multiplicands[offset + j] * delta + last;
        book->values[offset + j] = saturate(value);
        if (book->sequence_p) {
          last = value;
        }
      }
    }
  }
  return GAUD_OK;
}

void gaud_vorbis_codebook_free(
    const GAUD_Allocator * allocator, VORBIS_Codebook * book) {
  gcu_allocator_free(allocator, book->lengths);
  gcu_allocator_free(allocator, book->codewords);
  gcu_allocator_free(allocator, book->entry_of);
  gcu_allocator_free(allocator, book->values);
  gcu_allocator_free(allocator, book->tree);
  memset(book, 0, sizeof(*book));
}

GAUD_Result gaud_vorbis_parse_codebook(VORBIS_Bits * bits,
    const GAUD_Allocator * allocator, VORBIS_Codebook * out) {
  memset(out, 0, sizeof(*out));
  if (gaud_vorbis_bits_read(bits, 24) != VORBIS_CODEBOOK_SYNC) {
    return GAUD_ERR_CORRUPT;
  }
  out->dimensions = gaud_vorbis_bits_read(bits, 16);
  out->entries = gaud_vorbis_bits_read(bits, 24);
  if (bits->past_end || out->entries == 0) {
    return GAUD_ERR_CORRUPT;
  }

  out->lengths = gcu_allocator_malloc(allocator, out->entries);
  if (!out->lengths) {
    return GAUD_ERR_OOM;
  }
  memset(out->lengths, 0, out->entries);

  bool ordered = gaud_vorbis_bits_read(bits, 1) != 0;
  if (!ordered) {
    bool sparse = gaud_vorbis_bits_read(bits, 1) != 0;
    for (uint32_t i = 0; i < out->entries; ++i) {
      if (sparse && gaud_vorbis_bits_read(bits, 1) == 0) {
        continue; /* An unused entry, which a sparse book may state. */
      }
      out->lengths[i] = (unsigned char)(gaud_vorbis_bits_read(bits, 5) + 1u);
    }
  }
  else {
    /*
     * An ordered book states runs rather than lengths: the first length,
     * then how many entries share it, then how many share the next, and
     * so on - which is why the lengths of an ordered book are
     * non-decreasing by construction and a run count is read with
     * `ilog(entries - filled)` bits rather than a fixed width.
     */
    uint32_t filled = 0;
    uint32_t length = gaud_vorbis_bits_read(bits, 5) + 1u;
    while (filled < out->entries) {
      if (length > VORBIS_MAX_LENGTH) {
        return GAUD_ERR_CORRUPT;
      }
      uint32_t run = gaud_vorbis_bits_read(
          bits, gaud_vorbis_ilog(out->entries - filled));
      if (bits->past_end || run > out->entries - filled) {
        return GAUD_ERR_CORRUPT;
      }
      for (uint32_t i = 0; i < run; ++i) {
        out->lengths[filled + i] = (unsigned char)length;
      }
      filled += run;
      ++length;
      if (run == 0 && length > VORBIS_MAX_LENGTH + 1u) {
        return GAUD_ERR_CORRUPT; /* No progress and no lengths left. */
      }
    }
  }
  if (bits->past_end) {
    return GAUD_ERR_CORRUPT;
  }

  uint32_t used = 0;
  for (uint32_t i = 0; i < out->entries; ++i) {
    if (out->lengths[i] > VORBIS_MAX_LENGTH) {
      return GAUD_ERR_CORRUPT;
    }
    if (out->lengths[i]) {
      ++used;
    }
  }
  if (used == 0) {
    /* Every entry unused. Legal to state and useless to decode with, so
     * it is accepted here and refused where it is used - a stream may
     * define a codebook it never names. */
    out->used = 0;
  }
  else {
    out->codewords
        = gcu_allocator_malloc(allocator, used * sizeof(*out->codewords));
    out->entry_of
        = gcu_allocator_malloc(allocator, used * sizeof(*out->entry_of));
    if (!out->codewords || !out->entry_of) {
      return GAUD_ERR_OOM;
    }
    GAUD_Result result = assign_codewords(out);
    if (result != GAUD_OK) {
      return result;
    }
    result = build_tree(allocator, out);
    if (result != GAUD_OK) {
      return result;
    }
  }

  out->lookup_type = (unsigned)gaud_vorbis_bits_read(bits, 4);
  if (out->lookup_type == 0) {
    return bits->past_end ? GAUD_ERR_CORRUPT : GAUD_OK;
  }
  if (out->lookup_type > 2u) {
    /* The format reserves 3 to 15 and says a decoder must refuse them.
     * Guessing would be worse than declining: the fields that follow
     * differ per type. */
    return GAUD_ERR_CORRUPT;
  }
  if (out->dimensions == 0) {
    return GAUD_ERR_CORRUPT; /* A vector of no values. */
  }

  int64_t minimum = float32_unpack_q(gaud_vorbis_bits_read(bits, 32));
  int64_t delta = float32_unpack_q(gaud_vorbis_bits_read(bits, 32));
  unsigned value_bits = (unsigned)gaud_vorbis_bits_read(bits, 4) + 1u;
  out->sequence_p = gaud_vorbis_bits_read(bits, 1) != 0;
  if (bits->past_end) {
    return GAUD_ERR_CORRUPT;
  }

  uint32_t lookup_values = out->lookup_type == 1u
      ? gaud_vorbis_lookup1_values(out->entries, out->dimensions)
      : out->entries * out->dimensions;
  if (out->lookup_type == 2u
      && (uint64_t)out->entries * out->dimensions > 0xFFFFFFFFu) {
    return GAUD_ERR_CORRUPT;
  }
  if (lookup_values == 0) {
    return GAUD_ERR_CORRUPT;
  }
  uint32_t * multiplicands = gcu_allocator_malloc(
      allocator, (size_t)lookup_values * sizeof(*multiplicands));
  if (!multiplicands) {
    return GAUD_ERR_OOM;
  }
  for (uint32_t i = 0; i < lookup_values; ++i) {
    multiplicands[i] = gaud_vorbis_bits_read(bits, value_bits);
  }
  GAUD_Result result = bits->past_end ? GAUD_ERR_CORRUPT : GAUD_OK;
  if (result == GAUD_OK) {
    /* `delta` is multiplied by a `value_bits`-wide multiplicand, so the
     * product is bounded by 2^16 times what float32_unpack_q returned -
     * which is why the accumulation is in int64 and only the stored
     * value saturates. */
    result = build_values(
        allocator, out, multiplicands, lookup_values, minimum, delta);
  }
  gcu_allocator_free(allocator, multiplicands);
  return result;
}
