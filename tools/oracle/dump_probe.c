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
 * Dump what this library reads from an audio file, for comparison.
 *
 *     dump_probe <file> [--pcm | --pcm-le]
 *
 * Without an option it prints one line of metadata. With `--pcm` the decoded
 * samples go to stdout as raw bytes in the **host's** order, which is what a
 * reference's dump of the same file can be compared against.
 *
 * `--pcm-le` writes them little-endian whatever the host is, and exists for
 * the cross-architecture gate. The distinction matters and is easy to get
 * wrong: this library decodes to host order deliberately, so a caller can do
 * arithmetic on the samples without swapping first. That means the raw bytes
 * of a decode are *supposed* to differ between a little-endian and a
 * big-endian machine, and a gate comparing them would report a difference on
 * every multi-byte fixture and be wrong to. What has to be identical across
 * architectures is the sample **values**; this option is how they are put in
 * one spelling so that a hash can compare them.
 *
 * **Neither a test nor an example**, which is why it is under tools/. A
 * differential needs this library's reading to leave the process so that
 * ffmpeg, sox and libsndfile can be asked the same question about the same
 * bytes, and a GoogleTest binary cannot be piped into a comparison. Built
 * like an example - the static archive whole - so the binary carries the code
 * under test rather than whatever happens to be installed.
 */

#include <ghoti.io/audio/audio.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

/* Decided from the bytes of a known value rather than from a macro: the
 * macros that answer this differ between compilers, and the wrong one
 * answers for the compiler rather than for the target. */
static bool host_is_little_endian(void) {
  const uint16_t one = 1;
  unsigned char bytes[sizeof(one)];
  memcpy(bytes, &one, sizeof(one));
  return bytes[0] == 1;
}

int main(int argc, char ** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: dump_probe <file> [--pcm]\n");
    return 2;
  }
  bool want_pcm = argc > 2
      && (strcmp(argv[2], "--pcm") == 0 || strcmp(argv[2], "--pcm-le") == 0);
  bool canonical = argc > 2 && strcmp(argv[2], "--pcm-le") == 0;

  gaud_register_builtin_codecs();

  GAUD_Stream * stream = NULL;
  if (gaud_stream_create_file(argv[1], &stream) != GAUD_OK) {
    fprintf(stderr, "cannot open %s\n", argv[1]);
    return 1;
  }

  GAUD_Diagnostics diagnostics = {0};
  GAUD_Doc * doc = NULL;
  GAUD_Result result
      = gaud_doc_load(NULL, stream, NULL, &diagnostics, &doc);
  if (result != GAUD_OK) {
    fprintf(stderr, "load failed: %s\n", gaud_result_string(result));
    gaud_stream_destroy(stream);
    return 1;
  }

  GAUD_Track * track = gaud_doc_track(doc, 0);
  GAUD_Channel_Layout layout = gaud_track_layout(track);

  if (!want_pcm) {
    printf("codec=%s format=%s coding=%s rate=%u channels=%u mask=0x%x "
           "frames=%llu duration=%.6f diagnostics=%zu\n",
        gaud_doc_codec_name(doc), gaud_sample_format_string(
            gaud_track_format(track)),
        gaud_sample_coding_name(gaud_track_coding(track)),
        gaud_track_sample_rate(track), layout.channels, layout.mask,
        (unsigned long long)gaud_track_frames(track),
        gaud_track_duration(track), diagnostics.count);
  }
  else {
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    GAUD_Decoder * decoder = NULL;
    if (gaud_decoder_create(track, &decoder) != GAUD_OK) {
      fprintf(stderr, "no decoder\n");
      gaud_doc_destroy(doc);
      gaud_stream_destroy(stream);
      return 1;
    }
    GAUD_Buffer * buffer = NULL;
    /* A block size that is not a divisor of any fixture's length, so that
     * the last read is always short and the short-read path is exercised by
     * every single comparison rather than by whichever fixture happened to
     * have an awkward length. */
    if (gaud_decoder_buffer_create(decoder, NULL, 1000, &buffer) != GAUD_OK) {
      gaud_decoder_destroy(decoder);
      gaud_doc_destroy(doc);
      gaud_stream_destroy(stream);
      return 1;
    }
    for (;;) {
      if (gaud_decoder_read(decoder, buffer) != GAUD_OK) {
        fprintf(stderr, "read failed\n");
        return 1;
      }
      size_t frames = gaud_buffer_frames(buffer);
      if (frames == 0) {
        break;
      }
      size_t bytes = gaud_buffer_bytes_used(buffer);
      (void)frames;
      const unsigned char * out = gaud_buffer_data_const(buffer);

      /* Put the samples in one spelling when asked, so that two machines of
       * different endianness can be compared at all. Done here rather than
       * in the gate because the width and the host's order are both known
       * here and neither is obvious from the other side. */
      static unsigned char swapped[65536];
      if (canonical && !host_is_little_endian()) {
        size_t width = gaud_sample_format_bits(gaud_buffer_format(buffer)) / 8u;
        if (width > 1 && bytes <= sizeof(swapped)) {
          memcpy(swapped, out, bytes);
          for (size_t i = 0; i + width <= bytes; i += width) {
            for (size_t a = 0, b = width - 1; a < b; ++a, --b) {
              unsigned char t = swapped[i + a];
              swapped[i + a] = swapped[i + b];
              swapped[i + b] = t;
            }
          }
          out = swapped;
        }
      }
      if (fwrite(out, 1, bytes, stdout) != bytes) {
        fprintf(stderr, "short write\n");
        return 1;
      }
    }
    gaud_buffer_destroy(buffer);
    gaud_decoder_destroy(decoder);
  }

  gaud_diagnostics_destroy(&diagnostics);
  gaud_doc_destroy(doc);
  gaud_stream_destroy(stream);
  return 0;
}
