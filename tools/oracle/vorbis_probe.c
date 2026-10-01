/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Audio.
 *
 * Ghoti.io Audio is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 */

/**
 * @file
 *
 * What a Vorbis stream's setup header actually contains, printed.
 *
 * **Not a test and not a differential.** No reference can be asked this:
 * the question is which *arms of this library's own parser* a corpus
 * reaches, and the answer is a property of the encoders that wrote the
 * fixtures. `make vorbis-coverage` is the instrument; this is the part of
 * it that has to be inside the library, because the setup header is
 * internal and nothing public exposes it.
 *
 * One line per codebook, floor, residue, mapping and mode, in a
 * tab-separated form the script reads, plus the extremes of the vector
 * values - which is what decided ::VORBIS_Q and is why this exists rather
 * than a comment claiming a range.
 */

#include "../../src/codec/vorbis/vorbis_internal.h"
#include <ghoti.io/audio/audio.h>
#include <ghoti.io/audio/codecs.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char ** argv) {
  gaud_register_builtin_codecs();
  if (argc < 2) {
    fprintf(stderr, "usage: vorbis_probe FILE...\n");
    return 2;
  }
  for (int a = 1; a < argc; ++a) {
    GAUD_Stream * stream = NULL;
    if (gaud_stream_create_file(argv[a], &stream) != GAUD_OK) {
      fprintf(stderr, "vorbis_probe: cannot open %s\n", argv[a]);
      return 1;
    }
    GAUD_Doc * doc = NULL;
    if (gaud_doc_load(NULL, stream, NULL, NULL, &doc) != GAUD_OK) {
      fprintf(stderr, "vorbis_probe: cannot load %s\n", argv[a]);
      gaud_stream_destroy(stream);
      return 1;
    }
    VORBIS_File * file = gaud_doc_private(doc);
    const VORBIS_Setup * setup = &file->setup;
    printf("file\t%s\n", argv[a]);
    printf("info\t%u\t%u\t%u\t%u\n", file->info.channels,
        file->info.sample_rate, file->info.blocksize_short,
        file->info.blocksize_long);

    for (uint32_t i = 0; i < setup->codebook_count; ++i) {
      const VORBIS_Codebook * book = &setup->codebooks[i];
      unsigned longest = 0;
      uint32_t unused = 0;
      for (uint32_t e = 0; e < book->entries; ++e) {
        if (book->lengths[e] > longest) {
          longest = book->lengths[e];
        }
        if (book->lengths[e] == 0) {
          ++unused;
        }
      }
      int64_t low = 0;
      int64_t high = 0;
      int64_t smallest = 0;
      if (book->values) {
        size_t count = (size_t)book->entries * book->dimensions;
        for (size_t k = 0; k < count; ++k) {
          int64_t value = book->values[k];
          if (value < low) {
            low = value;
          }
          if (value > high) {
            high = value;
          }
          int64_t magnitude = value < 0 ? -value : value;
          if (magnitude && (smallest == 0 || magnitude < smallest)) {
            smallest = magnitude;
          }
        }
      }
      printf("book\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%" PRId64 "\t%" PRId64
             "\t%" PRId64 "\n",
          i, book->dimensions, book->entries, book->lookup_type,
          book->sequence_p ? 1u : 0u, longest, unused, low, high, smallest);
    }
    for (uint32_t i = 0; i < setup->floor_count; ++i) {
      const VORBIS_Floor * one = &setup->floors[i];
      if (one->type == 1u) {
        printf("floor\t%u\t1\t%u\t%u\t%u\n", i, one->u.one.partitions,
            one->u.one.multiplier, one->u.one.values);
      }
      else {
        printf("floor\t%u\t0\t%u\t%u\t%u\n", i, one->u.zero.order,
            one->u.zero.book_count, one->u.zero.bark_map_size);
      }
    }
    for (uint32_t i = 0; i < setup->residue_count; ++i) {
      const VORBIS_Residue * residue = &setup->residues[i];
      printf("residue\t%u\t%u\t%u\t%u\t%u\t%u\t%u\n", i, residue->type,
          residue->begin, residue->end, residue->partition_size,
          residue->classifications, residue->passes);
    }
    for (uint32_t i = 0; i < setup->mapping_count; ++i) {
      const VORBIS_Mapping * mapping = &setup->mappings[i];
      printf("mapping\t%u\t%u\t%u\n", i, mapping->submaps,
          mapping->coupling_steps);
    }
    for (uint32_t i = 0; i < setup->mode_count; ++i) {
      printf("mode\t%u\t%u\t%u\n", i, setup->modes[i].block_flag ? 1u : 0u,
          setup->modes[i].mapping);
    }
    gaud_doc_destroy(doc);
    gaud_stream_destroy(stream);
  }
  return 0;
}
