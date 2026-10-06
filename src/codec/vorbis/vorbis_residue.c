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
 * The residue: the spectrum itself, relative to the floor.
 *
 * Three types, and the specification describes them as three because of
 * one difference: **where a partition's values land.**
 *
 *   - **Type 2 interleaves the channels.** All the channels of a submap
 *     are read as one long vector of `channels * lines` values, and the
 *     value at position `k` belongs to channel `k % channels` at line
 *     `k / channels`. Which means a stereo submap's two channels are
 *     coded together, and a decoder that read them separately gets
 *     neither. This is what libvorbis uses for coupled stereo and is 13
 *     of the 19 residues in this corpus.
 *   - **Type 1 is contiguous.** Each channel's vector is read on its
 *     own, and a partition's values are consecutive lines of it.
 *   - **Type 0 interleaves within a partition**: a partition of `n`
 *     values is read as `n / dimensions` vectors, and the `i`-th value of
 *     the `v`-th lands at `i * (n / dimensions) + v` - the vectors are
 *     laid across the partition and not end to end. It is in the
 *     specification and produced by no encoder, so nothing checked this
 *     until a stream was written for the purpose, and the first version
 *     read it wrongly: see tools/oracle/vorbis_synth.py.
 *
 * Everything else about the three is identical, which is why one function
 * reads all of them and the type decides two line numbers.
 *
 * **The pass structure is what makes this a residue and not a spectrum.**
 * A partition is coded up to eight times over, by up to eight different
 * codebooks, and each pass *adds* to what the ones before it put there.
 * So the first pass is a coarse value and the later ones refine it, and a
 * decoder that overwrote rather than accumulated would decode the
 * refinement alone - which is quiet, and sounds like a decoder that
 * almost works.
 *
 * The classification is read once, in the first pass, for every partition
 * of every channel, and reused by the passes after it. That ordering is
 * not an optimisation: the class numbers are packed several to a codebook
 * entry, so they cannot be read per pass even in principle.
 */

#include "vorbis_internal.h"
#include <string.h>

/**
 * The most partitions one residue call will classify.
 *
 * A residue covers at most `channels * lines` values for type 2, which
 * for the format's largest block and most channels is far more than any
 * real stream - but the partition size is stated per residue and may be
 * one, so the count is bounded here rather than assumed. The array below
 * holds one class number per partition per vector, so the bound has to
 * cover the product.
 */
#define VORBIS_MAX_PARTITIONS 8192u

/** The most vectors one residue call classifies: a submap's channels. */
#define VORBIS_MAX_RESIDUE_VECTORS 256u

/** Saturating addition, so a pathological stream cannot wrap. */
static int32_t add_sat(int32_t a, int32_t b) {
  int32_t sum;
  if (!__builtin_add_overflow(a, b, &sum)) {
    return sum;
  }
  return b < 0 ? INT32_MIN : INT32_MAX;
}

/**
 * Read one partition's vectors into @p into.
 *
 * @param interleave False for types 1 and 2, where a partition's values are
 *   consecutive. True for type 0 (section 8.6.2), where a partition of `n`
 *   values is read as `n / dimensions` vectors and the `i`-th element of
 *   the `v`-th goes to position `i * (n / dimensions) + v`: the vectors
 *   are laid across the partition, not end to end.
 * @return false if the packet ran out or the bits matched no codeword,
 *   which for the last packet of a stream is an ordinary end and for any
 *   other packet is a corrupt one. The caller decides which.
 */
static bool read_vector(const VORBIS_Codebook * book, VORBIS_Bits * bits,
    int32_t * into, uint32_t count, bool interleave) {
  if (book->dimensions == 0) {
    return false; /* Would not advance; refused at parse, belt and braces. */
  }
  if (interleave) {
    uint32_t step = count / book->dimensions;
    for (uint32_t v = 0; v < step; ++v) {
      uint32_t entry = gaud_vorbis_codebook_decode(book, bits);
      if (entry == UINT32_MAX) {
        return false;
      }
      const int32_t * values
          = book->values + (size_t)entry * book->dimensions;
      for (uint32_t j = 0; j < book->dimensions; ++j) {
        uint32_t index = v + j * step;
        into[index] = add_sat(into[index], values[j]);
      }
    }
    return true;
  }
  for (uint32_t at = 0; at < count; at += book->dimensions) {
    uint32_t entry = gaud_vorbis_codebook_decode(book, bits);
    if (entry == UINT32_MAX) {
      return false;
    }
    const int32_t * values
        = book->values + (size_t)entry * book->dimensions;
    for (uint32_t j = 0; j < book->dimensions; ++j) {
      uint32_t index = at + j;
      if (index >= count) {
        /* A codebook whose dimension does not divide the partition. The
         * specification leaves the remainder undefined rather than
         * forbidding it, so the values past the end are dropped. */
        continue;
      }
      into[index] = add_sat(into[index], values[j]);
    }
  }
  return true;
}

GAUD_Result gaud_vorbis_residue_decode(const VORBIS_Residue * residue,
    const VORBIS_Setup * setup, VORBIS_Bits * bits, uint32_t channels,
    uint32_t lines, const bool * wanted, int32_t * vectors) {
  /*
   * **Type 2 is read as one vector of every channel's lines**, which is
   * what makes it a different type rather than a different setting: the
   * partition arithmetic below is over `channels * lines` values for
   * type 2 and over `lines` for the others, and the scatter at the end
   * is what puts them back.
   */
  bool interleaved = residue->type == 2u;
  uint32_t vector_lines = interleaved ? channels * lines : lines;
  uint32_t begin = residue->begin;
  uint32_t end = residue->end;
  if (end > vector_lines) {
    end = vector_lines;
  }
  if (begin >= end) {
    return GAUD_OK; /* Nothing in range for this block size. */
  }

  bool any = false;
  for (uint32_t ch = 0; ch < channels; ++ch) {
    if (wanted[ch]) {
      any = true;
    }
  }
  if (!any) {
    return GAUD_OK;
  }

  uint32_t partitions = (end - begin) / residue->partition_size;
  if (partitions == 0) {
    return GAUD_OK;
  }
  if (partitions > VORBIS_MAX_PARTITIONS) {
    return GAUD_ERR_CORRUPT;
  }
  const VORBIS_Codebook * classbook = &setup->codebooks[residue->classbook];
  if (classbook->dimensions == 0) {
    return GAUD_ERR_CORRUPT;
  }

  /*
   * One class number per partition per channel - or per partition alone
   * for type 2, which has one interleaved vector. Read in the first pass
   * only: the numbers are packed several to a codebook entry, lowest
   * digit first in base `classifications`, so they cannot be read per
   * pass even in principle.
   */
  uint32_t vectors_to_read = interleaved ? 1u : channels;
  if (vectors_to_read > VORBIS_MAX_RESIDUE_VECTORS
      || (uint64_t)partitions * vectors_to_read > VORBIS_MAX_PARTITIONS) {
    return GAUD_ERR_CORRUPT;
  }
  unsigned char classes[VORBIS_MAX_PARTITIONS];

  for (unsigned pass = 0; pass < residue->passes; ++pass) {
    uint32_t at = begin;
    uint32_t partition = 0;
    while (partition < partitions) {
      if (pass == 0) {
        for (uint32_t which = 0; which < vectors_to_read; ++which) {
          if (!interleaved && !wanted[which]) {
            continue;
          }
          uint32_t entry = gaud_vorbis_codebook_decode(classbook, bits);
          if (entry == UINT32_MAX) {
            return GAUD_ERR_CORRUPT;
          }
          /* The entry is `dimensions` class numbers in base
           * `classifications`, and the specification reads them out
           * highest-digit-first into increasing partitions - the
           * opposite of a lattice codebook's digit order, which is one
           * of the two places this format reverses itself. */
          uint32_t remaining = entry;
          uint32_t divisor = 1;
          for (uint32_t j = 1; j < classbook->dimensions; ++j) {
            divisor *= residue->classifications;
          }
          for (uint32_t j = 0; j < classbook->dimensions; ++j) {
            uint32_t index = partition + j;
            uint32_t value
                = divisor ? (remaining / divisor) % residue->classifications
                          : 0;
            if (divisor) {
              remaining %= divisor;
              divisor /= residue->classifications;
            }
            if (index < partitions) {
              classes[index * vectors_to_read + which]
                  = (unsigned char)value;
            }
          }
        }
      }
      /* One group of `classbook->dimensions` partitions per classbook
       * entry, which is why the loop advances by that and not by one. */
      for (uint32_t j = 0; j < classbook->dimensions
          && partition < partitions;
          ++j, ++partition, at += residue->partition_size) {
        for (uint32_t which = 0; which < vectors_to_read; ++which) {
          if (!interleaved && !wanted[which]) {
            continue;
          }
          unsigned char class_number
              = classes[partition * vectors_to_read + which];
          if (class_number >= residue->classifications) {
            return GAUD_ERR_CORRUPT;
          }
          int16_t book = residue->book[class_number][pass];
          if (book < 0) {
            continue; /* This class codes nothing in this pass. */
          }
          const VORBIS_Codebook * one = &setup->codebooks[book];
          int32_t * into;
          int32_t scratch[256];
          if (interleaved) {
            /*
             * Read into a scratch buffer and scatter, because the
             * interleaved vector's line `k` belongs to channel
             * `k % channels` - so consecutive values of one partition
             * go to different channels.
             */
            if (residue->partition_size > sizeof(scratch)
                / sizeof(scratch[0])) {
              return GAUD_ERR_CORRUPT;
            }
            memset(scratch, 0, residue->partition_size * sizeof(*scratch));
            into = scratch;
          }
          else {
            into = vectors + (size_t)which * lines + at;
          }
          if (!read_vector(one, bits, into, residue->partition_size,
                  residue->type == 0u)) {
            return GAUD_ERR_CORRUPT;
          }
          if (interleaved) {
            for (uint32_t k = 0; k < residue->partition_size; ++k) {
              uint32_t absolute = at + k;
              uint32_t channel = absolute % channels;
              uint32_t line = absolute / channels;
              if (!wanted[channel] || line >= lines) {
                continue;
              }
              int32_t * slot = vectors + (size_t)channel * lines + line;
              *slot = add_sat(*slot, scratch[k]);
            }
          }
        }
      }
    }
  }
  return GAUD_OK;
}
