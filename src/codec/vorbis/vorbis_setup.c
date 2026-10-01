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
 * The setup header: codebooks, floors, residues, mappings and modes.
 *
 * **This is where Vorbis differs most from every format already here.**
 * An MP3 decoder's tables are in a standard and were extracted from one
 * (planning/audio.md section 11.20). A Vorbis stream states its own: how
 * the spectrum is partitioned, which codebook codes each partition, what
 * shape the spectral envelope takes, which channels are coupled, and how
 * many samples a packet covers are all read out of this one packet. So
 * there is nothing here to calibrate against a document, and a defect in
 * this file is a stream that decodes to noise rather than one that
 * decodes slightly wrong.
 *
 * Which also means every structure in it is something an *encoder* chose.
 * A corpus therefore samples encoders and not the format - more sharply
 * than a corpus of MP3s does, because an MP3's tables at least are fixed.
 * notes/audio/vorbis.md names what no encoder in the oracle image
 * produces at any setting, which is floor type 0 and residue type 0.
 *
 * **Everything a later packet will index is range-checked here.** A
 * mapping names a floor and a residue by number; a residue names
 * codebooks; a floor names codebooks; a mode names a mapping. Checking
 * those once, at open, is the difference between a decoder that refuses a
 * malformed stream and one that indexes an array with a number out of a
 * file - and the audio path then has no range checks in its inner loops,
 * which is the other half of why it is done here.
 */

#include "vorbis_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/** Read a list of floor configurations. */
static GAUD_Result parse_floor(
    VORBIS_Bits * bits, const VORBIS_Setup * setup, VORBIS_Floor * out) {
  memset(out, 0, sizeof(*out));
  out->type = (unsigned)gaud_vorbis_bits_read(bits, 16);
  if (out->type == 0) {
    VORBIS_Floor0 * floor0 = &out->u.zero;
    floor0->order = gaud_vorbis_bits_read(bits, 8);
    floor0->rate = gaud_vorbis_bits_read(bits, 16);
    floor0->bark_map_size = gaud_vorbis_bits_read(bits, 16);
    floor0->amplitude_bits = gaud_vorbis_bits_read(bits, 6);
    floor0->amplitude_offset = gaud_vorbis_bits_read(bits, 8);
    floor0->book_count = gaud_vorbis_bits_read(bits, 4) + 1u;
    for (uint32_t i = 0; i < floor0->book_count; ++i) {
      uint32_t book = gaud_vorbis_bits_read(bits, 8);
      if (book >= setup->codebook_count) {
        return GAUD_ERR_CORRUPT;
      }
      floor0->books[i] = (unsigned char)book;
    }
    if (bits->past_end || floor0->order == 0 || floor0->bark_map_size == 0
        || floor0->amplitude_bits == 0 || floor0->amplitude_bits > 32u) {
      return GAUD_ERR_CORRUPT;
    }
    return GAUD_OK;
  }
  if (out->type != 1u) {
    /* The format reserves every other type and says a decoder must
     * refuse the stream. There is nothing to guess: a floor of an
     * unknown type has an unknown length in the packet, so the bits
     * after it cannot be found. */
    return GAUD_ERR_CORRUPT;
  }

  VORBIS_Floor1 * floor1 = &out->u.one;
  floor1->partitions = gaud_vorbis_bits_read(bits, 5);
  unsigned maximum_class = 0;
  for (uint32_t i = 0; i < floor1->partitions; ++i) {
    uint32_t class_number = gaud_vorbis_bits_read(bits, 4);
    if (class_number >= VORBIS_FLOOR1_CLASSES) {
      return GAUD_ERR_CORRUPT;
    }
    floor1->partition_class[i] = (unsigned char)class_number;
    if (class_number > maximum_class) {
      maximum_class = (unsigned)class_number;
    }
  }
  for (unsigned i = 0; i <= maximum_class && floor1->partitions; ++i) {
    floor1->class_dimensions[i]
        = (unsigned char)(gaud_vorbis_bits_read(bits, 3) + 1u);
    floor1->class_subclasses[i]
        = (unsigned char)gaud_vorbis_bits_read(bits, 2);
    if (floor1->class_subclasses[i]) {
      uint32_t book = gaud_vorbis_bits_read(bits, 8);
      if (book >= setup->codebook_count) {
        return GAUD_ERR_CORRUPT;
      }
      floor1->class_masterbook[i] = (unsigned char)book;
    }
    unsigned subclasses = 1u << floor1->class_subclasses[i];
    for (unsigned j = 0; j < subclasses; ++j) {
      /* **Minus one, and the bias is load-bearing.** The field is eight
       * bits and the value stored is one less, so zero means "this
       * subclass codes nothing" rather than "codebook zero". A reader
       * that skipped the subtraction would use codebook 0 everywhere a
       * subclass was meant to be silent. */
      int32_t book = (int32_t)gaud_vorbis_bits_read(bits, 8) - 1;
      if (book >= (int32_t)setup->codebook_count) {
        return GAUD_ERR_CORRUPT;
      }
      floor1->subclass_book[i][j] = (int16_t)book;
    }
  }
  floor1->multiplier = gaud_vorbis_bits_read(bits, 2) + 1u;
  unsigned rangebits = (unsigned)gaud_vorbis_bits_read(bits, 4);

  floor1->x_list[0] = 0;
  floor1->x_list[1] = 1u << rangebits;
  floor1->values = 2;
  for (uint32_t i = 0; i < floor1->partitions; ++i) {
    unsigned class_number = floor1->partition_class[i];
    for (unsigned j = 0; j < floor1->class_dimensions[class_number]; ++j) {
      if (floor1->values >= VORBIS_FLOOR1_VALUES) {
        /* 31 partitions of 8 dimensions is 248 values and the format
         * permits it to be stated; the specification caps the total at
         * 65 including the two implied, and a stream above that is
         * refused rather than truncated. */
        return GAUD_ERR_CORRUPT;
      }
      floor1->x_list[floor1->values++] = gaud_vorbis_bits_read(
          bits, rangebits);
    }
  }
  if (bits->past_end) {
    return GAUD_ERR_CORRUPT;
  }

  /*
   * The X positions in increasing order, for rendering.
   *
   * Sorted once here because rendering walks the curve in X order while
   * decoding fills it in the order the stream states them, and sorting
   * per packet would sit in the decoder's inner loop.
   *
   * **A repeated X position is refused.** Two control points at one X
   * have no slope between them, and the specification requires the
   * positions to be unique; a renderer handed a duplicate divides by a
   * run of zero. Insertion sort, because 65 is the most there can be.
   */
  for (uint32_t i = 0; i < floor1->values; ++i) {
    floor1->sorted[i] = i;
  }
  for (uint32_t i = 1; i < floor1->values; ++i) {
    uint32_t index = floor1->sorted[i];
    uint32_t key = floor1->x_list[index];
    uint32_t j = i;
    while (j > 0 && floor1->x_list[floor1->sorted[j - 1u]] > key) {
      floor1->sorted[j] = floor1->sorted[j - 1u];
      --j;
    }
    floor1->sorted[j] = index;
  }
  for (uint32_t i = 1; i < floor1->values; ++i) {
    if (floor1->x_list[floor1->sorted[i]]
        == floor1->x_list[floor1->sorted[i - 1u]]) {
      return GAUD_ERR_CORRUPT;
    }
  }
  return GAUD_OK;
}

/** Read one residue configuration; the three types share a layout. */
static GAUD_Result parse_residue(
    VORBIS_Bits * bits, const VORBIS_Setup * setup, VORBIS_Residue * out) {
  memset(out, 0, sizeof(*out));
  out->type = (unsigned)gaud_vorbis_bits_read(bits, 16);
  if (out->type > 2u) {
    return GAUD_ERR_CORRUPT;
  }
  out->begin = gaud_vorbis_bits_read(bits, 24);
  out->end = gaud_vorbis_bits_read(bits, 24);
  out->partition_size = gaud_vorbis_bits_read(bits, 24) + 1u;
  out->classifications = gaud_vorbis_bits_read(bits, 6) + 1u;
  uint32_t classbook = gaud_vorbis_bits_read(bits, 8);
  if (bits->past_end || classbook >= setup->codebook_count
      || out->begin > out->end
      || out->classifications > VORBIS_RESIDUE_CLASSES) {
    return GAUD_ERR_CORRUPT;
  }
  out->classbook = (unsigned char)classbook;

  /*
   * The cascade: for each classification, which of eight passes code
   * anything. Three bits, then a flag, then five more - so a
   * classification that uses only the first three passes costs four bits
   * and one that uses all eight costs nine.
   */
  unsigned char cascade[VORBIS_RESIDUE_CLASSES];
  for (uint32_t i = 0; i < out->classifications; ++i) {
    unsigned low = (unsigned)gaud_vorbis_bits_read(bits, 3);
    unsigned high = 0;
    if (gaud_vorbis_bits_read(bits, 1)) {
      high = (unsigned)gaud_vorbis_bits_read(bits, 5);
    }
    cascade[i] = (unsigned char)(low | (high << 3));
  }
  out->passes = 0;
  for (uint32_t i = 0; i < out->classifications; ++i) {
    for (unsigned pass = 0; pass < 8u; ++pass) {
      if ((cascade[i] & (1u << pass)) == 0) {
        out->book[i][pass] = -1;
        continue;
      }
      uint32_t book = gaud_vorbis_bits_read(bits, 8);
      if (book >= setup->codebook_count) {
        return GAUD_ERR_CORRUPT;
      }
      /* A residue book must have a lookup: its entries stand for
       * vectors, not for numbers. A stream naming a book with none is
       * refused here rather than producing a null dereference where the
       * vector would be read. */
      if (setup->codebooks[book].lookup_type == 0
          || !setup->codebooks[book].values) {
        return GAUD_ERR_CORRUPT;
      }
      out->book[i][pass] = (int16_t)book;
      if (pass + 1u > out->passes) {
        out->passes = pass + 1u;
      }
    }
  }
  /* The classbook's entries are read as a number of classifications in
   * base `classifications`, one digit per dimension, so a classbook
   * whose entries cannot cover that is a stream this refuses. */
  const VORBIS_Codebook * book = &setup->codebooks[out->classbook];
  if (book->dimensions == 0) {
    return GAUD_ERR_CORRUPT;
  }
  uint64_t reach = 1;
  for (uint32_t i = 0; i < book->dimensions; ++i) {
    reach *= out->classifications;
    if (reach > book->entries) {
      break;
    }
  }
  if (reach > book->entries) {
    return GAUD_ERR_CORRUPT;
  }
  return bits->past_end ? GAUD_ERR_CORRUPT : GAUD_OK;
}

/** Read one mapping: which floor and residue each channel uses. */
static GAUD_Result parse_mapping(VORBIS_Bits * bits, uint32_t channels,
    const VORBIS_Setup * setup, VORBIS_Mapping * out) {
  memset(out, 0, sizeof(*out));
  if (gaud_vorbis_bits_read(bits, 16) != 0) {
    return GAUD_ERR_CORRUPT; /* Only type 0 exists. */
  }
  out->submaps = gaud_vorbis_bits_read(bits, 1)
      ? gaud_vorbis_bits_read(bits, 4) + 1u
      : 1u;
  if (out->submaps > VORBIS_MAX_SUBMAPS) {
    return GAUD_ERR_CORRUPT;
  }
  if (gaud_vorbis_bits_read(bits, 1)) {
    out->coupling_steps = gaud_vorbis_bits_read(bits, 8) + 1u;
    unsigned width = gaud_vorbis_ilog(channels - 1u);
    for (uint32_t i = 0; i < out->coupling_steps; ++i) {
      uint32_t magnitude = gaud_vorbis_bits_read(bits, width);
      uint32_t angle = gaud_vorbis_bits_read(bits, width);
      /* **Both must name a channel and they must differ.** A step
       * couples two channels into a magnitude and an angle; a step whose
       * two channels are the same would read one vector as both halves
       * of a polar pair, and the inverse would then divide a value by
       * itself. The format forbids it and this is where that is
       * enforced, because the decoder's inverse coupling has no range
       * check in it. */
      if (magnitude == angle || magnitude >= channels || angle >= channels) {
        return GAUD_ERR_CORRUPT;
      }
      out->magnitude[i] = (unsigned char)magnitude;
      out->angle[i] = (unsigned char)angle;
    }
  }
  if (gaud_vorbis_bits_read(bits, 2) != 0) {
    return GAUD_ERR_CORRUPT; /* Reserved, and required to be zero. */
  }

  out->mux = gcu_allocator_malloc(setup->allocator, channels);
  if (!out->mux) {
    return GAUD_ERR_OOM;
  }
  memset(out->mux, 0, channels);
  if (out->submaps > 1u) {
    for (uint32_t i = 0; i < channels; ++i) {
      uint32_t which = gaud_vorbis_bits_read(bits, 4);
      if (which >= out->submaps) {
        return GAUD_ERR_CORRUPT;
      }
      out->mux[i] = (unsigned char)which;
    }
  }
  for (uint32_t i = 0; i < out->submaps; ++i) {
    /* Eight bits of nothing: the "time" configuration, which the format
     * reserved and never used. Read past rather than skipped silently,
     * because its width is what the fields after it depend on. */
    (void)gaud_vorbis_bits_read(bits, 8);
    uint32_t floor_number = gaud_vorbis_bits_read(bits, 8);
    uint32_t residue_number = gaud_vorbis_bits_read(bits, 8);
    if (floor_number >= setup->floor_count
        || residue_number >= setup->residue_count) {
      return GAUD_ERR_CORRUPT;
    }
    out->floor[i] = (unsigned char)floor_number;
    out->residue[i] = (unsigned char)residue_number;
  }
  return bits->past_end ? GAUD_ERR_CORRUPT : GAUD_OK;
}

void gaud_vorbis_setup_free(VORBIS_Setup * setup) {
  /*
   * **No guard on `allocator`, and the first draft had one.** It read
   * `if (!setup->allocator) return;`, on the reasoning that a setup
   * nobody had filled in has nothing to free - and NULL is not "no
   * allocator", it is *the default allocator*, which is what
   * gaud_stream_allocator() returns for a stream opened without one. So
   * the guard made this function a no-op for every document the library
   * actually opens, and every Vorbis file leaked its whole setup: 12.8 MB
   * in 8,952 allocations across `make test`.
   *
   * LeakSanitizer in the fuzz build found it in thirteen executions. The
   * general shape is worth keeping: a pointer field cannot double as an
   * "is this initialised" flag when NULL is a meaningful value for it.
   * Freeing a zeroed struct is safe without the guard - the counts are
   * zero so the loops do not run, and freeing a null pointer is defined.
   */
  const GAUD_Allocator * allocator = setup->allocator;
  for (uint32_t i = 0; i < setup->codebook_count; ++i) {
    gaud_vorbis_codebook_free(allocator, &setup->codebooks[i]);
  }
  gcu_allocator_free(allocator, setup->codebooks);
  gcu_allocator_free(allocator, setup->floors);
  gcu_allocator_free(allocator, setup->residues);
  for (uint32_t i = 0; i < setup->mapping_count; ++i) {
    gcu_allocator_free(allocator, setup->mappings[i].mux);
  }
  gcu_allocator_free(allocator, setup->mappings);
  memset(setup, 0, sizeof(*setup));
}

GAUD_Result gaud_vorbis_parse_setup(const unsigned char * data, size_t size,
    uint32_t channels, const GAUD_Limits * limits, VORBIS_Setup * out) {
  (void)limits;
  if (!gaud_vorbis_is_header(data, size, VORBIS_PACKET_SETUP)) {
    return GAUD_ERR_CORRUPT;
  }
  const GAUD_Allocator * allocator = out->allocator;
  VORBIS_Bits bits;
  gaud_vorbis_bits_init(
      &bits, data + VORBIS_HEAD_SIZE, size - VORBIS_HEAD_SIZE);

  out->codebook_count = gaud_vorbis_bits_read(&bits, 8) + 1u;
  out->codebooks = gcu_allocator_malloc(
      allocator, out->codebook_count * sizeof(*out->codebooks));
  if (!out->codebooks) {
    return GAUD_ERR_OOM;
  }
  memset(out->codebooks, 0,
      out->codebook_count * sizeof(*out->codebooks));
  for (uint32_t i = 0; i < out->codebook_count; ++i) {
    GAUD_Result result
        = gaud_vorbis_parse_codebook(&bits, allocator, &out->codebooks[i]);
    if (result != GAUD_OK) {
      return result;
    }
  }

  /*
   * The time domain transform list, which is sixteen bits of zero per
   * entry and nothing else. Vorbis I reserved it for a transform that was
   * never specified, and requires the value to be zero - so this is not a
   * field being skipped, it is a field being checked.
   */
  uint32_t times = gaud_vorbis_bits_read(&bits, 6) + 1u;
  for (uint32_t i = 0; i < times; ++i) {
    if (gaud_vorbis_bits_read(&bits, 16) != 0) {
      return GAUD_ERR_CORRUPT;
    }
  }

  out->floor_count = gaud_vorbis_bits_read(&bits, 6) + 1u;
  out->floors = gcu_allocator_malloc(
      allocator, out->floor_count * sizeof(*out->floors));
  if (!out->floors) {
    return GAUD_ERR_OOM;
  }
  memset(out->floors, 0, out->floor_count * sizeof(*out->floors));
  for (uint32_t i = 0; i < out->floor_count; ++i) {
    GAUD_Result result = parse_floor(&bits, out, &out->floors[i]);
    if (result != GAUD_OK) {
      return result;
    }
  }

  out->residue_count = gaud_vorbis_bits_read(&bits, 6) + 1u;
  out->residues = gcu_allocator_malloc(
      allocator, out->residue_count * sizeof(*out->residues));
  if (!out->residues) {
    return GAUD_ERR_OOM;
  }
  memset(out->residues, 0, out->residue_count * sizeof(*out->residues));
  for (uint32_t i = 0; i < out->residue_count; ++i) {
    GAUD_Result result = parse_residue(&bits, out, &out->residues[i]);
    if (result != GAUD_OK) {
      return result;
    }
  }

  out->mapping_count = gaud_vorbis_bits_read(&bits, 6) + 1u;
  out->mappings = gcu_allocator_malloc(
      allocator, out->mapping_count * sizeof(*out->mappings));
  if (!out->mappings) {
    return GAUD_ERR_OOM;
  }
  memset(out->mappings, 0, out->mapping_count * sizeof(*out->mappings));
  for (uint32_t i = 0; i < out->mapping_count; ++i) {
    GAUD_Result result
        = parse_mapping(&bits, channels, out, &out->mappings[i]);
    if (result != GAUD_OK) {
      return result;
    }
  }

  out->mode_count = gaud_vorbis_bits_read(&bits, 6) + 1u;
  if (out->mode_count > VORBIS_MAX_MODES) {
    return GAUD_ERR_CORRUPT;
  }
  for (uint32_t i = 0; i < out->mode_count; ++i) {
    out->modes[i].block_flag = gaud_vorbis_bits_read(&bits, 1) != 0;
    if (gaud_vorbis_bits_read(&bits, 16) != 0
        || gaud_vorbis_bits_read(&bits, 16) != 0) {
      return GAUD_ERR_CORRUPT; /* Window and transform type, both zero. */
    }
    uint32_t mapping = gaud_vorbis_bits_read(&bits, 8);
    if (mapping >= out->mapping_count) {
      return GAUD_ERR_CORRUPT;
    }
    out->modes[i].mapping = (unsigned char)mapping;
  }
  /* **ilog and not a logarithm**, which is why this is computed here
   * rather than in the audio path: the two differ by one at every power
   * of two, so a stream with exactly two or exactly four modes is where a
   * decoder that used a logarithm reads the mode number with the wrong
   * width and everything after it misaligned. */
  out->mode_bits = gaud_vorbis_ilog(out->mode_count - 1u);

  if (gaud_vorbis_bits_read(&bits, 1) == 0 || bits.past_end) {
    /* The framing bit. Its only job is to be set, and a cleared one
     * means the packet ended at exactly the right place to look whole. */
    return GAUD_ERR_CORRUPT;
  }
  return GAUD_OK;
}
