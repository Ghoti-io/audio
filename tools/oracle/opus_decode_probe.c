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
 * Decode one of RFC 6716's conformance vectors with this library's own
 * Opus decoder and write what comes out. Never installed.
 *
 * The vectors are `opus_demo`'s container - a 32-bit big-endian packet
 * length, a 32-bit big-endian final range decoder state, then the
 * packet - and the reference decodes them to 16-bit little-endian
 * interleaved samples at 48 kHz. This does the same, so that the two
 * outputs can be compared byte for byte and, as section 6 requires,
 * with `opus_compare`. It also checks, packet by packet, the one thing
 * section 6 insists on that a sample comparison cannot see: that the
 * decoder ended on exactly the range state the vector says.
 *
 * Output on stdout, one `name value` per line: capabilities (what the
 * registered codec claims, so that the gate can tell a decoder that
 * was withdrawn from one that was never asked), packets, compared (the
 * packets whose final range state was checked - every one but the
 * one-byte ones the format treats as a loss), range_wrong, refused,
 * samples.
 */

#include "../../src/codec/opus/opus_decoder.h"
#include <ghoti.io/audio/audio.h>
#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/audio/codecs.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const unsigned char * at) {
  return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16)
      | ((uint32_t)at[2] << 8) | at[3];
}

int main(int argc, char ** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: opus_decode_probe <in.bit> <out.pcm> [channels]\n");
    return 2;
  }
  uint32_t channels = argc > 3 ? (uint32_t)atoi(argv[3]) : 2u;
  gaud_register_builtin_codecs();
  const GAUD_Codec * codec = gaud_registry_find(NULL, "opus");
  printf("capabilities %s\n",
      codec && (codec->capabilities & GAUD_CAP_DECODE) ? "decode"
                                                       : "metadata-only");
  FILE * in = fopen(argv[1], "rb");
  FILE * out = fopen(argv[2], "wb");
  if (!in || !out) {
    fprintf(stderr, "opus_decode_probe: cannot open files\n");
    return 2;
  }
  fseek(in, 0, SEEK_END);
  long size = ftell(in);
  fseek(in, 0, SEEK_SET);
  unsigned char * data = malloc((size_t)size + 1u);
  if (!data || fread(data, 1, (size_t)size, in) != (size_t)size) {
    return 2;
  }
  OPUS_Decoder * decoder = NULL;
  if (gaud_opus_decoder_create(NULL, channels, &decoder) != GAUD_OK) {
    return 2;
  }
  int16_t * pcm = malloc((size_t)OPUS_MAX_PACKET_SAMPLES * channels * 2u);
  size_t at = 0;
  uint64_t packets = 0;
  uint64_t wrong = 0;
  uint64_t compared = 0;
  uint64_t refused = 0;
  uint64_t samples = 0;
  while (at + 8u <= (size_t)size) {
    uint32_t length = be32(data + at);
    uint32_t want = be32(data + at + 4u);
    at += 8u;
    if (length > (size_t)size - at) {
      break;
    }
    uint32_t got = 0;
    GAUD_Result result = gaud_opus_decode_packet(decoder, data + at, length,
        pcm, OPUS_MAX_PACKET_SAMPLES, 0, &got);
    if (result != GAUD_OK) {
      ++refused;
    } else {
      if (length > 1u) {
        ++compared;
        if (decoder->range_final != want) {
          ++wrong;
        }
      }
      fwrite(pcm, 2u * channels, got, out);
      samples += got;
    }
    at += length;
    ++packets;
  }
  printf("packets %" PRIu64 "\ncompared %" PRIu64 "\nrange_wrong %" PRIu64
         "\nrefused %" PRIu64 "\nsamples %" PRIu64 "\n",
      packets, compared, wrong, refused, samples);
  gaud_opus_decoder_destroy(NULL, decoder);
  free(pcm);
  free(data);
  fclose(in);
  fclose(out);
  return 0;
}
