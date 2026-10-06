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
 * Vorbis: the three headers, the tags, and the length.
 *
 * **The frame counts below are not this library's arithmetic written down
 * twice.** Every one of them was measured, and the measurement is worth
 * stating because the obvious reference is the wrong one. `ffprobe`'s
 * `duration_ts` for each fixture equals the number asserted here, and so
 * does the sample count libsndfile decodes - but the sample count *ffmpeg*
 * decodes does not, for any fixture. ffmpeg's two Vorbis decoders agree
 * with each other and disagree with both of those, because the trim is
 * applied in the Ogg demuxer they share rather than in either decoder.
 * planning/audio.md section 11.25 has the numbers and the argument;
 * the short version is that the granule position is the specification's
 * answer and libvorbis's own front end implements it.
 *
 * **Hand-built headers for the arithmetic.** The identification header is
 * 30 bytes with two four-bit fields sharing one byte, and the pair every
 * encoder writes - 256 and 2,048 - is plausible read either way round. So
 * the nibble order has a test of its own, and so does every field whose
 * value the specification bounds.
 */

#include "../../src/codec/vorbis/vorbis_internal.h"
#include <ghoti.io/audio/audio.h>
#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/audio/codecs.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string Fixture(const char * name) {
  return std::string(GAUD_TEST_DATA) + "/" + name;
}

struct Loaded {
  GAUD_Stream * stream = nullptr;
  GAUD_Doc * doc = nullptr;
  GAUD_Diagnostics diagnostics;
  Loaded() {
    gaud_diagnostics_init(&diagnostics, nullptr);
  }
  ~Loaded() {
    gaud_doc_destroy(doc);
    gaud_stream_destroy(stream);
    gaud_diagnostics_destroy(&diagnostics);
  }
  GAUD_Track * track() {
    return gaud_doc_track(doc, 0);
  }
};

GAUD_Result OpenFile(Loaded & out, const char * name) {
  GAUD_Result result
      = gaud_stream_create_file(Fixture(name).c_str(), &out.stream);
  if (result != GAUD_OK) {
    return result;
  }
  return gaud_doc_load(
      nullptr, out.stream, nullptr, &out.diagnostics, &out.doc);
}

/**
 * The corpus, with what each file is for.
 *
 * `frames` is the last page's granule position, which for Vorbis is the
 * only statement of the length that exists anywhere in the file.
 * `recording` is how many frames were fed to the encoder, which the
 * generator knows exactly - so the two being equal is a real assertion
 * and not a tautology, and the one fixture where they differ is named.
 */
const struct Fixtures {
  const char * name;
  uint32_t rate;
  unsigned channels;
  uint64_t frames;
  uint64_t recording;
  uint32_t blocksize_short;
  uint32_t blocksize_long;
} fixtures[] = {
    {"vorbis_lib_stereo_44100.ogg", 44100u, 2u, 4409u, 4409u, 256u, 2048u},
    {"vorbis_lib_mono_44100.ogg", 44100u, 1u, 3001u, 3001u, 256u, 2048u},
    {"vorbis_lib_transient_44100.ogg", 44100u, 2u, 8819u, 8819u, 256u,
        2048u},
    {"vorbis_lib_noise_48000.ogg", 48000u, 2u, 4799u, 4799u, 256u, 2048u},
    {"vorbis_lib_silence_44100.ogg", 44100u, 2u, 2003u, 2003u, 256u, 2048u},
    /* **The three fixtures whose block sizes are equal**, which is the
     * arm where a stream never switches: the window shape and the
     * overlap are the same for every packet, so a decoder that assumed
     * switching happens works on them and one that assumed it never does
     * works on nothing else. libvorbis chooses these from the sample
     * rate and the quality; no setting at 44.1 kHz produces one. */
    {"vorbis_lib_mono_8000.ogg", 8000u, 1u, 1601u, 1601u, 512u, 512u},
    {"vorbis_lib_mono_22050.ogg", 22050u, 1u, 2003u, 2003u, 512u, 1024u},
    {"vorbis_lib_5dot1_48000.ogg", 48000u, 6u, 1499u, 1499u, 256u, 2048u},
    /*
     * **The second writer, and the one fixture whose length is not the
     * recording's.** libavcodec's native Vorbis encoder pads the last
     * block and states the padded length - 4,416 for 4,409 frames in,
     * seven frames over - where libvorbis sets the final granule
     * position to the input count and so trims. That is a property of
     * the encoder and not of this reader: ffprobe reports
     * duration_ts=4416 for this file too. It is here *because* of that:
     * a stream whose granule position is not a round number of input
     * frames is the only thing that distinguishes reading the field from
     * computing it.
     */
    {"vorbis_ff_stereo_44100.ogg", 44100u, 2u, 4416u, 4409u, 2048u, 2048u},
    {"vorbis_tagged_stereo_44100.ogg", 44100u, 2u, 2003u, 2003u, 256u,
        2048u},
};

/** An identification header with every field settable, for the bounds. */
std::vector<unsigned char> Identification(uint32_t version = 0,
    unsigned channels = 2, uint32_t rate = 44100, unsigned blocksize_byte
    = 0xB8u, unsigned framing = 1) {
  std::vector<unsigned char> packet(VORBIS_IDENTIFICATION_SIZE, 0u);
  packet[0] = VORBIS_PACKET_IDENTIFICATION;
  memcpy(&packet[1], "vorbis", 6);
  unsigned char * p = &packet[VORBIS_HEAD_SIZE];
  for (unsigned i = 0; i < 4; ++i) {
    p[i] = (unsigned char)((version >> (8 * i)) & 0xFFu);
  }
  p[4] = (unsigned char)channels;
  for (unsigned i = 0; i < 4; ++i) {
    p[5 + i] = (unsigned char)((rate >> (8 * i)) & 0xFFu);
  }
  /* The three bitrate hints stay zero: they are advisory and no value of
   * them can make a stream illegal. */
  p[21] = (unsigned char)blocksize_byte;
  p[22] = (unsigned char)framing;
  return packet;
}

} // namespace

/* ------------------------------------------------ the fixtures, as opened */

TEST(VorbisLoad, EveryFixtureIdentifiesAsTheStreamItIs) {
  for (const auto & one : fixtures) {
    Loaded loaded;
    ASSERT_EQ(OpenFile(loaded, one.name), GAUD_OK) << one.name;
    GAUD_Track * track = loaded.track();
    ASSERT_NE(track, nullptr) << one.name;
    EXPECT_STREQ(gaud_doc_codec_name(loaded.doc), "vorbis") << one.name;
    EXPECT_EQ(gaud_track_sample_rate(track), one.rate) << one.name;
    EXPECT_EQ(gaud_track_layout(track).channels, one.channels) << one.name;
    EXPECT_EQ(gaud_track_coding(track), GAUD_CODING_VORBIS) << one.name;
    /* Vorbis carries no bit depth at all, so 16 is this library's choice
     * and is the same for every file - unlike FLAC, where the track
     * answers the file's own depth. */
    EXPECT_EQ(gaud_track_format(track), GAUD_SAMPLE_S16) << one.name;
    EXPECT_EQ(loaded.diagnostics.count, 0u) << one.name;
  }
}

TEST(VorbisLoad, TheLengthIsTheLastPagesGranulePosition) {
  for (const auto & one : fixtures) {
    Loaded loaded;
    ASSERT_EQ(OpenFile(loaded, one.name), GAUD_OK) << one.name;
    EXPECT_EQ(gaud_track_frames(loaded.track()), one.frames) << one.name;
    EXPECT_NEAR(gaud_track_duration(loaded.track()),
        (double)one.frames / (double)one.rate, 1e-9)
        << one.name;
  }
}

TEST(VorbisLoad, TheLengthIsWhatWentIntoTheEncoder) {
  /*
   * The same numbers from the other side. Every libvorbis fixture's
   * granule position is exactly the frame count the generator fed it,
   * which is what a correct final granule position means - and the one
   * exception is libavcodec's encoder, which does not trim. Asserting
   * both halves separately is what makes the table above checkable: the
   * first test says we read the field, this one says the field means what
   * it is supposed to.
   */
  unsigned trimmed = 0;
  unsigned untrimmed = 0;
  for (const auto & one : fixtures) {
    if (one.frames == one.recording) {
      ++trimmed;
    }
    else {
      ++untrimmed;
      EXPECT_GT(one.frames, one.recording) << one.name
          << ": a stream shorter than its input is not padding";
      /* Less than one long block over, which is what padding to a block
       * boundary can cost and all it can cost. */
      EXPECT_LT(one.frames - one.recording, one.blocksize_long) << one.name;
    }
  }
  EXPECT_EQ(trimmed, 9u);
  EXPECT_EQ(untrimmed, 1u) << "the writer that does not trim is one file; "
                              "if that changed, the table needs re-measuring";
}

TEST(VorbisLoad, TheBlockSizesAreTheOnesTheStreamStates) {
  /*
   * Read off the document's own state rather than through the public API,
   * because the block sizes are not something a caller can ask about -
   * nothing a caller could do differs between them. They are asserted
   * because they are what selects the window and the overlap in the
   * decoder that comes next, and because the corpus was chosen to cover
   * four distinct pairs rather than one.
   */
  std::vector<std::pair<uint32_t, uint32_t>> seen;
  for (const auto & one : fixtures) {
    Loaded loaded;
    ASSERT_EQ(OpenFile(loaded, one.name), GAUD_OK) << one.name;
    VORBIS_File * state
        = static_cast<VORBIS_File *>(gaud_doc_private(loaded.doc));
    ASSERT_NE(state, nullptr) << one.name;
    EXPECT_EQ(state->info.blocksize_short, one.blocksize_short) << one.name;
    EXPECT_EQ(state->info.blocksize_long, one.blocksize_long) << one.name;
    EXPECT_EQ(state->info.version, 0u) << one.name;
    auto pair = std::make_pair(one.blocksize_short, one.blocksize_long);
    if (std::find(seen.begin(), seen.end(), pair) == seen.end()) {
      seen.push_back(pair);
    }
  }
  /* 256/2048, 512/512, 512/1024 and 2048/2048. A corpus that covered one
   * pair would leave the window selection untested by construction, and
   * this is the assertion that notices if it shrinks back to that. */
  EXPECT_EQ(seen.size(), 4u);
}

TEST(VorbisLoad, TheTagsComeOutOfTheCommentHeader) {
  Loaded loaded;
  ASSERT_EQ(OpenFile(loaded, "vorbis_tagged_stereo_44100.ogg"), GAUD_OK);
  const GAUD_Meta * meta = gaud_doc_meta(loaded.doc);
  ASSERT_NE(meta, nullptr);
  ASSERT_EQ(gaud_meta_count(meta, GAUD_TAG_TITLE), 1u);
  EXPECT_STREQ(gaud_meta_get(meta, GAUD_TAG_TITLE, 0), "A Title");
  ASSERT_EQ(gaud_meta_count(meta, GAUD_TAG_ARTIST), 1u);
  EXPECT_STREQ(gaud_meta_get(meta, GAUD_TAG_ARTIST, 0), "An Artist");
  ASSERT_EQ(gaud_meta_count(meta, GAUD_TAG_ALBUM), 1u);
  EXPECT_STREQ(gaud_meta_get(meta, GAUD_TAG_ALBUM, 0), "An Album");
  ASSERT_EQ(gaud_meta_count(meta, GAUD_TAG_DATE), 1u);
  EXPECT_STREQ(gaud_meta_get(meta, GAUD_TAG_DATE, 0), "2026");
  ASSERT_EQ(gaud_meta_count(meta, GAUD_TAG_GENRE), 1u);
  EXPECT_STREQ(gaud_meta_get(meta, GAUD_TAG_GENRE, 0), "Ambient");
  /* Non-ASCII, because a Vorbis comment is UTF-8 by definition and a
   * reader that mangled the encoding would still answer every field
   * above correctly. */
  ASSERT_EQ(gaud_meta_count(meta, GAUD_TAG_COMMENT), 1u);
  EXPECT_STREQ(gaud_meta_get(meta, GAUD_TAG_COMMENT, 0),
      "café 日本語");

  /*
   * And an untagged fixture has none of them, so the test above is not
   * passing on something every file carries.
   *
   * **Except `encoder`, and the exception is the interesting half.** A
   * Vorbis comment header carries a *vendor string* outside the comment
   * list, which libvorbis always fills in and which this library
   * deliberately does not turn into a tag - src/meta/vorbis_comment.c
   * gives the reason at length, and it is that a field the writer
   * overwrites must not be a field the reader collects. ffmpeg writes
   * `encoder=Lavc libvorbis` as an ordinary comment *as well*, and that
   * one is a tag: a caller put it in the list, so it comes out of the
   * list. The two together are what shows the distinction is being made
   * rather than everything being dropped or everything kept.
   */
  Loaded plain;
  ASSERT_EQ(OpenFile(plain, "vorbis_lib_stereo_44100.ogg"), GAUD_OK);
  const GAUD_Meta * bare = gaud_doc_meta(plain.doc);
  EXPECT_EQ(gaud_meta_count(bare, GAUD_TAG_TITLE), 0u);
  EXPECT_EQ(gaud_meta_count(bare, GAUD_TAG_ARTIST), 0u);
  ASSERT_EQ(gaud_meta_count(bare, GAUD_TAG_ENCODER), 1u);
  EXPECT_STREQ(gaud_meta_get(bare, GAUD_TAG_ENCODER, 0), "Lavc libvorbis");
}

namespace {

/** One Ogg page holding one whole packet, with the right checksum. */
std::vector<unsigned char> OggPage(uint32_t serial, uint32_t sequence,
    unsigned flags, uint64_t granule, const std::vector<unsigned char> & packet) {
  std::vector<unsigned char> page(27, 0);
  memcpy(page.data(), "OggS", 4);
  page[5] = (unsigned char)flags;
  for (int i = 0; i < 8; ++i) page[6 + i] = (unsigned char)(granule >> (8 * i));
  for (int i = 0; i < 4; ++i) page[14 + i] = (unsigned char)(serial >> (8 * i));
  for (int i = 0; i < 4; ++i) page[18 + i] = (unsigned char)(sequence >> (8 * i));
  size_t left = packet.size();
  std::vector<unsigned char> table;
  while (left >= 255u) {
    table.push_back(255);
    left -= 255u;
  }
  table.push_back((unsigned char)left);
  page[26] = (unsigned char)table.size();
  page.insert(page.end(), table.begin(), table.end());
  page.insert(page.end(), packet.begin(), packet.end());
  uint32_t crc = gaud_ogg_crc32(page.data(), page.size());
  for (int i = 0; i < 4; ++i) page[22 + i] = (unsigned char)(crc >> (8 * i));
  return page;
}

/** Every packet of an Ogg file, in order. */
std::vector<std::vector<unsigned char>> Packets(const char * name) {
  std::vector<std::vector<unsigned char>> packets;
  FILE * file = fopen(Fixture(name).c_str(), "rb");
  if (!file) {
    return packets;
  }
  std::vector<unsigned char> bytes;
  unsigned char block[4096];
  size_t got;
  while ((got = fread(block, 1, sizeof block, file)) > 0) {
    bytes.insert(bytes.end(), block, block + got);
  }
  fclose(file);
  std::vector<unsigned char> current;
  for (size_t at = 0; at + 27 <= bytes.size();) {
    size_t segments = bytes[at + 26];
    size_t body = at + 27 + segments;
    for (size_t k = 0; k < segments; ++k) {
      size_t length = bytes[at + 27 + k];
      current.insert(current.end(), bytes.begin() + body,
          bytes.begin() + body + length);
      body += length;
      if (length < 255u) {
        packets.push_back(current);
        current.clear();
      }
    }
    at = body;
  }
  return packets;
}

/**
 * A page that may begin with the tail of a packet, and then holds either
 * one whole packet or the first 255 bytes of one that goes on.
 */
std::vector<unsigned char> OggPageSplit(uint32_t sequence, unsigned flags,
    uint64_t granule, const std::vector<unsigned char> & tail,
    const std::vector<unsigned char> & body, bool open) {
  std::vector<unsigned char> page(27, 0);
  memcpy(page.data(), "OggS", 4);
  page[5] = (unsigned char)flags;
  for (int i = 0; i < 8; ++i) page[6 + i] = (unsigned char)(granule >> (8 * i));
  for (int i = 0; i < 4; ++i) page[14 + i] = (unsigned char)(9u >> (8 * i));
  for (int i = 0; i < 4; ++i) page[18 + i] = (unsigned char)(sequence >> (8 * i));
  std::vector<unsigned char> table;
  auto lace = [&table](size_t length) {
    while (length >= 255u) {
      table.push_back(255);
      length -= 255u;
    }
    table.push_back((unsigned char)length);
  };
  if (!tail.empty()) {
    lace(tail.size());
  }
  if (open) {
    table.push_back(255);
  }
  else if (!body.empty()) {
    lace(body.size());
  }
  page[26] = (unsigned char)table.size();
  page.insert(page.end(), table.begin(), table.end());
  page.insert(page.end(), tail.begin(), tail.end());
  page.insert(page.end(), body.begin(), body.end());
  uint32_t crc = gaud_ogg_crc32(page.data(), page.size());
  for (int i = 0; i < 4; ++i) page[22 + i] = (unsigned char)(crc >> (8 * i));
  return page;
}

/** A fixture's audio looped @p repeats times, a page to every packet. */
struct Looped {
  std::vector<unsigned char> file;
  uint64_t frames = 0;
  uint64_t pages = 0;
};

/**
 * The granule position after each packet is the one before it plus a
 * quarter of each of the two block sizes, and the first packet produces
 * nothing - which is the specification's arithmetic and not this
 * decoder's, so the file's page positions are right whatever the decoder
 * does with them. The block sizes come from each packet's mode number.
 */
bool LoopVorbis(const char * name, int repeats, bool straddle,
    size_t per_page, uint64_t trim, size_t skip, Looped * out) {
  auto packets = Packets(name);
  if (packets.size() < 5) {
    return false;
  }
  Loaded loaded;
  if (OpenFile(loaded, name) != GAUD_OK) {
    return false;
  }
  const VORBIS_File * info
      = (const VORBIS_File *)gaud_track_private(loaded.track());
  std::vector<unsigned char> file;
  uint32_t sequence = 0;
  for (int k = 0; k < 3; ++k) {
    auto page = OggPage(9, sequence++, k == 0 ? 2u : 0u, 0, packets[k]);
    file.insert(file.end(), page.begin(), page.end());
  }
  // The audio, looped, with the window flags at each join made to say
  // what the neighbours really are: a long block states whether the one
  // after it is long and whether the one before it was, and the end of a
  // stream rarely agrees with its own beginning. Everything else in each
  // packet is as the encoder wrote it.
  std::vector<std::vector<unsigned char>> audio;
  for (int r = 0; r < repeats; ++r) {
    audio.insert(audio.end(), packets.begin() + 3, packets.end());
  }
  // Begin part-way through, so that the first block can be a long one
  // whose successor is short: the one arrangement in which the decoder's
  // numbering starts a quarter of a block away from the granule's.
  audio.erase(audio.begin(), audio.begin() + (ptrdiff_t)skip);
  auto is_long = [&](const std::vector<unsigned char> & packet) {
    uint32_t mode = (packet[0] >> 1) & ((1u << info->setup.mode_bits) - 1u);
    return info->setup.modes[mode].block_flag;
  };
  auto set_bit = [](std::vector<unsigned char> & packet, unsigned at,
                     bool value) {
    if (value) {
      packet[at / 8u] |= (unsigned char)(1u << (at % 8u));
    }
    else {
      packet[at / 8u] &= (unsigned char)~(1u << (at % 8u));
    }
  };
  unsigned flag_at = 1u + info->setup.mode_bits;
  for (size_t k = 1; k < audio.size(); ++k) {
    if (is_long(audio[k - 1])) {
      set_bit(audio[k - 1], flag_at + 1u, is_long(audio[k]));
    }
    if (is_long(audio[k])) {
      set_bit(audio[k], flag_at, is_long(audio[k - 1]));
    }
  }
  uint64_t granule = 0;
  uint32_t previous = 0;
  std::vector<uint64_t> after(audio.size());
  for (size_t k = 0; k < audio.size(); ++k) {
    uint32_t n = is_long(audio[k]) ? info->info.blocksize_long
                                   : info->info.blocksize_short;
    if (previous != 0) {
      granule += previous / 4u + n / 4u;
    }
    previous = n;
    after[k] = granule;
  }
  if (!straddle) {
    // Pages of several packets, the last of which states a position
    // `trim` frames short of where its blocks end - which is what an
    // encoder does to say the recording ended before its last block did.
    for (size_t first = 0; first < audio.size(); first += per_page) {
      size_t end = std::min(audio.size(), first + per_page);
      bool last = end == audio.size();
      std::vector<unsigned char> body;
      std::vector<unsigned char> lacing;
      for (size_t k = first; k < end; ++k) {
        size_t length = audio[k].size();
        while (length >= 255u) {
          lacing.push_back(255);
          length -= 255u;
        }
        lacing.push_back((unsigned char)length);
        body.insert(body.end(), audio[k].begin(), audio[k].end());
      }
      std::vector<unsigned char> page(27, 0);
      memcpy(page.data(), "OggS", 4);
      page[5] = last ? 4u : 0u;
      uint64_t position = after[end - 1] - (last ? trim : 0u);
      for (int i = 0; i < 8; ++i) {
        page[6 + i] = (unsigned char)(position >> (8 * i));
      }
      for (int i = 0; i < 4; ++i) page[14 + i] = (unsigned char)(9u >> (8 * i));
      for (int i = 0; i < 4; ++i) {
        page[18 + i] = (unsigned char)(sequence >> (8 * i));
      }
      ++sequence;
      page[26] = (unsigned char)lacing.size();
      page.insert(page.end(), lacing.begin(), lacing.end());
      page.insert(page.end(), body.begin(), body.end());
      uint32_t crc = gaud_ogg_crc32(page.data(), page.size());
      for (int i = 0; i < 4; ++i) page[22 + i] = (unsigned char)(crc >> (8 * i));
      file.insert(file.end(), page.begin(), page.end());
    }
  }
  else {
    // A packet longer than 255 bytes is cut there, and its tail starts
    // the next page, so that a page a bisection can land on begins with
    // the tail of a packet. A shorter one is a page of its own, since a
    // page cannot end in the middle of it.
    std::vector<unsigned char> tail;
    bool continues = false;
    for (size_t k = 0; k < audio.size(); ++k) {
      bool cut = audio[k].size() > 255u;
      std::vector<unsigned char> body
          = cut ? std::vector<unsigned char>(
                audio[k].begin(), audio[k].begin() + 255)
                : audio[k];
      // What finishes on this page: the packet whose tail it starts with,
      // and this one unless it is cut.
      uint64_t granule_here = ~0ULL;
      if (continues) {
        granule_here = after[k - 1];
      }
      if (!cut) {
        granule_here = after[k];
      }
      auto page = OggPageSplit(sequence++,
          (continues ? 1u : 0u) | (!cut && k + 1 == audio.size() ? 4u : 0u),
          granule_here, tail, body, cut);
      file.insert(file.end(), page.begin(), page.end());
      tail.clear();
      continues = cut;
      if (cut) {
        tail.assign(audio[k].begin() + 255, audio[k].end());
      }
    }
    if (continues) {
      auto page = OggPageSplit(
          sequence++, 1u | 4u, after[audio.size() - 1], tail, {}, false);
      file.insert(file.end(), page.begin(), page.end());
    }
  }
  uint64_t index = audio.size();
  out->file = file;
  out->frames = after[audio.size() - 1] - (straddle ? 0u : trim);
  out->pages = index;
  return true;
}

}  // namespace

namespace {

/** Seek around a looped fixture and compare every read with a straight one. */
void CheckSeeks(const char * name, int repeats, bool straddle,
    size_t per_page = 1, uint64_t trim = 0, size_t skip = 0) {
  {
    Looped looped;
    ASSERT_TRUE(
        LoopVorbis(name, repeats, straddle, per_page, trim, skip, &looped))
        << name;
    GAUD_Stream * stream = nullptr;
    ASSERT_EQ(gaud_stream_create_memory(
                  looped.file.data(), looped.file.size(), &stream),
        GAUD_OK);
    GAUD_Diagnostics diagnostics;
    gaud_diagnostics_init(&diagnostics, nullptr);
    GAUD_Doc * doc = nullptr;
    ASSERT_EQ(gaud_doc_load(nullptr, stream, nullptr, &diagnostics, &doc),
        GAUD_OK)
        << name;
    GAUD_Track * track = gaud_doc_track(doc, 0);
    unsigned channels = gaud_track_layout(track).channels;
    ASSERT_EQ(gaud_track_frames(track), looped.frames) << name;
    std::vector<int16_t> all;
    {
      GAUD_Decoder * straight = nullptr;
      ASSERT_EQ(gaud_decoder_create(track, &straight), GAUD_OK);
      GAUD_Buffer * buffer = nullptr;
      ASSERT_EQ(gaud_decoder_buffer_create(straight, nullptr, 4096, &buffer),
          GAUD_OK);
      for (;;) {
        ASSERT_EQ(gaud_decoder_read(straight, buffer), GAUD_OK) << name;
        size_t frames = gaud_buffer_frames(buffer);
        if (frames == 0) {
          break;
        }
        const int16_t * data
            = (const int16_t *)gaud_buffer_data_const(buffer);
        all.insert(all.end(), data, data + frames * channels);
      }
      gaud_buffer_destroy(buffer);
      gaud_decoder_destroy(straight);
    }
    ASSERT_EQ(all.size(), looped.frames * channels) << name;

    GAUD_Decoder * decoder = nullptr;
    ASSERT_EQ(gaud_decoder_create(track, &decoder), GAUD_OK);
    GAUD_Buffer * buffer = nullptr;
    ASSERT_EQ(gaud_decoder_buffer_create(decoder, nullptr, 3000, &buffer),
        GAUD_OK);
    uint64_t frames = looped.frames;
    for (uint64_t target : std::initializer_list<uint64_t>{frames / 2,
             frames / 3, frames / 5, 1u, 100u, 0u, frames - 77u, frames - 1500u,
             frames - 3000u, frames - 5000u, frames - 9000u,
             frames + 5u, frames / 2 + 1u, frames / 7, frames / 7 + 3000u,
             frames / 7 + 3001u + 8192u * 3u}) {
      uint64_t landed = 0;
      ASSERT_EQ(gaud_decoder_seek(decoder, target, &landed), GAUD_OK) << name;
      uint64_t expect = std::min(target, frames);
      EXPECT_EQ(landed, expect) << name << " " << target;
      ASSERT_EQ(gaud_decoder_read(decoder, buffer), GAUD_OK) << name;
      size_t got = gaud_buffer_frames(buffer);
      EXPECT_EQ(got, std::min<uint64_t>(3000u, frames - expect))
          << name << " " << target;
      const int16_t * data = (const int16_t *)gaud_buffer_data_const(buffer);
      for (size_t i = 0; i < got * channels; ++i) {
        ASSERT_EQ(data[i], all[expect * channels + i])
            << name << " target " << target << " sample " << i;
      }
    }
    gaud_buffer_destroy(buffer);
    gaud_decoder_destroy(decoder);
    gaud_doc_destroy(doc);
    gaud_stream_destroy(stream);
    gaud_diagnostics_destroy(&diagnostics);
  }
}

}  // namespace

TEST(VorbisDecode, ASeekLandsOnAPageAndReadsExactlyWhatAStraightReadReads) {
  // Nothing in a block depends on those before it except the lap, so the
  // seek decodes the block that provides it and the rest is the same to
  // the bit. Each fixture is looped until it has hundreds of pages to
  // bisect; the loop's joins are made legal by setting the window flags
  // at them to what the neighbours are, which is the one change made to
  // any packet.
  for (const char * name : {"vorbis_lib_mono_8000.ogg",
           "vorbis_lib_mono_22050.ogg", "vorbis_lib_stereo_44100.ogg",
           "vorbis_lib_5dot1_48000.ogg", "vorbis_lib_transient_44100.ogg"}) {
    CheckSeeks(name, 40, false);
  }
}

TEST(VorbisDecode, ASeekIntoPagesOfSeveralPacketsAndAShortenedLastOne) {
  // Several packets to a page, so a landing page has blocks to add up,
  // and a last page whose position is 100 frames short of what its blocks
  // decode to. That page cannot number anything, and a seek into the
  // last frames is where a decoder that let it would be wrong.
  for (const char * name : {"vorbis_lib_stereo_44100.ogg",
           "vorbis_lib_transient_44100.ogg", "vorbis_lib_mono_22050.ogg"}) {
    CheckSeeks(name, 40, false, 7, 100);
    CheckSeeks(name, 40, false, 7, 0);
    CheckSeeks(name, 40, false, 60, 100);
  }
}

TEST(VorbisDecode, ASeekDoesNotDependOnWhichBlockTheStreamBeginsWith) {
  // Rotated, so that the stream begins at every one of the first dozen
  // packets: some of those are long blocks followed by short ones, whose
  // first output is not where a granule position would put it.
  for (const char * name : {"vorbis_lib_stereo_44100.ogg",
           "vorbis_lib_transient_44100.ogg", "vorbis_lib_mono_22050.ogg"}) {
    for (size_t skip = 1; skip <= 12; ++skip) {
      SCOPED_TRACE(skip);
      CheckSeeks(name, 12, false, 1 + skip % 5, 0, skip);
    }
  }
}

TEST(VorbisDecode, ASeekOntoAPageThatContinuesAPacketIsStillExact) {
  // A page here that is not the stream's first begins with the end of a
  // packet, which the reader drops, so the first packet a landing decodes
  // is the one after it. The position is counted back from the end of
  // the page, so it does not matter which packet that is.
  CheckSeeks("vorbis_lib_noise_48000.ogg", 20, true);
}

namespace {

/** Bits least-significant first, as Vorbis packs them. */
struct PacketBits {
  std::vector<unsigned char> bytes;
  unsigned used = 0;
  void Write(uint32_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
      if (used == 0) {
        bytes.push_back(0);
      }
      if ((value >> i) & 1u) {
        bytes.back() |= (unsigned char)(1u << used);
      }
      used = (used + 1) % 8;
    }
  }
  /** A codeword: its first bit is its most significant. */
  void Code(uint32_t word, unsigned length) {
    for (unsigned i = length; i-- > 0;) {
      Write((word >> i) & 1u, 1);
    }
  }
};

uint64_t DigestOf(const std::vector<int16_t> & samples) {
  uint64_t digest = 1469598103934665603ULL;
  for (int16_t v : samples) {
    for (int i = 0; i < 8; ++i) {
      digest ^= (uint64_t)(((int64_t)v >> (8 * i)) & 0xFF);
      digest *= 1099511628211ULL;
    }
  }
  return digest;
}

double BarkOf(double x) {
  return 13.1 * atan(.00074 * x) + 2.24 * atan(.0000000185 * x * x)
      + .0001 * x;
}

}  // namespace

TEST(VorbisFloor0, TheCosineIsWithinAFewBillionthsOfLibms) {
  // Q30, so a billionth is about one unit. The angles run from far
  // negative to far positive through every boundary the reduction has -
  // multiples of a quarter turn and a hair either side - and then a few
  // that no stream would state.
  double worst = 0;
  std::vector<int64_t> angles;
  const int64_t quarter = 3373259426LL;
  for (int64_t k = -40; k <= 40; ++k) {
    for (int64_t nudge : {-2, -1, 0, 1, 2}) {
      angles.push_back(k * quarter + nudge);
    }
  }
  uint32_t state = 12345;
  for (int i = 0; i < 20000; ++i) {
    state = state * 1664525u + 1013904223u;
    angles.push_back((int64_t)((int32_t)state) * 8);
  }
  angles.push_back((int64_t)1 << 50);
  angles.push_back(-((int64_t)1 << 50));
  double worst_far = 0;
  for (int64_t angle : angles) {
    double expect = cos((double)angle / 4294967296.0);
    double got = (double)gaud_vorbis_f0_cos(angle) / 1073741824.0;
    double error = fabs(expect - got);
    // Beyond a thousand radians the constant two pi, rounded to 2^-32,
    // is what limits it: every period adds its own error. No stream
    // states an angle like that, and the answer is still the same on every
    // machine, which is what is required of it.
    if (fabs((double)angle) < 4294967296.0 * 1000.0) {
      worst = std::max(worst, error);
    }
    else {
      worst_far = std::max(worst_far, error);
    }
  }
  EXPECT_LT(worst, 4e-9) << "worst error " << worst;
  EXPECT_LT(worst_far, 1e-6) << "worst far error " << worst_far;
}

TEST(VorbisFloor0, TheArctangentIsWithinAFewHundredMillionthsOfLibms) {
  double worst = 0;
  std::vector<int64_t> arguments = {0, 1, 2, 1073741824LL / 2, 1073741824LL,
      1073741824LL + 1, 1073741824LL - 1, (int64_t)1 << 40};
  uint32_t state = 777;
  for (int i = 0; i < 20000; ++i) {
    state = state * 1664525u + 1013904223u;
    int shift = (int)(state >> 27); // 0 to 31
    arguments.push_back((int64_t)(state & 0x7FFFFFFFu) >> (shift > 30 ? 30 : shift));
  }
  for (int64_t t : arguments) {
    double expect = atan((double)t / 1073741824.0);
    double got = (double)gaud_vorbis_f0_atan(t) / 1073741824.0;
    worst = std::max(worst, fabs(expect - got));
  }
  EXPECT_LT(worst, 3e-8) << "worst error " << worst;
}

TEST(VorbisFloor0, TheBarkMapFallsInTheBandsTheFormulaSays) {
  // Section 6.2.3's map, computed here with libm in double precision. The
  // two can differ only where the formula lands within a hair of a band's
  // edge, so this counts them and expects almost none - and the references
  // themselves, working in single precision, disagree with each other
  // there far more often.
  struct Config {
    uint32_t rate;
    uint32_t bark;
    uint32_t lines;
  };
  size_t lines_total = 0;
  size_t differing = 0;
  for (const Config & c : {Config{44100, 256, 1024}, Config{44100, 256, 128},
           Config{48000, 1000, 4096}, Config{8000, 64, 256},
           Config{22050, 2, 512}, Config{96000, 256, 1024},
           Config{16000, 65535, 1024}, Config{44101, 100, 1024},
           Config{1000, 256, 1024}, Config{65535, 17, 32}}) {
    std::vector<uint16_t> map(c.lines);
    gaud_vorbis_f0_bark_map(c.rate, c.bark, c.lines, map.data());
    double scale = c.bark / BarkOf(c.rate / 2.0);
    for (uint32_t j = 0; j < c.lines; ++j) {
      int expect = (int)floor(BarkOf((c.rate / 2.0) / c.lines * j) * scale);
      if (expect >= (int)c.bark) {
        expect = (int)c.bark - 1;
      }
      ++lines_total;
      differing += map[j] != expect;
      EXPECT_LE(abs((int)map[j] - expect), 1) << c.rate << " " << j;
    }
  }
  EXPECT_LE(differing, lines_total / 2000u)
      << differing << " of " << lines_total;
}

namespace {

/** The first floor 0 in a fixture and the setup around it. */
struct Floor0Fixture {
  Loaded loaded;
  const VORBIS_File * file = nullptr;
  const VORBIS_Floor0 * floor = nullptr;
};

}  // namespace

TEST(VorbisFloor0, TheCurveIsTheSpecificationsFormula) {
  Floor0Fixture f;
  ASSERT_EQ(OpenFile(f.loaded, "vorbis_syn_floor0_mono.ogg"), GAUD_OK);
  f.file = (const VORBIS_File *)gaud_track_private(f.loaded.track());
  ASSERT_EQ(f.file->setup.floors[0].type, 0u);
  f.floor = &f.file->setup.floors[0].u.zero;
  const VORBIS_Codebook * book = &f.file->setup.codebooks[f.floor->books[0]];
  const uint32_t lines = 1024;
  std::vector<uint16_t> map(lines);
  gaud_vorbis_f0_bark_map(f.floor->rate, f.floor->bark_map_size, lines,
      map.data());

  uint32_t state = 99;
  double worst = 0;
  int checked = 0;
  for (int trial = 0; trial < 60; ++trial) {
    state = state * 1664525u + 1013904223u;
    uint32_t amplitude = 1 + (state >> 8) % 63u;
    // A packet: amplitude, the book number (one bit for one book), and
    // enough entries to fill the order. The book's code is uniform and
    // complete, so an entry's codeword is its number.
    PacketBits w;
    w.Write(amplitude, f.floor->amplitude_bits);
    w.Write(0, 1);
    std::vector<uint32_t> entries;
    for (uint32_t n = 0; n < f.floor->order; n += book->dimensions) {
      state = state * 1664525u + 1013904223u;
      uint32_t entry = (state >> 10) % book->entries;
      entries.push_back(entry);
      w.Code(entry, book->lengths[entry]);
    }
    VORBIS_Bits bits;
    gaud_vorbis_bits_init(&bits, w.bytes.data(), w.bytes.size());
    std::vector<int32_t> mantissa(lines);
    std::vector<int16_t> shift(lines);
    bool used = false;
    ASSERT_EQ(gaud_vorbis_floor0_decode(f.floor, &f.file->setup, &bits, lines,
                  map.data(), mantissa.data(), shift.data(), &used),
        GAUD_OK);
    ASSERT_TRUE(used);

    // The same curve in double precision, from the same coefficients.
    std::vector<double> lsp;
    double last = 0;
    for (uint32_t entry : entries) {
      int64_t vector[256];
      gaud_vorbis_codebook_vector_fine(book, entry, vector);
      for (uint32_t k = 0; k < book->dimensions && lsp.size() < f.floor->order;
           ++k) {
        lsp.push_back((double)vector[k] / 4294967296.0 + last);
      }
      last = lsp.back();
    }
    for (double & x : lsp) {
      x = 2.0 * cos(x);
    }
    uint32_t m = f.floor->order;
    for (uint32_t j = 0; j < lines; ++j) {
      double w2 = 2.0 * cos(M_PI / f.floor->bark_map_size * map[j]);
      double p = .5;
      double q = .5;
      uint32_t i = 1;
      for (; i < m; i += 2) {
        q *= w2 - lsp[i - 1];
        p *= w2 - lsp[i];
      }
      if (i == m) {
        q *= w2 - lsp[i - 1];
        p *= p * (4. - w2 * w2);
        q *= q;
      }
      else {
        p *= p * (2. - w2);
        q *= q * (2. + w2);
      }
      double off = f.floor->amplitude_offset;
      double a = (double)amplitude / ((1 << f.floor->amplitude_bits) - 1) * off;
      double expect = exp((a / sqrt(p + q) - off) * .11512925);
      double got = (double)mantissa[j] * pow(2.0, -(double)shift[j]);
      // The curve is bounded at both ends (2^40 above, 2^-100 below), so
      // a line the formula puts outside that is not compared. Random
      // coefficients put plenty there: they are not an interlaced set.
      if (expect < 1e-25 || expect > 1e9) {
        continue;
      }
      worst = std::max(worst, fabs(got / expect - 1.0));
      ++checked;
    }
  }
  EXPECT_GT(checked, 50000);
  EXPECT_LT(worst, 1e-5) << "worst relative error " << worst;
}

namespace {

/** Every sample of a fixture, decoded straight through. */
std::vector<int16_t> DecodeFixture(const char * name, unsigned * channels) {
  std::vector<int16_t> all;
  Loaded loaded;
  EXPECT_EQ(OpenFile(loaded, name), GAUD_OK) << name;
  GAUD_Track * track = loaded.track();
  *channels = gaud_track_layout(track).channels;
  GAUD_Decoder * decoder = nullptr;
  GAUD_Buffer * buffer = nullptr;
  if (gaud_decoder_create(track, &decoder) == GAUD_OK
      && gaud_decoder_buffer_create(decoder, nullptr, 1024, &buffer)
          == GAUD_OK) {
    for (;;) {
      EXPECT_EQ(gaud_decoder_read(decoder, buffer), GAUD_OK) << name;
      size_t frames = gaud_buffer_frames(buffer);
      if (frames == 0) {
        break;
      }
      const int16_t * data = (const int16_t *)gaud_buffer_data_const(buffer);
      all.insert(all.end(), data, data + frames * *channels);
    }
  }
  gaud_buffer_destroy(buffer);
  gaud_decoder_destroy(decoder);
  return all;
}

}  // namespace

TEST(VorbisFloor0, StreamsWrittenForTheUnusedPartsOfTheFormatDecodeAsTheReferencesDo) {
  // tools/oracle/vorbis_synth.py writes these from the specification:
  // a floor of type 0 in every shape its parameters allow, residue type
  // 0, codebooks that state every vector. `make check-vorbis-synth`
  // scored each against ffmpeg's decoder and libvorbis - no sample more
  // than two off either, one off in all but a few - and these digests
  // are what this library's own decode was then. The frame counts are
  // the specification's, written into the file by the generator.
  struct One {
    const char * name;
    uint64_t samples;
    uint64_t digest;
  };
  for (const One & one : {
           One{"vorbis_syn_floor0_8000.ogg", 7744, 15747972106037866980ULL},
           One{"vorbis_syn_floor0_barkmap1.ogg", 11904,
               9918005369529583434ULL},
           One{"vorbis_syn_floor0_big_blocks.ogg", 39616,
               6456521449833447283ULL},
           One{"vorbis_syn_floor0_coupled.ogg", 47616,
               6769520699941738338ULL},
           One{"vorbis_syn_floor0_coupled_res2.ogg", 48512,
               7684391101132030273ULL},
           One{"vorbis_syn_floor0_equal_blocks.ogg", 5888,
               16091302032093961777ULL},
           One{"vorbis_syn_floor0_explicit_ramp.ogg", 10112,
               10574510561962775911ULL},
           One{"vorbis_syn_floor0_explicit_seq.ogg", 15040,
               7840142512969825284ULL},
           One{"vorbis_syn_floor0_mono.ogg", 12800, 10071505467492811047ULL},
           One{"vorbis_syn_floor0_order17_dim1.ogg", 9216,
               10048464347182690279ULL},
           One{"vorbis_syn_floor0_order32_dim8.ogg", 11904,
               3173640078163260182ULL},
           One{"vorbis_syn_floor0_order64.ogg", 11456,
               12376354765215560575ULL},
           One{"vorbis_syn_floor0_order8.ogg", 14144, 3495387106158831558ULL},
           One{"vorbis_syn_floor0_rate_mismatch.ogg", 10560,
               9163696892424449560ULL},
           One{"vorbis_syn_floor0_stereo.ogg", 29184, 16794459185419367056ULL},
           One{"vorbis_syn_floor0_stereo_res2.ogg", 22016,
               1882594875512902301ULL},
           One{"vorbis_syn_res0_mono.ogg", 11904, 6141980546939861337ULL},
           One{"vorbis_syn_res0_stereo.ogg", 22016,
               17796420413390126798ULL}}) {
    unsigned channels = 0;
    std::vector<int16_t> samples = DecodeFixture(one.name, &channels);
    EXPECT_EQ(samples.size(), one.samples) << one.name;
    EXPECT_EQ(DigestOf(samples), one.digest) << one.name;
    size_t peak = 0;
    for (int16_t v : samples) {
      peak = std::max<size_t>(peak, (size_t)std::abs((int)v));
    }
    // Loud enough to say something and nowhere near clipping, which tells
    // two decoders nothing.
    EXPECT_GT(peak, 1000u) << one.name;
    EXPECT_LT(peak, 30000u) << one.name;
  }
}

TEST(VorbisDecode, EveryFixtureDecodesToTheLengthItStated) {
  /*
   * **The frame count asserted exactly, before anything else is
   * compared**, which planning/audio.md section 12 puts first in its
   * list for a reason: a decoder one block short passes any comparison
   * that aligns the two signals before measuring, and that is what
   * every convenient comparison does.
   *
   * For Vorbis the count is also the one thing the file states about
   * itself, so this is the loop closing: the length read from the last
   * page's granule position, and the length the packets actually decode
   * to, have to be the same number.
   */
  for (const auto & one : fixtures) {
    Loaded loaded;
    ASSERT_EQ(OpenFile(loaded, one.name), GAUD_OK) << one.name;
    GAUD_Track * track = loaded.track();
    GAUD_Decoder * decoder = nullptr;
    ASSERT_EQ(gaud_decoder_create(track, &decoder), GAUD_OK) << one.name;
    GAUD_Buffer * buffer = nullptr;
    ASSERT_EQ(gaud_decoder_buffer_create(decoder, nullptr, 317u, &buffer),
        GAUD_OK)
        << one.name;
    uint64_t total = 0;
    int64_t peak = 0;
    for (;;) {
      ASSERT_EQ(gaud_decoder_read(decoder, buffer), GAUD_OK) << one.name;
      size_t frames = gaud_buffer_frames(buffer);
      if (frames == 0) {
        break;
      }
      const int16_t * data
          = (const int16_t *)gaud_buffer_data_const(buffer);
      for (size_t i = 0; i < frames * one.channels; ++i) {
        int64_t magnitude = data[i] < 0 ? -(int64_t)data[i] : data[i];
        peak = std::max(peak, magnitude);
      }
      total += frames;
    }
    EXPECT_EQ(total, one.frames) << one.name;
    /* And it is not silence, which is the decoder failure that produces
     * the right number of bytes and sounds like a quiet passage. The
     * silence fixture is silence by construction and is the control for
     * this check rather than an exception to it. */
    if (std::string(one.name).find("silence") == std::string::npos) {
      EXPECT_GT(peak, 1000) << one.name << " decoded to near silence";
    }
    else {
      EXPECT_EQ(peak, 0) << one.name << " is silence and did not decode "
                         << "to silence";
    }
    gaud_buffer_destroy(buffer);
    gaud_decoder_destroy(decoder);
  }
}

TEST(VorbisDecode, TheBufferSizeDoesNotChangeTheSamples) {
  /*
   * A decode is a function of its input. The block size a caller asks
   * for is its own business, and a decoder whose output depended on it
   * would have state leaking across a read boundary - which for this
   * codec would be the lap, the one piece of state that spans packets.
   *
   * The sizes are chosen to be awkward: one far smaller than a block,
   * one a prime, one larger than the longest block.
   */
  for (const char * name : {"vorbis_lib_stereo_44100.ogg",
           "vorbis_lib_transient_44100.ogg", "vorbis_lib_5dot1_48000.ogg"}) {
    std::vector<int16_t> reference;
    for (size_t size : {4096u, 7u, 317u, 65536u}) {
      Loaded loaded;
      ASSERT_EQ(OpenFile(loaded, name), GAUD_OK) << name;
      GAUD_Decoder * decoder = nullptr;
      ASSERT_EQ(gaud_decoder_create(loaded.track(), &decoder), GAUD_OK);
      GAUD_Buffer * buffer = nullptr;
      ASSERT_EQ(
          gaud_decoder_buffer_create(decoder, nullptr, size, &buffer),
          GAUD_OK);
      unsigned channels = gaud_track_layout(loaded.track()).channels;
      std::vector<int16_t> got;
      for (;;) {
        ASSERT_EQ(gaud_decoder_read(decoder, buffer), GAUD_OK);
        size_t frames = gaud_buffer_frames(buffer);
        if (frames == 0) {
          break;
        }
        const int16_t * data
            = (const int16_t *)gaud_buffer_data_const(buffer);
        got.insert(got.end(), data, data + frames * channels);
      }
      gaud_buffer_destroy(buffer);
      gaud_decoder_destroy(decoder);
      if (reference.empty()) {
        reference = got;
        ASSERT_GT(reference.size(), 0u) << name;
      }
      else {
        EXPECT_EQ(got, reference) << name << " with a buffer of " << size;
      }
    }
  }
}

TEST(VorbisDecode, TheChannelOrderIsTheOneBufferHPromises) {
  /*
   * **`buffer.h` says the order is WAV's `dwChannelMask`** and that
   * every other container's is mapped onto it on the way in. Vorbis's
   * own order is not that one: from three channels up it puts the
   * centre second and the low-frequency channel last, where WAV puts
   * the centre third and the low-frequency fourth.
   *
   * One and two channels are the identity, so this is invisible until a
   * file has three - and the corpus has exactly one such file, with six.
   * It was found by that file disagreeing with ffmpeg, which reorders
   * to WAV, while agreeing with libsndfile, which hands back the
   * stream's own order.
   *
   * What is asserted here is the table rather than a decode, because a
   * decode can only check the counts the corpus happens to have. The
   * specification's order for each count is written out, the table is
   * applied to it, and the result must be the mask's own bits in
   * increasing order - which is what "WAV order" means. Three, five and
   * seven channels have no fixture and are checked here and nowhere
   * else.
   */
  struct Count {
    unsigned channels;
    uint32_t vorbis_order[8]; /* The specification's, section 4.3.9. */
  };
  const Count counts[] = {
      {1, {GAUD_CH_FRONT_CENTER}},
      {2, {GAUD_CH_FRONT_LEFT, GAUD_CH_FRONT_RIGHT}},
      {3, {GAUD_CH_FRONT_LEFT, GAUD_CH_FRONT_CENTER, GAUD_CH_FRONT_RIGHT}},
      {4, {GAUD_CH_FRONT_LEFT, GAUD_CH_FRONT_RIGHT, GAUD_CH_BACK_LEFT,
              GAUD_CH_BACK_RIGHT}},
      {5, {GAUD_CH_FRONT_LEFT, GAUD_CH_FRONT_CENTER, GAUD_CH_FRONT_RIGHT,
              GAUD_CH_BACK_LEFT, GAUD_CH_BACK_RIGHT}},
      {6, {GAUD_CH_FRONT_LEFT, GAUD_CH_FRONT_CENTER, GAUD_CH_FRONT_RIGHT,
              GAUD_CH_BACK_LEFT, GAUD_CH_BACK_RIGHT,
              GAUD_CH_LOW_FREQUENCY}},
      {7, {GAUD_CH_FRONT_LEFT, GAUD_CH_FRONT_CENTER, GAUD_CH_FRONT_RIGHT,
              GAUD_CH_SIDE_LEFT, GAUD_CH_SIDE_RIGHT, GAUD_CH_BACK_CENTER,
              GAUD_CH_LOW_FREQUENCY}},
      {8, {GAUD_CH_FRONT_LEFT, GAUD_CH_FRONT_CENTER, GAUD_CH_FRONT_RIGHT,
              GAUD_CH_SIDE_LEFT, GAUD_CH_SIDE_RIGHT, GAUD_CH_BACK_LEFT,
              GAUD_CH_BACK_RIGHT, GAUD_CH_LOW_FREQUENCY}},
  };
  for (const auto & one : counts) {
    GAUD_Channel_Layout layout = gaud_channel_layout_default(one.channels);
    ASSERT_EQ(layout.channels, one.channels);
    /* The mask's bits in increasing order: this library's slot order. */
    std::vector<uint32_t> wanted;
    for (unsigned bit = 0; bit < 32u; ++bit) {
      if (layout.mask & (1u << bit)) {
        wanted.push_back(1u << bit);
      }
    }
    ASSERT_EQ(wanted.size(), one.channels) << one.channels;
    std::vector<uint32_t> got;
    for (unsigned slot = 0; slot < one.channels; ++slot) {
      got.push_back(one.vorbis_order[gaud_vorbis_channel_slot(
          one.channels, slot)]);
    }
    EXPECT_EQ(got, wanted) << one.channels << " channels: the permutation "
                           << "does not put the stream's channels into the "
                           << "slots the mask names";
    /* And it is a permutation, not merely a mapping. */
    std::vector<unsigned> seen;
    for (unsigned slot = 0; slot < one.channels; ++slot) {
      unsigned which = gaud_vorbis_channel_slot(one.channels, slot);
      EXPECT_LT(which, one.channels) << one.channels;
      EXPECT_EQ(std::find(seen.begin(), seen.end(), which), seen.end())
          << one.channels << ": channel " << which << " twice";
      seen.push_back(which);
    }
  }
}

TEST(VorbisDecode, ASeekLandsOnTheFrameAskedFor) {
  /*
   * Seeking is linear here - the bisection exists and Vorbis does not
   * use it yet, for the reason vorbis_decode.c gives - so what this
   * checks is the arithmetic rather than the search: that the position
   * reported is the one asked for, and that the samples from there are
   * the ones a straight decode produces at that offset.
   *
   * The second half is what makes it a test. A seek that reset the lap
   * and decoded forward would land on the right *frame* and produce
   * different *samples* for the first block after it, because a Vorbis
   * block's output depends on the one before it.
   */
  const char * name = "vorbis_lib_stereo_44100.ogg";
  Loaded whole;
  ASSERT_EQ(OpenFile(whole, name), GAUD_OK);
  unsigned channels = gaud_track_layout(whole.track()).channels;
  std::vector<int16_t> all;
  {
    GAUD_Decoder * decoder = nullptr;
    ASSERT_EQ(gaud_decoder_create(whole.track(), &decoder), GAUD_OK);
    GAUD_Buffer * buffer = nullptr;
    ASSERT_EQ(gaud_decoder_buffer_create(decoder, nullptr, 1024u, &buffer),
        GAUD_OK);
    for (;;) {
      ASSERT_EQ(gaud_decoder_read(decoder, buffer), GAUD_OK);
      size_t frames = gaud_buffer_frames(buffer);
      if (frames == 0) {
        break;
      }
      const int16_t * data
          = (const int16_t *)gaud_buffer_data_const(buffer);
      all.insert(all.end(), data, data + frames * channels);
    }
    gaud_buffer_destroy(buffer);
    gaud_decoder_destroy(decoder);
  }
  ASSERT_EQ(all.size(), 4409u * channels);

  Loaded loaded;
  ASSERT_EQ(OpenFile(loaded, name), GAUD_OK);
  GAUD_Decoder * decoder = nullptr;
  ASSERT_EQ(gaud_decoder_create(loaded.track(), &decoder), GAUD_OK);
  GAUD_Buffer * buffer = nullptr;
  ASSERT_EQ(gaud_decoder_buffer_create(decoder, nullptr, 64u, &buffer),
      GAUD_OK);
  for (uint64_t target : {0u, 1u, 1023u, 1024u, 1025u, 3000u, 100u, 4408u}) {
    uint64_t landed = UINT64_MAX;
    ASSERT_EQ(gaud_decoder_seek(decoder, target, &landed), GAUD_OK)
        << target;
    EXPECT_EQ(landed, target) << target;
    ASSERT_EQ(gaud_decoder_read(decoder, buffer), GAUD_OK) << target;
    size_t frames = gaud_buffer_frames(buffer);
    ASSERT_GT(frames, 0u) << target;
    const int16_t * data = (const int16_t *)gaud_buffer_data_const(buffer);
    size_t compare = std::min(frames, (size_t)(4409u - target));
    for (size_t i = 0; i < compare * channels; ++i) {
      ASSERT_EQ(data[i], all[target * channels + i])
          << "seek to " << target << ", sample " << i;
    }
  }
  gaud_buffer_destroy(buffer);
  gaud_decoder_destroy(decoder);
}

TEST(VorbisDecode, TheCapabilityBitAndTheDecoderAgree) {
  const GAUD_Codec * codec = gaud_registry_find(NULL, "vorbis");
  ASSERT_NE(codec, nullptr);
  EXPECT_TRUE((codec->capabilities & GAUD_CAP_DECODE) != 0);
  EXPECT_TRUE((codec->capabilities & GAUD_CAP_METADATA_READ) != 0);
  /* No encoder yet, and the tier says so rather than a README. */
  EXPECT_EQ(codec->encoder_tier, GAUD_ENCODER_NONE);
  EXPECT_EQ(codec->encoder_open, nullptr);
}

/* ------------------------------------------- the identification header */

TEST(VorbisHeader, TheBlockSizeNibblesAreTheWayRoundTheSpecificationSays) {
  /*
   * **The one field in this header that can be wrong and look right.**
   * blocksize_0 is the *low* nibble and is the short block; every
   * libvorbis stream at a normal rate writes 0xB8, which is 256 and
   * 2,048 - and read backwards it is 2,048 and 256, which is a legal pair
   * in the other order and would be refused by the "short is not larger
   * than long" check. So the value that catches a swap is one where both
   * readings are legal: 0xA9 is 512 and 1,024 the right way round, and
   * 1,024 and 512 the wrong way, and both pass every bound.
   */
  VORBIS_Info info;
  auto packet = Identification(0, 2, 22050, 0xA9u);
  ASSERT_EQ(gaud_vorbis_parse_identification(
                packet.data(), packet.size(), &info),
      GAUD_OK);
  EXPECT_EQ(info.blocksize_short, 512u);
  EXPECT_EQ(info.blocksize_long, 1024u);

  /* And the pair every encoder writes, which is the one a swap would
   * have been noticed on. */
  packet = Identification(0, 2, 44100, 0xB8u);
  ASSERT_EQ(gaud_vorbis_parse_identification(
                packet.data(), packet.size(), &info),
      GAUD_OK);
  EXPECT_EQ(info.blocksize_short, 256u);
  EXPECT_EQ(info.blocksize_long, 2048u);
}

TEST(VorbisHeader, EveryBoundTheSpecificationStatesIsChecked) {
  VORBIS_Info info;
  struct Case {
    const char * what;
    std::vector<unsigned char> packet;
    GAUD_Result expected;
  };
  const Case cases[] = {
      {"a legal header", Identification(), GAUD_OK},
      /* A version this does not know is a different bitstream wearing
       * the same signature, so FORMAT - the file is not wrong, it is not
       * ours - and the registry may go on to ask another codec. */
      {"version 1", Identification(1), GAUD_ERR_FORMAT},
      {"version 0xFFFFFFFF", Identification(0xFFFFFFFFu),
          GAUD_ERR_FORMAT},
      {"no channels", Identification(0, 0), GAUD_ERR_CORRUPT},
      {"no sample rate", Identification(0, 2, 0), GAUD_ERR_CORRUPT},
      /* 2^5 = 32, below the 64 the format allows. */
      {"a block below the minimum", Identification(0, 2, 44100, 0xB5u),
          GAUD_ERR_CORRUPT},
      /* 2^14 = 16,384, above the 8,192 the format allows. */
      {"a block above the maximum", Identification(0, 2, 44100, 0xE8u),
          GAUD_ERR_CORRUPT},
      /* Short larger than long, which the format forbids outright. */
      {"the short block larger", Identification(0, 2, 44100, 0x8Bu),
          GAUD_ERR_CORRUPT},
      /* The framing bit's whole job is to be set. A packet truncated at
       * exactly the right place looks whole without it. */
      {"a cleared framing bit", Identification(0, 2, 44100, 0xB8u, 0),
          GAUD_ERR_CORRUPT},
  };
  for (const auto & one : cases) {
    EXPECT_EQ(gaud_vorbis_parse_identification(
                  one.packet.data(), one.packet.size(), &info),
        one.expected)
        << one.what;
  }

  /* A packet one byte short of the header, which is the length at which
   * every field above is readable except the framing bit. */
  auto truncated = Identification();
  truncated.pop_back();
  EXPECT_EQ(gaud_vorbis_parse_identification(
                truncated.data(), truncated.size(), &info),
      GAUD_ERR_CORRUPT);

  /* And the signature, which is what separates "not a Vorbis header" from
   * "a broken one". Every other packet type is a header of some kind and
   * none of them is this one. */
  for (unsigned type : {0u, 2u, 3u, 5u, 255u}) {
    auto wrong = Identification();
    wrong[0] = (unsigned char)type;
    EXPECT_EQ(gaud_vorbis_parse_identification(
                  wrong.data(), wrong.size(), &info),
        GAUD_ERR_FORMAT)
        << "packet type " << type;
  }
  auto misspelled = Identification();
  misspelled[3] = 'R';
  EXPECT_EQ(gaud_vorbis_parse_identification(
                misspelled.data(), misspelled.size(), &info),
      GAUD_ERR_FORMAT);
}

/* -------------------------------------------------------------- the probe */

TEST(VorbisProbe, OggIsFourFormatsAndTheProbeDecidesWhich) {
  /*
   * `OggS` is shared by FLAC, Vorbis, Opus, Speex and Theora, and two of
   * those are registered here - so the magic selects a family and the
   * probe decides which member. The assertion is in both directions: a
   * Vorbis file must not open as Ogg FLAC and an Ogg FLAC file must not
   * open as Vorbis, and the registry picking the right one is what
   * gaud_doc_codec_name() reports.
   */
  Loaded vorbis;
  ASSERT_EQ(OpenFile(vorbis, "vorbis_lib_stereo_44100.ogg"), GAUD_OK);
  EXPECT_STREQ(gaud_doc_codec_name(vorbis.doc), "vorbis");

  Loaded flac;
  ASSERT_EQ(OpenFile(flac, "oggflac_lib_s16_stereo_44100.oga"), GAUD_OK);
  EXPECT_STREQ(gaud_doc_codec_name(flac.doc), "ogg-flac");
}

TEST(VorbisProbe, AnOggFileOfSomeOtherMappingIsDeclinedAndNotGuessedAt) {
  /*
   * A page whose first packet is neither mapping's. Hand-built, because
   * the corpus has no such file and the point is the answer rather than
   * the file: ::GAUD_ERR_FORMAT from every codec means the registry ran
   * out of candidates, which is a different answer from a Vorbis stream
   * that is broken.
   */
  GAUD_Stream * out = nullptr;
  ASSERT_EQ(gaud_stream_create_memory_writer(nullptr, &out), GAUD_OK);
  OGG_Writer writer;
  gaud_ogg_writer_init(&writer, out, 1u);
  const unsigned char speex[] = {'S', 'p', 'e', 'e', 'x', ' ', ' ', ' ',
      '1', '.', '2', 0, 0, 0, 0, 0};
  ASSERT_EQ(gaud_ogg_writer_packet(&writer, speex, sizeof(speex), 0u, true,
                true),
      GAUD_OK);
  const void * bytes = nullptr;
  size_t length = 0;
  ASSERT_EQ(gaud_stream_writer_bytes(out, &bytes, &length), GAUD_OK);
  std::vector<unsigned char> file((const unsigned char *)bytes,
      (const unsigned char *)bytes + length);
  gaud_stream_destroy(out);

  GAUD_Stream * in = nullptr;
  ASSERT_EQ(gaud_stream_create_memory(file.data(), file.size(), &in),
      GAUD_OK);
  GAUD_Doc * doc = nullptr;
  EXPECT_NE(gaud_doc_load(nullptr, in, nullptr, nullptr, &doc), GAUD_OK);
  EXPECT_EQ(doc, nullptr);
  gaud_stream_destroy(in);
}

TEST(VorbisLoad, AnUnseekableStreamIsRefusedWithAReason) {
  /*
   * The same refusal mp3_load.c makes, for a stronger reason: an MPEG
   * stream's length can at least be estimated from its bitrate, and a
   * Vorbis stream's cannot be estimated from anything. The length is one
   * number on the last page and there is no second way to reach it.
   */
  GAUD_Stream * file = nullptr;
  ASSERT_EQ(gaud_stream_create_file(
                Fixture("vorbis_lib_stereo_44100.ogg").c_str(), &file),
      GAUD_OK);
  GAUD_Stream * pipe = nullptr;
  ASSERT_EQ(gaud_stream_create_unseekable(file, &pipe), GAUD_OK);
  GAUD_Diagnostics diagnostics;
  gaud_diagnostics_init(&diagnostics, nullptr);
  GAUD_Doc * doc = nullptr;
  /* The codec's own open rather than gaud_doc_load(), for the reason the
   * MPEG version of this test gives: the registry cannot even probe an
   * unseekable stream - a probe has to read and then put the position
   * back - so gaud_doc_load() answers ::GAUD_ERR_FORMAT and the
   * refusal being tested here never runs. */
  const GAUD_Codec * codec = gaud_registry_find(nullptr, "vorbis");
  ASSERT_NE(codec, nullptr);
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  EXPECT_EQ(codec->open(codec, pipe, &limits, &diagnostics, &doc),
      GAUD_ERR_UNSUPPORTED);
  EXPECT_EQ(doc, nullptr);
  ASSERT_GE(diagnostics.count, 1u);
  EXPECT_NE(
      std::string(diagnostics.items[0].recommended_action).find("seekable"),
      std::string::npos);
  gaud_diagnostics_destroy(&diagnostics);
  gaud_stream_destroy(pipe);
  gaud_stream_destroy(file);
}

/* --------------------------------------------- the bit packing and ilog */

namespace {

/** A least-significant-bit-first writer, for building packets by hand. */
class BitWriter {
public:
  void put(uint32_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) {
      if (at_ == 0) {
        bytes_.push_back(0);
      }
      if ((value >> i) & 1u) {
        bytes_.back() |= (unsigned char)(1u << at_);
      }
      at_ = (at_ + 1u) & 7u;
    }
  }
  const std::vector<unsigned char> & bytes() const {
    return bytes_;
  }

private:
  std::vector<unsigned char> bytes_;
  unsigned at_ = 0;
};

/** The 32-bit form of @p value as the specification's float32 packing. */
uint32_t Float32(int mantissa, int exponent) {
  uint32_t packed = (uint32_t)(mantissa < 0 ? -mantissa : mantissa);
  packed |= (uint32_t)(exponent + 788) << 21;
  if (mantissa < 0) {
    packed |= 0x80000000u;
  }
  return packed;
}

} // namespace

TEST(VorbisBits, TheFirstBitReadIsTheLeastSignificant) {
  /*
   * **The single most consequential difference between reading Vorbis and
   * reading anything else here.** FLAC and MPEG take a field's first bit
   * from the most significant end; Vorbis takes it from the least. A
   * reader that got it backwards reads the identification header
   * perfectly - it is byte-aligned apart from two nibbles sharing a byte
   * - and reads every codebook as noise.
   *
   * The byte 0x4D is 0b01001101. Read as 8 one-bit fields the answers are
   * 1,0,1,1,0,0,1,0 - the bits from the bottom up. Read as one 8-bit
   * field the answer is 0x4D, which is the same either way and is why a
   * test on whole bytes cannot see the difference. Read as 3 then 5 the
   * answers are 5 and 9; the other order gives 2 and 13.
   */
  const unsigned char data[] = {0x4Du, 0xA2u};
  VORBIS_Bits bits;
  gaud_vorbis_bits_init(&bits, data, sizeof(data));
  const unsigned expected[] = {1u, 0u, 1u, 1u, 0u, 0u, 1u, 0u};
  for (unsigned i = 0; i < 8u; ++i) {
    EXPECT_EQ(gaud_vorbis_bits_read(&bits, 1), expected[i]) << i;
  }

  gaud_vorbis_bits_init(&bits, data, sizeof(data));
  EXPECT_EQ(gaud_vorbis_bits_read(&bits, 3), 5u);
  EXPECT_EQ(gaud_vorbis_bits_read(&bits, 5), 9u);

  /* And across a byte boundary, where the field's low bits come from the
   * first byte and its high bits from the second. */
  gaud_vorbis_bits_init(&bits, data, sizeof(data));
  EXPECT_EQ(gaud_vorbis_bits_read(&bits, 4), 0xDu);
  EXPECT_EQ(gaud_vorbis_bits_read(&bits, 8), 0x24u);
  EXPECT_FALSE(bits.past_end);
}

TEST(VorbisBits, ReadingPastTheEndYieldsZeroAndSaysSo) {
  /* Not an error by itself: the specification makes a truncated packet at
   * the end of a stream a legitimate end of decode. What must not happen
   * is a read of memory past the packet, and what must happen is that the
   * caller can tell. */
  const unsigned char data[] = {0xFFu};
  VORBIS_Bits bits;
  gaud_vorbis_bits_init(&bits, data, sizeof(data));
  EXPECT_EQ(gaud_vorbis_bits_read(&bits, 8), 0xFFu);
  EXPECT_FALSE(bits.past_end);
  EXPECT_EQ(gaud_vorbis_bits_read(&bits, 4), 0u);
  EXPECT_TRUE(bits.past_end);

  /* A read that straddles the end returns the bits it had and zeros for
   * the rest, which for an LSB-first field means the low bits are real. */
  gaud_vorbis_bits_init(&bits, data, sizeof(data));
  EXPECT_EQ(gaud_vorbis_bits_read(&bits, 4), 0xFu);
  EXPECT_EQ(gaud_vorbis_bits_read(&bits, 8), 0xFu);
  EXPECT_TRUE(bits.past_end);
}

TEST(VorbisBits, IlogIsABitCountAndNotALogarithm) {
  /*
   * They differ by exactly one at every power of two, and a reader that
   * used a logarithm reads a mode number or an ordered codebook's run
   * length with the wrong field width - which misaligns everything after
   * it. So the powers of two are the whole test, and the values between
   * them are there to show the function is not simply off by one.
   */
  EXPECT_EQ(gaud_vorbis_ilog(0u), 0u);
  EXPECT_EQ(gaud_vorbis_ilog(1u), 1u);
  EXPECT_EQ(gaud_vorbis_ilog(2u), 2u);
  EXPECT_EQ(gaud_vorbis_ilog(3u), 2u);
  EXPECT_EQ(gaud_vorbis_ilog(4u), 3u);
  EXPECT_EQ(gaud_vorbis_ilog(7u), 3u);
  EXPECT_EQ(gaud_vorbis_ilog(8u), 4u);
  EXPECT_EQ(gaud_vorbis_ilog(255u), 8u);
  EXPECT_EQ(gaud_vorbis_ilog(256u), 9u);
  EXPECT_EQ(gaud_vorbis_ilog(0xFFFFFFFFu), 32u);
}

TEST(VorbisBits, Lookup1ValuesIsComputedAndNotEstimated) {
  /*
   * How many multiplicands a lattice codebook stores: the greatest r with
   * r to the dimensions not above the entries. **By search and not by
   * `pow`**, because `floor(pow(entries, 1.0/dim))` is off by one at
   * exact powers on some platforms - and that would make a codebook's
   * multiplicand count differ by architecture, so every bit read after it
   * would be misaligned on one machine and not another. Section 11.1
   * promises the same bytes everywhere; this is one of the places that
   * could quietly break it.
   *
   * The exact powers are therefore the test. 6,561 is 3 to the 8th and
   * appears in this corpus; 6,560 must give 2.
   */
  EXPECT_EQ(gaud_vorbis_lookup1_values(6561u, 8u), 3u);
  EXPECT_EQ(gaud_vorbis_lookup1_values(6560u, 8u), 2u);
  EXPECT_EQ(gaud_vorbis_lookup1_values(6562u, 8u), 3u);
  EXPECT_EQ(gaud_vorbis_lookup1_values(1u, 1u), 1u);
  EXPECT_EQ(gaud_vorbis_lookup1_values(100u, 1u), 100u);
  EXPECT_EQ(gaud_vorbis_lookup1_values(289u, 2u), 17u);
  EXPECT_EQ(gaud_vorbis_lookup1_values(288u, 2u), 16u);
  /* 2^31 is 2 to the 31st exactly, where a double has no room to be
   * wrong but a float would. */
  EXPECT_EQ(gaud_vorbis_lookup1_values(0x80000000u, 31u), 2u);
  EXPECT_EQ(gaud_vorbis_lookup1_values(0x7FFFFFFFu, 31u), 1u);
}

/* ------------------------------------------------------------ codebooks */

namespace {

/** A codebook packet with the given lengths and no lookup. */
std::vector<unsigned char> Codebook(const std::vector<unsigned> & lengths,
    bool ordered = false, unsigned dimensions = 1u) {
  BitWriter w;
  w.put(0x564342u, 24);
  w.put(dimensions, 16);
  w.put((uint32_t)lengths.size(), 24);
  w.put(ordered ? 1u : 0u, 1);
  if (!ordered) {
    bool sparse = false;
    for (unsigned length : lengths) {
      if (length == 0) {
        sparse = true;
      }
    }
    w.put(sparse ? 1u : 0u, 1);
    for (unsigned length : lengths) {
      if (sparse) {
        w.put(length ? 1u : 0u, 1);
        if (!length) {
          continue;
        }
      }
      w.put(length - 1u, 5);
    }
  }
  else {
    /* Runs: the first length, then how many entries share it, then how
     * many share the next, and so on. The run width shrinks as entries
     * are filled, which is what ilog is for. */
    size_t filled = 0;
    unsigned length = lengths[0];
    while (filled < lengths.size()) {
      size_t run = 0;
      while (filled + run < lengths.size()
          && lengths[filled + run] == length) {
        ++run;
      }
      if (filled == 0) {
        w.put(length - 1u, 5);
      }
      w.put((uint32_t)run,
          gaud_vorbis_ilog((uint32_t)(lengths.size() - filled)));
      filled += run;
      ++length;
    }
  }
  w.put(0, 4); /* lookup_type 0 */
  return w.bytes();
}

} // namespace

TEST(VorbisCodebook, TheSpecificationsWorkedExampleComesOutExactly) {
  /*
   * **A codebook states lengths and not codewords**, and this is the one
   * test that can show the assignment is right rather than merely
   * self-consistent: the eight lengths and the eight codewords below are
   * the worked example in the Vorbis I specification's codebook section,
   * copied from the document and not from this implementation.
   *
   * There is no partial credit here. A decoder that assigned codewords
   * differently reads every entry of every codebook wrong, which looks
   * like noise rather than like a parse failure - so a stream either
   * decodes or does not, and nothing in between would point at this.
   *
   * Note that entry order and not length order is what drives it: entry 5
   * has length 2 and comes after four entries of length 4, and its
   * codeword is 10. A canonical Huffman code - lengths sorted first -
   * would give a different answer for the same multiset of lengths.
   */
  const std::vector<unsigned> lengths = {2u, 4u, 4u, 4u, 4u, 2u, 3u, 3u};
  const char * expected[] = {"00", "0100", "0101", "0110", "0111", "10",
      "110", "111"};

  std::vector<unsigned char> packet = Codebook(lengths);
  VORBIS_Bits bits;
  gaud_vorbis_bits_init(&bits, packet.data(), packet.size());
  VORBIS_Codebook book;
  ASSERT_EQ(gaud_vorbis_parse_codebook(&bits, nullptr, &book), GAUD_OK);
  ASSERT_EQ(book.entries, lengths.size());
  ASSERT_EQ(book.used, lengths.size());

  for (uint32_t k = 0; k < book.used; ++k) {
    uint32_t entry = book.entry_of[k];
    unsigned length = book.lengths[entry];
    std::string written;
    for (unsigned bit = 0; bit < length; ++bit) {
      written += ((book.codewords[k] >> (length - 1u - bit)) & 1u) ? '1'
                                                                   : '0';
    }
    EXPECT_EQ(written, std::string(expected[entry])) << "entry " << entry;
  }

  /* And the other direction: the codeword's bits, fed to the decoder,
   * must come back as the entry. The codeword is read most significant
   * bit first - the tree is walked from the root - which is the one place
   * in this format where bits are *not* taken least significant first,
   * and it falls out of the bit reader rather than being a special case:
   * one bit at a time is the same in either order. */
  for (size_t entry = 0; entry < lengths.size(); ++entry) {
    BitWriter w;
    const char * code = expected[entry];
    for (const char * c = code; *c; ++c) {
      w.put(*c == '1' ? 1u : 0u, 1);
    }
    /* A trailing one, so a decoder that read one bit too many lands
     * somewhere rather than running off the packet and reporting the
     * end. */
    w.put(1u, 1);
    std::vector<unsigned char> encoded = w.bytes();
    VORBIS_Bits reading;
    gaud_vorbis_bits_init(&reading, encoded.data(), encoded.size());
    EXPECT_EQ(gaud_vorbis_codebook_decode(&book, &reading), entry)
        << "codeword " << code;
  }
  gaud_vorbis_codebook_free(nullptr, &book);
}

TEST(VorbisCodebook, AnOverpopulatedTreeIsRefusedAndAnUnderpopulatedOneIsNot) {
  /*
   * Two directions, and they are not symmetric.
   *
   * **Overpopulated is refused**: three codewords of length one cannot
   * exist, because a one-bit code has two of them. Building the code is
   * the only way to find that out, which is the argument for building it
   * at open rather than decoding lazily.
   *
   * **Underpopulated is accepted**, because real streams contain them -
   * an encoder that reserved code space it then did not use writes one -
   * and the leftover nodes simply match no codeword. So the refusal has
   * to be at decode time and per bit pattern, not at parse time for the
   * whole book.
   */
  std::vector<unsigned char> over = Codebook({1u, 1u, 1u});
  VORBIS_Bits bits;
  gaud_vorbis_bits_init(&bits, over.data(), over.size());
  VORBIS_Codebook book;
  EXPECT_EQ(gaud_vorbis_parse_codebook(&bits, nullptr, &book),
      GAUD_ERR_CORRUPT);
  gaud_vorbis_codebook_free(nullptr, &book);

  /* Two codewords of length two leaves half the tree empty. */
  std::vector<unsigned char> under = Codebook({2u, 2u});
  gaud_vorbis_bits_init(&bits, under.data(), under.size());
  ASSERT_EQ(gaud_vorbis_parse_codebook(&bits, nullptr, &book), GAUD_OK);
  EXPECT_EQ(book.used, 2u);

  /* 00 and 01 are the two entries; 10 is a pattern no entry stands for. */
  const struct {
    const char * code;
    uint32_t entry;
  } cases[] = {{"00", 0u}, {"01", 1u}, {"10", UINT32_MAX},
      {"11", UINT32_MAX}};
  for (const auto & one : cases) {
    BitWriter w;
    for (const char * c = one.code; *c; ++c) {
      w.put(*c == '1' ? 1u : 0u, 1);
    }
    w.put(0u, 6);
    std::vector<unsigned char> encoded = w.bytes();
    VORBIS_Bits reading;
    gaud_vorbis_bits_init(&reading, encoded.data(), encoded.size());
    EXPECT_EQ(gaud_vorbis_codebook_decode(&book, &reading), one.entry)
        << one.code;
  }
  gaud_vorbis_codebook_free(nullptr, &book);
}

TEST(VorbisCodebook, AnOrderedBookStatesRunsAndASparseOneStatesGaps) {
  /*
   * Two ways of spelling the same lengths, and both are in the corpus -
   * 160 of its 363 codebooks are sparse. An ordered book's lengths are
   * non-decreasing by construction and are stated as run lengths, each
   * read with `ilog(entries remaining)` bits rather than a fixed width;
   * a sparse book states a flag per entry and omits the length where the
   * flag is clear.
   */
  const std::vector<unsigned> lengths = {2u, 2u, 3u, 3u, 3u, 3u};
  std::vector<unsigned char> packet = Codebook(lengths, true);
  VORBIS_Bits bits;
  gaud_vorbis_bits_init(&bits, packet.data(), packet.size());
  VORBIS_Codebook book;
  ASSERT_EQ(gaud_vorbis_parse_codebook(&bits, nullptr, &book), GAUD_OK);
  ASSERT_EQ(book.entries, lengths.size());
  for (size_t i = 0; i < lengths.size(); ++i) {
    EXPECT_EQ(book.lengths[i], lengths[i]) << i;
  }
  gaud_vorbis_codebook_free(nullptr, &book);

  /* Sparse: entries 1 and 3 unused, so the code is over three entries
   * and the unused ones have no codeword at all. */
  std::vector<unsigned char> sparse = Codebook({2u, 0u, 2u, 0u, 1u});
  gaud_vorbis_bits_init(&bits, sparse.data(), sparse.size());
  ASSERT_EQ(gaud_vorbis_parse_codebook(&bits, nullptr, &book), GAUD_OK);
  EXPECT_EQ(book.entries, 5u);
  EXPECT_EQ(book.used, 3u);
  EXPECT_EQ(book.lengths[1], 0u);
  EXPECT_EQ(book.lengths[3], 0u);
  gaud_vorbis_codebook_free(nullptr, &book);
}

namespace {

/** A lattice or explicit codebook with the given scalar parameters. */
std::vector<unsigned char> LookupCodebook(unsigned lookup_type,
    unsigned dimensions, uint32_t entries, int minimum_mantissa,
    int minimum_exponent, int delta_mantissa, int delta_exponent,
    unsigned value_bits, bool sequence_p,
    const std::vector<uint32_t> & multiplicands) {
  BitWriter w;
  w.put(0x564342u, 24);
  w.put(dimensions, 16);
  w.put(entries, 24);
  w.put(0, 1); /* not ordered */
  w.put(0, 1); /* not sparse */
  for (uint32_t i = 0; i < entries; ++i) {
    /* Every entry the same length, which for a power-of-two entry count
     * is a complete tree. The code is not what this test is about. */
    w.put(gaud_vorbis_ilog(entries - 1u) - 1u, 5);
  }
  w.put(lookup_type, 4);
  w.put(Float32(minimum_mantissa, minimum_exponent), 32);
  w.put(Float32(delta_mantissa, delta_exponent), 32);
  w.put(value_bits - 1u, 4);
  w.put(sequence_p ? 1u : 0u, 1);
  for (uint32_t one : multiplicands) {
    w.put(one, value_bits);
  }
  return w.bytes();
}

} // namespace

TEST(VorbisCodebook, ALatticeBookReadsTheEntryAsANumberInBaseR) {
  /*
   * A lattice codebook stores one multiplicand per axis and reads the
   * entry number as a number in that base, lowest digit first - which is
   * how a book of 6,561 entries fits in three multiplicands. Four
   * entries, two dimensions, base two:
   *
   *   entry 0 -> digits (0, 0)   entry 2 -> digits (0, 1)
   *   entry 1 -> digits (1, 0)   entry 3 -> digits (1, 1)
   *
   * **Lowest digit first, and that is the part a reader gets backwards.**
   * Entry 1 is (1, 0) and not (0, 1), so its vector is the second
   * multiplicand then the first. With a symmetric set of multiplicands
   * the two readings agree, which is why the values below are not
   * symmetric.
   */
  const std::vector<uint32_t> multiplicands = {1u, 5u};
  /* minimum -1, delta 1: value = -1 + multiplicand. */
  std::vector<unsigned char> packet = LookupCodebook(1u, 2u, 4u,
      -(1 << 20), -20, 1 << 20, -20, 4u, false, multiplicands);
  VORBIS_Bits bits;
  gaud_vorbis_bits_init(&bits, packet.data(), packet.size());
  VORBIS_Codebook book;
  ASSERT_EQ(gaud_vorbis_parse_codebook(&bits, nullptr, &book), GAUD_OK);
  ASSERT_NE(book.values, nullptr);
  EXPECT_EQ(book.lookup_type, 1u);

  const int expected[4][2] = {{0, 0}, {4, 0}, {0, 4}, {4, 4}};
  for (unsigned entry = 0; entry < 4u; ++entry) {
    for (unsigned j = 0; j < 2u; ++j) {
      EXPECT_EQ(book.values[entry * 2u + j], expected[entry][j] * VORBIS_ONE)
          << "entry " << entry << " value " << j;
    }
  }
  gaud_vorbis_codebook_free(nullptr, &book);
}

TEST(VorbisCodebook, AnExplicitBookAndASequentialOneAreBuiltByHand) {
  /*
   * **Two of the four arms `make vorbis-coverage` reports as dark**, and
   * the only way to reach either: no encoder in the oracle image emits a
   * lookup type 2 codebook at any setting - it stores entries times
   * dimensions multiplicands where a lattice stores one per axis, so it
   * is legal and enormous - and none sets `sequence_p`, which floor type
   * 0's codebooks use and which nothing else does.
   *
   * Both are in the format and both are therefore in this parser, so
   * both get a stream built for them here rather than being left to a
   * corpus that cannot produce one.
   */
  /* Explicit: three entries of two values, listed in order. */
  const std::vector<uint32_t> listed = {1u, 2u, 3u, 4u, 5u, 6u};
  std::vector<unsigned char> packet = LookupCodebook(2u, 2u, 3u, 0, 0,
      1 << 20, -20, 4u, false, listed);
  VORBIS_Bits bits;
  gaud_vorbis_bits_init(&bits, packet.data(), packet.size());
  VORBIS_Codebook book;
  ASSERT_EQ(gaud_vorbis_parse_codebook(&bits, nullptr, &book), GAUD_OK);
  ASSERT_NE(book.values, nullptr);
  EXPECT_EQ(book.lookup_type, 2u);
  for (unsigned k = 0; k < 6u; ++k) {
    EXPECT_EQ(book.values[k], (int32_t)(k + 1u) * VORBIS_ONE) << k;
  }
  gaud_vorbis_codebook_free(nullptr, &book);

  /* Sequential: the same book with `sequence_p`, where each value is
   * added to the one before it *within an entry*. So entry 0 is 1 then
   * 1+2=3, and entry 1 is 3 then 3+4=7 - the running sum restarting per
   * entry, which is the part a reader that carried it across entries
   * would get wrong on everything after the first. */
  std::vector<unsigned char> sequential = LookupCodebook(2u, 2u, 3u, 0, 0,
      1 << 20, -20, 4u, true, listed);
  gaud_vorbis_bits_init(&bits, sequential.data(), sequential.size());
  ASSERT_EQ(gaud_vorbis_parse_codebook(&bits, nullptr, &book), GAUD_OK);
  EXPECT_TRUE(book.sequence_p);
  const int expected[3][2] = {{1, 3}, {3, 7}, {5, 11}};
  for (unsigned entry = 0; entry < 3u; ++entry) {
    for (unsigned j = 0; j < 2u; ++j) {
      EXPECT_EQ(book.values[entry * 2u + j], expected[entry][j] * VORBIS_ONE)
          << "entry " << entry << " value " << j;
    }
  }
  gaud_vorbis_codebook_free(nullptr, &book);
}

TEST(VorbisCodebook, Float32UnpackIsExactForThePowersOfTwoAroundOne) {
  /*
   * The specification's float32 is a 21-bit mantissa and a power of two
   * biased by 788 - not an IEEE float, and a reader that treated the
   * four bytes as one would get a plausible-looking wrong answer. The
   * values below are exact in Q16, so the assertion is equality and not
   * a tolerance.
   *
   * The exponents around one are the interesting range: 2^-20 times
   * 2^20 is one, and the shift this library applies is
   * `exponent - 788 + 16`, which is zero at exponent 772 and changes
   * sign on either side - so these cases cover the left shift, the right
   * shift and the boundary between them.
   */
  struct Case {
    int mantissa;
    int exponent;
    int64_t expected_q16;
  };
  const Case cases[] = {
      {1 << 20, -20, VORBIS_ONE},            /* 1.0 */
      {1 << 20, -19, 2 * VORBIS_ONE},        /* 2.0 */
      {1 << 20, -21, VORBIS_ONE / 2},        /* 0.5 */
      {-(1 << 20), -20, -VORBIS_ONE},        /* -1.0 */
      {3 << 19, -20, 3 * VORBIS_ONE / 2},    /* 1.5 */
      /* 65,536.0, which Q16 cannot hold at all: the value saturates
       * rather than wrapping, which is what section 11.1's promise
       * requires - signed overflow is undefined, not modular, so a
       * wrapped value could differ between compilers. */
      {1 << 20, -4, INT32_MAX},
      /* The bottom of the format's range, where rounding to nearest is
       * what decides the answer. 2^-16 is exactly one in Q16; 2^-17 is
       * a half and rounds up; 2^-18 is a quarter and rounds down. A
       * truncating implementation answers 1, 0, 0 and differs from this
       * on the middle one. */
      {1, -16, 1},
      {1, -17, 1},
      {1, -18, 0},
      {0, 0, 0},
  };
  for (const auto & one : cases) {
    /* Read through a codebook, which is the only caller: a lattice of
     * one entry and one dimension whose single multiplicand is zero, so
     * the value is the minimum alone. */
    std::vector<unsigned char> packet = LookupCodebook(1u, 1u, 2u,
        one.mantissa, one.exponent, 0, 0, 4u, false, {0u, 0u});
    VORBIS_Bits bits;
    gaud_vorbis_bits_init(&bits, packet.data(), packet.size());
    VORBIS_Codebook book;
    ASSERT_EQ(gaud_vorbis_parse_codebook(&bits, nullptr, &book), GAUD_OK)
        << one.mantissa << " x 2^" << one.exponent;
    ASSERT_NE(book.values, nullptr);
    EXPECT_EQ(book.values[0], (int32_t)one.expected_q16)
        << one.mantissa << " x 2^" << one.exponent;
    gaud_vorbis_codebook_free(nullptr, &book);
  }
}

TEST(VorbisCodebook, AReservedLookupTypeIsRefusedRatherThanSkipped) {
  /* The format reserves 3 to 15. There is nothing to guess past one: the
   * fields that follow a lookup differ per type, so a reader that
   * ignored an unknown type would read the next codebook's sync pattern
   * out of the middle of this one's data. */
  for (unsigned type = 3u; type < 16u; ++type) {
    BitWriter w;
    w.put(0x564342u, 24);
    w.put(1u, 16);
    w.put(2u, 24);
    w.put(0, 1);
    w.put(0, 1);
    w.put(0u, 5);
    w.put(0u, 5);
    w.put(type, 4);
    w.put(0u, 32);
    std::vector<unsigned char> packet = w.bytes();
    VORBIS_Bits bits;
    gaud_vorbis_bits_init(&bits, packet.data(), packet.size());
    VORBIS_Codebook book;
    EXPECT_EQ(gaud_vorbis_parse_codebook(&bits, nullptr, &book),
        GAUD_ERR_CORRUPT)
        << "lookup type " << type;
    gaud_vorbis_codebook_free(nullptr, &book);
  }
}

/* --------------------------------------------------------- the setup header */

TEST(VorbisSetup, EveryFixturesSetupHeaderParsesAndIsRangeChecked) {
  /*
   * The counts are the probe's, which is to say measured: `make
   * vorbis-coverage` prints them and this asserts them, so a fixture
   * replaced by a differently-encoded one fails here rather than quietly
   * changing what the corpus covers.
   *
   * What is being checked beyond "it parses" is that every number a later
   * packet will index is in range. A mapping names a floor by number, a
   * residue names codebooks, a mode names a mapping; all of those come
   * out of a file, and checking them once here is what lets the audio
   * path index an array with no bound check in its inner loop.
   */
  struct Expected {
    const char * name;
    uint32_t codebooks;
    uint32_t floors;
    uint32_t residues;
    uint32_t mappings;
    uint32_t modes;
  };
  const Expected expected[] = {
      {"vorbis_lib_stereo_44100.ogg", 42u, 2u, 2u, 2u, 2u},
      {"vorbis_lib_mono_44100.ogg", 35u, 2u, 2u, 2u, 2u},
      {"vorbis_lib_transient_44100.ogg", 44u, 2u, 2u, 2u, 2u},
      {"vorbis_lib_noise_48000.ogg", 44u, 2u, 2u, 2u, 2u},
      {"vorbis_lib_silence_44100.ogg", 34u, 2u, 2u, 2u, 2u},
      /* One mode and one mapping: the 8 kHz stream has equal block sizes,
       * so there is nothing for a second mode to select. */
      {"vorbis_lib_mono_8000.ogg", 19u, 1u, 1u, 1u, 1u},
      {"vorbis_lib_mono_22050.ogg", 35u, 2u, 2u, 2u, 2u},
      /* Six channels: three floors and three residues, and two submaps
       * per mapping - the only fixture that reaches the submap
       * demultiplexer at all. */
      {"vorbis_lib_5dot1_48000.ogg", 43u, 3u, 3u, 2u, 2u},
      {"vorbis_ff_stereo_44100.ogg", 29u, 1u, 1u, 1u, 2u},
      {"vorbis_tagged_stereo_44100.ogg", 38u, 2u, 2u, 2u, 2u},
  };
  static_assert(sizeof(expected) / sizeof(expected[0])
          == sizeof(fixtures) / sizeof(fixtures[0]),
      "a fixture was added: give it a row here too");

  for (const auto & one : expected) {
    Loaded loaded;
    ASSERT_EQ(OpenFile(loaded, one.name), GAUD_OK) << one.name;
    VORBIS_File * state
        = static_cast<VORBIS_File *>(gaud_doc_private(loaded.doc));
    ASSERT_NE(state, nullptr) << one.name;
    const VORBIS_Setup * setup = &state->setup;
    EXPECT_EQ(setup->codebook_count, one.codebooks) << one.name;
    EXPECT_EQ(setup->floor_count, one.floors) << one.name;
    EXPECT_EQ(setup->residue_count, one.residues) << one.name;
    EXPECT_EQ(setup->mapping_count, one.mappings) << one.name;
    EXPECT_EQ(setup->mode_count, one.modes) << one.name;
    EXPECT_EQ(setup->mode_bits, gaud_vorbis_ilog(one.modes - 1u))
        << one.name;

    for (uint32_t i = 0; i < setup->mode_count; ++i) {
      EXPECT_LT(setup->modes[i].mapping, setup->mapping_count) << one.name;
    }
    for (uint32_t i = 0; i < setup->mapping_count; ++i) {
      const VORBIS_Mapping * mapping = &setup->mappings[i];
      for (uint32_t j = 0; j < mapping->submaps; ++j) {
        EXPECT_LT(mapping->floor[j], setup->floor_count) << one.name;
        EXPECT_LT(mapping->residue[j], setup->residue_count) << one.name;
      }
      for (uint32_t j = 0; j < mapping->coupling_steps; ++j) {
        EXPECT_LT(mapping->magnitude[j], one.codebooks) << one.name;
        EXPECT_NE(mapping->magnitude[j], mapping->angle[j]) << one.name;
      }
      for (uint32_t ch = 0; ch < state->info.channels; ++ch) {
        EXPECT_LT(mapping->mux[ch], mapping->submaps) << one.name;
      }
    }
  }
}

TEST(VorbisSetup, TheCodebookValuesStayWellInsideWhatQSixteenHolds) {
  /*
   * **The measurement that chose VORBIS_Q, as an assertion.** The first
   * draft was Q20 and saturated on two of these ten fixtures, which is
   * how the real extreme came to be measured rather than assumed: 7,448,
   * needing 13 integer bits, with every value in the corpus an integer
   * because a residue codebook quantises the spectrum and the floor
   * carries the scale.
   *
   * The bound asserted is a quarter of what the word holds, which leaves
   * the two-times margin a value from a differently-configured encoder
   * would need. `make vorbis-coverage` prints the number; this is what
   * fails if a fixture added later moves it.
   */
  int64_t extreme = 0;
  int64_t smallest = 0;
  size_t values = 0;
  for (const auto & one : fixtures) {
    Loaded loaded;
    ASSERT_EQ(OpenFile(loaded, one.name), GAUD_OK) << one.name;
    VORBIS_File * state
        = static_cast<VORBIS_File *>(gaud_doc_private(loaded.doc));
    const VORBIS_Setup * setup = &state->setup;
    for (uint32_t i = 0; i < setup->codebook_count; ++i) {
      const VORBIS_Codebook * book = &setup->codebooks[i];
      if (!book->values) {
        continue;
      }
      size_t count = (size_t)book->entries * book->dimensions;
      values += count;
      for (size_t k = 0; k < count; ++k) {
        int64_t magnitude = book->values[k] < 0 ? -(int64_t)book->values[k]
                                                : book->values[k];
        if (magnitude > extreme) {
          extreme = magnitude;
        }
        if (magnitude && (smallest == 0 || magnitude < smallest)) {
          smallest = magnitude;
        }
      }
    }
  }
  EXPECT_GT(values, 100000u)
      << "the corpus has almost no codebook vectors in it, so the bound "
      << "below is not measuring anything";
  EXPECT_EQ(extreme, 7448LL * VORBIS_ONE)
      << "the largest codebook value is not the measured 7,448; "
      << "VORBIS_Q and vorbis-coverage both need re-reading";
  EXPECT_EQ(smallest, (int64_t)VORBIS_ONE)
      << "the smallest nonzero codebook value is not one, so some "
      << "encoder here no longer quantises to integers and the "
      << "fractional half of Q16 is now load-bearing";
  EXPECT_LT(extreme, (int64_t)INT32_MAX / 4)
      << "a codebook value is within a factor of four of saturating";
}

/* ------------------------------------------------ the inverse transform */

namespace {

/** The specification's own inverse transform, O(n^2), in double. */
std::vector<double> ImdctDirect(const std::vector<double> & spectrum,
    unsigned n) {
  const unsigned m = n / 2u;
  std::vector<double> out(n);
  for (unsigned i = 0; i < n; ++i) {
    double sum = 0.0;
    for (unsigned k = 0; k < m; ++k) {
      sum += spectrum[k]
          * std::cos(M_PI / m * ((double)i + 0.5 + (double)n / 4.0)
              * ((double)k + 0.5));
    }
    out[i] = sum;
  }
  return out;
}

} // namespace

TEST(VorbisTransform, TheIntegerTransformIsTheSpecificationsFormula) {
  /*
   * **The one part of this decoder whose answer is not exactly the
   * specification's**, so it is the one that needs a tolerance rather
   * than an equality - and the reference it is compared against is the
   * specification's own formula, evaluated in double here, rather than
   * another decoder. A disagreement is then this library's arithmetic
   * and not a question about whose rounding is right.
   *
   * Two spectra per block size, and they test different things:
   *
   *   - **a single coefficient**, which the transform returns at its own
   *     amplitude and whose output is a pure cosine. This is the case
   *     that catches the rotation's offset: 1/4 instead of 1/8 gives a
   *     cosine of very slightly the wrong frequency, which is a few
   *     percent of error spread over the block rather than a visible
   *     break.
   *   - **a full spectrum**, which is where the transform's gain is
   *     about the square root of the coefficient count and where the
   *     halving every second stage has to track it. A version that
   *     halved every stage passes the single-coefficient case and comes
   *     out quiet here.
   *
   * **The tolerance is in units of the output's own least significant
   * bit, not of the block's peak**, and the first draft had it the other
   * way - which penalised a quiet block for being quiet. What matters is
   * whether the error can move a 16-bit sample, so the error is measured
   * against full scale and the bound is two of the 32,768ths that a
   * 16-bit sample is quantised to.
   *
   * Measured, worst case over every block size and both spectra: **0.55
   * of a least significant bit**, at n = 8,192 with a full spectrum -
   * which is where eleven transform stages and two rotations have each
   * contributed their rounding. The bound of two leaves room for a
   * compiler to order a sum differently without leaving room for a
   * defect: the factor-of-two error the halving count had was 100% of
   * peak, and the rotation's wrong offset is a few percent.
   */
  double observed = 0.0;
  for (unsigned n : {64u, 128u, 256u, 512u, 1024u, 2048u, 4096u, 8192u}) {
    const unsigned m = n / 2u;
    for (int which = 0; which < 2; ++which) {
      std::vector<double> wanted(m, 0.0);
      std::vector<int32_t> spectrum(m, 0);
      if (which == 0) {
        /* One coefficient, a quarter of the way up, at 0.5. */
        wanted[m / 4u] = 0.5;
      }
      else {
        /* A deterministic pseudo-random spectrum, scaled so the output
         * stays inside what the time-domain word holds: the transform's
         * gain here is about sqrt(m), so 1/sqrt(m) of full scale in is
         * about full scale out. */
        double scale = 1.0 / std::sqrt((double)m);
        uint32_t state = 12345u + n;
        for (unsigned k = 0; k < m; ++k) {
          state = state * 1103515245u + 12345u;
          double value = (double)(int32_t)(state >> 8) / 2147483648.0;
          wanted[k] = value * scale;
        }
      }
      for (unsigned k = 0; k < m; ++k) {
        spectrum[k] = (int32_t)std::llround(wanted[k] * (1 << VORBIS_SPECTRUM_Q));
      }

      std::vector<int32_t> scratch(n);
      std::vector<int32_t> got(n);
      gaud_vorbis_imdct(spectrum.data(), n, scratch.data(), got.data());
      unsigned shift = gaud_vorbis_imdct_shift(n);
      double unit = (double)(1u << (VORBIS_SPECTRUM_Q - shift));

      std::vector<double> expected = ImdctDirect(wanted, n);
      double peak = 0.0;
      for (double value : expected) {
        peak = std::max(peak, std::abs(value));
      }
      ASSERT_GT(peak, 1e-6) << "n=" << n << " case " << which
                            << ": the reference output is silence, so this "
                               "compares nothing";
      double worst = 0.0;
      for (unsigned i = 0; i < n; ++i) {
        worst = std::max(worst, std::abs((double)got[i] / unit
            - expected[i]));
      }
      /* One 16-bit least significant bit, as a fraction of full scale. */
      const double lsb = 1.0 / 32768.0;
      EXPECT_LT(worst / lsb, 2.0)
          << "n=" << n << " case " << which << ": worst " << worst
          << " is " << (worst / lsb) << " least significant bits of 16, "
          << "against a block peak of " << peak;
      if (worst / lsb > observed) {
        observed = worst / lsb;
      }
    }
  }
  /* **And the other direction**: an error of zero would mean the
   * comparison is against this library's own answer rather than against
   * the formula, which is what a test that accidentally called the same
   * code twice would report. A fixed-point transform cannot be exact. */
  EXPECT_GT(observed, 1e-3)
      << "the integer transform agrees with a double-precision formula to "
      << "better than a thousandth of a least significant bit, which it "
      << "cannot: the two sides are probably the same code";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

/* ------------------------------------------------- floor 0's refusals */

TEST(VorbisFloor0, ABookWithNoLookupCannotBeOneOfTheFloorsBooks) {
  // A floor of type 0 reads vectors out of its books. A book that states
  // none - lookup type 0, which is how a book that only classifies is
  // written - is refused when the setup is read, and not when the first
  // packet reaches for a vector it does not have.
  BitWriter w;
  for (char c : std::string("\x05vorbis")) {
    w.put((unsigned char)c, 8);
  }
  w.put(0, 8);            // one codebook
  w.put(0x564342u, 24);   // its sync pattern
  w.put(1, 16);           // dimension
  w.put(2, 24);           // two entries
  w.put(0, 1);            // not ordered
  w.put(0, 1);            // not sparse
  w.put(0, 5);            // length one
  w.put(0, 5);
  w.put(0, 4);            // lookup type 0: no vectors
  w.put(0, 6);            // one time domain transform
  w.put(0, 16);
  w.put(0, 6);            // one floor
  w.put(0, 16);           // of type 0
  w.put(4, 8);            // order
  w.put(44100, 16);       // rate
  w.put(256, 16);         // bark map size
  w.put(6, 6);            // amplitude bits
  w.put(100, 8);          // amplitude offset
  w.put(0, 4);            // one book
  w.put(0, 8);            // book 0, which has no lookup
  VORBIS_Setup setup;
  memset(&setup, 0, sizeof setup);
  setup.allocator = gaud_allocator_default();
  EXPECT_EQ(gaud_vorbis_parse_setup(w.bytes().data(), w.bytes().size(), 1,
                nullptr, &setup),
      GAUD_ERR_CORRUPT);
  gaud_vorbis_setup_free(&setup);
}

TEST(VorbisFloor0, AFloorPacketThatEndsOrNamesNoBookIsNothingOrRefused) {
  Loaded loaded;
  ASSERT_EQ(OpenFile(loaded, "vorbis_syn_floor0_mono.ogg"), GAUD_OK);
  const VORBIS_File * file
      = (const VORBIS_File *)gaud_track_private(loaded.track());
  const VORBIS_Floor0 * floor = &file->setup.floors[0].u.zero;
  const uint32_t lines = 128;
  std::vector<uint16_t> map(lines);
  gaud_vorbis_f0_bark_map(floor->rate, floor->bark_map_size, lines, map.data());
  std::vector<int32_t> mantissa(lines);
  std::vector<int16_t> shift(lines);

  auto decode = [&](const PacketBits & w, bool * used) {
    VORBIS_Bits bits;
    gaud_vorbis_bits_init(&bits, w.bytes.data(), w.bytes.size());
    return gaud_vorbis_floor0_decode(floor, &file->setup, &bits, lines,
        map.data(), mantissa.data(), shift.data(), used);
  };
  bool used = true;
  // Amplitude zero: the channel carries nothing, and says so by one field.
  PacketBits zero;
  zero.Write(0, floor->amplitude_bits);
  EXPECT_EQ(decode(zero, &used), GAUD_OK);
  EXPECT_FALSE(used);
  // A book number the floor does not have. One book, so a bit of one.
  PacketBits bad;
  bad.Write(5, floor->amplitude_bits);
  bad.Write(1, 1);
  EXPECT_EQ(decode(bad, &used), GAUD_ERR_CORRUPT);
  EXPECT_FALSE(used);
  // A packet that ends before the vectors do carries nothing, as the
  // reference decoders treat it; it is not an error.
  PacketBits cut;
  cut.Write(5, floor->amplitude_bits);
  cut.Write(0, 1);
  cut.Write(0, 8);
  used = true;
  EXPECT_EQ(decode(cut, &used), GAUD_OK);
  EXPECT_FALSE(used);
  // A packet with no bytes at all.
  PacketBits empty;
  used = true;
  EXPECT_EQ(decode(empty, &used), GAUD_OK);
  EXPECT_FALSE(used);
}
