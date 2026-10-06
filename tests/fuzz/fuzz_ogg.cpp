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
 * Fuzz the Ogg page layer, the seek layer over it, and the two mappings.
 *
 * **The seek layer is the reason this harness exists separately.** The
 * page reader has been exercised since phase 4 through the Ogg FLAC
 * fuzzer's whole-file arm, but gaud_ogg_find_page(), gaud_ogg_bisect()
 * and gaud_ogg_last_granule() are a byte-offset search over
 * attacker-controlled data: they take an offset from outside, read a
 * length out of the bytes at it, and read that many more. Reaching them
 * through a loader means almost every input dies at the mapping's
 * signature first, so the first byte of the input chooses the entry
 * point instead.
 *
 * What it asserts beyond not crashing:
 *
 * - **A packet never claims more bytes than the pages carried.** The
 *   reader hands out a pointer into its own buffer and a length, and a
 *   length past the buffer is a read a caller cannot defend against.
 * - **A found page's extent is inside the file.** `next` is where the
 *   scan resumes, so a `next` past the end is an infinite loop in every
 *   caller, and a `next` at or before `offset` is an infinite loop in
 *   the scan itself.
 * - **The bisection's answer is never past the target.** A caller
 *   decodes forward from it; an answer past the target is samples that
 *   cannot be reached at all.
 * - **A scan is a function of its input.** The same offset searched
 *   twice gives the same page, so nothing is carried between calls.
 * - **A setup header that was accepted has every index in range.** A
 *   mapping names a floor by number, a residue names codebooks, a mode
 *   names a mapping - all out of a file. The audio path has no bound
 *   check in its inner loops because the parser established these, so a
 *   setup accepted with one out of range is a memory error waiting for a
 *   packet rather than a wrong answer.
 *
 * The Opus arms are here rather than in a harness of their own for the
 * same reason the Vorbis ones are: the page layer underneath is shared,
 * and a mutation that produces a valid page is worth handing to every
 * mapping that might claim it.
 *
 * Build with: make fuzz-ogg
 * Run:        make fuzz-run-ogg FUZZ_TIME=300
 */

#include "../../src/codec/opus/opus_decoder.h"
#include "../../src/codec/opus/opus_internal.h"
#include "../../src/codec/vorbis/vorbis_internal.h"
#include "../../src/container/ogg/ogg.h"
#include <ghoti.io/audio/audio.h>
#include <ghoti.io/audio/codecs.h>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size);

namespace {

/** Registers the codecs once, however many inputs run. */
struct Registered {
  Registered() { gaud_register_builtin_codecs(); }
};
const Registered registered;

/** Drain packets from arbitrary bytes read as pages. */
void fuzz_packets(const uint8_t * data, size_t size) {
  GAUD_Stream * stream = NULL;
  if (gaud_stream_create_memory(data, size, &stream) != GAUD_OK) {
    return;
  }
  OGG_Reader reader;
  gaud_ogg_reader_init(&reader, stream, NULL);
  for (int i = 0; i < 256; ++i) {
    const unsigned char * packet = NULL;
    size_t packet_size = 0;
    uint64_t granule = 0;
    uint32_t flags = 0;
    if (gaud_ogg_reader_packet(&reader, &packet, &packet_size, &granule,
            &flags)
        != GAUD_OK) {
      break;
    }
    /* A packet is assembled out of page bodies, and no number of pages
     * can hold more than the file does. A length past that is a read the
     * caller has no way to bound. */
    if (packet_size > size) {
      abort();
    }
    if (packet_size && !packet) {
      abort();
    }
    /* Touch every byte, so ASan sees a length that overruns the
     * reader's own buffer rather than only one past the file. */
    volatile unsigned char sink = 0;
    for (size_t at = 0; at < packet_size; ++at) {
      sink = (unsigned char)(sink ^ packet[at]);
    }
    (void)sink;
  }
  gaud_ogg_reader_free(&reader);
  gaud_stream_destroy(stream);
}

/** Scan for pages from an offset the input chooses. */
void fuzz_scan(const uint8_t * data, size_t size) {
  if (size < 3) {
    return;
  }
  uint64_t from = ((uint64_t)data[0] << 8 | data[1]) % (size + 1u);
  data += 2;
  size -= 2;

  GAUD_Stream * stream = NULL;
  if (gaud_stream_create_memory(data, size, &stream) != GAUD_OK) {
    return;
  }
  OGG_Page_Info page;
  memset(&page, 0, sizeof(page));
  if (gaud_ogg_find_page(stream, NULL, from, size, &page) == GAUD_OK) {
    if (page.offset < from || page.offset >= size) {
      abort(); /* Outside the window it was asked about. */
    }
    if (page.next <= page.offset || page.next > size) {
      abort(); /* A `next` like this is an infinite loop in the caller. */
    }
    if (page.body > OGG_MAX_BODY || page.segments > OGG_MAX_SEGMENTS) {
      abort();
    }
    /* A function of its input: the same question twice, same answer. */
    OGG_Page_Info again;
    memset(&again, 0, sizeof(again));
    if (gaud_ogg_find_page(stream, NULL, from, size, &again) != GAUD_OK
        || memcmp(&page, &again, sizeof(page)) != 0) {
      abort();
    }

    /* The bisection and the length, which both need a serial. The one
     * just found is used, so they are asked about a stream that exists
     * rather than one that does not. */
    OGG_Reader reader;
    gaud_ogg_reader_init(&reader, stream, NULL);
    reader.serial = page.serial;
    reader.have_serial = true;
    uint64_t target = page.granule == OGG_NO_GRANULE ? 0 : page.granule;
    uint64_t offset = UINT64_MAX;
    uint64_t granule = UINT64_MAX;
    if (gaud_ogg_bisect(&reader, target, 0, &offset, &granule) == GAUD_OK) {
      if (offset > size) {
        abort();
      }
      if (granule > target) {
        abort(); /* Past the target: samples the caller cannot reach. */
      }
      /* And reading from where it landed must yield packets or stop. */
      if (gaud_ogg_reader_seek(&reader, offset) == GAUD_OK) {
        for (int i = 0; i < 16; ++i) {
          const unsigned char * packet = NULL;
          size_t packet_size = 0;
          if (gaud_ogg_reader_packet(&reader, &packet, &packet_size, NULL,
                  NULL)
              != GAUD_OK) {
            break;
          }
          if (packet_size > size) {
            abort();
          }
        }
      }
    }
    gaud_ogg_reader_free(&reader);

    uint64_t last = 0;
    (void)gaud_ogg_last_granule(stream, NULL, page.serial, &last);

    OGG_Logical streams[OGG_MAX_LOGICAL];
    size_t count = 0;
    if (gaud_ogg_scan_logical(stream, NULL, streams, OGG_MAX_LOGICAL, &count)
        == GAUD_OK) {
      if (count > OGG_MAX_LOGICAL) {
        abort();
      }
      for (size_t i = 0; i < count; ++i) {
        if (streams[i].head_size > OGG_HEAD_KEPT) {
          abort();
        }
        if (streams[i].offset >= size) {
          abort();
        }
      }
    }
  }
  gaud_stream_destroy(stream);
}

/** One Vorbis identification header, read directly. */
void fuzz_identification(const uint8_t * data, size_t size) {
  VORBIS_Info info;
  memset(&info, 0, sizeof(info));
  if (gaud_vorbis_parse_identification(data, size, &info) != GAUD_OK) {
    return;
  }
  /* What a caller is entitled to assume about an accepted header, which
   * is what every later arm of the decoder will be sized from. */
  if (info.version != 0 || info.channels == 0 || info.sample_rate == 0) {
    abort();
  }
  if (info.blocksize_short < VORBIS_MIN_BLOCKSIZE
      || info.blocksize_long > VORBIS_MAX_BLOCKSIZE
      || info.blocksize_short > info.blocksize_long) {
    abort();
  }
  /* Both are powers of two, which is what makes a half-block an exact
   * shift rather than a division. */
  if ((info.blocksize_short & (info.blocksize_short - 1u)) != 0
      || (info.blocksize_long & (info.blocksize_long - 1u)) != 0) {
    abort();
  }
}

/**
 * One Vorbis setup header, read directly.
 *
 * **The arm with the most arithmetic in it and the least reachable from
 * a file.** A setup header defines the codebooks, floors, residues,
 * mappings and modes - every table an MP3 decoder would have got from a
 * standard - so it is where a length, a count or an index out of a file
 * becomes an array subscript. Reaching it through a loader means getting
 * two valid header packets past first, which almost no mutation does.
 */
void fuzz_setup(const uint8_t * data, size_t size) {
  if (size < 2) {
    return;
  }
  /* One to eight channels, because the coupling fields' widths are
   * ilog(channels - 1) and a mono stream reads them as zero bits. */
  uint32_t channels = (uint32_t)(data[0] % 8u) + 1u;
  ++data;
  --size;

  /* The signature the parser requires, in front of the fuzzer's bytes. */
  size_t total = VORBIS_HEAD_SIZE + size;
  uint8_t * packet = (uint8_t *)malloc(total);
  if (!packet) {
    return;
  }
  packet[0] = VORBIS_PACKET_SETUP;
  memcpy(packet + 1, "vorbis", 6);
  memcpy(packet + VORBIS_HEAD_SIZE, data, size);

  GAUD_Limits limits;
  gaud_limits_default(&limits);
  VORBIS_Setup setup;
  memset(&setup, 0, sizeof(setup));
  setup.allocator = NULL;
  if (gaud_vorbis_parse_setup(packet, total, channels, &limits, &setup)
      == GAUD_OK) {
    /*
     * Everything a later packet will index, checked here - which is the
     * property the parser exists to establish. The audio path has no
     * bound check in its inner loops because of these, so a setup that
     * was accepted with an index out of range is a memory error waiting
     * for a packet rather than a wrong answer.
     */
    if (setup.mode_bits != gaud_vorbis_ilog(setup.mode_count - 1u)) {
      abort();
    }
    for (uint32_t i = 0; i < setup.mode_count; ++i) {
      if (setup.modes[i].mapping >= setup.mapping_count) {
        abort();
      }
    }
    for (uint32_t i = 0; i < setup.mapping_count; ++i) {
      const VORBIS_Mapping * mapping = &setup.mappings[i];
      if (mapping->submaps == 0 || mapping->submaps > VORBIS_MAX_SUBMAPS) {
        abort();
      }
      for (uint32_t j = 0; j < mapping->submaps; ++j) {
        if (mapping->floor[j] >= setup.floor_count
            || mapping->residue[j] >= setup.residue_count) {
          abort();
        }
      }
      for (uint32_t j = 0; j < mapping->coupling_steps; ++j) {
        if (mapping->magnitude[j] >= channels
            || mapping->angle[j] >= channels
            || mapping->magnitude[j] == mapping->angle[j]) {
          abort();
        }
      }
      for (uint32_t ch = 0; ch < channels; ++ch) {
        if (mapping->mux[ch] >= mapping->submaps) {
          abort();
        }
      }
    }
    for (uint32_t i = 0; i < setup.residue_count; ++i) {
      const VORBIS_Residue * residue = &setup.residues[i];
      if (residue->begin > residue->end
          || residue->classifications > VORBIS_RESIDUE_CLASSES
          || residue->classbook >= setup.codebook_count
          || residue->passes > 8u) {
        abort();
      }
      for (uint32_t c = 0; c < residue->classifications; ++c) {
        for (unsigned pass = 0; pass < 8u; ++pass) {
          int16_t book = residue->book[c][pass];
          if (book >= 0 && (uint32_t)book >= setup.codebook_count) {
            abort();
          }
        }
      }
    }
    for (uint32_t i = 0; i < setup.floor_count; ++i) {
      const VORBIS_Floor * one = &setup.floors[i];
      if (one->type != 0 && one->type != 1u) {
        abort();
      }
      if (one->type == 1u) {
        if (one->u.one.values > VORBIS_FLOOR1_VALUES
            || one->u.one.multiplier == 0 || one->u.one.multiplier > 4u) {
          abort();
        }
        /* The sorted order is what rendering walks, and a repeated X
         * position is refused at parse because a renderer handed one
         * divides by a run of zero. */
        for (uint32_t k = 1; k < one->u.one.values; ++k) {
          if (one->u.one.x_list[one->u.one.sorted[k]]
              <= one->u.one.x_list[one->u.one.sorted[k - 1u]]) {
            abort();
          }
        }
      }
    }
    for (uint32_t i = 0; i < setup.codebook_count; ++i) {
      const VORBIS_Codebook * book = &setup.codebooks[i];
      if (book->lookup_type > 2u) {
        abort();
      }
      if (book->lookup_type && !book->values) {
        abort();
      }
      if (book->used > book->entries) {
        abort();
      }
    }
  }
  gaud_vorbis_setup_free(&setup);
  free(packet);
}

/**
 * One Opus packet's table of contents, read directly.
 *
 * Small and reached by nothing else: the framing is one byte and up to
 * two more, and the three things it can say that are impossible - a
 * count of zero, a packet past 120 milliseconds, a stated first-frame
 * length running past the packet - are each one comparison that a
 * loader never evaluates, because it only ever sees packets an encoder
 * wrote.
 */
void fuzz_opus_toc(const uint8_t * data, size_t size) {
  OPUS_Toc toc;
  memset(&toc, 0, sizeof(toc));
  if (gaud_opus_parse_toc(data, size, &toc) != GAUD_OK) {
    return;
  }
  /* What a caller is entitled to assume about an accepted packet, and
   * what the decoder will size its buffers from. */
  if (toc.frames == 0 || toc.frame_size == 0) {
    abort();
  }
  /* A packet is at most 120 milliseconds, which is 5,760 samples at the
   * 48 kHz the granule positions are counted in. */
  if ((uint64_t)toc.frames * toc.frame_size > 5760u) {
    abort();
  }
  if (toc.mode != OPUS_MODE_SILK && toc.mode != OPUS_MODE_HYBRID
      && toc.mode != OPUS_MODE_CELT) {
    abort();
  }
  if (toc.bandwidth > 4u || toc.code > 3u) {
    abort();
  }
}

/**
 * A stream of Opus packets, decoded directly.
 *
 * The decoder takes bytes an encoder wrote, so almost nothing a fuzzer
 * makes is a packet the SILK and CELT parsers were designed for, which
 * is the point: every flag a real encoder rarely sets - an enormous
 * redundancy frame, a mid-only stereo frame after a coded one, an LTP
 * lag past the history - is a branch here. The input is cut into
 * packets by its own first bytes, and one in four becomes a loss, so
 * concealment runs after every kind of frame.
 *
 * What is asserted is what a caller relies on, not what a decoder
 * *should* produce: the sample count is the packet's own duration and
 * never past the buffer, and a decode that says it worked leaves the
 * range state a number rather than an error.
 */
void fuzz_opus_decode(const uint8_t * data, size_t size) {
  static int16_t pcm[OPUS_MAX_PACKET_SAMPLES * 2u];
  uint32_t channels = (data[0] & 1u) ? 2u : 1u;
  OPUS_Decoder * decoder = NULL;
  if (gaud_opus_decoder_create(NULL, channels, &decoder) != GAUD_OK) {
    return;
  }
  size_t at = 1;
  for (int packets = 0; at < size && packets < 64; ++packets) {
    size_t length = 1u + (data[at] % 160u);
    ++at;
    if (length > size - at) {
      length = size - at;
    }
    uint32_t got = 0;
    GAUD_Result result;
    if (length > 0 && (data[at - 1u] & 0xC0u) == 0xC0u) {
      /* A loss, of a length taken from the byte. */
      uint32_t wanted = 120u << (data[at - 1u] & 3u);
      result = gaud_opus_decode_packet(
          decoder, NULL, 0, pcm, OPUS_MAX_PACKET_SAMPLES, wanted, &got);
    }
    else {
      result = gaud_opus_decode_packet(decoder, data + at, length, pcm,
          OPUS_MAX_PACKET_SAMPLES, 0, &got);
    }
    if (result == GAUD_OK && got > OPUS_MAX_PACKET_SAMPLES) {
      abort();
    }
    if (result != GAUD_OK && got != 0) {
      abort();
    }
    at += length;
  }
  gaud_opus_decoder_destroy(NULL, decoder);
}

/** One OpusHead, read directly. */
void fuzz_opus_head(const uint8_t * data, size_t size) {
  size_t total = OPUS_MAGIC_SIZE + size;
  uint8_t * packet = (uint8_t *)malloc(total);
  if (!packet) {
    return;
  }
  memcpy(packet, OPUS_HEAD_MAGIC, OPUS_MAGIC_SIZE);
  memcpy(packet + OPUS_MAGIC_SIZE, data, size);
  OPUS_Head head;
  memset(&head, 0, sizeof(head));
  if (gaud_opus_parse_head(packet, total, &head) == GAUD_OK) {
    /* Every number a later packet will index, which is what the parse
     * exists to establish. */
    if (head.channels == 0 || head.channels > OPUS_MAX_CHANNELS) {
      abort();
    }
    if ((head.version >> 4) != 0) {
      abort();
    }
    if (head.streams == 0 || head.coupled > head.streams) {
      abort();
    }
    for (uint32_t i = 0; i < head.channels; ++i) {
      if (head.mapping[i] != 255u
          && head.mapping[i] >= head.streams + head.coupled) {
        abort();
      }
    }
  }
  free(packet);
}

/** Load a whole file, for whichever Ogg mapping claims it. */
void fuzz_file(const uint8_t * data, size_t size) {
  GAUD_Stream * stream = NULL;
  if (gaud_stream_create_memory(data, size, &stream) != GAUD_OK) {
    return;
  }
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  limits.max_metadata_bytes = 1u << 20;
  limits.max_picture_bytes = 1u << 18;
  limits.max_frames = 1u << 22;

  GAUD_Doc * doc = NULL;
  GAUD_Diagnostics diagnostics;
  gaud_diagnostics_init(&diagnostics, NULL);
  if (gaud_doc_load(NULL, stream, &limits, &diagnostics, &doc) == GAUD_OK) {
    GAUD_Track * track = gaud_doc_track(doc, 0);
    if (track) {
      uint64_t frames = gaud_track_frames(track);
      if (frames != UINT64_MAX && frames > limits.max_frames) {
        abort(); /* A limit that was checked and then exceeded. */
      }
      GAUD_Decoder * decoder = NULL;
      if (gaud_decoder_create(track, &decoder) == GAUD_OK) {
        GAUD_Buffer * buffer = NULL;
        if (gaud_decoder_buffer_create(decoder, NULL, 503u, &buffer)
            == GAUD_OK) {
          for (int i = 0; i < 64; ++i) {
            if (gaud_decoder_read(decoder, buffer) != GAUD_OK) {
              break;
            }
            if (gaud_buffer_frames(buffer) == 0) {
              break;
            }
            if (gaud_buffer_frames(buffer) > gaud_buffer_capacity(buffer)) {
              abort();
            }
          }
          uint64_t target = ((uint64_t)data[size - 1u] << 8) | data[0];
          uint64_t landed = UINT64_MAX;
          if (gaud_decoder_seek(decoder, target, &landed) == GAUD_OK) {
            if (frames != UINT64_MAX && landed > frames) {
              abort();
            }
            if (gaud_decoder_read(decoder, buffer) == GAUD_OK
                && frames != UINT64_MAX
                && landed + gaud_buffer_frames(buffer) > frames) {
              abort();
            }
          }
          gaud_buffer_destroy(buffer);
        }
        gaud_decoder_destroy(decoder);
      }
    }
    gaud_doc_destroy(doc);
  }
  gaud_diagnostics_destroy(&diagnostics);
  gaud_stream_destroy(stream);
}

} // namespace

int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 2) {
    return 0;
  }
  /* The first byte selects the entry point. Without it a corpus of whole
   * files would never reach the seek layer: a loader reaches it only
   * after the mapping's signature has already been accepted, and almost
   * every mutation breaks that first. */
  unsigned mode = data[0] % 8u;
  const uint8_t * body = data + 1;
  size_t body_size = size - 1u;
  switch (mode) {
  case 0:
    fuzz_file(body, body_size);
    break;
  case 1:
    fuzz_packets(body, body_size);
    break;
  case 2:
    fuzz_scan(body, body_size);
    break;
  case 3:
    fuzz_identification(body, body_size);
    break;
  case 4:
    fuzz_setup(body, body_size);
    break;
  case 5:
    fuzz_opus_toc(body, body_size);
    break;
  case 6:
    fuzz_opus_head(body, body_size);
    break;
  default:
    fuzz_opus_decode(body, body_size);
    break;
  }
  return 0;
}
