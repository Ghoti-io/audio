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
 * Reading RFC 6716's conformance vectors with this library's Opus code.
 *
 * The vectors are not Ogg files and cannot be opened through the public
 * API. They are `opus_demo`'s own container: for each packet, a 32-bit
 * big-endian length, then a 32-bit big-endian **final range decoder
 * state**, then the packet. That second field is the reason this file
 * exists.
 *
 * RFC 6716 section 6 requires two things of a compliant decoder. Its
 * output must pass `opus_compare` against the reference output, and - a
 * separate and far stricter requirement - it "MUST have the same final
 * range decoder state as that of the reference decoder". The vectors
 * carry that state for every packet, so the second requirement is
 * checkable per packet, by a number, with no tolerance to argue about.
 * Twenty thousand packets each either match or do not.
 *
 * That makes this the strongest gate in the library. Every other decode
 * differential here compares samples and has to choose a threshold,
 * because a reference may round differently. A range state is an exact
 * equality over the whole sequence of symbols the frame contained, so a
 * single misread probability anywhere in the frame changes it. It cannot
 * be passed by a decoder that is approximately right.
 *
 * This prints one line per packet and nothing else interprets it; the
 * comparison lives in check_opus_vectors.py, because a differential
 * needs this library's answer to leave the process.
 */

#include "../../src/codec/opus/opus_celt.h"
#include "../../src/codec/opus/opus_internal.h"
#include <ghoti.io/audio/audio.h>
#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/audio/codecs.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** One big-endian 32-bit field of the vector's framing. */
static uint32_t be32(const unsigned char * at) {
  return ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16)
      | ((uint32_t)at[2] << 8) | (uint32_t)at[3];
}

/** Read a whole file, or return NULL. */
static unsigned char * slurp(const char * path, size_t * out_size) {
  FILE * file = fopen(path, "rb");
  if (!file) {
    return NULL;
  }
  if (fseek(file, 0, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  long end = ftell(file);
  if (end < 0 || fseek(file, 0, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  unsigned char * data = malloc((size_t)end ? (size_t)end : 1u);
  if (!data) {
    fclose(file);
    return NULL;
  }
  if (fread(data, 1, (size_t)end, file) != (size_t)end) {
    free(data);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *out_size = (size_t)end;
  return data;
}

int main(int argc, char ** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: opus_probe <testvector.bit>...\n");
    return 2;
  }
  /*
   * What the codec claims it can do, printed so the gate can tell the
   * difference between "there is no decoder yet" and "there is one and
   * it answered nothing". Without this the gate would pass forever on a
   * decoder that had silently stopped working.
   */
  gaud_register_builtin_codecs();
  const GAUD_Codec * codec = gaud_registry_find(NULL, "opus");
  printf("capabilities\t%s\n",
      codec && (codec->capabilities & GAUD_CAP_DECODE) ? "decode"
                                                       : "metadata-only");

  CELT_Decoder * decoder = malloc(sizeof *decoder);
  CELT_Scratch * scratch = malloc(sizeof *scratch);
  int16_t * pcm = malloc(2u * 960u * sizeof *pcm);
  if (!decoder || !scratch || !pcm) {
    fprintf(stderr, "opus_probe: out of memory\n");
    return 2;
  }

  for (int arg = 1; arg < argc; ++arg) {
    bool seen_other_mode = false;
    size_t size = 0;
    unsigned char * data = slurp(argv[arg], &size);
    if (!data) {
      fprintf(stderr, "opus_probe: cannot read %s\n", argv[arg]);
      return 2;
    }
    printf("file\t%s\n", argv[arg]);
    /*
     * Each vector is its own stream, so the CELT state restarts here.
     * It is NOT restarted per packet: the envelope, the overlap and the
     * post-filter's history all carry, and a decoder that forgot them
     * would still produce plausible audio and the wrong range state.
     */
    gaud_celt_decoder_init(decoder, 2u, 1);
    size_t at = 0;
    uint64_t index = 0;
    uint64_t samples = 0;
    while (at + 8u <= size) {
      uint32_t length = be32(data + at);
      uint32_t want_range = be32(data + at + 4u);
      at += 8u;
      if (length > size - at) {
        printf("truncated\t%" PRIu64 "\n", index);
        break;
      }
      OPUS_Packet packet;
      GAUD_Result result
          = gaud_opus_parse_packet(data + at, length, false, &packet);
      if (result != GAUD_OK) {
        printf("packet\t%" PRIu64 "\trefused\t%d\n", index, (int)result);
      } else {
        uint32_t duration = packet.count * packet.toc.frame_size;
        samples += duration;
        /*
         * `range` is the final range decoder state once every symbol of
         * every frame in the packet has been read, which is what the
         * vector states. CELT packets have it; the other two modes do
         * not yet, and say "none" rather than a zero that a gate could
         * mistake for an answer.
         *
         * A CELT packet in a stream that has carried SILK or hybrid
         * packets cannot match either, because the state those left is
         * missing - so the mode of every packet so far is tracked, and
         * such a packet reports "stale" rather than a failure. That
         * distinction is the difference between "not written yet" and
         * "written and wrong".
         */
        char range_text[32];
        if (packet.toc.mode != OPUS_MODE_CELT) {
          seen_other_mode = true;
          snprintf(range_text, sizeof range_text, "none");
        } else if (seen_other_mode) {
          snprintf(range_text, sizeof range_text, "stale");
        } else {
          unsigned lm = 0;
          while (lm < 3u && (120u << lm) < packet.toc.frame_size) {
            ++lm;
          }
          if ((120u << lm) != packet.toc.frame_size) {
            /* A SILK-only frame size; not reachable for a CELT TOC. */
            snprintf(range_text, sizeof range_text, "none");
          } else {
            /*
             * The band range is not a property of CELT but of the Opus
             * packet around it: the TOC's bandwidth decides where the
             * spectrum stops, and a decoder that always codes all 21
             * bands reads the right symbols only for a fullband packet.
             * src/opus_decoder.c in Appendix A is where this mapping
             * lives; leaving it out made 3,122 packets of one vector
             * disagree while two other vectors matched completely.
             */
            static const uint32_t kEndBand[5] = {13u, 17u, 17u, 19u, 21u};
            decoder->start = 0u;
            decoder->end = kEndBand[packet.toc.bandwidth < 5u
                    ? packet.toc.bandwidth
                    : 4u];
            for (uint32_t frame = 0; frame < packet.count; ++frame) {
              OPUS_Range range;
              gaud_opus_range_init(
                  &range, packet.frame[frame], packet.length[frame]);
              gaud_celt_decode_frame(decoder, &range, packet.length[frame],
                  packet.toc.stereo ? 2u : 1u, lm, pcm, scratch);
            }
            snprintf(range_text, sizeof range_text, "%" PRIu32, decoder->rng);
          }
        }
        printf("packet\t%" PRIu64 "\tconfig\t%u\tstereo\t%d\tframes\t%u"
               "\tsamples\t%u\twant_range\t%" PRIu32 "\trange\t%s\n",
            index, (unsigned)(data[at] >> 3),
            packet.toc.stereo ? 1 : 0, packet.count, duration, want_range,
            range_text);
      }
      at += length;
      ++index;
    }
    printf("total\t%" PRIu64 "\t%" PRIu64 "\n", index, samples);
    free(data);
  }
  free(decoder);
  free(scratch);
  free(pcm);
  return 0;
}
