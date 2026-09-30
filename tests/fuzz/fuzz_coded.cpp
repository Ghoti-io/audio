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
 * Fuzz the coded-block layer directly, below any container.
 *
 * `fuzz_wav` and `fuzz_aiff` reach these decoders too, but only through a
 * header the fuzzer must first stumble into making valid - so almost every
 * input dies at the chunk walk and the nibble loops are barely exercised.
 * This harness hands the bytes straight to gaud_coded_decode_block() with a
 * geometry built from the first few, which is the only way to get real
 * coverage of the arithmetic.
 *
 * What it asserts, beyond not crashing:
 *
 * - **The output buffer is exactly `block_frames * channels` samples and is
 *   allocated at exactly that size**, so an over-write is a heap overflow
 *   ASan reports rather than a quiet corruption of whatever followed. This
 *   is the invariant the whole layer rests on: every container sizes its
 *   cache from the geometry, so a decoder that writes one frame more than
 *   the geometry promised corrupts memory in every caller at once.
 * - A decode reports no more frames than the geometry allows, and no more
 *   than gaud_coded_tail_frames() predicted for that many bytes. Those two
 *   numbers are computed by different code and a container believes the
 *   second before the first has run.
 * - A geometry the builder refused is never handed to a decoder.
 * - Encoding then decoding is stable: the second encode of a decoded block
 *   produces the same bytes as the first. ADPCM is lossy, so the samples do
 *   not survive, but the *encoder* is a deterministic function of its input
 *   and a difference means state leaking between blocks.
 *
 * Build with: make fuzz-coded
 * Run:        make fuzz-run-coded FUZZ_TIME=300
 */

#include "../../src/codec/shared/adpcm.h"
#include <ghoti.io/audio/audio.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size);

namespace {

const GAUD_Sample_Coding CODINGS[] = {
    GAUD_CODING_G711_ULAW,
    GAUD_CODING_G711_ALAW,
    GAUD_CODING_ADPCM_IMA_WAV,
    GAUD_CODING_ADPCM_IMA_QT,
    GAUD_CODING_ADPCM_MS,
};

} // namespace

int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 4) {
    return 0;
  }
  /* Three harness bytes, not the block's: they choose the geometry, which
   * a fuzzer would otherwise almost never produce a valid one of. */
  GAUD_Sample_Coding coding = CODINGS[data[0] % 5u];
  uint32_t channels = (uint32_t)(data[1] % 68u); /* 0 and >64 both tested */
  uint32_t block_bytes = (uint32_t)data[2] * 16u;
  data += 3;
  size -= 3;

  GAUD_Coded_Geometry geometry;
  if (gaud_coded_geometry(coding, channels, block_bytes, 0, &geometry)
      != GAUD_OK) {
    /* A refused geometry must not be usable. Nothing to check but that we
     * do not go on to use it, which is what this early return is. */
    return 0;
  }

  /* Exactly the promised size, so ASan is the bounds check. A slack
   * allocation here would hide the one defect this harness exists to
   * find. */
  size_t samples = (size_t)geometry.block_frames * geometry.channels;
  if (samples == 0 || samples > (16u << 20)) {
    return 0;
  }
  int16_t * out = new (std::nothrow) int16_t[samples];
  if (!out) {
    return 0;
  }

  size_t got = gaud_coded_decode_block(&geometry, data, size, out);

  if (got > geometry.block_frames) {
    __builtin_trap(); /* Reported more frames than the buffer holds. */
  }
  size_t predicted = gaud_coded_tail_frames(
      &geometry, size < geometry.block_bytes ? size : geometry.block_bytes);
  if (got > predicted) {
    /* A container states its track length from the prediction and then
     * decodes with the other function. The prediction being an
     * underestimate is how a decoder writes past a caller's buffer. */
    __builtin_trap();
  }

  if (got > 0) {
    unsigned char * first = new (std::nothrow)
        unsigned char[geometry.block_bytes];
    unsigned char * second = new (std::nothrow)
        unsigned char[geometry.block_bytes];
    if (first && second) {
      memset(first, 0, geometry.block_bytes);
      memset(second, 0, geometry.block_bytes);
      size_t a = gaud_coded_encode_block(&geometry, out, got, first);
      size_t b = gaud_coded_encode_block(&geometry, out, got, second);
      if (a != b || memcmp(first, second, geometry.block_bytes) != 0) {
        /* The encoder is a pure function of the block it is given. If two
         * calls differ, something survived between them. */
        __builtin_trap();
      }
    }
    delete[] first;
    delete[] second;
  }

  delete[] out;
  return 0;
}
