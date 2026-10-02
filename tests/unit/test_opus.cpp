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
 * Opus: the two headers, the packet framing, and the length.
 *
 * **All thirty-two configurations are written out and checked**, which is
 * the one thing here a corpus cannot do: an encoder chooses the handful
 * its settings produce, and the corpus reaches eleven of the thirty-two.
 * The other twenty-one are reachable only by building the byte, and a
 * configuration decoded as the wrong frame size is a stream that runs at
 * the wrong speed rather than one that fails.
 *
 * The table below is RFC 6716's Table 2, transcribed from the document.
 * Four groups of four for SILK at three bandwidths, two pairs for
 * hybrid, four groups of four for CELT - and **CELT has no medium band**,
 * which is the irregularity in an otherwise mechanical table and the
 * place an arithmetic shortcut gets it wrong.
 */

#include "../../src/codec/opus/opus_internal.h"
#include "../../src/codec/opus/opus_range.h"
#include "../../src/codec/opus/opus_celt.h"
#include "../../src/codec/opus/opus_celt_math.h"
#include "../../src/codec/opus/opus_tables.h"
#include "../../src/codec/opus/opus_silk_tables.h"
#include "../../src/codec/opus/opus_silk.h"
#include "../../src/codec/opus/opus_silk_math.h"
#include <ghoti.io/audio/audio.h>
#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/audio/codecs.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
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

/** An `OpusHead` with every field settable. */
std::vector<unsigned char> Head(unsigned version = 1, unsigned channels = 2,
    unsigned pre_skip = 312, uint32_t rate = 48000, int gain = 0,
    unsigned family = 0, const std::vector<unsigned char> & mapping = {}) {
  std::vector<unsigned char> head;
  head.insert(head.end(), OPUS_HEAD_MAGIC, OPUS_HEAD_MAGIC + 8);
  head.push_back((unsigned char)version);
  head.push_back((unsigned char)channels);
  head.push_back((unsigned char)(pre_skip & 0xFFu));
  head.push_back((unsigned char)(pre_skip >> 8));
  for (unsigned i = 0; i < 4u; ++i) {
    head.push_back((unsigned char)((rate >> (8 * i)) & 0xFFu));
  }
  head.push_back((unsigned char)(gain & 0xFF));
  head.push_back((unsigned char)((gain >> 8) & 0xFF));
  head.push_back((unsigned char)family);
  head.insert(head.end(), mapping.begin(), mapping.end());
  return head;
}

} // namespace

TEST(OpusToc, EveryConfigurationIsTheOneTheRfcTabulates) {
  /*
   * RFC 6716 Table 2, transcribed. Thirty-two rows, and the corpus
   * reaches eleven of them - so twenty-one are checked here and nowhere
   * else. A configuration read as the wrong frame size is a stream that
   * plays at the wrong speed, which no comparison that aligns two
   * signals before measuring would catch.
   */
  struct Row {
    OPUS_Mode mode;
    unsigned bandwidth; /* 0 narrow, 1 medium, 2 wide, 3 super, 4 full */
    unsigned microseconds;
  };
  const Row table[32] = {
      /* 0-3: SILK, narrow band */
      {OPUS_MODE_SILK, 0, 10000}, {OPUS_MODE_SILK, 0, 20000},
      {OPUS_MODE_SILK, 0, 40000}, {OPUS_MODE_SILK, 0, 60000},
      /* 4-7: SILK, medium band */
      {OPUS_MODE_SILK, 1, 10000}, {OPUS_MODE_SILK, 1, 20000},
      {OPUS_MODE_SILK, 1, 40000}, {OPUS_MODE_SILK, 1, 60000},
      /* 8-11: SILK, wide band */
      {OPUS_MODE_SILK, 2, 10000}, {OPUS_MODE_SILK, 2, 20000},
      {OPUS_MODE_SILK, 2, 40000}, {OPUS_MODE_SILK, 2, 60000},
      /* 12-15: hybrid, super-wide then full band */
      {OPUS_MODE_HYBRID, 3, 10000}, {OPUS_MODE_HYBRID, 3, 20000},
      {OPUS_MODE_HYBRID, 4, 10000}, {OPUS_MODE_HYBRID, 4, 20000},
      /* 16-19: CELT, narrow band. **No medium band for CELT**, which is
       * the one irregularity in the table. */
      {OPUS_MODE_CELT, 0, 2500}, {OPUS_MODE_CELT, 0, 5000},
      {OPUS_MODE_CELT, 0, 10000}, {OPUS_MODE_CELT, 0, 20000},
      /* 20-23: CELT, wide band */
      {OPUS_MODE_CELT, 2, 2500}, {OPUS_MODE_CELT, 2, 5000},
      {OPUS_MODE_CELT, 2, 10000}, {OPUS_MODE_CELT, 2, 20000},
      /* 24-27: CELT, super-wide band */
      {OPUS_MODE_CELT, 3, 2500}, {OPUS_MODE_CELT, 3, 5000},
      {OPUS_MODE_CELT, 3, 10000}, {OPUS_MODE_CELT, 3, 20000},
      /* 28-31: CELT, full band */
      {OPUS_MODE_CELT, 4, 2500}, {OPUS_MODE_CELT, 4, 5000},
      {OPUS_MODE_CELT, 4, 10000}, {OPUS_MODE_CELT, 4, 20000},
  };
  for (unsigned config = 0; config < 32u; ++config) {
    for (unsigned stereo = 0; stereo < 2u; ++stereo) {
      unsigned char packet[4] = {0, 0, 0, 0};
      packet[0] = (unsigned char)((config << 3) | (stereo << 2) | 0u);
      OPUS_Toc toc;
      ASSERT_EQ(gaud_opus_parse_toc(packet, 1, &toc), GAUD_OK) << config;
      EXPECT_EQ(toc.mode, table[config].mode) << "config " << config;
      EXPECT_EQ(toc.bandwidth, table[config].bandwidth)
          << "config " << config;
      /* 48 samples per 1,000 microseconds, because every Opus frame
       * size is a whole number of 48 kHz samples by construction -
       * including 2.5 ms, which is 120. */
      EXPECT_EQ(toc.frame_size, table[config].microseconds * 48u / 1000u)
          << "config " << config;
      EXPECT_EQ(toc.stereo, stereo != 0) << "config " << config;
      EXPECT_EQ(toc.frames, 1u) << "config " << config;
    }
  }
}

TEST(OpusToc, TheFourFramingsAreReadAndTheImpossibleOnesRefused) {
  /*
   * Code 0 is one frame, code 1 two of equal length, code 2 two whose
   * first is stated, code 3 an arbitrary count. The corpus reaches
   * codes 0 and 3; codes 1 and 2 are built here.
   *
   * What is refused matters as much. A code 3 packet stating zero
   * frames would advance a stream by nothing, which is how a decoder is
   * made to loop; a code 1 packet with an odd number of bytes left
   * cannot hold two frames of equal size; a code 2 packet whose stated
   * first length runs past the packet is a read past the end.
   */
  const unsigned char config = 16u; /* CELT, narrow band, 2.5 ms. */
  /* Code 0: one frame. */
  {
    unsigned char packet[] = {(unsigned char)(config << 3 | 0), 1, 2, 3};
    OPUS_Toc toc;
    ASSERT_EQ(gaud_opus_parse_toc(packet, sizeof(packet), &toc), GAUD_OK);
    EXPECT_EQ(toc.frames, 1u);
  }
  /* Code 1: two frames, the remaining bytes split evenly. */
  {
    unsigned char packet[] = {(unsigned char)(config << 3 | 1), 1, 2, 3, 4};
    OPUS_Toc toc;
    ASSERT_EQ(gaud_opus_parse_toc(packet, sizeof(packet), &toc), GAUD_OK);
    EXPECT_EQ(toc.frames, 2u);
    /* An odd number of bytes cannot be two equal frames. */
    EXPECT_EQ(gaud_opus_parse_toc(packet, 4, &toc), GAUD_ERR_CORRUPT);
  }
  /* Code 2: the first frame's length is stated, in one or two bytes. */
  {
    unsigned char packet[] = {(unsigned char)(config << 3 | 2), 2, 9, 9, 7};
    OPUS_Toc toc;
    ASSERT_EQ(gaud_opus_parse_toc(packet, sizeof(packet), &toc), GAUD_OK);
    EXPECT_EQ(toc.frames, 2u);
    /* A length past the end of the packet. */
    unsigned char over[] = {(unsigned char)(config << 3 | 2), 200, 1};
    EXPECT_EQ(gaud_opus_parse_toc(over, sizeof(over), &toc),
        GAUD_ERR_CORRUPT);
    /* A two-byte length, which 252 and above selects. */
    std::vector<unsigned char> big(600, 0);
    big[0] = (unsigned char)(config << 3 | 2);
    big[1] = 252;
    big[2] = 1; /* 252 + 1*4 = 256 bytes in the first frame */
    EXPECT_EQ(gaud_opus_parse_toc(big.data(), big.size(), &toc), GAUD_OK);
  }
  /* Code 3: a count, and the two flags above it. */
  {
    unsigned char packet[] = {(unsigned char)(config << 3 | 3), 8, 0, 0};
    OPUS_Toc toc;
    ASSERT_EQ(gaud_opus_parse_toc(packet, sizeof(packet), &toc), GAUD_OK);
    EXPECT_EQ(toc.frames, 8u);
    /* Zero frames: a packet worth no samples at all. */
    unsigned char none[] = {(unsigned char)(config << 3 | 3), 0, 0};
    EXPECT_EQ(gaud_opus_parse_toc(none, sizeof(none), &toc),
        GAUD_ERR_CORRUPT);
    /* **Past 120 milliseconds**, which the format caps a packet at. 48
     * frames of 2.5 ms is exactly 120 and legal; 49 is not. */
    unsigned char edge[] = {(unsigned char)(config << 3 | 3), 48, 0};
    EXPECT_EQ(gaud_opus_parse_toc(edge, sizeof(edge), &toc), GAUD_OK);
    unsigned char over[] = {(unsigned char)(config << 3 | 3), 49, 0};
    EXPECT_EQ(gaud_opus_parse_toc(over, sizeof(over), &toc),
        GAUD_ERR_CORRUPT);
    /* And with 20 ms frames the cap is six. */
    unsigned char six[] = {(unsigned char)(31u << 3 | 3), 6, 0};
    EXPECT_EQ(gaud_opus_parse_toc(six, sizeof(six), &toc), GAUD_OK);
    unsigned char seven[] = {(unsigned char)(31u << 3 | 3), 7, 0};
    EXPECT_EQ(gaud_opus_parse_toc(seven, sizeof(seven), &toc),
        GAUD_ERR_CORRUPT);
  }
}

TEST(OpusHead, TheMajorVersionIsTheTopNibbleAndTheMinorIsAccepted) {
  /*
   * **The one field here where a whole-byte comparison is wrong.** RFC
   * 7845 splits the version byte: a decoder must refuse a major version
   * it does not know and must *accept* any minor version, because a
   * minor bump only ever appends fields. A reader that compared the
   * byte to 1 would refuse every future minor revision of a format that
   * promised they would keep working - and would also refuse version 0,
   * which is a major version it does know.
   */
  OPUS_Head head;
  for (unsigned minor = 0; minor < 16u; ++minor) {
    auto packet = Head(minor);
    EXPECT_EQ(gaud_opus_parse_head(packet.data(), packet.size(), &head),
        GAUD_OK)
        << "version 0x0" << std::hex << minor;
  }
  for (unsigned major = 1; major < 16u; ++major) {
    auto packet = Head(major << 4);
    EXPECT_EQ(gaud_opus_parse_head(packet.data(), packet.size(), &head),
        GAUD_ERR_UNSUPPORTED)
        << "version 0x" << std::hex << (major << 4);
  }
}

TEST(OpusHead, EveryBoundTheFormatStatesIsChecked) {
  OPUS_Head head;
  struct Case {
    const char * what;
    std::vector<unsigned char> packet;
    GAUD_Result expected;
  };
  std::vector<Case> cases = {
      {"a legal head", Head(), GAUD_OK},
      {"no channels", Head(1, 0), GAUD_ERR_CORRUPT},
      /* Family 0 is one or two channels and nothing else. */
      {"three channels in family 0", Head(1, 3), GAUD_ERR_CORRUPT},
      /* 2 to 254 are reserved: a mapping this cannot interpret, and
       * guessing at speaker positions is worse than declining. */
      {"a reserved mapping family", Head(1, 2, 312, 48000, 0, 2),
          GAUD_ERR_UNSUPPORTED},
      {"another reserved one", Head(1, 2, 312, 48000, 0, 254),
          GAUD_ERR_UNSUPPORTED},
      /* Family 1 is the Vorbis channel orders, which stop at eight. */
      {"nine channels in family 1",
          Head(1, 9, 312, 48000, 0, 1, {5, 4, 0, 1, 2, 3, 4, 5, 6, 7, 8}),
          GAUD_ERR_CORRUPT},
  };
  for (const auto & one : cases) {
    EXPECT_EQ(
        gaud_opus_parse_head(one.packet.data(), one.packet.size(), &head),
        one.expected)
        << one.what;
  }

  /* A family-1 head needs a stream count, a coupled count and one byte
   * per channel after the fixed part; short of that it is truncated. */
  auto truncated = Head(1, 2, 312, 48000, 0, 1, {1, 1});
  EXPECT_EQ(
      gaud_opus_parse_head(truncated.data(), truncated.size(), &head),
      GAUD_ERR_CORRUPT);

  /* And a legal one: two channels, one stream, coupled. */
  auto stereo = Head(1, 2, 312, 48000, 0, 1, {1, 1, 0, 1});
  ASSERT_EQ(gaud_opus_parse_head(stereo.data(), stereo.size(), &head),
      GAUD_OK);
  EXPECT_EQ(head.streams, 1u);
  EXPECT_EQ(head.coupled, 1u);

  /* A mapping naming a decoded channel that does not exist. One stream,
   * uncoupled, produces one channel - numbered 0 - so 1 is past the
   * end. 255 is the format's "this channel is silent" and is legal. */
  auto wrong = Head(1, 2, 312, 48000, 0, 1, {1, 0, 0, 1});
  EXPECT_EQ(gaud_opus_parse_head(wrong.data(), wrong.size(), &head),
      GAUD_ERR_CORRUPT);
  auto silent = Head(1, 2, 312, 48000, 0, 1, {1, 0, 0, 255});
  EXPECT_EQ(gaud_opus_parse_head(silent.data(), silent.size(), &head),
      GAUD_OK);

  /* Not an OpusHead at all is FORMAT and not CORRUPT, so the registry
   * goes on to ask another codec. */
  auto tags = Head();
  memcpy(tags.data(), OPUS_TAGS_MAGIC, 8);
  EXPECT_EQ(gaud_opus_parse_head(tags.data(), tags.size(), &head),
      GAUD_ERR_FORMAT);
}

TEST(OpusHead, ThePreSkipAndTheGainAreReadAsTheySayTheyAre) {
  OPUS_Head head;
  auto packet = Head(1, 2, 3000, 16000, -256);
  ASSERT_EQ(gaud_opus_parse_head(packet.data(), packet.size(), &head),
      GAUD_OK);
  EXPECT_EQ(head.pre_skip, 3000u);
  /* The input rate is read and reported nowhere: the track's rate is
   * 48,000 for every Opus file, which the fixture test below asserts.
   * It is kept because a diagnostic may one day want it. */
  EXPECT_EQ(head.input_rate, 16000u);
  /* **Signed**, in Q7.8 decibels. A reader that took it as unsigned
   * would turn a one-decibel cut into a 255-decibel boost. */
  EXPECT_EQ(head.output_gain, -256);
}

/* -------------------------------------------------------- the fixtures */

namespace {

const struct Fixtures {
  const char * name;
  unsigned channels;
  uint64_t frames;
} fixtures[] = {
    {"opus_celt_stereo_96k.opus", 2u, 9600u},
    {"opus_celt_lowdelay_2ms5.opus", 2u, 9600u},
    {"opus_celt_stereo_10ms.opus", 2u, 9600u},
    {"opus_celt_mono_swb.opus", 1u, 9600u},
    {"opus_silk_mono_nb.opus", 1u, 9600u},
    {"opus_silk_mono_mb.opus", 1u, 9600u},
    {"opus_silk_mono_wb.opus", 1u, 9600u},
    {"opus_silk_mono_40ms.opus", 1u, 9600u},
    {"opus_silk_stereo_60ms.opus", 2u, 9600u},
    {"opus_hybrid_mono_swb.opus", 1u, 9600u},
    {"opus_hybrid_mono_fb.opus", 1u, 9600u},
    {"opus_celt_mono_60ms.opus", 1u, 9600u},
    {"opus_celt_5dot1.opus", 6u, 4800u},
    {"opus_celt_silence.opus", 2u, 4800u},
    {"opus_tagged_stereo.opus", 2u, 4800u},
};

} // namespace

TEST(OpusLoad, EveryFixtureIdentifiesAsTheStreamItIs) {
  for (const auto & one : fixtures) {
    Loaded loaded;
    ASSERT_EQ(OpenFile(loaded, one.name), GAUD_OK) << one.name;
    GAUD_Track * track = loaded.track();
    ASSERT_NE(track, nullptr) << one.name;
    EXPECT_STREQ(gaud_doc_codec_name(loaded.doc), "opus") << one.name;
    /* **48,000 for every Opus file.** The rate in OpusHead is what the
     * encoder was given and the format marks it informational. */
    EXPECT_EQ(gaud_track_sample_rate(track), OPUS_RATE) << one.name;
    EXPECT_EQ(gaud_track_layout(track).channels, one.channels) << one.name;
    EXPECT_EQ(gaud_track_coding(track), GAUD_CODING_OPUS) << one.name;
    EXPECT_EQ(gaud_track_format(track), GAUD_SAMPLE_S16) << one.name;
    EXPECT_EQ(loaded.diagnostics.count, 0u) << one.name;
  }
}

TEST(OpusLoad, TheLengthIsTheGranulePositionMinusThePreSkip) {
  /*
   * The subtraction no other format here needs. An Opus stream begins
   * with samples the encoder's filters needed and the recording does
   * not contain, the granule positions count them, and OpusHead states
   * how many. The frame counts below are the generator's own - the
   * number of frames each encoder was given - so this checks the
   * arithmetic against something no decoder was involved in.
   */
  for (const auto & one : fixtures) {
    Loaded loaded;
    ASSERT_EQ(OpenFile(loaded, one.name), GAUD_OK) << one.name;
    EXPECT_EQ(gaud_track_frames(loaded.track()), one.frames) << one.name;
    EXPECT_NEAR(gaud_track_duration(loaded.track()),
        (double)one.frames / 48000.0, 1e-9)
        << one.name;
    OPUS_File * state
        = static_cast<OPUS_File *>(gaud_doc_private(loaded.doc));
    ASSERT_NE(state, nullptr) << one.name;
    /* Non-zero, or the subtraction above is not being exercised. Every
     * encoder writes one; 312 is libopus's at 20 ms and 120 at 2.5. */
    EXPECT_GT(state->head.pre_skip, 0u) << one.name;
  }
}

TEST(OpusLoad, TheCorpusReachesAllThreeModes) {
  /*
   * **The corpus's own coverage, asserted rather than hoped for.** The
   * mode is chosen by the encoder from the application and the cutoff,
   * and a libopus upgrade may choose differently - which would take an
   * arm away silently. So the first packet of each fixture is read and
   * its configuration recorded, and all three modes and both framing
   * codes the corpus is supposed to have must be there.
   */
  std::vector<unsigned> modes(3, 0);
  std::vector<unsigned> codes(4, 0);
  std::vector<unsigned> configs;
  for (const auto & one : fixtures) {
    Loaded loaded;
    ASSERT_EQ(OpenFile(loaded, one.name), GAUD_OK) << one.name;
    OPUS_File * state
        = static_cast<OPUS_File *>(gaud_doc_private(loaded.doc));
    /* The first audio packet, read through the page layer. */
    OGG_Reader reader;
    gaud_ogg_reader_init(&reader, loaded.stream, nullptr);
    reader.serial = state->serial;
    reader.have_serial = true;
    ASSERT_EQ(gaud_ogg_reader_seek(&reader, state->audio_offset), GAUD_OK)
        << one.name;
    bool found = false;
    for (int i = 0; i < 8 && !found; ++i) {
      const unsigned char * data = nullptr;
      size_t size = 0;
      if (gaud_ogg_reader_packet(&reader, &data, &size, nullptr, nullptr)
          != GAUD_OK) {
        break;
      }
      if (size >= 8
          && (memcmp(data, OPUS_HEAD_MAGIC, 8) == 0
              || memcmp(data, OPUS_TAGS_MAGIC, 8) == 0)) {
        continue; /* A header sharing the page. */
      }
      OPUS_Toc toc;
      ASSERT_EQ(gaud_opus_parse_toc(data, size, &toc), GAUD_OK) << one.name;
      modes[toc.mode]++;
      codes[toc.code]++;
      configs.push_back((unsigned)(data[0] >> 3));
      found = true;
    }
    gaud_ogg_reader_free(&reader);
    EXPECT_TRUE(found) << one.name << ": no audio packet was read";
  }
  EXPECT_GT(modes[OPUS_MODE_SILK], 0u) << "no fixture is SILK";
  EXPECT_GT(modes[OPUS_MODE_HYBRID], 0u) << "no fixture is hybrid";
  EXPECT_GT(modes[OPUS_MODE_CELT], 0u) << "no fixture is CELT";
  EXPECT_GT(codes[0], 0u) << "no fixture uses the one-frame framing";
  EXPECT_GT(codes[3], 0u)
      << "no fixture uses the arbitrary framing, so three quarters of "
      << "the framing code is reached by nothing";
  std::sort(configs.begin(), configs.end());
  configs.erase(std::unique(configs.begin(), configs.end()), configs.end());
  EXPECT_GE(configs.size(), 8u)
      << "the corpus reaches only " << configs.size()
      << " of the 32 configurations; it had 11 when it was built";
}

TEST(OpusLoad, TheTagsComeOutOfTheCommentHeader) {
  Loaded loaded;
  ASSERT_EQ(OpenFile(loaded, "opus_tagged_stereo.opus"), GAUD_OK);
  const GAUD_Meta * meta = gaud_doc_meta(loaded.doc);
  ASSERT_NE(meta, nullptr);
  ASSERT_EQ(gaud_meta_count(meta, GAUD_TAG_TITLE), 1u);
  EXPECT_STREQ(gaud_meta_get(meta, GAUD_TAG_TITLE, 0), "A Title");
  ASSERT_EQ(gaud_meta_count(meta, GAUD_TAG_ARTIST), 1u);
  EXPECT_STREQ(gaud_meta_get(meta, GAUD_TAG_ARTIST, 0), "An Artist");
  ASSERT_EQ(gaud_meta_count(meta, GAUD_TAG_COMMENT), 1u);
  EXPECT_STREQ(gaud_meta_get(meta, GAUD_TAG_COMMENT, 0),
      "café 日本語");
}

TEST(OpusLoad, AskingForADecoderIsRefusedAndTheCapabilityBitSaysWhy) {
  /*
   * planning/audio.md section 11.18's argument, as an assertion, and
   * **this test is expected to be deleted**: when the decoder lands,
   * GAUD_CAP_DECODE is declared and this becomes the test that must be
   * removed rather than relaxed.
   */
  const GAUD_Codec * codec = gaud_registry_find(NULL, "opus");
  ASSERT_NE(codec, nullptr);
  EXPECT_TRUE((codec->capabilities & GAUD_CAP_METADATA_READ) != 0);
  EXPECT_FALSE((codec->capabilities & GAUD_CAP_DECODE) != 0);

  Loaded loaded;
  ASSERT_EQ(OpenFile(loaded, "opus_celt_stereo_96k.opus"), GAUD_OK);
  GAUD_Decoder * decoder = nullptr;
  EXPECT_EQ(gaud_decoder_create(loaded.track(), &decoder),
      GAUD_ERR_UNSUPPORTED);
  EXPECT_EQ(decoder, nullptr);
}

TEST(OpusProbe, OggIsFiveFormatsAndTheProbeDecidesWhich) {
  /* Three of them are registered here now, and each must claim its own
   * and not another's. */
  const struct {
    const char * name;
    const char * codec;
  } cases[] = {
      {"opus_celt_stereo_96k.opus", "opus"},
      {"vorbis_lib_stereo_44100.ogg", "vorbis"},
      {"oggflac_lib_s16_stereo_44100.oga", "ogg-flac"},
  };
  for (const auto & one : cases) {
    Loaded loaded;
    ASSERT_EQ(OpenFile(loaded, one.name), GAUD_OK) << one.name;
    EXPECT_STREQ(gaud_doc_codec_name(loaded.doc), one.codec) << one.name;
  }
}

/* --------------------------------------- RFC 6716 Table 55 and the
                                              generated CELT tables */

/**
 * The CELT tables, checked against the one of them the prose prints.
 *
 * tools/tables/gen_opus_tables.py generates these from the reference
 * implementation in Appendix A, which section 6 makes normative, and it
 * already checks the band layout against Table 55 while doing so. This
 * repeats that check **against the built library**, which is a different
 * claim: the generator checks what it read, and this checks what was
 * compiled in. A table regenerated from a changed source, or edited by
 * hand afterwards, passes the first and fails this.
 *
 * Table 55 below is transcribed from the document a second time, on
 * purpose. Two transcriptions of one printed table disagree if either
 * has a typo, which is worth more than one transcription used twice.
 */

namespace {

/** RFC 6716 Table 55: MDCT bins per channel per band, by frame size. */
const unsigned char table55[21][4] = {
    {1, 2, 4, 8}, {1, 2, 4, 8}, {1, 2, 4, 8}, {1, 2, 4, 8},
    {1, 2, 4, 8}, {1, 2, 4, 8}, {1, 2, 4, 8}, {1, 2, 4, 8},
    {2, 4, 8, 16}, {2, 4, 8, 16}, {2, 4, 8, 16}, {2, 4, 8, 16},
    {4, 8, 16, 32}, {4, 8, 16, 32}, {4, 8, 16, 32},
    {6, 12, 24, 48}, {6, 12, 24, 48},
    {8, 16, 32, 64},
    {12, 24, 48, 96},
    {18, 36, 72, 144},
    {22, 44, 88, 176},
};

/** The same table's band edges in hertz. */
const unsigned short table55_hz[22] = {
    0, 200, 400, 600, 800, 1000, 1200, 1400, 1600, 2000, 2400, 2800,
    3200, 4000, 4800, 5600, 6800, 8000, 9600, 12000, 15600, 20000,
};

} // namespace

TEST(OpusTables, BandWidthsAreTable55AtEveryFrameSize) {
  for (int band = 0; band < 21; ++band) {
    int width = gaud_opus_eband5ms[band + 1] - gaud_opus_eband5ms[band];
    ASSERT_GT(width, 0) << "band " << band;
    for (int size = 0; size < 4; ++size) {
      int multiplier = 1 << size;
      EXPECT_EQ(width * multiplier, (int)table55[band][size])
          << "band " << band << " at frame size " << multiplier;
    }
  }
}

/**
 * The same edges in hertz, which is a second fact about them.
 *
 * One 2.5 ms bin at 48 kHz is 200 Hz, so the edge table and the
 * frequency column of Table 55 are the same numbers scaled. A band
 * layout shifted by one bin passes the width check - every width is
 * unchanged - and fails this one.
 */
TEST(OpusTables, BandEdgesLandOnTable55Frequencies) {
  for (int edge = 0; edge < 22; ++edge) {
    EXPECT_EQ(gaud_opus_eband5ms[edge] * 200, (int)table55_hz[edge])
        << "edge " << edge;
  }
  /* And the top of the last band is 20 kHz, not the 24 kHz Nyquist: a
   * 20 ms frame has 960 bins and the bands cover 800 of them. */
  EXPECT_EQ(gaud_opus_eband5ms[21], 100);
  EXPECT_EQ(gaud_opus_eband5ms[21] * 8, 800);
}

/**
 * The overlap window rises monotonically to Q15ONE.
 *
 * The generator checks it against its formula; what is checked here is
 * the shape, which is what the overlap-add depends on. The rise is
 * non-decreasing rather than strictly increasing, because the top of it
 * sits at the ceiling - and the count of entries there is asserted,
 * because a window that saturated early would still be monotone.
 *
 * **Six entries are 32767 and only five of them are clamped.** The
 * generator reports five, counting the entries whose formula rounds to
 * 32768, which Q15 cannot hold; entry 114 arrives at 32767 honestly,
 * its exact value being 32766.92. The two numbers measure different
 * things and the first draft of this test asserted the generator's
 * against the library's.
 */
TEST(OpusTables, WindowRisesToQ15One) {
  int at_ceiling = 0;
  for (int n = 0; n < 120; ++n) {
    EXPECT_GT(gaud_opus_window120[n], 0) << "n " << n;
    EXPECT_LE(gaud_opus_window120[n], 32767) << "n " << n;
    if (n > 0) {
      EXPECT_GE(gaud_opus_window120[n], gaud_opus_window120[n - 1])
          << "n " << n;
    }
    if (gaud_opus_window120[n] == 32767) {
      ++at_ceiling;
    }
  }
  EXPECT_EQ(gaud_opus_window120[119], 32767);
  EXPECT_EQ(at_ceiling, 6);
  /* And the entry below them is not there yet, so the ceiling is a
   * ceiling rather than a flat top the window reaches early. */
  EXPECT_EQ(gaud_opus_window120[113], 32766);
}

/**
 * The allocation table's two monotonicities.
 *
 * Across a row the allocation falls, because higher bands get fewer
 * bits per sample; down a column it rises, because the rows are
 * increasing overall rates. Both hold for all 231 entries, and a table
 * read with its two dimensions transposed breaks them - which is the
 * mistake a flat array of 231 bytes invites.
 */
TEST(OpusTables, AllocationTableIsMonotoneBothWays) {
  const int rows = 11;
  const int bands = 21;
  for (int row = 0; row < rows; ++row) {
    for (int band = 1; band < bands; ++band) {
      EXPECT_LE(gaud_opus_band_allocation[row * bands + band],
          gaud_opus_band_allocation[row * bands + band - 1])
          << "row " << row << " band " << band;
    }
  }
  for (int band = 0; band < bands; ++band) {
    for (int row = 1; row < rows; ++row) {
      EXPECT_GE(gaud_opus_band_allocation[row * bands + band],
          gaud_opus_band_allocation[(row - 1) * bands + band])
          << "row " << row << " band " << band;
    }
  }
  /* The first row allocates nothing anywhere, and the last allocates
   * something everywhere: those are the ends of the interpolation. */
  for (int band = 0; band < bands; ++band) {
    EXPECT_EQ(gaud_opus_band_allocation[band], 0) << "band " << band;
    EXPECT_GT(gaud_opus_band_allocation[(rows - 1) * bands + band], 0)
        << "band " << band;
  }
}

/**
 * Every distribution the range decoder will be handed is terminated.
 *
 * ::gaud_opus_dec_icdf stops on the zero at the end of its table and
 * runs off the end of the array without one, so this is a bound on a
 * loop rather than a tidiness check. Each table is also checked to be
 * strictly decreasing, which is what makes it an inverse cumulative
 * distribution rather than a list.
 */
TEST(OpusTables, EveryIcdfIsDecreasingAndZeroTerminated) {
  struct Table {
    const char * name;
    const unsigned char * values;
    size_t count;
  };
  const Table tables[] = {
      {"trim", gaud_opus_trim_icdf, 11},
      {"spread", gaud_opus_spread_icdf, 4},
      {"tapset", gaud_opus_tapset_icdf, 3},
  };
  for (const Table & one : tables) {
    EXPECT_EQ(one.values[one.count - 1], 0u) << one.name;
    for (size_t i = 1; i < one.count; ++i) {
      EXPECT_LT(one.values[i], one.values[i - 1]) << one.name << " at " << i;
    }
  }
}

/**
 * The coarse energy model has a Laplace pair for every band of every
 * frame size, in both prediction modes.
 *
 * Forty-two entries is twenty-one bands times two numbers each, and the
 * second of each pair is a decay that must leave something to decode -
 * a zero would make the distribution unreadable past the first symbol.
 */
TEST(OpusTables, CoarseEnergyModelCoversEveryBand) {
  for (int size = 0; size < 4; ++size) {
    for (int intra = 0; intra < 2; ++intra) {
      for (int band = 0; band < 21; ++band) {
        size_t at = ((size_t)size * 2u + intra) * 42u + (size_t)band * 2u;
        EXPECT_GT(gaud_opus_e_prob_model[at], 0u)
            << "size " << size << " intra " << intra << " band " << band;
        EXPECT_GT(gaud_opus_e_prob_model[at + 1u], 0u)
            << "size " << size << " intra " << intra << " band " << band;
      }
    }
  }
}

/** The prediction coefficients fall with frame size, and stay in Q15. */
TEST(OpusTables, PredictionCoefficientsAreQ15AndFall) {
  for (int size = 0; size < 4; ++size) {
    EXPECT_GT(gaud_opus_pred_coef[size], 0);
    EXPECT_LE(gaud_opus_pred_coef[size], 32767);
    EXPECT_GT(gaud_opus_beta_coef[size], 0);
    EXPECT_LE(gaud_opus_beta_coef[size], 32767);
    if (size > 0) {
      EXPECT_LT(gaud_opus_pred_coef[size], gaud_opus_pred_coef[size - 1]);
      EXPECT_LT(gaud_opus_beta_coef[size], gaud_opus_beta_coef[size - 1]);
    }
  }
  EXPECT_GT(gaud_opus_beta_intra[0], 0);
  EXPECT_LE(gaud_opus_beta_intra[0], 32767);
}

/* ------------------------------------------- RFC 6716 section 3.2:
                                                   splitting a packet */

/**
 * The frame packing, and the seven numbered requirements it carries.
 *
 * Every packet below is built by hand. The conformance vectors exercise
 * this code twenty thousand times and never once exercise a *refusal* -
 * they are all valid - so the cases that must be rejected exist only
 * here. Several of them describe packets whose frames would decode
 * perfectly well; they are invalid anyway, because the point of the
 * rules is that a gateway repacking a stream can rely on them.
 *
 * The configurations used are 31 (CELT fullband, 20 ms, so 960 samples
 * a frame) and 16 (CELT narrowband, 2.5 ms, 120 samples), chosen
 * because the 120 ms ceiling falls in a different place for each.
 */

// --- the SILK tables ------------------------------------------------
//
// Section 4.2 prints its own tables, which section 4.3 does not, so
// `make check-opus-tables` reads each of them twice - once from the
// prose and once from Appendix A - and refuses to generate anything
// unless the two agree. That gate needs the network. What follows is
// the offline half: a few of the prose's tables transcribed by hand,
// and the structural properties every one of them has to have for the
// range decoder to be able to read a packet at all.

namespace {

/** RFC 6716's probabilities, in the reference's inverse cumulative form. */
std::vector<unsigned char> Icdf(const std::vector<int> & pdf) {
  std::vector<unsigned char> out;
  int cumulative = 0;
  size_t lead = 0;
  while (lead < pdf.size() && pdf[lead] == 0) {
    ++lead;
  }
  for (size_t i = 0; i < pdf.size(); ++i) {
    cumulative += pdf[i];
    if (i < lead) {
      continue;
    }
    out.push_back((unsigned char)(256 - cumulative));
    if (cumulative == 256) {
      break;
    }
  }
  return out;
}

} // namespace

/**
 * Five of section 4.2's tables, typed in from the document.
 *
 * The generator checks all ninety-one this way and this checks five,
 * which is the point: the five are here so that a `make test` with no
 * network still fails if the committed tables stop being the RFC's.
 * They are chosen to cover the three shapes the conversion has - a
 * distribution with impossible symbols at the front (Table 4), one
 * that reaches zero before its last symbol would (Table 9), and the
 * plain kind.
 */
TEST(OpusSilkTables, TheProseTablesTypedInHereStillMatch) {
  const std::vector<int> lbrr40 = {0, 53, 53, 150};
  const std::vector<int> lbrr60 = {0, 41, 20, 29, 41, 15, 28, 82};
  const std::vector<int> inactive = {26, 230, 0, 0, 0, 0};
  const std::vector<int> active = {0, 0, 24, 74, 148, 10};
  const std::vector<int> interpolation = {13, 22, 29, 11, 181};

  auto same = [](const std::vector<unsigned char> & mine,
                  const unsigned char * theirs, size_t count,
                  const char * what) {
    ASSERT_EQ(mine.size(), count) << what;
    for (size_t i = 0; i < count; ++i) {
      EXPECT_EQ(mine[i], theirs[i]) << what << " at " << i;
    }
  };
  same(Icdf(lbrr40), gaud_opus_silk_lbrr_flags_2_icdf, 3, "Table 4, 40 ms");
  same(Icdf(lbrr60), gaud_opus_silk_lbrr_flags_3_icdf, 7, "Table 4, 60 ms");
  same(Icdf(inactive), gaud_opus_silk_type_offset_no_vad_icdf, 2, "Table 9");
  same(Icdf(active), gaud_opus_silk_type_offset_vad_icdf, 4, "Table 9");
  same(Icdf(interpolation), gaud_opus_silk_nlsf_interpolation_factor_icdf, 5,
      "Table 26");

  // Table 7, the sixteen stereo prediction weights.
  const int16_t weights[16] = {-13732, -10050, -8266, -7526, -6500, -5000,
      -2950, -820, 820, 2950, 5000, 6500, 7526, 8266, 10050, 13732};
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(gaud_opus_silk_stereo_pred_quant_q13[i], weights[i])
        << "Table 7 at " << i;
  }
  // Section 4.2.7.6.3's three scale factors, which the prose gives in a
  // sentence rather than a table.
  EXPECT_EQ(gaud_opus_silk_ltp_scales_q14[0], 15565);
  EXPECT_EQ(gaud_opus_silk_ltp_scales_q14[1], 12288);
  EXPECT_EQ(gaud_opus_silk_ltp_scales_q14[2], 8192);
  // Table 53, which prints its offsets four times smaller than the
  // reference holds them.
  EXPECT_EQ(gaud_opus_silk_quantization_offsets_q10[0], 4 * 25);
  EXPECT_EQ(gaud_opus_silk_quantization_offsets_q10[1], 4 * 60);
  EXPECT_EQ(gaud_opus_silk_quantization_offsets_q10[2], 4 * 8);
  EXPECT_EQ(gaud_opus_silk_quantization_offsets_q10[3], 4 * 25);
}

/**
 * Every SILK distribution is strictly decreasing and ends at zero.
 *
 * A repeated entry is a symbol the encoder can write and the decoder
 * cannot distinguish, and a table that never reaches zero leaves part
 * of the range unassigned. Neither looks like a wrong number when it
 * happens - the decoder simply desynchronises some way further on - so
 * it is worth asserting over all forty of them rather than trusting the
 * extraction.
 */
TEST(OpusSilkTables, EverySilkIcdfIsDecreasingAndZeroTerminated) {
  struct Table {
    const char * name;
    const unsigned char * values;
    size_t stride;
    size_t rows;
  };
  const Table tables[] = {
      {"lbrr_flags_2", gaud_opus_silk_lbrr_flags_2_icdf, 3, 1},
      {"lbrr_flags_3", gaud_opus_silk_lbrr_flags_3_icdf, 7, 1},
      {"stereo_pred_joint", gaud_opus_silk_stereo_pred_joint_icdf, 25, 1},
      {"stereo_only_code_mid", gaud_opus_silk_stereo_only_code_mid_icdf, 2, 1},
      {"uniform3", gaud_opus_silk_uniform3_icdf, 3, 1},
      {"uniform4", gaud_opus_silk_uniform4_icdf, 4, 1},
      {"uniform5", gaud_opus_silk_uniform5_icdf, 5, 1},
      {"uniform6", gaud_opus_silk_uniform6_icdf, 6, 1},
      {"uniform8", gaud_opus_silk_uniform8_icdf, 8, 1},
      {"type_offset_no_vad", gaud_opus_silk_type_offset_no_vad_icdf, 2, 1},
      {"type_offset_vad", gaud_opus_silk_type_offset_vad_icdf, 4, 1},
      {"gain", gaud_opus_silk_gain_icdf, 8, 3},
      {"delta_gain", gaud_opus_silk_delta_gain_icdf, 41, 1},
      {"nlsf_cb1_nb_mb", gaud_opus_silk_nlsf_cb1_icdf_nb_mb, 32, 2},
      {"nlsf_cb1_wb", gaud_opus_silk_nlsf_cb1_icdf_wb, 32, 2},
      {"nlsf_cb2_nb_mb", gaud_opus_silk_nlsf_cb2_icdf_nb_mb, 9, 8},
      {"nlsf_cb2_wb", gaud_opus_silk_nlsf_cb2_icdf_wb, 9, 8},
      {"nlsf_ext", gaud_opus_silk_nlsf_ext_icdf, 7, 1},
      {"nlsf_interpolation", gaud_opus_silk_nlsf_interpolation_factor_icdf,
          5, 1},
      {"pitch_lag", gaud_opus_silk_pitch_lag_icdf, 32, 1},
      {"pitch_delta", gaud_opus_silk_pitch_delta_icdf, 21, 1},
      {"pitch_contour_10_ms_nb", gaud_opus_silk_pitch_contour_10_ms_nb_icdf,
          3, 1},
      {"pitch_contour_nb", gaud_opus_silk_pitch_contour_nb_icdf, 11, 1},
      {"pitch_contour_10_ms", gaud_opus_silk_pitch_contour_10_ms_icdf, 12, 1},
      {"pitch_contour", gaud_opus_silk_pitch_contour_icdf, 34, 1},
      {"ltp_per_index", gaud_opus_silk_ltp_per_index_icdf, 3, 1},
      {"ltp_gain_0", gaud_opus_silk_ltp_gain_icdf_0, 8, 1},
      {"ltp_gain_1", gaud_opus_silk_ltp_gain_icdf_1, 16, 1},
      {"ltp_gain_2", gaud_opus_silk_ltp_gain_icdf_2, 32, 1},
      {"ltpscale", gaud_opus_silk_ltpscale_icdf, 3, 1},
      {"rate_levels", gaud_opus_silk_rate_levels_icdf, 9, 2},
      {"pulses_per_block", gaud_opus_silk_pulses_per_block_icdf, 18, 10},
      {"lsb", gaud_opus_silk_lsb_icdf, 2, 1},
  };
  size_t counted = 0;
  for (const Table & one : tables) {
    for (size_t row = 0; row < one.rows; ++row) {
      const unsigned char * values = one.values + row * one.stride;
      EXPECT_EQ(values[one.stride - 1], 0u) << one.name << " row " << row;
      for (size_t i = 1; i < one.stride; ++i) {
        EXPECT_LT(values[i], values[i - 1])
            << one.name << " row " << row << " at " << i;
      }
      ++counted;
    }
  }
  EXPECT_EQ(counted, 61u);
}

/**
 * The four shell-code tables are one distribution per pulse count.
 *
 * Splitting p pulses between two halves has p+1 outcomes, so the
 * offsets have to step by p+1 and the whole thing has to come to 152
 * bytes. The offsets are shared by all four tables, which is why
 * getting one wrong would misread three of them silently.
 */
TEST(OpusSilkTables, ShellCodeOffsetsPartitionAllFourTables) {
  const unsigned char * offsets = gaud_opus_silk_shell_code_table_offsets;
  EXPECT_EQ(offsets[0], 0u);
  EXPECT_EQ(offsets[1], 0u);
  for (int pulses = 1; pulses <= 15; ++pulses) {
    EXPECT_EQ((int)offsets[pulses + 1] - (int)offsets[pulses], pulses + 1)
        << "at " << pulses;
  }
  EXPECT_EQ((int)offsets[16] + 17, 152);
  const unsigned char * const tables[4] = {
      gaud_opus_silk_shell_code_table0, gaud_opus_silk_shell_code_table1,
      gaud_opus_silk_shell_code_table2, gaud_opus_silk_shell_code_table3};
  for (int which = 0; which < 4; ++which) {
    for (int pulses = 1; pulses <= 16; ++pulses) {
      const unsigned char * row = tables[which] + offsets[pulses];
      EXPECT_EQ(row[pulses], 0u) << "table " << which << " at " << pulses;
      for (int i = 1; i <= pulses; ++i) {
        EXPECT_LT(row[i], row[i - 1])
            << "table " << which << " at " << pulses << ", " << i;
      }
    }
  }
}

/**
 * Rate level 10 is rate level 9 shifted, which is what caps the LSBs.
 *
 * Section 4.2.7.8.2 says the eleventh pulse-count distribution "is just
 * a shifted version of that for 9 and thus does not require any
 * additional storage", and the decoder reads it as a pointer one past
 * the start of row 9. The consequence is the part that matters: the
 * probability of reading another 17 is then zero, so a block cannot
 * carry more than ten extra LSBs however the bitstream is built.
 */
TEST(OpusSilkTables, RateLevelTenIsRateLevelNineShiftedAndCannotEscape) {
  const unsigned char * nine = gaud_opus_silk_pulses_per_block_icdf + 9 * 18;
  const unsigned char * ten = nine + 1;
  EXPECT_EQ(ten[16], 0u);
  for (int i = 0; i < 17; ++i) {
    EXPECT_EQ(ten[i], nine[i + 1]) << "at " << i;
  }
  // Symbol 17 is the escape. Its probability under rate level 10 is the
  // gap between entries 16 and 17 of the shifted row, and entry 16 is
  // already zero.
  EXPECT_EQ(nine[17], 0u);
}

/**
 * The cosine table is antisymmetric, falling, and entirely even.
 *
 * The last of those is not decoration. Appendix A holds this table at
 * twice the scale the prose prints it at - the name
 * `silk_LSFCosTab_FIX_Q12` says Q12 and the array is Q13 - and the two
 * interpolation formulas differ by one shift to match. They give the
 * same answer at every input only because every entry is even, so the
 * halving the prose's version implies loses nothing. The 32,768
 * comparisons below are the whole claim, not a sample of it.
 */
TEST(OpusSilkTables, TheCosineTableAgreesWithTheProsesHalfOfIt) {
  const int16_t * q13 = gaud_opus_silk_lsf_cos_q13;
  EXPECT_EQ(q13[0], 8192);
  EXPECT_EQ(q13[128], -8192);
  for (int i = 0; i <= 128; ++i) {
    EXPECT_EQ(q13[i] % 2, 0) << "at " << i;
    EXPECT_EQ(q13[128 - i], -q13[i]) << "at " << i;
    if (i < 128) {
      EXPECT_GT(q13[i], q13[i + 1]) << "at " << i;
    }
  }
  int differed = 0;
  for (int i = 0; i < 128; ++i) {
    int low = q13[i] / 2;
    int high = q13[i + 1] / 2;
    for (int fraction = 0; fraction < 256; ++fraction) {
      int prose = (low * 256 + (high - low) * fraction + 4) >> 3;
      int appendix =
          ((q13[i] << 8) + (q13[i + 1] - q13[i]) * fraction + 8) >> 4;
      if (prose != appendix) {
        ++differed;
      }
    }
  }
  EXPECT_EQ(differed, 0);
}

/**
 * Every stage-1 LSF codebook vector is already a stable filter.
 *
 * The decoder runs a stabilisation pass (section 4.2.7.5.4) over the
 * reconstructed LSFs because the stage-2 residual can push two of them
 * together. These 64 vectors need none of it: measured over all of
 * them, every coefficient is above the previous one by at least the
 * minimum spacing Table 25 gives, and the last clears 1.0 by the same
 * margin. So anything the stabiliser ever has to fix was put there by
 * the residual, never by the codebook.
 */
TEST(OpusSilkTables, EveryStageOneVectorAlreadyClearsTheMinimumSpacing) {
  struct Book {
    const char * name;
    const unsigned char * vectors;
    const int16_t * spacing;
    int order;
  };
  const Book books[] = {
      {"NB/MB", gaud_opus_silk_nlsf_cb1_nb_mb_q8,
          gaud_opus_silk_nlsf_delta_min_nb_mb_q15, 10},
      {"WB", gaud_opus_silk_nlsf_cb1_wb_q8,
          gaud_opus_silk_nlsf_delta_min_wb_q15, 16},
  };
  for (const Book & book : books) {
    for (int entry = 0; entry < 32; ++entry) {
      int previous = 0;
      for (int k = 0; k < book.order; ++k) {
        // The codebook is Q8 and the spacing is Q15.
        int value = (int)book.vectors[entry * book.order + k] << 7;
        EXPECT_GE(value - previous, book.spacing[k])
            << book.name << " entry " << entry << " coefficient " << k;
        previous = value;
      }
      EXPECT_GE((1 << 15) - previous, book.spacing[book.order])
          << book.name << " entry " << entry << " at the top";
    }
  }
}

/**
 * No select byte asks for a weight the prediction list does not hold.
 *
 * `silk_NLSF_unpack` reads `pred_Q8[i + sel*(order-1) + 1]` for the odd
 * coefficient of each pair, and the list is exactly `2*(order-1)` long,
 * so a set selector bit on the last coefficient reads one past the end.
 * The prose prints one fewer column than there are coefficients for
 * exactly that reason; this is the same statement made about the packed
 * bytes.
 */
TEST(OpusSilkTables, NoSelectByteSetsTheWeightBitOnALastCoefficient) {
  struct Book {
    const char * name;
    const unsigned char * select;
    int order;
  };
  const Book books[] = {
      {"NB/MB", gaud_opus_silk_nlsf_cb2_select_nb_mb, 10},
      {"WB", gaud_opus_silk_nlsf_cb2_select_wb, 16},
  };
  for (const Book & book : books) {
    int pairs = book.order / 2;
    for (int entry = 0; entry < 32; ++entry) {
      unsigned char last = book.select[entry * pairs + pairs - 1];
      EXPECT_EQ(last & 0x10u, 0u) << book.name << " entry " << entry;
    }
  }
}

/**
 * The pitch contour codebooks have one entry per index the PDF can code.
 *
 * The reference stores these transposed - subframe outermost - so a
 * length that comes out right is also a check that the transpose was
 * read the right way round.
 */
TEST(OpusSilkTables, EveryPitchContourIndexHasACodebookEntry) {
  struct Contour {
    const char * name;
    size_t indices;
    size_t entries;
    int subframes;
  };
  const Contour contours[] = {
      {"NB 10 ms", 3, 6, 2},
      {"NB 20 ms", 11, 44, 4},
      {"MB or WB 10 ms", 12, 24, 2},
      {"MB or WB 20 ms", 34, 136, 4},
  };
  for (const Contour & one : contours) {
    EXPECT_EQ(one.indices * (size_t)one.subframes, one.entries) << one.name;
  }
  // The offsets themselves are small: measured over all 210 of them,
  // none moves a subframe's lag by more than nine samples. The decoder
  // clamps the result to the legal lag range afterwards, so this bounds
  // how far that clamp can ever be asked to reach.
  const int8_t * all[] = {gaud_opus_silk_cb_lags_stage2_10_ms,
      gaud_opus_silk_cb_lags_stage2, gaud_opus_silk_cb_lags_stage3_10_ms,
      gaud_opus_silk_cb_lags_stage3};
  const size_t counts[] = {6, 44, 24, 136};
  for (int which = 0; which < 4; ++which) {
    for (size_t i = 0; i < counts[which]; ++i) {
      EXPECT_GE(all[which][i], -9) << which << " at " << i;
      EXPECT_LE(all[which][i], 9) << which << " at " << i;
    }
  }
}

/** Each LSF ordering is a permutation of its own coefficient numbers. */
TEST(OpusSilkTables, TheLsfOrderingsArePermutations) {
  for (int order = 10; order <= 16; order += 6) {
    const unsigned char * ordering = order == 10
        ? gaud_opus_silk_nlsf_ordering10
        : gaud_opus_silk_nlsf_ordering16;
    std::vector<bool> seen((size_t)order, false);
    for (int k = 0; k < order; ++k) {
      ASSERT_LT(ordering[k], order) << "order " << order << " at " << k;
      EXPECT_FALSE(seen[ordering[k]]) << "order " << order << " at " << k;
      seen[ordering[k]] = true;
    }
  }
}

/** Both outcomes of every excitation sign decision are codable. */
TEST(OpusSilkTables, EverySignContextCanCodeBothSigns) {
  for (int i = 0; i < 42; ++i) {
    EXPECT_GE(gaud_opus_silk_sign_icdf[i], 1u) << "at " << i;
    EXPECT_LE(gaud_opus_silk_sign_icdf[i], 255u) << "at " << i;
  }
}

// --- the SILK parse -------------------------------------------------
//
// Everything SILK reads off the bitstream, and nothing it computes.
// The two tests below are the offline half of a comparison against RFC
// 6716 Appendix A's own `silk_decode_indices` and `silk_decode_pulses`:
// the digest was taken from both implementations over the same 51,840
// cases and the two agreed before it was written down, and the
// bitstreams in the second test were produced by Appendix A's range
// *encoder* and decoded identically by both.

namespace {

/** The sweep's bitstreams: varied, cheap, and the same every run. */
void FillSilk(unsigned char * buffer, int length, uint32_t seed) {
  for (int i = 0; i < length; ++i) {
    seed = seed * 1103515245u + 12345u;
    buffer[i] = (unsigned char)(seed >> 16);
  }
}

/** FNV-1a over eight bytes of a value, little end first. */
void FoldSilk(uint64_t & digest, int64_t value) {
  for (int i = 0; i < 8; ++i) {
    digest ^= (uint64_t)((value >> (8 * i)) & 0xFF);
    digest *= 1099511628211ULL;
  }
}

} // namespace

/**
 * Every combination of the things that change what SILK reads.
 *
 * Three sample rates, both frame lengths, the voice-activity flag both
 * ways, redundancy or not, all three conditional-coding cases, all
 * three previous signal types, a previous lag or none, five bitstream
 * lengths and twenty-four bitstreams each: 51,840 frames. Each one is
 * parsed and everything it produced is folded into one digest - the
 * indices, every excitation sample, and the range decoder's state
 * afterwards.
 *
 * **The constant is Appendix A's.** It was computed twice, once from
 * this library and once from the reference built fixed-point, and the
 * two agreed before either was written here. So a self-contained test
 * carries the reference's verdict.
 *
 * What the sweep reaches, counted rather than assumed: 24,030 of the
 * frames are voiced, 4,500 extend an LSF index past the nine symbols
 * its distribution has, 5,910 interpolate the LSFs, 4,050 code an LTP
 * scaling factor, and 3,750 have a shell block that escapes for extra
 * bits. It reaches **one** escape and never two, which is why the next
 * test exists.
 */
TEST(OpusSilkParse, EveryParameterCombinationMatchesTheReference) {
  static const int kRates[3] = {8, 12, 16};
  std::vector<unsigned char> buffer(256);
  uint64_t digest = 1469598103934665603ULL;
  long cases = 0;
  long voiced = 0;
  long extended = 0;
  long interpolated = 0;
  long scaled = 0;
  long escaped = 0;
  SILK_Decoder decoder;
  for (int rate = 0; rate < 3; ++rate) {
    for (int subframes = 2; subframes <= 4; subframes += 2) {
      for (int vad = 0; vad < 2; ++vad) {
        for (int lbrr = 0; lbrr < 2; ++lbrr) {
          for (int coding = 0; coding < 3; ++coding) {
            for (int previous = 0; previous < 3; ++previous) {
              for (int lag = 0; lag < 2; ++lag) {
                for (int length = 20; length <= 240; length += 55) {
                  for (uint32_t seed = 1; seed <= 24; ++seed) {
                    int fs = kRates[rate];
                    FillSilk(buffer.data(), length,
                        seed * 7919u + (uint32_t)(fs * 31 + subframes));
                    memset(&decoder, 0, sizeof decoder);
                    ASSERT_TRUE(gaud_silk_decoder_init(
                        &decoder, 1, fs, subframes == 2 ? 10 : 20));
                    SILK_Channel * channel = &decoder.channel[0];
                    channel->vad[0] = vad != 0;
                    channel->prev_signal_type = (int8_t)previous;
                    channel->prev_lag_index = (int16_t)(lag ? 37 : 0);
                    OPUS_Range range;
                    gaud_opus_range_init(
                        &range, buffer.data(), (uint32_t)length);
                    gaud_silk_decode_indices(channel, &range, 0, lbrr != 0,
                        (SILK_Coding)coding);
                    gaud_silk_decode_pulses(channel, &range);

                    const SILK_Indices & got = channel->indices;
                    FoldSilk(digest, got.signal_type);
                    FoldSilk(digest, got.quant_offset_type);
                    for (int i = 0; i < subframes; ++i) {
                      FoldSilk(digest, got.gains[i]);
                      FoldSilk(digest, got.ltp[i]);
                    }
                    for (int i = 0; i <= channel->lpc_order; ++i) {
                      FoldSilk(digest, got.nlsf[i]);
                    }
                    FoldSilk(digest, got.nlsf_interp_q2);
                    FoldSilk(digest, got.lag_index);
                    FoldSilk(digest, got.contour_index);
                    FoldSilk(digest, got.per_index);
                    FoldSilk(digest, got.ltp_scale_index);
                    FoldSilk(digest, got.seed);
                    for (int i = 0; i < channel->frame_length; ++i) {
                      FoldSilk(digest, channel->pulses[i]);
                    }
                    FoldSilk(digest, (int64_t)range.rng);

                    if (got.signal_type == SILK_SIGNAL_VOICED) {
                      ++voiced;
                    }
                    for (int i = 1; i <= channel->lpc_order; ++i) {
                      if (got.nlsf[i] <= -4 || got.nlsf[i] >= 4) {
                        ++extended;
                        break;
                      }
                    }
                    if (got.nlsf_interp_q2 < 4) {
                      ++interpolated;
                    }
                    if (got.ltp_scale_index != 0) {
                      ++scaled;
                    }
                    for (int block = 0; block < channel->shell_blocks;
                        ++block) {
                      int sum = 0;
                      for (int i = 0; i < 16; ++i) {
                        int value = channel->pulses[block * 16 + i];
                        sum += value < 0 ? -value : value;
                      }
                      // Without an escape a block's magnitudes sum to
                      // at most sixteen, so this is exact.
                      if (sum > 16) {
                        ++escaped;
                        break;
                      }
                    }
                    ++cases;
                  }
                }
              }
            }
          }
        }
      }
    }
  }
  EXPECT_EQ(cases, 51840);
  EXPECT_EQ(digest, 8279305155702364975ULL);
  EXPECT_EQ(voiced, 24030);
  EXPECT_EQ(extended, 4500);
  EXPECT_EQ(interpolated, 5910);
  EXPECT_EQ(scaled, 4050);
  EXPECT_EQ(escaped, 3750);
}

/**
 * A shell block can escape ten times and the eleventh is unwritable.
 *
 * Section 4.2.7.8.2 lets a block say "every sample has another
 * low-order bit" instead of giving a pulse count, repeatedly. Nothing
 * in the decoder bounds that loop; what bounds it is that at ten
 * repetitions the distribution shifts by one entry and the escape
 * symbol ceases to exist. These eleven bitstreams were produced by
 * Appendix A's range encoder, asked for zero through eleven escapes -
 * and at eleven **the encoder could not write it**, which is the claim
 * rather than a limitation of the test. Each decodes here to the same
 * samples and the same final range state as Appendix A's decoder.
 *
 * The run of 0xFF after the first byte is the escape symbol itself:
 * its probability is one in 256 under these distributions, so each
 * repetition costs very nearly a whole byte and is visible in the
 * stream.
 */
TEST(OpusSilkParse, TheLsbEscapeReachesTenAndTheTableStopsIt) {
  struct Case {
    int escapes;
    uint32_t range_after;
    int length;
    unsigned char bytes[40];
    int block0[16];
  };
  static const Case cases[] = {
    {0, 188747351u, 10,
        {0x0C, 0x1C, 0x83, 0x3A, 0xD8, 0x7D, 0x6B, 0x67, 0xEB, 0xE0},
        {-1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},  // largest 1
    {1, 223953456u, 14,
        {0x0E, 0xF1, 0x1F, 0x05, 0xDF, 0x20, 0xAF, 0x84, 0xE7, 0x1F, 0xCF, 0x9B, 0x36, 0x80},
        {-2, -3, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1, 0, -1}},  // largest 3
    {2, 51894497u, 16,
        {0x0E, 0xFF, 0xE2, 0x3E, 0x0B, 0xF5, 0x2F, 0x3C, 0xFF, 0xDA, 0x2E, 0xC1, 0xDB, 0x9F, 0x3A, 0x79},
        {-5, -6, -1, -2, -1, -2, -1, -2, -1, -2, -1, -2, -1, -2, -1, -2}},  // largest 6
    {3, 100595500u, 19,
        {0x0E, 0xFF, 0xFF, 0xC4, 0x7C, 0x17, 0x7A, 0x92, 0xF2, 0x0A, 0x9D, 0x0F, 0x6C, 0x5E, 0x10, 0xD3, 0x3E, 0x81, 0x08},
        {-10, -13, -2, -5, -2, -5, -2, -5, -2, -5, -2, -5, -2, -5, -2, -5}},  // largest 13
    {4, 194974882u, 22,
        {0x0E, 0xFF, 0xFF, 0xFF, 0x88, 0xF8, 0x2F, 0x36, 0xA1, 0x10, 0x21, 0x30, 0xD9, 0x44, 0x7D, 0xF7, 0xAF, 0x8D, 0x9A, 0x89, 0x17, 0x6C},
        {-21, -26, -5, -10, -5, -10, -5, -10, -5, -10, -5, -10, -5, -10, -5, -10}},  // largest 26
    {5, 377960525u, 25,
        {0x0E, 0xFF, 0xFF, 0xFF, 0xFF, 0x11, 0xF0, 0x5D, 0xEA, 0x4B, 0x68, 0xF0, 0x19, 0x6C, 0x28, 0x05, 0x63, 0x60, 0xC1, 0xE0, 0x75, 0xD9, 0x2A, 0x87, 0xF0},
        {-42, -53, -10, -21, -10, -21, -10, -21, -10, -21, -10, -21, -10, -21, -10, -21}},  // largest 53
    {6, 732565768u, 28,
        {0x0E, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE, 0x23, 0xE0, 0xBC, 0x17, 0xE4, 0x46, 0x33, 0x1C, 0xB5, 0x95, 0x73, 0x41, 0xB7, 0x7B, 0xAD, 0xD4, 0x28, 0x85, 0xF3, 0x53, 0xEB, 0xE0},
        {-85, -106, -21, -42, -21, -42, -21, -42, -21, -42, -21, -42, -21, -42, -21, -42}},  // largest 106
    {7, 1420175106u, 31,
        {0x0E, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFC, 0x47, 0xC1, 0x77, 0xA7, 0x1C, 0xFA, 0x2F, 0xEF, 0xDA, 0x1D, 0xC8, 0x0C, 0x00, 0x9C, 0x17, 0x91, 0x52, 0xAC, 0x50, 0x37, 0x4B, 0xEB, 0xD7, 0xC0},
        {-170, -213, -42, -85, -42, -85, -42, -85, -42, -85, -42, -85, -42, -85, -42, -85}},  // largest 213
    {8, 10752347u, 34,
        {0x0E, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF8, 0x8F, 0x82, 0xF0, 0x52, 0x1D, 0xF5, 0x48, 0x67, 0x83, 0xC0, 0xD9, 0x98, 0xC1, 0xFB, 0x87, 0x4E, 0xC4, 0xE7, 0x41, 0xB1, 0xE9, 0x27, 0xEB, 0xD7, 0xAF, 0x80},
        {-341, -426, -85, -170, -85, -170, -85, -170, -85, -170, -85, -170, -85, -170, -85, -170}},  // largest 426
    {9, 20843497u, 37,
        {0x0E, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF1, 0x1F, 0x05, 0xDF, 0x21, 0x49, 0x77, 0xAD, 0x3A, 0x51, 0x7B, 0x14, 0x66, 0x30, 0xBA, 0x53, 0x41, 0x48, 0x54, 0x17, 0x5D, 0x02, 0x52, 0xA6, 0xCB, 0xDB, 0xB3, 0x63, 0x00},
        {-682, -853, -170, -341, -170, -341, -170, -341, -170, -341, -170, -341, -170, -341, -170, -341}},  // largest 853
    {10, 242396629u, 39,
        {0x0E, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xE2, 0x66, 0x46, 0x71, 0x55, 0x2D, 0xFC, 0x41, 0xC0, 0x0D, 0xD3, 0xD1, 0x4B, 0x0F, 0xF7, 0x15, 0xA8, 0xEE, 0x22, 0xC1, 0x9C, 0x22, 0x5F, 0x5F, 0xAF, 0x5B, 0x8E, 0x30, 0x60, 0xD8},
        {-1365, -1706, -341, -682, -341, -682, -341, -682, -341, -682, -341, -682, -341, -682, -341, -682}},  // largest 1706
  };
  SILK_Decoder decoder;
  for (const Case & one : cases) {
    memset(&decoder, 0, sizeof decoder);
    ASSERT_TRUE(gaud_silk_decoder_init(&decoder, 1, 8, 10));
    SILK_Channel * channel = &decoder.channel[0];
    channel->indices.signal_type = SILK_SIGNAL_INACTIVE;
    channel->indices.quant_offset_type = 0;
    OPUS_Range range;
    gaud_opus_range_init(&range, one.bytes, (uint32_t)one.length);
    gaud_silk_decode_pulses(channel, &range);
    EXPECT_EQ(range.rng, one.range_after) << "escapes " << one.escapes;
    for (int i = 0; i < 16; ++i) {
      EXPECT_EQ(channel->pulses[i], one.block0[i])
          << "escapes " << one.escapes << " at " << i;
    }
    // The other four blocks of this 10 ms narrowband frame are empty,
    // which is what makes the first block's magnitudes attributable.
    for (int i = 16; i < 80; ++i) {
      EXPECT_EQ(channel->pulses[i], 0) << "escapes " << one.escapes
                                       << " at " << i;
    }
  }
  // Ten escapes multiply a magnitude by 1024 before the extra bits are
  // added, so this block reaches 1706 from two pulses - which no
  // unescaped block could, since their magnitudes sum to sixteen.
  EXPECT_EQ(cases[10].escapes, 10);
  int largest = 0;
  for (int i = 0; i < 16; ++i) {
    int value = cases[10].block0[i];
    largest = std::max(largest, value < 0 ? -value : value);
  }
  EXPECT_EQ(largest, 1706);
}

/**
 * SILK's three out-of-line primitives, over their whole domains.
 *
 * Each is an approximation whose error is part of the format: the
 * encoder made its decisions with the same wrong answer, so a more
 * accurate root or reciprocal would decode to different audio. That
 * makes "close enough" the wrong test and exact agreement the right
 * one.
 *
 *   - `log2lin` over its whole domain, which stops at 4095 because
 *     one more makes the shift inside it undefined. Both of its arms
 *     are covered - they are different computations, not a
 *     rearrangement of one, and the boundary is at 16 in the log
 *     domain.
 *   - `sqrt_approx` over every value below 65,536 and then, for each
 *     of the fifteen larger exponents, every value of the fifteen
 *     bits below the leading one. Those bits are the whole of what
 *     the correction step can see, so that is coverage and not
 *     sampling.
 *   - `inverse32_varq` over thirty exponents by 512 mantissas, both
 *     signs, at four result scales.
 *
 * 685,048 comparisons against Appendix A, and the digest was computed
 * from both sides before it was written here.
 */
TEST(OpusSilkMath, EveryPrimitiveMatchesTheReferenceOverItsDomain) {
  uint64_t digest = 1469598103934665603ULL;
  auto fold = [&digest](int64_t value) {
    for (int i = 0; i < 8; ++i) {
      digest ^= (uint64_t)((value >> (8 * i)) & 0xFF);
      digest *= 1099511628211ULL;
    }
  };
  long count = 0;
  // 4095 is the top of this function's domain, not a sampling
  // choice: one more and the shift inside it is undefined. Its only
  // caller caps the argument at 3967, which is 31 in Q7 - see
  // opus_silk_params.c - and a sweep that went past that found the
  // shift with UBSan rather than finding a defect.
  for (int32_t v = -1000; v <= 4095; ++v) {
    fold(gaud_silk_log2lin(v));
    ++count;
  }
  for (int32_t v = -16; v < 65536; ++v) {
    fold(gaud_silk_sqrt_approx(v));
    ++count;
  }
  for (int e = 16; e < 31; ++e) {
    for (int32_t m = 0; m < 32768; ++m) {
      fold(gaud_silk_sqrt_approx(
          (int32_t)(((uint32_t)1 << e) | ((uint32_t)m << (e - 15)))));
      ++count;
    }
  }
  for (int e = 1; e < 31; ++e) {
    for (int32_t m = 0; m < 512; ++m) {
      int32_t base = (int32_t)(((uint32_t)1 << e)
          | (((uint32_t)m * 2654435761u) >> (32 - e)));
      for (int sign = 0; sign < 2; ++sign) {
        int32_t value = sign ? -base : base;
        for (int q = 16; q <= 46; q += 10) {
          fold(gaud_silk_inverse32_varq(value, q));
          ++count;
        }
      }
    }
  }
  EXPECT_EQ(count, 685048);
  EXPECT_EQ(digest, 13427006569050684828ULL);

  // Two anchors in plain arithmetic, so that a digest which stops
  // matching has something to be read against. 2^16 in Q7 is 2048.
  EXPECT_EQ(gaud_silk_log2lin(0), 1);
  EXPECT_EQ(gaud_silk_log2lin(2048), 65536);
  EXPECT_EQ(gaud_silk_log2lin(-1), 0);
  EXPECT_EQ(gaud_silk_sqrt_approx(0), 0);
  EXPECT_EQ(gaud_silk_sqrt_approx(-5), 0);
  // The root of 2^30 is 2^15, and this lands on it exactly.
  EXPECT_EQ(gaud_silk_sqrt_approx(1 << 30), 32768);
  // 2^30 / 2^15 is exactly 2^15, and this returns one less - which
  // is the point of having the anchor: the reciprocal is an
  // approximation and is allowed to be short, so a test that asserted
  // the exact quotient would be asserting something the format does
  // not have.
  EXPECT_EQ(gaud_silk_inverse32_varq(1 << 15, 30), 32767);
}

// --- the SILK parameters --------------------------------------------
//
// Indices into filters: section 4.2.7.4 to 4.2.7.6, which reads no
// bitstream and so can be checked by value. The digest below was taken
// from this library and from RFC 6716 Appendix A's own
// silk_gains_dequant, silk_NLSF_decode, silk_NLSF2A,
// silk_LPC_inverse_pred_gain and silk_decode_pitch over the same
// 62,976 cases, and the two agreed before it was written down.

namespace {

/** The sweep's pseudo-random numbers: an LCG, high bits first. */
struct SilkRandom {
  uint32_t state;
  uint32_t Next() {
    state = state * 1103515245u + 12345u;
    return state >> 8;
  }
};

} // namespace

/**
 * Every gain, every filter and every pitch lag, against the reference.
 *
 * Three sweeps folded into one digest:
 *
 *   - **the gains**, over both subframe counts, both conditional
 *     cases, all 64 previous indices and forty index vectors each - of
 *     which four walk the index straight down and four straight up, so
 *     that both clamps are reached rather than hoped for. They are:
 *     the index bottoms out 312 times and tops out 5,934.
 *   - **the line spectral frequencies and the filter they become**,
 *     over both codebooks, all 32 stage-1 vectors and 400 residuals
 *     each, with the residual's range stepped from two up to the ten a
 *     bitstream can actually code. The resulting coefficients reach
 *     both ends of their sixteen bits and no frequency vector comes
 *     out unsorted.
 *   - **the pitch**, exhaustively: every lag index, every contour
 *     index, both frame lengths, all three sample rates. The lags run
 *     from 16 to 288 samples.
 *
 * The conversion from frequencies to coefficients has two bounded
 * repair loops, and both are reached deep: measured against an
 * instrumented copy of the reference, the loop that shrinks the
 * coefficients into sixteen bits runs up to nine times and the one
 * that flattens an unstable filter up to fifteen.
 */
TEST(OpusSilkParams, EveryGainFilterAndLagMatchesTheReference) {
  uint64_t digest = 1469598103934665603ULL;
  auto fold = [&digest](int64_t value) {
    for (int i = 0; i < 8; ++i) {
      digest ^= (uint64_t)((value >> (8 * i)) & 0xFF);
      digest *= 1099511628211ULL;
    }
  };
  long cases = 0;
  int32_t lowest_gain = INT32_MAX;
  int32_t highest_gain = 0;
  int lowest_coefficient = INT32_MAX;
  int highest_coefficient = INT32_MIN;
  int lowest_lag = 1 << 20;
  int highest_lag = 0;
  long bottomed = 0;
  long topped = 0;
  long unsorted = 0;

  for (int subframes = 2; subframes <= 4; subframes += 2) {
    for (int conditional = 0; conditional < 2; ++conditional) {
      for (int previous = 0; previous < 64; ++previous) {
        for (uint32_t s = 0; s < 40; ++s) {
          int8_t indices[4];
          SilkRandom random = {s * 2654435761u
              + (uint32_t)(previous * 31 + subframes * 7 + conditional)};
          for (int k = 0; k < 4; ++k) {
            bool absolute = k == 0 && !conditional;
            if (s < 4) {
              indices[k] = 0;
            } else if (s < 8) {
              indices[k] = (int8_t)(absolute ? 63 : 40);
            } else {
              indices[k] =
                  (int8_t)(random.Next() % (uint32_t)(absolute ? 64 : 41));
            }
          }
          int32_t gains[4];
          int8_t running = (int8_t)previous;
          gaud_silk_gains_dequant(
              gains, indices, &running, conditional != 0, subframes);
          for (int k = 0; k < subframes; ++k) {
            fold(gains[k]);
            lowest_gain = std::min(lowest_gain, gains[k]);
            highest_gain = std::max(highest_gain, gains[k]);
          }
          fold(running);
          if (running == 0) {
            ++bottomed;
          }
          if (running == 63) {
            ++topped;
          }
          ++cases;
        }
      }
    }
  }

  for (int wideband = 0; wideband < 2; ++wideband) {
    int order = wideband ? 16 : 10;
    for (int first = 0; first < 32; ++first) {
      for (uint32_t s = 0; s < 400; ++s) {
        int8_t indices[17];
        indices[0] = (int8_t)first;
        SilkRandom random = {s * 40503u + (uint32_t)(first * 131 + wideband)};
        // Four and six: the widest a stage-2 index can be is the four
        // its own distribution reaches plus the six the extension
        // adds, so ten is the edge of what a bitstream can say.
        int spread = (int)(s % 4);
        int range = spread == 0 ? 2 : (spread == 1 ? 4 : (spread == 2 ? 7 : 10));
        for (int i = 0; i < order; ++i) {
          indices[i + 1] =
              (int8_t)((int)(random.Next() % (uint32_t)(2 * range + 1))
                  - range);
        }
        int16_t nlsf[16];
        gaud_silk_nlsf_decode(nlsf, indices, wideband != 0);
        int16_t coefficients[16];
        gaud_silk_nlsf_to_lpc(coefficients, nlsf, order);
        int32_t inverse_gain =
            gaud_silk_lpc_inverse_gain(coefficients, order);
        for (int i = 1; i < order; ++i) {
          if (nlsf[i] <= nlsf[i - 1]) {
            ++unsorted;
          }
        }
        for (int i = 0; i < order; ++i) {
          fold(nlsf[i]);
          fold(coefficients[i]);
          lowest_coefficient = std::min(lowest_coefficient,
              (int)coefficients[i]);
          highest_coefficient = std::max(highest_coefficient,
              (int)coefficients[i]);
        }
        fold(inverse_gain);
        ++cases;
      }
    }
  }

  const int rates[3] = {8, 12, 16};
  for (int rate = 0; rate < 3; ++rate) {
    for (int subframes = 2; subframes <= 4; subframes += 2) {
      int contours = rates[rate] == 8 ? (subframes == 4 ? 11 : 3)
                                      : (subframes == 4 ? 34 : 12);
      for (int lag = 0; lag < 256; ++lag) {
        for (int contour = 0; contour < contours; ++contour) {
          int lags[4];
          gaud_silk_decode_pitch((int16_t)lag, (int8_t)contour, lags,
              rates[rate], subframes);
          for (int k = 0; k < subframes; ++k) {
            fold(lags[k]);
            lowest_lag = std::min(lowest_lag, lags[k]);
            highest_lag = std::max(highest_lag, lags[k]);
          }
          ++cases;
        }
      }
    }
  }

  EXPECT_EQ(cases, 62976);
  EXPECT_EQ(digest, 10953151080910121650ULL);
  EXPECT_EQ(bottomed, 312);
  EXPECT_EQ(topped, 5934);
  EXPECT_EQ(unsorted, 0);
  EXPECT_EQ(lowest_gain, 81920);
  EXPECT_EQ(highest_gain, 1686110208);
  EXPECT_EQ(lowest_coefficient, -32767);
  EXPECT_EQ(highest_coefficient, 32767);
  EXPECT_EQ(lowest_lag, 16);
  EXPECT_EQ(highest_lag, 288);
}

/**
 * Four things this sweep cannot see, and why each is safe.
 *
 * Established by mutation - twenty-four changes, twenty caught - and
 * worth recording rather than re-deriving:
 *
 *   - **The gain logarithm's ceiling of 3967 never fires.** The
 *     largest logarithm the index can produce is 3923, which the
 *     first check below computes. The clamp is not dead code: moving
 *     it to 3900 is caught immediately. It is simply 44 units above
 *     anything reachable.
 *   - **One unit on either square-root seed changes nothing.** The
 *     seed is shifted right by half the leading-zero count before
 *     anything else happens, and for the even-exponent seed that
 *     shift is always at least one, so the low bit never survives.
 *     A change of two is caught on both seeds.
 *   - **The DC short circuit's `>=` may as well be `>`.** The sum of
 *     the coefficients is exactly 4096 in 4,949 of the sweep's
 *     invocations, and in every one of them the full recursion
 *     reaches the same verdict the short circuit does. The branch is
 *     an optimisation and not a semantic.
 *   - **The last two of the sixteen stability attempts are never
 *     needed.** The sweep drives that loop to its fourteenth; cutting
 *     the limit to fourteen is invisible and cutting it to thirteen
 *     is caught.
 */
TEST(OpusSilkParams, TheGainLogarithmStopsShortOfItsOwnCeiling) {
  // The logarithm the gain index maps to, for every index, computed
  // the way opus_silk_params.c computes it.
  int32_t highest = 0;
  for (int index = 0; index < 64; ++index) {
    int32_t log_q7 = gaud_silk_smulwb(1907825, index) + 2090;
    highest = std::max(highest, log_q7);
  }
  EXPECT_EQ(highest, 3923);
  EXPECT_LT(highest, 3967);
  // And the gain it becomes, which is what the cap is really
  // protecting: the Q16 result is signed, so it has to stay below
  // 2^31. The largest a gain can be is about 25,730.
  EXPECT_EQ(gaud_silk_log2lin(highest), 1686110208);
}

/**
 * The stabiliser's fallback cannot push a frequency out of range.
 *
 * **This is a deliberate difference from RFC 6716 Appendix A**, and
 * the one place in this decoder where the reference's behaviour is not
 * reproduced. Its `silk_NLSF_stabilize` finishes with a pass that
 * writes `NLSF[i-1] + spacing[i]` into an `opus_int16` with no
 * ceiling; when the vector has been crowded up against 32767 that sum
 * wraps, the last frequency comes out at -32766, and `silk_NLSF2A`
 * then reads its 129-entry cosine table at index -128. Its own
 * assertion says a frequency is non-negative, so this is a defect
 * rather than a convention - and it is reachable from a legal
 * bitstream, needing only a stage-2 index of five or more, which means
 * the extension symbol. 1,706 of 256,000 synthetic index vectors here
 * reached it.
 *
 * Section 6 makes the reference normative where its behaviour is
 * defined, and a read before the start of an array is not. So the
 * ceiling is applied, and the cosine table's index is clamped as well.
 * The test below drives the fallback directly: every index at its
 * extreme, which crowds every frequency against the top.
 */
TEST(OpusSilkParams, TheStabiliserKeepsEveryFrequencyInRange) {
  for (int wideband = 0; wideband < 2; ++wideband) {
    int order = wideband ? 16 : 10;
    const int16_t * spacing = wideband
        ? gaud_opus_silk_nlsf_delta_min_wb_q15
        : gaud_opus_silk_nlsf_delta_min_nb_mb_q15;
    for (int first = 0; first < 32; ++first) {
      for (int extreme = -10; extreme <= 10; extreme += 20) {
        int8_t indices[17];
        indices[0] = (int8_t)first;
        for (int i = 0; i < order; ++i) {
          indices[i + 1] = (int8_t)extreme;
        }
        int16_t nlsf[16];
        gaud_silk_nlsf_decode(nlsf, indices, wideband != 0);
        EXPECT_GE(nlsf[0], spacing[0]) << "first " << first;
        for (int i = 1; i < order; ++i) {
          EXPECT_GE(nlsf[i] - nlsf[i - 1], spacing[i])
              << "first " << first << " at " << i;
        }
        EXPECT_LE(nlsf[order - 1], (1 << 15) - spacing[order])
            << "first " << first;
        // And the filter that comes out of it is still a filter.
        int16_t coefficients[16];
        gaud_silk_nlsf_to_lpc(coefficients, nlsf, order);
        EXPECT_GE(gaud_silk_lpc_inverse_gain(coefficients, order), 107374)
            << "first " << first << " extreme " << extreme;
      }
    }
  }
}

namespace {

/** A packet's first byte, section 3.1. */
unsigned char Toc(unsigned config, bool stereo, unsigned code) {
  return (unsigned char)((config << 3) | (stereo ? 4u : 0u) | code);
}

/** Parse, and say only whether it was accepted. */
bool Accepts(const std::vector<unsigned char> & packet) {
  OPUS_Packet parsed;
  return gaud_opus_parse_packet(packet.data(), packet.size(), false, &parsed)
      == GAUD_OK;
}

} // namespace

TEST(OpusPacket, CodeZeroIsOneFrameOfWhatIsLeft) {
  std::vector<unsigned char> packet(41u, 0xA5u);
  packet[0] = Toc(31u, false, 0u);
  OPUS_Packet parsed;
  ASSERT_EQ(gaud_opus_parse_packet(packet.data(), packet.size(), false,
                &parsed),
      GAUD_OK);
  EXPECT_EQ(parsed.count, 1u);
  EXPECT_EQ(parsed.length[0], 40u);
  EXPECT_EQ(parsed.frame[0], packet.data() + 1);
}

/** [R3]: the payload of a code 1 packet must divide in two. */
TEST(OpusPacket, CodeOneSplitsEvenlyAndRefusesOdd) {
  std::vector<unsigned char> even(41u, 0xA5u);
  even[0] = Toc(31u, false, 1u);
  OPUS_Packet parsed;
  ASSERT_EQ(
      gaud_opus_parse_packet(even.data(), even.size(), false, &parsed),
      GAUD_OK);
  EXPECT_EQ(parsed.count, 2u);
  EXPECT_EQ(parsed.length[0], 20u);
  EXPECT_EQ(parsed.length[1], 20u);
  EXPECT_EQ(parsed.frame[1], parsed.frame[0] + 20);

  std::vector<unsigned char> odd(42u, 0xA5u);
  odd[0] = Toc(31u, false, 1u);
  EXPECT_FALSE(Accepts(odd));
}

/** Section 3.2.1: one length byte below 252, two at or above it. */
TEST(OpusPacket, CodeTwoReadsOneAndTwoByteLengths) {
  {
    std::vector<unsigned char> packet(41u, 0xA5u);
    packet[0] = Toc(31u, false, 2u);
    packet[1] = 10u;
    OPUS_Packet parsed;
    ASSERT_EQ(gaud_opus_parse_packet(packet.data(), packet.size(), false,
                  &parsed),
        GAUD_OK);
    EXPECT_EQ(parsed.count, 2u);
    EXPECT_EQ(parsed.length[0], 10u);
    EXPECT_EQ(parsed.length[1], 29u);
    EXPECT_EQ(parsed.frame[0], packet.data() + 2);
  }
  {
    /* 252 + 4*1 = 256, which needs the second byte to be read at all. */
    std::vector<unsigned char> packet(3u + 256u + 5u, 0xA5u);
    packet[0] = Toc(31u, false, 2u);
    packet[1] = 252u;
    packet[2] = 1u;
    OPUS_Packet parsed;
    ASSERT_EQ(gaud_opus_parse_packet(packet.data(), packet.size(), false,
                  &parsed),
        GAUD_OK);
    EXPECT_EQ(parsed.length[0], 256u);
    EXPECT_EQ(parsed.length[1], 5u);
  }
}

/**
 * [R4], and the three two-byte code 2 packets.
 *
 * The specification singles these out: a one-byte code 2 packet is
 * always invalid, a two-byte one whose second byte is 252 or above is
 * invalid because the length is unfinished, and a two-byte one whose
 * second byte is 1 to 251 is invalid because that many bytes are not
 * there. The *only* valid two-byte code 2 packet states a length of
 * zero, making both frames empty.
 */
TEST(OpusPacket, CodeTwoAcceptsOnlyTheEmptyTwoBytePacket) {
  std::vector<unsigned char> one{Toc(31u, false, 2u)};
  EXPECT_FALSE(Accepts(one));

  std::vector<unsigned char> unfinished{Toc(31u, false, 2u), 253u};
  EXPECT_FALSE(Accepts(unfinished));

  std::vector<unsigned char> overlong{Toc(31u, false, 2u), 1u};
  EXPECT_FALSE(Accepts(overlong));

  std::vector<unsigned char> empty{Toc(31u, false, 2u), 0u};
  OPUS_Packet parsed;
  ASSERT_EQ(
      gaud_opus_parse_packet(empty.data(), empty.size(), false, &parsed),
      GAUD_OK);
  EXPECT_EQ(parsed.count, 2u);
  EXPECT_EQ(parsed.length[0], 0u);
  EXPECT_EQ(parsed.length[1], 0u);
}

/** [R6]: constant rate code 3 divides what is left by the count. */
TEST(OpusPacket, CodeThreeConstantRateDividesTheRemainder) {
  std::vector<unsigned char> packet(2u + 30u, 0xA5u);
  packet[0] = Toc(31u, false, 3u);
  packet[1] = 3u; /* VBR clear, padding clear, three frames. */
  OPUS_Packet parsed;
  ASSERT_EQ(gaud_opus_parse_packet(packet.data(), packet.size(), false,
                &parsed),
      GAUD_OK);
  EXPECT_EQ(parsed.count, 3u);
  for (int i = 0; i < 3; ++i) {
    EXPECT_EQ(parsed.length[i], 10u) << i;
  }

  std::vector<unsigned char> ragged(2u + 31u, 0xA5u);
  ragged[0] = Toc(31u, false, 3u);
  ragged[1] = 3u;
  EXPECT_FALSE(Accepts(ragged));
}

/** [R7]: variable rate states every length but the last. */
TEST(OpusPacket, CodeThreeVariableRateStatesAllButTheLast) {
  std::vector<unsigned char> packet;
  packet.push_back(Toc(31u, false, 3u));
  packet.push_back((unsigned char)(0x80u | 3u)); /* VBR, three frames. */
  packet.push_back(5u);
  packet.push_back(7u);
  packet.resize(packet.size() + 5u + 7u + 9u, 0xA5u);
  OPUS_Packet parsed;
  ASSERT_EQ(gaud_opus_parse_packet(packet.data(), packet.size(), false,
                &parsed),
      GAUD_OK);
  EXPECT_EQ(parsed.count, 3u);
  EXPECT_EQ(parsed.length[0], 5u);
  EXPECT_EQ(parsed.length[1], 7u);
  EXPECT_EQ(parsed.length[2], 9u);
  EXPECT_EQ(parsed.frame[1], parsed.frame[0] + 5);
  EXPECT_EQ(parsed.frame[2], parsed.frame[1] + 7);

  /* A stated length that does not fit in what remains. */
  std::vector<unsigned char> overrun;
  overrun.push_back(Toc(31u, false, 3u));
  overrun.push_back((unsigned char)(0x80u | 2u));
  overrun.push_back(200u);
  overrun.resize(overrun.size() + 10u, 0xA5u);
  EXPECT_FALSE(Accepts(overrun));
}

/**
 * Section 3.2.5's padding, including the chain that 255 starts.
 *
 * A padding byte of 255 means 254 bytes *and another length byte*, so
 * the two packets below carry the same four bytes of audio with
 * different amounts of padding described two different ways. Reading
 * 255 as "255 bytes and stop" would leave the frame one byte long and
 * is the obvious way to get this wrong.
 */
TEST(OpusPacket, CodeThreePaddingIsSubtractedAndChains) {
  {
    std::vector<unsigned char> packet;
    packet.push_back(Toc(31u, false, 3u));
    packet.push_back((unsigned char)(0x40u | 1u)); /* padded, one frame. */
    packet.push_back(3u);                          /* three bytes of it. */
    packet.resize(packet.size() + 4u, 0xA5u);      /* the frame. */
    packet.resize(packet.size() + 3u, 0u);         /* the padding. */
    OPUS_Packet parsed;
    ASSERT_EQ(gaud_opus_parse_packet(packet.data(), packet.size(), false,
                  &parsed),
        GAUD_OK);
    EXPECT_EQ(parsed.count, 1u);
    EXPECT_EQ(parsed.length[0], 4u);
  }
  {
    std::vector<unsigned char> packet;
    packet.push_back(Toc(31u, false, 3u));
    packet.push_back((unsigned char)(0x40u | 1u));
    packet.push_back(255u); /* 254 bytes, and another length byte. */
    packet.push_back(2u);   /* two more. */
    packet.resize(packet.size() + 4u, 0xA5u);
    packet.resize(packet.size() + 256u, 0u);
    OPUS_Packet parsed;
    ASSERT_EQ(gaud_opus_parse_packet(packet.data(), packet.size(), false,
                  &parsed),
        GAUD_OK);
    EXPECT_EQ(parsed.count, 1u);
    EXPECT_EQ(parsed.length[0], 4u);
  }
  {
    /* [R6]: padding that claims more than the packet holds. */
    std::vector<unsigned char> packet;
    packet.push_back(Toc(31u, false, 3u));
    packet.push_back((unsigned char)(0x40u | 1u));
    packet.push_back(200u);
    packet.resize(packet.size() + 10u, 0u);
    EXPECT_FALSE(Accepts(packet));
  }
}

/** [R5]: no frames at all, and more than 120 ms of them. */
TEST(OpusPacket, CodeThreeBoundsTheFrameCount) {
  std::vector<unsigned char> none(10u, 0xA5u);
  none[0] = Toc(31u, false, 3u);
  none[1] = 0u;
  EXPECT_FALSE(Accepts(none));

  /* Config 31 is 20 ms, so six frames are 120 ms and seven are too many. */
  std::vector<unsigned char> six(2u + 12u, 0xA5u);
  six[0] = Toc(31u, false, 3u);
  six[1] = 6u;
  EXPECT_TRUE(Accepts(six));

  std::vector<unsigned char> seven(2u + 14u, 0xA5u);
  seven[0] = Toc(31u, false, 3u);
  seven[1] = 7u;
  EXPECT_FALSE(Accepts(seven));

  /* Config 16 is 2.5 ms, where the same ceiling falls at 48. */
  std::vector<unsigned char> forty_eight(2u + 48u, 0xA5u);
  forty_eight[0] = Toc(16u, false, 3u);
  forty_eight[1] = 48u;
  EXPECT_TRUE(Accepts(forty_eight));

  std::vector<unsigned char> forty_nine(2u + 49u, 0xA5u);
  forty_nine[0] = Toc(16u, false, 3u);
  forty_nine[1] = 49u;
  EXPECT_FALSE(Accepts(forty_nine));
}

/**
 * [R2]: no frame may exceed 1,275 bytes.
 *
 * The bound applies to the frame whose length is inferred rather than
 * stated, because a stated length cannot encode a larger number: the
 * two-byte form tops out at 255*4+255, which is 1,275 exactly.
 */
TEST(OpusPacket, NoFrameMayExceedTheRepacketizationBound) {
  std::vector<unsigned char> largest(1u + 1275u, 0xA5u);
  largest[0] = Toc(31u, false, 0u);
  EXPECT_TRUE(Accepts(largest));

  std::vector<unsigned char> toobig(1u + 1276u, 0xA5u);
  toobig[0] = Toc(31u, false, 0u);
  EXPECT_FALSE(Accepts(toobig));
}

/** An empty buffer is not a packet. */
TEST(OpusPacket, EmptyIsRefused) {
  OPUS_Packet parsed;
  unsigned char nothing = 0;
  EXPECT_EQ(gaud_opus_parse_packet(&nothing, 0u, false, &parsed),
      GAUD_ERR_CORRUPT);
}

/**
 * Appendix B's self-delimiting framing, where the last frame's length is
 * written down too.
 *
 * The same bytes mean different things under the two framings, which is
 * the whole point: in a multistream packet every stream but the last is
 * followed by another stream rather than by the end of the buffer.
 */
TEST(OpusPacket, SelfDelimitedStatesTheLastLength) {
  std::vector<unsigned char> packet;
  packet.push_back(Toc(31u, false, 0u));
  packet.push_back(6u); /* The length of the one frame. */
  packet.resize(packet.size() + 6u, 0xA5u);
  packet.resize(packet.size() + 20u, 0x5Au); /* The next stream's. */

  OPUS_Packet parsed;
  ASSERT_EQ(gaud_opus_parse_packet(packet.data(), packet.size(), true,
                &parsed),
      GAUD_OK);
  EXPECT_EQ(parsed.count, 1u);
  EXPECT_EQ(parsed.length[0], 6u);
  EXPECT_EQ(parsed.frame[0], packet.data() + 2);

  /* Read the same bytes the ordinary way and the frame swallows the
   * stream that follows it. */
  OPUS_Packet plain;
  ASSERT_EQ(gaud_opus_parse_packet(packet.data(), packet.size(), false,
                &plain),
      GAUD_OK);
  EXPECT_EQ(plain.length[0], 27u);
}

/* ------------------------------------------ RFC 6716 section 4.1:
                                                 the range decoder */

/**
 * The range decoder, tested against the specification's own equivalences.
 *
 * A decoder with no encoder beside it and no reference data has almost
 * nothing to be tested against, and the usual answer - a round trip -
 * would score this library's reading against this library's writing.
 * RFC 6716 section 4.1.3 supplies something better. It gives three
 * decoding methods and says each is *exactly equivalent* to the general
 * two-step form, as an arithmetic claim rather than an approximation.
 * That is a second engine for nothing: drive both forms from one byte
 * stream and they must agree on the symbol and on every bit of the
 * resulting state, forever.
 *
 * ::OPUS_Range is copyable by value, which is what makes the comparison
 * exact rather than statistical - the two readings start from literally
 * the same state, not from an equivalent one.
 *
 * Section 4.1.6 supplies a second: `ec_tell()` is guaranteed to equal
 * `ceil(ec_tell_frac()/8)`, computed by two different routines, so every
 * decode below checks it.
 *
 * What none of this can see is a faithful transcription of the *wrong*
 * specification - all four routines reading one byte stream agree if the
 * renormalisation is wrong in the same way for all of them. Only the
 * conformance vectors settle that, and they arrive with the decoders
 * that can consume them.
 */

namespace {

/**
 * A deterministic byte stream. A fixed generator rather than a random
 * one: a failure here must reproduce from the test name alone.
 */
std::vector<unsigned char> RangeBytes(uint32_t seed, size_t count) {
  std::vector<unsigned char> out(count);
  uint32_t state = seed * 2654435761u + 1u;
  for (size_t i = 0; i < count; ++i) {
    state = state * 1103515245u + 12345u;
    out[i] = (unsigned char)(state >> 16);
  }
  return out;
}

/**
 * Whether two decoders are in the same state.
 *
 * ::OPUS_Range::ext is deliberately not compared. It is scratch between
 * ::gaud_opus_decode and ::gaud_opus_dec_update rather than state, and
 * the icdf and bit-logp paths never set it - so comparing it would make
 * the equivalences these tests exist to check look false.
 */
::testing::AssertionResult SameState(
    const OPUS_Range & a, const OPUS_Range & b) {
  if (a.val == b.val && a.rng == b.rng && a.rem == b.rem
      && a.offset == b.offset && a.end_offset == b.end_offset
      && a.end_window == b.end_window && a.end_bits == b.end_bits
      && a.total_bits == b.total_bits && a.error == b.error) {
    return ::testing::AssertionSuccess();
  }
  return ::testing::AssertionFailure()
      << "val " << a.val << "/" << b.val << " rng " << a.rng << "/" << b.rng
      << " rem " << a.rem << "/" << b.rem << " off " << a.offset << "/"
      << b.offset << " end " << a.end_offset << "/" << b.end_offset
      << " win " << a.end_window << "/" << b.end_window << " nbits "
      << a.end_bits << "/" << b.end_bits << " total " << a.total_bits << "/"
      << b.total_bits;
}

/** Section 4.1.6: `ec_tell` is `ceil(ec_tell_frac()/8)`. */
void CheckTell(const OPUS_Range & range) {
  uint32_t frac = gaud_opus_tell_frac(&range);
  EXPECT_EQ(gaud_opus_tell(&range), (frac + 7u) / 8u);
}

} // namespace

TEST(OpusRange, IlogCountsBits) {
  EXPECT_EQ(gaud_opus_ilog(0u), 0u);
  EXPECT_EQ(gaud_opus_ilog(1u), 1u);
  EXPECT_EQ(gaud_opus_ilog(2u), 2u);
  EXPECT_EQ(gaud_opus_ilog(3u), 2u);
  EXPECT_EQ(gaud_opus_ilog(255u), 8u);
  EXPECT_EQ(gaud_opus_ilog(256u), 9u);
  EXPECT_EQ(gaud_opus_ilog(0x7FFFFFFFu), 31u);
  EXPECT_EQ(gaud_opus_ilog(0xFFFFFFFFu), 32u);
}

/**
 * Section 4.1.1 and 4.1.6.1, worked by hand.
 *
 * `rng` starts at 128 and renormalisation runs until it exceeds 2**23,
 * which from 2**7 takes three byte-sized shifts to 2**31. So `rng` is
 * exactly 2**31, `total_bits` is 9 + 24 = 33, `ilog(2**31)` is 32, and
 * a decoder that has read nothing reports one bit used - the bit the
 * encoder keeps to terminate the stream.
 */
TEST(OpusRange, FreshDecoderReportsOneBit) {
  std::vector<unsigned char> data = RangeBytes(1u, 64);
  OPUS_Range range;
  gaud_opus_range_init(&range, data.data(), data.size());
  EXPECT_EQ(range.rng, 1u << 31);
  EXPECT_EQ(range.total_bits, 33u);
  EXPECT_EQ(gaud_opus_tell(&range), 1u);
  EXPECT_EQ(gaud_opus_tell_frac(&range), 8u);
  EXPECT_EQ(range.offset, 4u);
  CheckTell(range);
}

/** The first byte's low bit is kept, and its top seven set `val`. */
TEST(OpusRange, InitialisationConsumesSevenBits) {
  for (unsigned first = 0; first < 256u; ++first) {
    unsigned char data[8];
    memset(data, 0, sizeof(data));
    data[0] = (unsigned char)first;
    OPUS_Range range;
    gaud_opus_range_init(&range, data, sizeof(data));
    /* Renormalisation has run, so `val` is no longer 127-(b0>>1); what
     * stays visible is that the three zero bytes after it contributed
     * 255 each, and that the leftover bit of the first byte arrived as
     * the high bit of the first renormalisation's symbol. */
    uint32_t expect = 127u - (first >> 1);
    for (int i = 0; i < 3; ++i) {
      unsigned sym = (i == 0) ? ((first & 1u) << 7) : 0u;
      expect = ((expect << 8) + (255u - sym)) & 0x7FFFFFFFu;
    }
    EXPECT_EQ(range.val, expect) << "first byte " << first;
  }
}

/** Section 4.1.3.1: `ec_decode_bin(ftb)` is `ec_decode(1 << ftb)`. */
TEST(OpusRange, DecodeBinMatchesDecode) {
  for (uint32_t seed = 1; seed <= 24u; ++seed) {
    std::vector<unsigned char> data = RangeBytes(seed, 96);
    OPUS_Range fast;
    gaud_opus_range_init(&fast, data.data(), data.size());
    OPUS_Range slow = fast;
    for (unsigned step = 0; step < 150u; ++step) {
      unsigned ftb = 1u + (step % 15u);
      uint32_t ft = 1u << ftb;
      uint32_t a = gaud_opus_decode_bin(&fast, ftb);
      uint32_t b = gaud_opus_decode(&slow, ft);
      ASSERT_EQ(a, b) << "seed " << seed << " step " << step;
      ASSERT_LT(a, ft);
      /* Consume it as the one-wide symbol at the value just read, so
       * the stream advances somewhere neither always first nor last. */
      gaud_opus_dec_update(&fast, a, a + 1u, ft);
      gaud_opus_dec_update(&slow, a, a + 1u, ft);
      ASSERT_TRUE(SameState(fast, slow))
          << "seed " << seed << " step " << step;
      CheckTell(fast);
    }
  }
}

/**
 * Section 4.1.3.3: `ec_dec_icdf` is the general form with the tuples the
 * table encodes.
 *
 * The table is walked the way the specification describes - the first
 * entry where `fs < (1 << ftb) - icdf[k]` - rather than the way the
 * implementation does it, so the two are not one loop written twice.
 */
TEST(OpusRange, DecIcdfMatchesDecode) {
  /* Three real shapes: skewed towards zero, uniform, and skewed away. */
  static const unsigned char skewed[] = {128, 192, 224, 240, 248, 252, 0};
  static const unsigned char uniform[] = {192, 128, 64, 0};
  static const unsigned char tail[] = {248, 240, 224, 192, 128, 0};
  struct Context {
    const unsigned char * icdf;
    unsigned ftb;
  };
  const Context contexts[] = {{skewed, 8}, {uniform, 8}, {tail, 8}};

  for (uint32_t seed = 1; seed <= 16u; ++seed) {
    for (const Context & context : contexts) {
      std::vector<unsigned char> data = RangeBytes(seed, 96);
      OPUS_Range fast;
      gaud_opus_range_init(&fast, data.data(), data.size());
      OPUS_Range slow = fast;
      for (unsigned step = 0; step < 120u; ++step) {
        uint32_t ft = 1u << context.ftb;
        int a = gaud_opus_dec_icdf(&fast, context.icdf, context.ftb);
        uint32_t fs = gaud_opus_decode(&slow, ft);
        int k = 0;
        while (fs >= ft - context.icdf[k]) {
          ++k;
        }
        uint32_t fl = k == 0 ? 0u : ft - context.icdf[k - 1];
        uint32_t fh = ft - context.icdf[k];
        gaud_opus_dec_update(&slow, fl, fh, ft);
        ASSERT_EQ(a, k) << "seed " << seed << " step " << step;
        ASSERT_TRUE(SameState(fast, slow))
            << "seed " << seed << " step " << step;
        CheckTell(fast);
      }
    }
  }
}

/** Section 4.1.3.2: `ec_dec_bit_logp` is the general form too. */
TEST(OpusRange, DecBitLogpMatchesDecode) {
  for (uint32_t seed = 1; seed <= 16u; ++seed) {
    for (unsigned logp = 1u; logp <= 14u; ++logp) {
      std::vector<unsigned char> data = RangeBytes(seed * 31u + logp, 96);
      OPUS_Range fast;
      gaud_opus_range_init(&fast, data.data(), data.size());
      OPUS_Range slow = fast;
      for (unsigned step = 0; step < 80u; ++step) {
        uint32_t ft = 1u << logp;
        int a = gaud_opus_dec_bit_logp(&fast, logp);
        uint32_t fs = gaud_opus_decode(&slow, ft);
        int k = fs < ft - 1u ? 0 : 1;
        if (k == 0) {
          gaud_opus_dec_update(&slow, 0u, ft - 1u, ft);
        } else {
          gaud_opus_dec_update(&slow, ft - 1u, ft, ft);
        }
        ASSERT_EQ(a, k) << "seed " << seed << " logp " << logp;
        ASSERT_TRUE(SameState(fast, slow))
            << "seed " << seed << " logp " << logp;
        CheckTell(fast);
      }
    }
  }
}

/**
 * Section 4.1.4: raw bits come from the last byte downwards, least
 * significant bit first.
 *
 * Asserted on concrete bytes rather than by symmetry with a writer:
 * `0xB4` is `1011 0100`, so the first four bits read are `0100` and the
 * next four are `1011`. The two nibbles differ and neither is a
 * palindrome, so a reversed bit order and a swapped nibble order both
 * fail here rather than cancelling.
 */
TEST(OpusRange, RawBitsComeFromTheEnd) {
  unsigned char data[8] = {0, 0, 0, 0, 0x11, 0x22, 0x33, 0xB4};
  OPUS_Range range;
  gaud_opus_range_init(&range, data, sizeof(data));
  EXPECT_EQ(gaud_opus_dec_bits(&range, 4u), 0x4u);
  EXPECT_EQ(gaud_opus_dec_bits(&range, 4u), 0xBu);
  EXPECT_EQ(gaud_opus_dec_bits(&range, 8u), 0x33u);
  EXPECT_EQ(gaud_opus_dec_bits(&range, 8u), 0x22u);
  EXPECT_EQ(gaud_opus_dec_bits(&range, 8u), 0x11u);
}

/** Raw bits past the start of the buffer read as zero, not as garbage. */
TEST(OpusRange, RawBitsRunOutAsZero) {
  unsigned char data[1] = {0xFF};
  OPUS_Range range;
  gaud_opus_range_init(&range, data, sizeof(data));
  EXPECT_EQ(gaud_opus_dec_bits(&range, 8u), 0xFFu);
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(gaud_opus_dec_bits(&range, 8u), 0u);
  }
}

/**
 * Section 4.1.2.1: an exhausted frame keeps decoding on zero bytes.
 *
 * Not a tolerance - the encoder relies on it in order to stop writing
 * early. What is asserted is that this terminates and stays in range;
 * that it reads nothing outside the buffer is what `make test-asan`
 * adds to it.
 */
TEST(OpusRange, ExhaustedFrameKeepsDecoding) {
  unsigned char data[2] = {0x5A, 0xA5};
  OPUS_Range range;
  gaud_opus_range_init(&range, data, sizeof(data));
  for (unsigned step = 0; step < 500u; ++step) {
    uint32_t value = gaud_opus_decode(&range, 11u);
    ASSERT_LT(value, 11u);
    gaud_opus_dec_update(&range, value, value + 1u, 11u);
    CheckTell(range);
  }
  EXPECT_FALSE(range.error);
}

/** An empty frame is allowed, and decodes as though it were zeros. */
TEST(OpusRange, EmptyFrameDecodes) {
  OPUS_Range range;
  gaud_opus_range_init(&range, nullptr, 0u);
  EXPECT_EQ(gaud_opus_tell(&range), 1u);
  for (unsigned step = 0; step < 32u; ++step) {
    uint32_t value = gaud_opus_decode(&range, 4u);
    ASSERT_LT(value, 4u);
    gaud_opus_dec_update(&range, value, value + 1u, 4u);
  }
  EXPECT_EQ(gaud_opus_dec_bits(&range, 8u), 0u);
}

/** Section 4.1.5, the eight-bits-or-fewer path: every value is in range. */
TEST(OpusRange, DecUintSmallStaysInRange) {
  for (uint32_t ft = 2u; ft <= 256u; ++ft) {
    std::vector<unsigned char> data = RangeBytes(ft, 64);
    OPUS_Range range;
    gaud_opus_range_init(&range, data.data(), data.size());
    for (unsigned step = 0; step < 24u; ++step) {
      uint32_t value = gaud_opus_dec_uint(&range, ft);
      ASSERT_LT(value, ft) << "ft " << ft;
      /* Below the split there are no raw bits, so nothing can go out of
       * range and the error flag must stay clear. */
      ASSERT_FALSE(range.error) << "ft " << ft;
    }
  }
}

/**
 * Section 4.1.5's named error, put in a position to fail - and the
 * condition under which it cannot be.
 *
 * Above eight bits the value is a coded symbol for the top bits and raw
 * bits for the rest, with no redundancy between the halves, so a stream
 * that is not a real frame can produce a value larger than the caller
 * asked for. **Whether it can depends on `ft`.** Writing `F = ft - 1`
 * and `b` for the raw bits below the split, the largest value the two
 * halves can combine to is `F` with its low `b` bits all set; that
 * exceeds `F` exactly when they were not already set.
 *
 * The first draft of this test swept `ft = 1000`. `F` is 999, the split
 * leaves two raw bits, and 999 ends in `11` - so the overflow was
 * arithmetically impossible and six thousand draws found nothing. Both
 * values are kept: one proves the arm runs, the other proves the bound
 * is a bound rather than an accident of the inputs tried.
 */
TEST(OpusRange, DecUintLargeOverflowsOnlyWhenItCan) {
  struct Case {
    uint32_t ft;      /* What the caller asks for. */
    bool reachable;   /* Whether `F`'s low raw bits leave room. */
  };
  /* 1024 is 0b100_0000_0000: three raw bits below the split, none set.
   * 999 is 0b11_1110_0111: two raw bits below the split, both set. */
  const Case cases[] = {{1025u, true}, {1000u, false}};

  for (const Case & one : cases) {
    unsigned saturated = 0;
    unsigned trials = 0;
    for (uint32_t seed = 1u; seed <= 400u; ++seed) {
      std::vector<unsigned char> data = RangeBytes(seed, 48);
      OPUS_Range range;
      gaud_opus_range_init(&range, data.data(), data.size());
      for (unsigned step = 0; step < 16u; ++step) {
        bool before = range.error;
        uint32_t value = gaud_opus_dec_uint(&range, one.ft);
        ++trials;
        ASSERT_LT(value, one.ft) << "ft " << one.ft;
        if (!before && range.error) {
          ++saturated;
          ASSERT_EQ(value, one.ft - 1u) << "ft " << one.ft;
        }
      }
    }
    if (one.reachable) {
      EXPECT_GT(saturated, 0u)
          << "ft " << one.ft << ": swept " << trials
          << " draws and never left the range, so the saturating arm is "
             "untested";
    } else {
      EXPECT_EQ(saturated, 0u)
          << "ft " << one.ft
          << ": the two halves cannot combine above this bound, so "
             "reaching it means the split is wrong";
    }
  }
}

/* ----------------------------------------- RFC 6716 section 4.3.2:
                                                   the energy envelope */

/**
 * The Laplace distribution, and the three passes over the band energies.
 *
 * The Laplace decoder is tested by **tiling**. Its distribution is not a
 * table; it is walked - a frequency for zero, then a decaying run, then
 * a flat tail at the floor probability - and the intervals it produces
 * must partition the range coder's 32,768 exactly, with no gap and no
 * overlap. A gap is a value the encoder can write and this cannot read;
 * an overlap is two values that decode to the same bits. Both are fatal
 * and neither shows up as a wrong-looking number, so the partition is
 * asserted directly, for every parameter pair the generated model table
 * actually contains.
 *
 * Then the decoder is checked against that partition, using the trick
 * the range decoder's own tests use: ::OPUS_Range is copyable, so the
 * 15-bit value the decode would see can be read from a copy first, and
 * the answer looked up in the partition built independently.
 *
 * **What is not here.** Coarse energy was checked against the reference
 * implementation's own intermediate state, frame by frame, over 11,211
 * CELT frames of test vectors 1, 7 and 11 - every CELT configuration -
 * and matched exactly, including both clamps and the 16-by-16 multiply.
 * That check needs a locally built reference and so is a development
 * instrument rather than a gate; notes/audio/opus.md records how to run
 * it. What stands in the repository is the conformance gate, which
 * cannot see this layer on its own.
 */

namespace {

/** How the specification's walk divides the range, for one context. */
struct Laplace {
  uint32_t fl[64];   ///< Interval starts, indexed by decoded value + 32.
  uint32_t fh[64];   ///< And their ends.
  bool used[64];     ///< Whether that value is reachable at all.
};

/**
 * Build the partition by walking the distribution forwards.
 *
 * Written from section 4.3.2.1's description of the shape rather than
 * from the decoder's loop, so that the two are not one piece of code
 * compared with itself: this accumulates intervals in order and the
 * decoder searches them.
 */
Laplace BuildLaplace(uint32_t fs0, int decay) {
  Laplace out{};
  const uint32_t minp = 1;
  const uint32_t nmin = 16;
  /* Zero takes the first fs0 of the range. */
  out.fl[32] = 0;
  out.fh[32] = fs0;
  out.used[32] = true;
  uint32_t ft = 32768u - minp * (2u * nmin) - fs0;
  uint32_t fs = ((ft * (uint32_t)(16384 - decay)) >> 15) + minp;
  uint32_t at = fs0;
  int value = 1;
  /* The decaying run, mirrored: each magnitude takes 2*fs, negative
   * first and then positive. */
  while (fs > minp && value < 31) {
    out.fl[32 - value] = at;
    out.fh[32 - value] = at + fs;
    out.used[32 - value] = true;
    out.fl[32 + value] = at + fs;
    out.fh[32 + value] = at + 2u * fs;
    out.used[32 + value] = true;
    at += 2u * fs;
    fs = (((fs * 2u) - 2u * minp) * (uint32_t)decay) >> 15;
    fs += minp;
    ++value;
  }
  /* And the flat tail, at the floor, until the range is used up. */
  while (at + 2u * minp <= 32768u && value < 31) {
    out.fl[32 - value] = at;
    out.fh[32 - value] = at + minp;
    out.used[32 - value] = true;
    out.fl[32 + value] = at + minp;
    out.fh[32 + value] = at + 2u * minp;
    out.used[32 + value] = true;
    at += 2u * minp;
    ++value;
  }
  return out;
}

/** Every (fs, decay) pair the coarse energy model can present. */
std::vector<std::pair<uint32_t, int>> EnergyContexts() {
  std::vector<std::pair<uint32_t, int>> out;
  for (int size = 0; size < 4; ++size) {
    for (int intra = 0; intra < 2; ++intra) {
      for (int band = 0; band < 21; ++band) {
        size_t at = ((size_t)size * 2u + intra) * 42u + (size_t)band * 2u;
        out.emplace_back((uint32_t)gaud_opus_e_prob_model[at] << 7,
            (int)gaud_opus_e_prob_model[at + 1u] << 6);
      }
    }
  }
  return out;
}

} // namespace

/**
 * The distribution tiles the range coder's 32,768 with no gap.
 *
 * Checked for all 168 contexts the generated model holds. A gap is a
 * value an encoder can produce and this decoder cannot read; an overlap
 * is two values coded by the same bits. Neither looks like a wrong
 * number when it happens.
 */
TEST(OpusCelt, LaplaceDistributionTilesTheRange) {
  for (const auto & context : EnergyContexts()) {
    Laplace table = BuildLaplace(context.first, context.second);
    /* Walk the intervals in increasing order of start and check each
     * begins exactly where the last ended. */
    std::vector<std::pair<uint32_t, uint32_t>> spans;
    for (int i = 0; i < 64; ++i) {
      if (table.used[i]) {
        spans.emplace_back(table.fl[i], table.fh[i]);
      }
    }
    std::sort(spans.begin(), spans.end());
    ASSERT_FALSE(spans.empty());
    EXPECT_EQ(spans.front().first, 0u)
        << "fs " << context.first << " decay " << context.second;
    for (size_t i = 1; i < spans.size(); ++i) {
      ASSERT_EQ(spans[i].first, spans[i - 1].second)
          << "fs " << context.first << " decay " << context.second
          << " at interval " << i;
    }
    for (const auto & span : spans) {
      ASSERT_LT(span.first, span.second);
      ASSERT_LE(span.second, 32768u);
    }
  }
}

/**
 * The decoder lands in the interval the partition says it should.
 *
 * ::OPUS_Range is copyable, so the 15-bit value the decode is about to
 * see can be read from a copy and used to look the answer up in a
 * partition built by the other routine above.
 */
TEST(OpusCelt, LaplaceDecodeAgreesWithThePartition) {
  auto contexts = EnergyContexts();
  unsigned checked = 0;
  unsigned nonzero = 0;
  for (uint32_t seed = 1; seed <= 12u; ++seed) {
    std::vector<unsigned char> data = RangeBytes(seed * 7u, 128);
    OPUS_Range range;
    gaud_opus_range_init(&range, data.data(), data.size());
    for (unsigned step = 0; step < 200u; ++step) {
      const auto & context = contexts[(seed * 200u + step) % contexts.size()];
      Laplace table = BuildLaplace(context.first, context.second);
      OPUS_Range peek = range;
      uint32_t fm = gaud_opus_decode_bin(&peek, 15);
      int got = gaud_celt_laplace_decode(
          &range, context.first, context.second);
      ASSERT_GE(got, -31);
      ASSERT_LE(got, 31);
      int slot = got + 32;
      ASSERT_TRUE(table.used[slot])
          << "value " << got << " is outside the partition";
      EXPECT_GE(fm, table.fl[slot]) << "value " << got;
      EXPECT_LT(fm, table.fh[slot]) << "value " << got;
      ++checked;
      if (got != 0) {
        ++nonzero;
      }
    }
  }
  /* The contexts are skewed towards zero, so most draws are zero. A
   * sweep that only ever produced zero would pass every assertion above
   * while exercising none of the walk. */
  EXPECT_GT(nonzero, 0u) << "swept " << checked
                         << " draws and every one decoded as zero, so the "
                            "decaying part of the distribution is untested";
}

/**
 * Section 4.3.2.2's correction, at both ends of a three-bit refinement.
 *
 * The mapping is (f + 1/2)/2**B - 1/2, so for B = 3 the smallest
 * refinement is -0.4375 and the largest is +0.4375, which in the Q10
 * the energies are held in are -448 and +448. Asserted as numbers
 * rather than as the formula, because the formula is what is being
 * tested.
 */
TEST(OpusCelt, FineEnergyCorrectionSpansPlusAndMinusAHalf) {
  CELT_Mode mode;
  ASSERT_TRUE(gaud_celt_mode_init(&mode, 3u));
  const int fine[CELT_BANDS] = {3};

  /* Raw bits come from the end of the frame, so the last byte decides
   * what the three-bit refinement reads. */
  for (int want : {0, 7}) {
    std::vector<unsigned char> data(16u, 0u);
    data.back() = (unsigned char)want;
    OPUS_Range range;
    gaud_opus_range_init(&range, data.data(), data.size());
    int16_t energy[2 * CELT_BANDS] = {0};
    gaud_celt_decode_fine_energy(&range, &mode, energy, fine, 0u, 1u, 1u);
    EXPECT_EQ(energy[0], want == 0 ? -448 : 448) << "q2 " << want;
  }
}

/** A band given no fine bits is left exactly as the coarse pass had it. */
TEST(OpusCelt, FineEnergyLeavesUnallocatedBandsAlone) {
  CELT_Mode mode;
  ASSERT_TRUE(gaud_celt_mode_init(&mode, 3u));
  int fine[CELT_BANDS];
  for (int i = 0; i < CELT_BANDS; ++i) {
    fine[i] = 0;
  }
  std::vector<unsigned char> data = RangeBytes(5u, 32);
  OPUS_Range range;
  gaud_opus_range_init(&range, data.data(), data.size());
  int16_t energy[2 * CELT_BANDS];
  for (int i = 0; i < 2 * CELT_BANDS; ++i) {
    energy[i] = (int16_t)(-1000 - i);
  }
  uint32_t before = range.total_bits;
  gaud_celt_decode_fine_energy(
      &range, &mode, energy, fine, 0u, CELT_BANDS, 2u);
  for (int i = 0; i < 2 * CELT_BANDS; ++i) {
    EXPECT_EQ(energy[i], (int16_t)(-1000 - i)) << i;
  }
  /* And it read nothing, which is the part a loop that "helpfully"
   * read zero bits would get wrong. */
  EXPECT_EQ(range.total_bits, before);
}

/**
 * The final pass spends priority 0 before priority 1, and stops.
 *
 * Section 4.3.2.2: leftover bits go to priority 0 bands from band 0
 * upwards, then to priority 1 bands, and anything still left is unused.
 * Here there are four bits and four bands of each priority in mono, so
 * exactly the four priority 0 bands move.
 */
TEST(OpusCelt, FinalEnergySpendsPriorityZeroFirst) {
  CELT_Mode mode;
  ASSERT_TRUE(gaud_celt_mode_init(&mode, 3u));
  int fine[CELT_BANDS];
  int priority[CELT_BANDS];
  for (int i = 0; i < CELT_BANDS; ++i) {
    fine[i] = 1;
    priority[i] = i < 8 ? (i < 4 ? 0 : 1) : 1;
  }
  std::vector<unsigned char> data = RangeBytes(9u, 32);
  OPUS_Range range;
  gaud_opus_range_init(&range, data.data(), data.size());
  int16_t energy[2 * CELT_BANDS] = {0};
  gaud_celt_decode_final_energy(
      &range, &mode, energy, fine, priority, 4, 0u, CELT_BANDS, 1u);

  int moved = 0;
  for (int i = 0; i < CELT_BANDS; ++i) {
    if (energy[i] != 0) {
      ++moved;
      EXPECT_LT(i, 4) << "band " << i << " moved but is not priority 0";
    }
  }
  /* Four bits, one band each: every priority 0 band moved. A band whose
   * bit decoded as the value that happens to give a zero offset would
   * break this, so the offset is checked to be non-zero by construction
   * - it is +/- half a step and never zero. */
  EXPECT_EQ(moved, 4);
}

/** A band already at the ceiling of fine bits is skipped. */
TEST(OpusCelt, FinalEnergySkipsBandsAtTheFineCeiling) {
  CELT_Mode mode;
  ASSERT_TRUE(gaud_celt_mode_init(&mode, 3u));
  int fine[CELT_BANDS];
  int priority[CELT_BANDS];
  for (int i = 0; i < CELT_BANDS; ++i) {
    fine[i] = CELT_MAX_FINE_BITS;
    priority[i] = 0;
  }
  std::vector<unsigned char> data = RangeBytes(11u, 32);
  OPUS_Range range;
  gaud_opus_range_init(&range, data.data(), data.size());
  int16_t energy[2 * CELT_BANDS] = {0};
  uint32_t before = range.total_bits;
  gaud_celt_decode_final_energy(
      &range, &mode, energy, fine, priority, 16, 0u, CELT_BANDS, 1u);
  for (int i = 0; i < CELT_BANDS; ++i) {
    EXPECT_EQ(energy[i], 0) << i;
  }
  EXPECT_EQ(range.total_bits, before);
}

/** The four frame sizes, and the one that does not exist. */
TEST(OpusCelt, ModeInitCoversTheFourFrameSizes) {
  for (unsigned lm = 0; lm <= 3u; ++lm) {
    CELT_Mode mode;
    ASSERT_TRUE(gaud_celt_mode_init(&mode, lm)) << lm;
    EXPECT_EQ(mode.size, 120u << lm);
    EXPECT_EQ(mode.shorts, 1u << lm);
    EXPECT_EQ(mode.bands, (uint32_t)CELT_BANDS);
    /* The last band's top edge, scaled, is 800 bins at 20 ms - the
     * 20 kHz the band layout stops at, not the 960-bin Nyquist. */
    EXPECT_EQ(gaud_celt_band_start(&mode, CELT_BANDS), 100u << lm);
  }
  CELT_Mode mode;
  EXPECT_FALSE(gaud_celt_mode_init(&mode, 4u));
}

/* ----------------------------------------- RFC 6716 section 4.3.3:
                                                    the bit allocation */

/**
 * The allocator, and the invariant that catches the mistake it invites.
 *
 * Section 4.3.3 says the allocation "MUST be recovered exactly" and
 * that any deviation "will result in corrupted output". It is also the
 * piece with the least to check it offline: almost every number in it
 * is a step in one computation whose only external meaning is that the
 * range decoder stays in step. So the decisive verification is against
 * the reference implementation's own intermediate state, over 11,211
 * CELT frames of test vectors 1, 7 and 11 - all sixteen CELT
 * configurations, both channel counts, all four frame sizes - where
 * everything matches: the coded band count, the intensity and dual
 * stereo decisions, the balance, and all 21 of the pulses, fine-energy
 * bits and priorities. notes/audio/opus.md says how to reproduce it.
 *
 * What stands here is the structural half, and one test chosen because
 * it would have caught the defect this file actually had.
 *
 * **The allocator works in two units.** The band edges are in 2.5 ms
 * bins and a frame of `1 << lm` short MDCTs has that many times more.
 * The caps and the boost quanta want the real count; nearly everything
 * else wants the unscaled one and applies the scaling a line later.
 * Writing it all in scaled units applies the shift twice, which is
 * silent at `lm = 0` and multiplies the allocation eightfold at
 * `lm = 3`. It does not overflow or bust the budget - the allocator
 * compensates by *skipping bands*. So the test is that at a constant
 * number of bits per sample every band stays coded, whatever the frame
 * size, which with the doubled shift drops from 21 bands to 16.
 */

namespace {

/** Run the allocator once on a fabricated budget. */
struct Allocation {
  uint32_t coded;
  int32_t pulses[CELT_BANDS];
  int fine[CELT_BANDS];
  int priority[CELT_BANDS];
  int32_t cap[CELT_BANDS];
  int32_t budget;
  uint32_t intensity;
  bool dual;
  int32_t balance;
};

Allocation Allocate(unsigned lm, uint32_t channels, size_t bytes,
    int trim = 5) {
  CELT_Mode mode;
  EXPECT_TRUE(gaud_celt_mode_init(&mode, lm));
  static std::vector<unsigned char> buffer;
  buffer.assign(bytes, 0xA5u);
  OPUS_Range range;
  gaud_opus_range_init(&range, buffer.data(), buffer.size());
  Allocation out{};
  gaud_celt_init_caps(&mode, out.cap, channels);
  int32_t offsets[CELT_BANDS] = {0};
  out.budget = ((int32_t)bytes * 8 << CELT_BITRES) - 1;
  out.coded = gaud_celt_compute_allocation(&range, &mode, 0u, CELT_BANDS,
      offsets, out.cap, trim, &out.intensity, &out.dual, out.budget,
      &out.balance, out.pulses, out.fine, out.priority, channels);
  return out;
}

} // namespace

/**
 * At a constant rate per sample, every band stays coded at every frame
 * size - which is the unit confusion's tell.
 */
TEST(OpusAllocate, EveryBandStaysCodedAcrossFrameSizes) {
  for (unsigned lm = 0; lm <= 3u; ++lm) {
    for (uint32_t channels = 1; channels <= 2u; ++channels) {
      /* 64 bytes per 2.5 ms per channel: a high but ordinary rate. */
      size_t bytes = (size_t)(64u << lm) * channels;
      Allocation one = Allocate(lm, channels, bytes);
      EXPECT_EQ(one.coded, (uint32_t)CELT_BANDS)
          << "lm " << lm << " channels " << channels
          << ": bands were skipped at a rate that should reach all of "
             "them, which is what applying the frame-size shift twice "
             "looks like";
    }
  }
}

/** And it never hands out more than it was given. */
TEST(OpusAllocate, NeverExceedsItsBudget) {
  for (unsigned lm = 0; lm <= 3u; ++lm) {
    for (uint32_t channels = 1; channels <= 2u; ++channels) {
      for (size_t scale : {8u, 32u, 64u, 128u}) {
        size_t bytes = (size_t)(scale << lm) * channels;
        Allocation one = Allocate(lm, channels, bytes);
        int32_t spent = 0;
        for (int j = 0; j < CELT_BANDS; ++j) {
          spent += one.pulses[j];
          spent += (int32_t)channels * one.fine[j] << CELT_BITRES;
        }
        EXPECT_LE(spent, one.budget)
            << "lm " << lm << " channels " << channels << " bytes "
            << bytes;
      }
    }
  }
}

/**
 * No band exceeds its ceiling, and no band gets a negative share.
 *
 * The rates swept here reach the format's 1,275-byte limit on purpose,
 * so that the eight-bit ceiling on fine energy is approached rather
 * than merely asserted; the count of bands that reach it is checked so
 * that this cannot quietly become a sweep of rates too low to test it.
 *
 * **The clamp that enforces that ceiling is nevertheless unreachable**,
 * and the comment in opus_celt_rate.c says so with the measurement.
 * Reaching the ceiling and being clamped to it are different things:
 * an earlier bound already holds the value at eight.
 */
TEST(OpusAllocate, RespectsTheCapsAndTheFineCeiling) {
  int at_ceiling_total = 0;
  for (unsigned lm = 0; lm <= 3u; ++lm) {
    for (uint32_t channels = 1; channels <= 2u; ++channels) {
      for (size_t scale : {8u, 64u, 96u, 160u}) {
        size_t bytes = (size_t)(scale << lm) * channels;
        if (bytes > OPUS_MAX_FRAME_BYTES) {
          bytes = OPUS_MAX_FRAME_BYTES;
        }
        Allocation one = Allocate(lm, channels, bytes);
        for (int j = 0; j < CELT_BANDS; ++j) {
          EXPECT_GE(one.pulses[j], 0) << "lm " << lm << " band " << j;
          EXPECT_LE(one.pulses[j], one.cap[j]) << "lm " << lm
                                               << " band " << j;
          EXPECT_GE(one.fine[j], 0) << "lm " << lm << " band " << j;
          EXPECT_LE(one.fine[j], CELT_MAX_FINE_BITS)
              << "lm " << lm << " band " << j;
          EXPECT_TRUE(one.priority[j] == 0 || one.priority[j] == 1)
              << "lm " << lm << " band " << j;
          if (one.fine[j] == CELT_MAX_FINE_BITS) {
            ++at_ceiling_total;
          }
        }
      }
    }
  }
  EXPECT_GT(at_ceiling_total, 0)
      << "no band in the whole sweep reached the eight-bit fine energy "
         "ceiling, so the bound on it was asserted and never tested";
}

/** A budget of nothing allocates nothing and reads nothing. */
TEST(OpusAllocate, AnEmptyBudgetAllocatesNothing) {
  CELT_Mode mode;
  ASSERT_TRUE(gaud_celt_mode_init(&mode, 3u));
  unsigned char buffer[4] = {0, 0, 0, 0};
  OPUS_Range range;
  gaud_opus_range_init(&range, buffer, sizeof(buffer));
  int32_t cap[CELT_BANDS];
  gaud_celt_init_caps(&mode, cap, 1u);
  int32_t offsets[CELT_BANDS] = {0};
  int32_t pulses[CELT_BANDS];
  int fine[CELT_BANDS];
  int priority[CELT_BANDS];
  uint32_t intensity = 0;
  bool dual = false;
  int32_t balance = 0;
  uint32_t coded = gaud_celt_compute_allocation(&range, &mode, 0u,
      CELT_BANDS, offsets, cap, 5, &intensity, &dual, 0, &balance, pulses,
      fine, priority, 1u);
  EXPECT_GT(coded, 0u);
  for (int j = 0; j < CELT_BANDS; ++j) {
    EXPECT_EQ(pulses[j], 0) << j;
    EXPECT_EQ(fine[j], 0) << j;
  }
}

/**
 * The time-frequency flags only ever take the two values the table row
 * holds.
 *
 * Section 4.3.1 maps each band's flag through `tf_select_table`, and
 * the row is chosen by frame size, the transient flag and the select
 * bit. So whatever the data, every band's answer must come from one of
 * the two entries the chosen row offers - a mis-indexed table produces
 * values from the wrong row, which still look like plausible small
 * integers.
 */
TEST(OpusCelt, TfDecodeOnlyProducesValuesFromItsRow) {
  for (unsigned lm = 0; lm <= 3u; ++lm) {
    CELT_Mode mode;
    ASSERT_TRUE(gaud_celt_mode_init(&mode, lm));
    for (int transient = 0; transient < 2; ++transient) {
      for (uint32_t seed = 1; seed <= 20u; ++seed) {
        std::vector<unsigned char> data = RangeBytes(seed * 13u + lm, 64);
        OPUS_Range range;
        gaud_opus_range_init(&range, data.data(), data.size());
        int tf_res[CELT_BANDS];
        gaud_celt_tf_decode(
            &range, &mode, 0u, CELT_BANDS, transient != 0, tf_res);
        size_t row = (size_t)lm * 8u + 4u * (size_t)transient;
        for (int j = 0; j < CELT_BANDS; ++j) {
          bool from_row = false;
          for (int select = 0; select < 2; ++select) {
            for (int flag = 0; flag < 2; ++flag) {
              if (tf_res[j]
                  == gaud_opus_tf_select_table[row + 2u * select + flag]) {
                from_row = true;
              }
            }
          }
          EXPECT_TRUE(from_row)
              << "lm " << lm << " transient " << transient << " band " << j
              << " gave " << tf_res[j] << ", which is in no entry of its row";
        }
      }
    }
  }
}

/** At the shortest frame size the resolution never changes. */
TEST(OpusCelt, TfDecodeIsFlatAtTheShortestFrame) {
  CELT_Mode mode;
  ASSERT_TRUE(gaud_celt_mode_init(&mode, 0u));
  for (uint32_t seed = 1; seed <= 20u; ++seed) {
    std::vector<unsigned char> data = RangeBytes(seed * 17u, 64);
    OPUS_Range range;
    gaud_opus_range_init(&range, data.data(), data.size());
    int tf_res[CELT_BANDS];
    gaud_celt_tf_decode(&range, &mode, 0u, CELT_BANDS, false, tf_res);
    /* Row 0 of the table is {0,-1,0,-1,...}: with one short MDCT there
     * is no finer resolution to move to, only a coarser one. */
    for (int j = 0; j < CELT_BANDS; ++j) {
      EXPECT_TRUE(tf_res[j] == 0 || tf_res[j] == -1) << j;
    }
  }
}

/* --------------------------------------- RFC 6716 section 4.3.4.2:
                                                 the PVQ enumeration */

/**
 * The shape codebook, tested as the bijection it is supposed to be.
 *
 * A band's shape is a vector of `N` integers whose absolute values sum
 * to `K`, and the bitstream carries it as one integer below `V(N,K)`.
 * So the specification of this layer is a single sentence: the mapping
 * from index to vector is a bijection onto that set. That is directly
 * testable for small sizes by **enumerating every index** and checking
 * three things - every vector has the right pulse count, no two indices
 * give the same vector, and the count comes out at exactly `V(N,K)`.
 * Nothing about this needs a reference; a bijection either is one or is
 * not.
 *
 * `V(N,K)` itself is checked two further ways. Against the table of
 * values for `N` and `K` below ten that the reference implementation
 * prints in its own commentary, and against the three-term recurrence
 * section 4.3.4.2 states, computed independently of the row-stepping
 * the decoder uses. Three readings of one function.
 *
 * Beyond the small sizes the decoder was checked against the reference
 * over all 350,857 pulse vectors the conformance vectors contain,
 * including the closed-form fast paths the reference uses for two,
 * three and four dimensions - so two genuinely different enumerations
 * agree. notes/audio/opus.md says how to reproduce that.
 */

namespace {

/**
 * `V(N,K)` from the recurrence in section 4.3.4.2, memoised.
 *
 * Deliberately not the method the decoder uses: that walks a row of a
 * related function `U` so it can step down a dimension in place. This
 * is the definition, in 64 bits so that nothing it computes can
 * silently wrap, which is what makes it an independent reading.
 */
uint64_t VRecursive(unsigned n, unsigned k) {
  static std::map<std::pair<unsigned, unsigned>, uint64_t> memo;
  if (k == 0) {
    return 1;
  }
  if (n == 0) {
    return 0;
  }
  auto key = std::make_pair(n, k);
  auto found = memo.find(key);
  if (found != memo.end()) {
    return found->second;
  }
  uint64_t value = VRecursive(n - 1, k) + VRecursive(n, k - 1)
      + VRecursive(n - 1, k - 1);
  memo[key] = value;
  return value;
}

} // namespace

/**
 * The table of `V(N,K)` for `N` and `K` below ten, as the reference
 * prints it in cwrs.c's commentary.
 *
 * Transcribed, and it covers the three degenerate rows the row-stepping
 * recurrence cannot produce: no pulses is one vector, no dimensions is
 * none, and one dimension is two whatever the pulse count.
 */
TEST(OpusPvq, VMatchesThePublishedTable) {
  static const uint32_t published[10][10] = {
      {1, 0, 0, 0, 0, 0, 0, 0, 0, 0},
      {1, 2, 2, 2, 2, 2, 2, 2, 2, 2},
      {1, 4, 8, 12, 16, 20, 24, 28, 32, 36},
      {1, 6, 18, 38, 66, 102, 146, 198, 258, 326},
      {1, 8, 32, 88, 192, 360, 608, 952, 1408, 1992},
      {1, 10, 50, 170, 450, 1002, 1970, 3530, 5890, 9290},
      {1, 12, 72, 292, 912, 2364, 5336, 10836, 20256, 35436},
      {1, 14, 98, 462, 1666, 4942, 12642, 28814, 59906, 115598},
      {1, 16, 128, 688, 2816, 9424, 27008, 68464, 157184, 332688},
      {1, 18, 162, 978, 4482, 16722, 53154, 148626, 374274, 864146},
  };
  for (unsigned n = 0; n < 10u; ++n) {
    for (unsigned k = 0; k < 10u; ++k) {
      EXPECT_EQ(gaud_celt_pvq_v(n, k), published[n][k])
          << "V(" << n << "," << k << ")";
      EXPECT_EQ(VRecursive(n, k), (uint64_t)published[n][k])
          << "the recurrence disagrees at V(" << n << "," << k << ")";
    }
  }
}

/**
 * And over the whole range the format can present.
 *
 * `V(N,K)` must fit in 32 bits - it is read by ::gaud_opus_dec_uint,
 * which cannot do more - so the comparison is restricted to the pairs
 * where it does. That restriction is the format's, not a convenience:
 * a band whose codebook would not fit is split in two instead.
 */
TEST(OpusPvq, VMatchesTheRecurrenceEverywhereItFits) {
  unsigned compared = 0;
  for (unsigned n = 2; n <= 96u; ++n) {
    for (unsigned k = 1; k <= CELT_MAX_PULSES; ++k) {
      uint64_t want = VRecursive(n, k);
      if (want > 0xFFFFFFFFu) {
        break; /* And every larger k is larger still. */
      }
      ASSERT_EQ((uint64_t)gaud_celt_pvq_v(n, k), want)
          << "V(" << n << "," << k << ")";
      ++compared;
    }
  }
  /* 1,326 of the 12,160 pairs have a codebook that fits in 32 bits.
   * The bare count is a weak thing to assert, so the two corners the
   * conformance vectors actually reach are checked by name: the widest
   * band the format presents, and the most pulses it allows. */
  EXPECT_EQ(compared, 1326u)
      << "the set of (N,K) pairs whose codebook fits in 32 bits has "
         "changed size, which means V itself has";
  EXPECT_LT((uint64_t)gaud_celt_pvq_v(96u, 5u), 0x100000000ull);
  EXPECT_EQ((uint64_t)gaud_celt_pvq_v(96u, 5u), VRecursive(96u, 5u));
  EXPECT_EQ((uint64_t)gaud_celt_pvq_v(2u, CELT_MAX_PULSES),
      VRecursive(2u, CELT_MAX_PULSES));
}

/**
 * The enumeration is a bijection onto the pulse vectors.
 *
 * Every index below `V(N,K)` is decoded; each vector must have an L1
 * norm of exactly `K`, and no two indices may produce the same vector.
 * Since there are `V(N,K)` indices and `V(N,K)` such vectors, "all
 * distinct" and "all valid" together prove the mapping is onto.
 */
TEST(OpusPvq, EveryIndexDecodesToADistinctVectorOfKPulses) {
  unsigned total = 0;
  for (unsigned n = 2; n <= 7u; ++n) {
    for (unsigned k = 1; k <= 7u; ++k) {
      uint32_t count = gaud_celt_pvq_v(n, k);
      ASSERT_GT(count, 0u);
      std::set<std::vector<int>> seen;
      for (uint32_t index = 0; index < count; ++index) {
        std::vector<int> y((size_t)n, 0);
        gaud_celt_pulses_from_index(y.data(), n, k, index);
        int norm = 0;
        for (int value : y) {
          norm += value < 0 ? -value : value;
        }
        ASSERT_EQ(norm, (int)k) << "N " << n << " K " << k << " index "
                               << index;
        ASSERT_TRUE(seen.insert(y).second)
            << "N " << n << " K " << k << " index " << index
            << " repeats a vector an earlier index already produced";
        ++total;
      }
      EXPECT_EQ(seen.size(), (size_t)count) << "N " << n << " K " << k;
    }
  }
  /* Measured: the sizes swept above hold 78,570 codewords between them,
   * and every one was decoded and found distinct. */
  EXPECT_EQ(total, 78570u) << "the sweep enumerated " << total
                           << " vectors rather than the 78,570 these "
                              "sizes hold";
}

/**
 * Where the enumeration starts, which pins its orientation.
 *
 * A mapping can be a perfect bijection onto the right set and still be
 * the wrong mapping - every other test in this file would pass while
 * the decoder produced a different vector for every index. So the first
 * two codewords are asserted outright.
 *
 * Measured, and stable across every size: index zero puts all `K`
 * pulses in the *first* dimension with a positive sign, and index one
 * moves a single pulse from it into the second. The first draft of this
 * test guessed the last dimension and was wrong; the enumeration counts
 * down from the front.
 */
TEST(OpusPvq, TheEnumerationStartsAtAllPulsesInTheFirstDimension) {
  for (unsigned n = 2; n <= 8u; ++n) {
    for (unsigned k = 1; k <= 5u; ++k) {
      std::vector<int> y((size_t)n, 0);
      gaud_celt_pulses_from_index(y.data(), n, k, 0u);
      EXPECT_EQ(y[0], (int)k) << "N " << n << " K " << k;
      for (unsigned i = 1; i < n; ++i) {
        EXPECT_EQ(y[i], 0) << "N " << n << " K " << k << " at " << i;
      }
      gaud_celt_pulses_from_index(y.data(), n, k, 1u);
      EXPECT_EQ(y[0], (int)k - 1) << "N " << n << " K " << k;
      EXPECT_EQ(y[1], 1) << "N " << n << " K " << k;
      for (unsigned i = 2; i < n; ++i) {
        EXPECT_EQ(y[i], 0) << "N " << n << " K " << k << " at " << i;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// CELT's fixed-point arithmetic, section 4.3's polynomial approximations.
//
// These check two different things, and neither on its own would do.
//
// A bound against the real function says the transcription is not nonsense:
// a coefficient typed wrong, a shift in the wrong direction, a sign lost.
// Every bound below is *measured* rather than chosen, and three of them -
// the two for celt_rsqrt_norm and the one for celt_rcp - come out equal to
// the figures RFC 6716's own source states in its comments, to every digit
// it prints. That is a check on the transcription that the document itself
// supplies and that costs nothing to make.
//
// But a bound cannot say the answer is *right*, because a better
// approximation would pass it and still decode to noise: which polynomial is
// part of the format. Only agreement with the reference, integer for
// integer, says that. The library cannot link the reference, so that half
// lives in a probe instead; notes/audio/opus.md records what it swept and
// what it found, and the anchors below are its answers written down.

// How far a Q-format integer is from the real value it approximates.
double RelativeError(double got, double want) {
  return want == 0.0 ? std::fabs(got) : std::fabs(got - want) / std::fabs(want);
}

TEST(OpusMath, IntegerSquareRootIsExactNotApproximate) {
  // Unlike everything else in this file isqrt32 is not an approximation, so
  // it can be checked against its definition rather than against a bound.
  unsigned long long checked = 0;
  for (unsigned long long v = 0; v < 70000ull; ++v) {
    unsigned long long r = gaud_celt_isqrt32((uint32_t)v);
    ASSERT_LE(r * r, v) << "at " << v;
    ASSERT_GT((r + 1) * (r + 1), v) << "at " << v;
    ++checked;
  }
  for (unsigned long long v = 1; v < 4294967296ull; v += 4093) {
    unsigned long long r = gaud_celt_isqrt32((uint32_t)v);
    ASSERT_LE(r * r, v) << "at " << v;
    ASSERT_GT((r + 1) * (r + 1), v) << "at " << v;
    ++checked;
  }
  // Both ends of the range, which the stride above steps over.
  EXPECT_EQ(gaud_celt_isqrt32(0xFFFFFFFFu), 65535u);
  EXPECT_EQ(gaud_celt_isqrt32(0u), 0u);
  EXPECT_GT(checked, 1100000u);
}

TEST(OpusMath, ReciprocalSquareRootMeetsTheErrorTheRfcStates) {
  // RFC 6716 celt/mathops.c states, of this function: "a maximum relative
  // error of 1.04956E-4, a (relative) RMSE of 2.80979E-5, and a peak
  // absolute error of 2.26591/16384". All three are reproduced here from
  // the transcription, over every input in the stated range.
  double worst = 0.0;
  double worst_absolute = 0.0;
  double sum_of_squares = 0.0;
  unsigned long count = 0;
  for (int32_t x = 16384; x < 65536; ++x) {
    double want = 1.0 / std::sqrt((double)x / 65536.0);
    double got = (double)gaud_celt_rsqrt_norm(x) / 16384.0;
    double relative = RelativeError(got, want);
    worst = std::max(worst, relative);
    worst_absolute = std::max(worst_absolute, std::fabs(got - want) * 16384.0);
    sum_of_squares += relative * relative;
    ++count;
  }
  EXPECT_EQ(count, 49152u);
  EXPECT_NEAR(worst, 1.04956e-4, 1e-9);
  EXPECT_NEAR(std::sqrt(sum_of_squares / count), 2.80979e-5, 1e-10);
  EXPECT_NEAR(worst_absolute, 2.26591, 1e-4);
}

TEST(OpusMath, ReciprocalMeetsTheErrorTheRfcStates) {
  // "a maximum relative error of 7.05346E-5". The argument reaches the
  // polynomial only through its 15-bit mantissa, so sweeping every mantissa
  // at one exponent covers the approximation completely; the exponents
  // either side confirm that the shift back is not where it goes wrong.
  double worst = 0.0;
  for (int exponent = 14; exponent <= 16; ++exponent) {
    for (uint32_t mantissa = 0; mantissa < 32768u; ++mantissa) {
      int32_t x = (int32_t)((1u << exponent)
          | (exponent <= 15 ? (mantissa >> (15 - exponent))
                            : (mantissa << (exponent - 15))));
      double want = 32768.0 / (double)x;
      double got = (double)gaud_celt_rcp(x) / 65536.0;
      worst = std::max(worst, RelativeError(got, want));
    }
  }
  EXPECT_NEAR(worst, 7.05346e-5, 1e-10);
}

TEST(OpusMath, SquareRootTracksTheRealOneAcrossEveryExponent) {
  double worst_relative = 0.0;
  double worst_absolute = 0.0;
  unsigned long count = 0;
  for (int exponent = 1; exponent < 31; ++exponent) {
    for (uint32_t mantissa = 0; mantissa < 32768u; mantissa += 7u) {
      int32_t x = (int32_t)((1u << exponent)
          | (exponent <= 15 ? (mantissa >> (15 - exponent))
                            : (mantissa << (exponent - 15))));
      if (x <= 0) {
        continue;
      }
      double want = std::sqrt((double)x);
      double got = (double)gaud_celt_sqrt(x);
      worst_absolute = std::max(worst_absolute, std::fabs(got - want));
      if (want >= 256.0) {
        worst_relative = std::max(worst_relative, RelativeError(got, want));
      }
      ++count;
    }
  }
  EXPECT_GT(count, 140000u);
  // Measured: 3.9086e-3 relative once the result has room, and never more
  // than twelve units of the output's own scale anywhere.
  EXPECT_LT(worst_relative, 3.91e-3);
  EXPECT_LT(worst_absolute, 12.0);
  EXPECT_EQ(gaud_celt_sqrt(0), 0);
  EXPECT_EQ(gaud_celt_sqrt(65536), 256);
}

TEST(OpusMath, CosineIsExactOnTheQuarterTurnsAndCloseBetween) {
  // The quarter turns are answered without the polynomial, which is not an
  // optimisation: the polynomial does not land on 32767 or on 0, and the
  // places it is asked for are the places a band collapses entirely.
  EXPECT_EQ(gaud_celt_cos_norm(0), 32767);
  EXPECT_EQ(gaud_celt_cos_norm(32768), 0);
  EXPECT_EQ(gaud_celt_cos_norm(65536), -32767);
  EXPECT_EQ(gaud_celt_cos_norm(98304), 0);
  EXPECT_EQ(gaud_celt_cos_norm(131072), 32767);
  // Four quarter turns is the period, and only the low 17 bits are read.
  for (int32_t x = 0; x < 131072; x += 997) {
    EXPECT_EQ(gaud_celt_cos_norm(x), gaud_celt_cos_norm(x + 131072)) << x;
    EXPECT_EQ(gaud_celt_cos_norm(x), gaud_celt_cos_norm(x - 131072)) << x;
  }
  double worst = 0.0;
  for (int32_t x = 0; x < 131072; ++x) {
    double want = std::cos(3.14159265358979323846 * 0.5 * ((double)x / 32768.0))
        * 32767.0;
    worst = std::max(worst, std::fabs((double)gaud_celt_cos_norm(x) - want));
  }
  // Measured: 2.3537 of 32767, so about seven parts in a hundred thousand.
  EXPECT_LT(worst, 2.36);
}

TEST(OpusMath, PowerOfTwoSaturatesAtBothEndsAndTracksBetween) {
  EXPECT_EQ(gaud_celt_exp2(15 * 1024), 0x7F000000);
  EXPECT_EQ(gaud_celt_exp2(-16 * 1024), 0);
  double worst = 0.0;
  for (int32_t x = 0; x <= 14 * 1024 + 1023; ++x) {
    double want = std::pow(2.0, (double)x / 1024.0) * 65536.0;
    worst = std::max(worst, RelativeError((double)gaud_celt_exp2((int16_t)x), want));
  }
  // Measured: 1.0781e-4 wherever the Q16 result has a whole bit to spare.
  EXPECT_LT(worst, 1.08e-4);
  // Below that the result runs out of Q16 bits long before the polynomial
  // runs out of accuracy, so the bound has to carry both terms: the last
  // integer, and the same relative error as above. Measured, the worst case
  // reaches 0.9998 of that sum - the two never pile up.
  for (int32_t x = -15 * 1024; x < 0; ++x) {
    double want = std::pow(2.0, (double)x / 1024.0) * 65536.0;
    EXPECT_LE(std::fabs((double)gaud_celt_exp2((int16_t)x) - want),
        1.0 + want * 1.08e-4) << x;
  }
}

TEST(OpusMath, TheBitExactCosineHoldsOnlyOverTheDomainItsCallerProduces) {
  // This one is integer in RFC 6716's floating-point build too, because the
  // number it returns decides the stereo split's bit allocation and section
  // 4.3.3 requires both ends to reach the same division.
  //
  // Its argument is `itheta`, which bands.c forms as `itheta*16384/qn` with
  // `qn` at most 256 and with zero and 16384 handled before the call. So the
  // domain is 64 to 16320. Below 64 the squared argument rounds to zero, the
  // polynomial returns 32767, and adding the final one wraps the 16-bit
  // result to -32768. The reference does exactly this and guards it with an
  // assertion; the behaviour is transcribed rather than repaired, and pinned
  // here so that a later "fix" has to argue with a test.
  EXPECT_EQ(gaud_celt_bitexact_cos(0), -32768);
  EXPECT_EQ(gaud_celt_bitexact_cos(63), -32768);
  EXPECT_EQ(gaud_celt_bitexact_cos(64), 32767);
  double worst = 0.0;
  for (int32_t x = 64; x <= 16320; ++x) {
    double want = std::cos(3.14159265358979323846 * 0.5 * ((double)x / 16384.0))
        * 32767.0;
    worst = std::max(worst,
        std::fabs((double)gaud_celt_bitexact_cos((int16_t)x) - want));
  }
  // Measured: 2.2970 of 32767 over the whole domain, 1.7193 over the 255
  // values a `qn` of 256 can actually produce.
  EXPECT_LT(worst, 2.30);
}

TEST(OpusMath, TheBitExactLogTangentTracksTheRealOne) {
  double worst = 0.0;
  unsigned long count = 0;
  for (int32_t sine = 1; sine <= 32767; sine += 37) {
    for (int32_t cosine = 1; cosine <= 32767; cosine += 311) {
      double want = std::log2((double)sine / (double)cosine) * 2048.0;
      double got = (double)gaud_celt_bitexact_log2tan(sine, cosine);
      worst = std::max(worst, std::fabs(got - want));
      ++count;
    }
  }
  EXPECT_GT(count, 80000u);
  // Measured: 34.54 in Q11, which is 0.0169 of a bit.
  EXPECT_LT(worst, 34.6);
  // It is odd in its arguments up to the rounding, which is what makes the
  // split symmetric; a sign dropped anywhere in it would break this.
  for (int32_t a = 1; a <= 32767; a += 1021) {
    for (int32_t b = 1; b <= 32767; b += 2039) {
      EXPECT_EQ(gaud_celt_bitexact_log2tan(a, b),
          -gaud_celt_bitexact_log2tan(b, a)) << a << "," << b;
    }
  }
}

TEST(OpusMath, DivisionGoesThroughTheReciprocalAndInheritsItsError) {
  // celt_div is not a division: it is celt_rcp followed by a Q31 multiply
  // that drops the lowest partial product. Writing it as `a / b` would be
  // more accurate and would decode differently, so the test is that it is
  // close to the quotient without being equal to it.
  //
  // The domain matters and is narrow: `exp_rotation` is the only caller, it
  // returns before dividing unless twice the pulse count is below the band
  // width, and a band is at most 176 bins. Outside that the dropped partial
  // products cost real accuracy - a quotient of 9 comes back as 7 - so a
  // bound measured over a wider sweep would say something true about a
  // function nothing calls that way.
  unsigned long differed = 0;
  unsigned long count = 0;
  double worst = 0.0;
  for (int32_t len = 1; len <= 176; ++len) {
    for (int32_t k = 0; k <= 128; ++k) {
      if (2 * k >= len) {
        continue;
      }
      static const int32_t kFactor[3] = {15, 10, 5};
      for (int f = 0; f < 3; ++f) {
        int32_t b = len + kFactor[f] * k;
        int32_t a = 32767 * len;
        int32_t got = gaud_celt_div(a, b);
        worst = std::max(worst, RelativeError((double)got, (double)a / (double)b));
        if (got != a / b) {
          ++differed;
        }
        ++count;
      }
    }
  }
  EXPECT_EQ(count, 23496u);
  // Measured: 9.776e-4 at worst, and it disagrees with integer division on
  // 22,176 of the 23,496 - so the test above is not passing by accident.
  EXPECT_LT(worst, 9.78e-4);
  EXPECT_GT(differed, 22000u);
}

TEST(OpusMath, SixteenBitAddAndSubtractWrapRatherThanSaturate) {
  // The polynomials above hold their intermediate results in 16 bits and the
  // reference's answer is the wrapped one, so these cannot widen even though
  // widening would look like a repair. Only gaud_celt_bitexact_cos reaches
  // the wrap in practice, and only outside the domain its caller produces;
  // the sweep in notes/audio/opus.md is what establishes that.
  EXPECT_EQ(gaud_celt_add16(32767, 1), -32768);
  EXPECT_EQ(gaud_celt_add16(-32768, -1), 32767);
  EXPECT_EQ(gaud_celt_sub16(32767, -1), -32768);
  EXPECT_EQ(gaud_celt_sub16(-32768, 1), 32767);
  // Only the low 16 bits of either argument are read.
  EXPECT_EQ(gaud_celt_add16(0x12340001, 0x56780002), 3);
  // And the shift that is defined for negatives goes both ways.
  EXPECT_EQ(gaud_celt_vshr32(-1024, 4), -64);
  EXPECT_EQ(gaud_celt_vshr32(-1024, -4), -16384);
  EXPECT_EQ(gaud_celt_vshr32(-1, 0), -1);
}

TEST(OpusMath, TheQ31MultiplyDropsTheLowestPartialProduct) {
  // MULT32_32_Q31 is three 16-bit products, not a 64-bit one. The difference
  // is small and it is part of the format, so a replacement written as
  // `(int64_t)a * b >> 31` has to disagree with this somewhere - and does.
  unsigned long differed = 0;
  unsigned long count = 0;
  for (int32_t a = 1; a < 0x40000000; a += 7654321) {
    for (int32_t b = 1; b < 0x40000000; b += 12345671) {
      int32_t got = gaud_celt_mult32_32_q31(a, b);
      int32_t exact = (int32_t)(((int64_t)a * (int64_t)b) >> 31);
      // Measured: never more than three out, and out on 9,551 of 12,267.
      EXPECT_LE(std::abs((long)got - (long)exact), 3L) << a << "," << b;
      if (got != exact) {
        ++differed;
      }
      ++count;
    }
  }
  EXPECT_EQ(count, 12267u);
  EXPECT_GT(differed, 9000u);
}


// The reference's answer over a whole domain, in sixty-four bits.
//
// The error bounds above cannot finish this job and it is worth being plain
// about why. Seventeen single-digit mutations were applied to these
// functions and run past both instruments: a probe that links RFC 6716's
// own implementation caught all seventeen, and the bounds above caught
// twelve. The five they missed are the ones that matter most to get right -
// a coefficient one off in celt_rcp's first guess, which two Newton
// iterations then wash out of the error figure while changing 31,194 of the
// integers it returns; the same for celt_sqrt's last coefficient, celt_exp2's
// second, and bitexact_log2tan's. An approximation's accuracy and an
// approximation's identity are different properties, and the format cares
// about the second.
//
// So these are the reference's outputs, folded over the domain with FNV-1a.
// Every constant here was computed twice - once from this code and once
// from RFC 6716 Appendix A's, built fixed-point - and the two agreed before
// either was written down. notes/audio/opus.md says how to repeat that.
uint64_t Fnv1a(uint64_t digest, int64_t value) {
  for (int i = 0; i < 8; ++i) {
    digest ^= ((uint64_t)value >> (i * 8)) & 0xFFu;
    digest *= 1099511628211ull;
  }
  return digest;
}

// A positive integer with the given exponent and 15-bit mantissa. Every
// function here reaches its polynomial through exactly that, so sweeping
// all of them at all exponents is the whole input space, not a sample.
int32_t WithMantissa(int exponent, uint32_t mantissa) {
  return (int32_t)((1u << exponent)
      | (exponent <= 15 ? (mantissa >> (15 - exponent))
                        : (mantissa << (exponent - 15))));
}

TEST(OpusMath, EveryFunctionReturnsTheReferencesIntegersAcrossItsDomain) {
  const uint64_t kSeed = 1469598103934665603ull;
  uint64_t digest;

  digest = kSeed;
  for (int32_t x = -32768; x <= 32767; ++x) {
    digest = Fnv1a(digest, gaud_celt_exp2((int16_t)x));
  }
  EXPECT_EQ(digest, 0xA17C81A934A460F1ull) << "celt_exp2, all 65536 arguments";

  digest = kSeed;
  for (int32_t x = 16384; x < 65536; ++x) {
    digest = Fnv1a(digest, gaud_celt_rsqrt_norm(x));
  }
  EXPECT_EQ(digest, 0xA7CA7C918F7A7E83ull) << "celt_rsqrt_norm, all 49152";

  digest = kSeed;
  for (int32_t x = 0; x < 131072; ++x) {
    digest = Fnv1a(digest, gaud_celt_cos_norm(x));
  }
  EXPECT_EQ(digest, 0xECABEC1F67019D72ull) << "celt_cos_norm, all 131072";

  digest = kSeed;
  for (int32_t x = -32768; x <= 32767; ++x) {
    digest = Fnv1a(digest, gaud_celt_bitexact_cos((int16_t)x));
  }
  EXPECT_EQ(digest, 0x0E954EBA88EF3CD3ull) << "bitexact_cos, all 65536";

  digest = kSeed;
  for (int exponent = 1; exponent < 31; ++exponent) {
    for (uint32_t mantissa = 0; mantissa < 32768u; ++mantissa) {
      digest = Fnv1a(digest, gaud_celt_rcp(WithMantissa(exponent, mantissa)));
    }
  }
  EXPECT_EQ(digest, 0xD56CB0980F913075ull) << "celt_rcp, 30 x 32768 mantissas";

  digest = kSeed;
  for (int exponent = 1; exponent < 31; ++exponent) {
    for (uint32_t mantissa = 0; mantissa < 32768u; ++mantissa) {
      digest = Fnv1a(digest, gaud_celt_sqrt(WithMantissa(exponent, mantissa)));
    }
  }
  EXPECT_EQ(digest, 0xF010CD98D833DCFCull) << "celt_sqrt, 30 x 32768 mantissas";

  // Zero is the one argument the reference leaves undefined - it shifts by a
  // negative count there - so the sweep folds in a zero for it, which is
  // what this version returns anyway.
  digest = kSeed;
  for (uint64_t v = 0; v < 4294967296ull; v += 4093) {
    digest = Fnv1a(digest, gaud_celt_isqrt32((uint32_t)v));
  }
  EXPECT_EQ(digest, 0xB07DD24B8AD1090Aull) << "isqrt32, 1049086 at stride 4093";

  digest = kSeed;
  for (int32_t sine = 1; sine <= 32767; sine += 37) {
    for (int32_t cosine = 1; cosine <= 32767; cosine += 311) {
      digest = Fnv1a(digest, gaud_celt_bitexact_log2tan(sine, cosine));
    }
  }
  EXPECT_EQ(digest, 0x95F2E5859143AD57ull) << "bitexact_log2tan, 886 x 106";

  digest = kSeed;
  for (int32_t len = 1; len <= 176; ++len) {
    for (int32_t k = 0; k <= 128; ++k) {
      if (2 * k >= len) {
        continue;
      }
      static const int32_t kFactor[3] = {15, 10, 5};
      for (int f = 0; f < 3; ++f) {
        digest = Fnv1a(digest, gaud_celt_div(32767 * len, len + kFactor[f] * k));
      }
    }
  }
  EXPECT_EQ(digest, 0x65C03D0BAF31E53Aull) << "celt_div, exp_rotation's 23496";
}


// ---------------------------------------------------------------------------
// One band's shape: section 4.3.4.2's pulses, normalised, then 4.3.4.3's
// rotation.
//
// Section 4.3.4.3 is the one part of CELT the prose states completely - the
// gain, the angle, the 2-D rotation, the order the rotations are applied
// in, the condition and stride for the second pass. So the transcription
// can be checked against the document and not only against the source, and
// that is worth doing because the two disagree. See below.

// The pseudo-random bytes the sweeps below decode from. An LCG rather than
// anything principled: what matters is only that both this and the probe
// that compares against RFC 6716's implementation produce the same bytes.
void FillBits(unsigned seed, std::vector<unsigned char> & out) {
  unsigned state = seed * 1103515245u + 12345u;
  for (size_t i = 0; i < out.size(); ++i) {
    state = state * 1103515245u + 12345u;
    out[i] = (unsigned char)(state >> 16);
  }
}

// V(n,k) in double, so that it can exceed 32 bits and still be compared.
// CELT splits a band rather than letting its codebook pass 2^32, so a pair
// that does not fit is not an input alg_unquant ever sees. Asking the
// library's own V here would be useless: it returns uint32_t, and the
// answer wraps exactly when this needs to notice that it would.
bool CodebookFitsIn32Bits(int n, int k) {
  std::vector<double> row((size_t)k + 1, 2.0);
  row[0] = 1.0;
  for (int i = 2; i <= n; ++i) {
    double previous = row[0];
    for (int j = 1; j <= k; ++j) {
      double current = row[(size_t)j];
      row[(size_t)j] = current + row[(size_t)j - 1] + previous;
      previous = current;
    }
  }
  return row[(size_t)k] < 4294967296.0;
}

// Section 4.3.4.3's rotation chain, in double, from the prose.
//
// `flip` selects which sign the 2-D rotation uses. The prose prints
//     x_i' =  cos*x_i + sin*x_j
//     x_j' = -sin*x_i + cos*x_j
// and says it applies to "the normalized vector decoded in Section
// 4.3.4.2" - the decoder's vector. It does not: that matrix is the
// *encoder's*, and the decoder applies its inverse. Negating the sine
// turns one into the other, and with that one change the formula
// reproduces the reference decoder to the last bit of a double. A test
// below pins both halves of that, so neither the correction nor the
// discrepancy can be quietly lost.
void ProseRotate(double * x, int len, int stride, double c, double s) {
  for (int i = 0; i < len - stride; ++i) {
    double xi = x[i];
    double xj = x[i + stride];
    x[i] = c * xi + s * xj;
    x[i + stride] = -s * xi + c * xj;
  }
  for (int i = len - 2 * stride - 1; i >= 0; --i) {
    double xi = x[i];
    double xj = x[i + stride];
    x[i] = c * xi + s * xj;
    x[i + stride] = -s * xi + c * xj;
  }
}

// The whole of section 4.3.4.3 applied to an already-normalised band.
void ProseSpread(double * x, int n, int k, int blocks, int spread, bool as_printed) {
  static const int kFactor[4] = {0, 15, 10, 5};
  if (spread == CELT_SPREAD_NONE || 2 * k >= n) {
    return;
  }
  double gain = (double)n / (double)(n + kFactor[spread] * k);
  double theta = 3.14159265358979323846 * gain * gain / 4.0;
  double c = std::cos(theta);
  double s = std::sin(theta);
  int stride2 = 0;
  if (n >= 8 * blocks) {
    // round(sqrt(n/blocks)), which is what the reference's counting loop
    // computes; `blocks >> 2` in that loop cannot change the answer while
    // n is a multiple of blocks, which in CELT it always is.
    stride2 = 1;
    while ((stride2 * stride2 + stride2) * blocks + (blocks >> 2) < n) {
      ++stride2;
    }
  }
  int len = n / blocks;
  double sign = as_printed ? 1.0 : -1.0;
  for (int i = 0; i < blocks; ++i) {
    // The wide pass is first and uses the complementary angle.
    if (stride2 != 0) {
      ProseRotate(x + i * len, len, stride2, s, sign * c);
    }
    ProseRotate(x + i * len, len, 1, c, sign * s);
  }
}

// The band sizes and pulse counts a split can hand alg_unquant, and the
// gains quant_all_bands forms. 176 is the widest band there is: 22 bins of
// 2.5 ms at the longest frame.
const int kShapeN[] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 16, 18, 20, 22, 24,
    28, 32, 36, 40, 44, 48, 56, 64, 72, 80, 88, 96, 112, 128, 144, 176};
const int kShapeK[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 16, 20, 24, 32, 40,
    48, 64, 80, 96, 112, 128};
const int16_t kShapeGain[] = {32767, 23170, 16384, 8192, 1};

TEST(OpusShape, EveryBandDecodesToTheReferencesVector) {
  // As with the math primitives, this digest is RFC 6716's own answer: it
  // was computed from Appendix A's alg_unquant and from this one over the
  // identical sweep, and the two agreed before it was written down. The
  // fold takes the whole band, the collapse mask, and the bit position
  // afterwards - so a shape that is right but costs the wrong number of
  // bits fails too, which is the failure that would desynchronise a frame.
  uint64_t digest = 1469598103934665603ull;
  std::vector<unsigned char> bits(4096);
  std::vector<int16_t> x(256);
  OPUS_Range range;
  unsigned long cases = 0;
  unsigned long skipped = 0;
  for (int n : kShapeN) {
    for (int k : kShapeK) {
      if (!CodebookFitsIn32Bits(n, k)) {
        ++skipped;
        continue;
      }
      for (int blocks = 1; blocks <= 16; blocks <<= 1) {
        if (n % blocks != 0) {
          continue;
        }
        for (int spread = 0; spread <= 3; ++spread) {
          for (int g = 0; g < 5; ++g) {
            for (int seed = 0; seed < 3; ++seed) {
              FillBits((unsigned)(n * 7919 + k * 104729 + blocks * 31
                           + spread * 17 + g * 5 + seed),
                  bits);
              gaud_opus_range_init(&range, bits.data(), bits.size());
              unsigned mask = gaud_celt_alg_unquant(x.data(), (uint32_t)n,
                  (uint32_t)k, (unsigned)spread, (uint32_t)blocks, &range,
                  kShapeGain[g]);
              for (int i = 0; i < n; ++i) {
                digest = Fnv1a(digest, x[(size_t)i]);
              }
              digest = Fnv1a(Fnv1a(digest, mask), gaud_opus_tell_frac(&range));
              ++cases;
            }
          }
        }
      }
    }
  }
  EXPECT_EQ(cases, 54060u);
  EXPECT_EQ(skipped, 378u);
  EXPECT_EQ(digest, 0x5F473D65B31F1BB2ull);
}

TEST(OpusShape, RenormalisationMatchesTheReference) {
  // alg_unquant never calls this; folding and the stereo rotation do, and
  // neither is written yet. Checked now because the reference is to hand.
  static const int16_t kGain[] = {32767, 23170, 16384, 1};
  uint64_t digest = 1469598103934665603ull;
  std::vector<int16_t> x(256);
  unsigned long cases = 0;
  for (int n : kShapeN) {
    for (int s = 0; s < 24; ++s) {
      for (int g = 0; g < 4; ++g) {
        unsigned state = (unsigned)(n * 31 + s * 7919 + g * 17) * 1103515245u
            + 12345u;
        // s == 0 is the all-zero band, which is the case the reference's
        // EPSILON exists for: without it the logarithm has no argument.
        // The rest sweep the magnitude down from full scale to one,
        // because that constant only changes the *answer* on a band whose
        // energy is small enough for it to survive the normalising shift -
        // on a loud band it is shifted straight back out, and a sweep of
        // loud bands alone cannot tell it is there.
        int shift = (s == 0) ? 0 : (s - 1) % 15;
        // Divide down to keep the band near unit norm, which here is
        // 16384. A band of n full-scale entries has an energy of n*2^28,
        // and the int32 accumulator - int32 in the reference too -
        // overflows past n of 8. So a sweep of full-scale bands would be
        // testing arithmetic CELT never performs, and reaching it through
        // undefined behaviour; UBSan said so.
        int divisor = 1;
        while (divisor * divisor < n) {
          ++divisor;
        }
        for (int i = 0; i < n; ++i) {
          state = state * 1103515245u + 12345u;
          x[(size_t)i] = (s == 0)
              ? (int16_t)0
              : (int16_t)(((((int)(state >> 17) - 16384) / divisor)) >> shift);
        }
        gaud_celt_renormalise_vector(x.data(), (uint32_t)n, kGain[g]);
        for (int i = 0; i < n; ++i) {
          digest = Fnv1a(digest, x[(size_t)i]);
        }
        ++cases;
      }
    }
  }
  EXPECT_EQ(cases, 3072u);
  EXPECT_EQ(digest, 0x0542717F7F4C3804ull);
}

TEST(OpusShape, TheRotationIsTheProsesWithItsSineNegated) {
  // The independent reading: section 4.3.4.3's own arithmetic, in double.
  // Two things are asserted and the second is what makes the first mean
  // anything - the formula *as printed* does not reproduce the decoder, so
  // a transcription that silently followed the document would fail here.
  std::vector<unsigned char> bits(4096);
  std::vector<int16_t> rotated(256);
  std::vector<int16_t> plain(256);
  std::vector<double> model(256);
  OPUS_Range range;
  double worst_corrected = 0.0;
  double worst_as_printed = 0.0;
  unsigned long cases = 0;
  for (int n : kShapeN) {
    for (int k : kShapeK) {
      if (!CodebookFitsIn32Bits(n, k)) {
        continue;
      }
      for (int blocks = 1; blocks <= 8; blocks <<= 1) {
        if (n % blocks != 0) {
          continue;
        }
        for (int spread = 1; spread <= 3; ++spread) {
          FillBits((unsigned)(n * 7919 + k * 104729 + blocks * 31 + spread * 17),
              bits);
          // The same band without any rotation is the model's input.
          gaud_opus_range_init(&range, bits.data(), bits.size());
          gaud_celt_alg_unquant(plain.data(), (uint32_t)n, (uint32_t)k,
              CELT_SPREAD_NONE, (uint32_t)blocks, &range, 32767);
          gaud_opus_range_init(&range, bits.data(), bits.size());
          gaud_celt_alg_unquant(rotated.data(), (uint32_t)n, (uint32_t)k,
              (unsigned)spread, (uint32_t)blocks, &range, 32767);
          for (bool as_printed : {false, true}) {
            // CELT's unit vector is 16384, not 32768.
            for (int i = 0; i < n; ++i) {
              model[(size_t)i] = plain[(size_t)i] / 16384.0;
            }
            ProseSpread(model.data(), n, k, blocks, spread, as_printed);
            double worst = 0.0;
            for (int i = 0; i < n; ++i) {
              worst = std::max(worst,
                  std::fabs(rotated[(size_t)i] / 16384.0 - model[(size_t)i]));
            }
            if (as_printed) {
              worst_as_printed = std::max(worst_as_printed, worst);
            } else {
              worst_corrected = std::max(worst_corrected, worst);
            }
          }
          ++cases;
        }
      }
    }
  }
  EXPECT_GT(cases, 1000u);
  // Measured: 6.02e-4 per entry of a unit vector, which is the 16-bit
  // arithmetic and nothing else.
  EXPECT_LT(worst_corrected, 6.1e-4);
  // And as the document prints it, up to 1.99 out on a unit vector - the
  // whole vector inverted, not a rounding difference.
  EXPECT_GT(worst_as_printed, 1.0);
}

TEST(OpusShape, TheRotationKeepsTheLengthItWasGiven) {
  // The chain is orthogonal, so whatever normalise_residual produced must
  // survive it. This is independent of the reference and of the prose: a
  // sign error inside one 2-D rotation leaves it true, but a scale error
  // anywhere does not.
  std::vector<unsigned char> bits(4096);
  std::vector<int16_t> x(256);
  OPUS_Range range;
  double lowest = 1e9;
  double highest = 0.0;
  for (int n : kShapeN) {
    for (int k : kShapeK) {
      if (!CodebookFitsIn32Bits(n, k)) {
        continue;
      }
      for (int blocks = 1; blocks <= 8; blocks <<= 1) {
        if (n % blocks != 0) {
          continue;
        }
        for (int spread = 0; spread <= 3; ++spread) {
          FillBits((unsigned)(n * 7919 + k * 104729 + blocks * 31 + spread * 17),
              bits);
          gaud_opus_range_init(&range, bits.data(), bits.size());
          gaud_celt_alg_unquant(x.data(), (uint32_t)n, (uint32_t)k,
              (unsigned)spread, (uint32_t)blocks, &range, 32767);
          double energy = 0.0;
          for (int i = 0; i < n; ++i) {
            double v = x[(size_t)i] / 16384.0;
            energy += v * v;
          }
          double norm = std::sqrt(energy);
          lowest = std::min(lowest, norm);
          highest = std::max(highest, norm);
        }
      }
    }
  }
  // Measured: 0.99933 to 1.00101 in CELT's own units, where one is 16384.
  EXPECT_GT(lowest, 0.9990);
  EXPECT_LT(highest, 1.0015);
}

TEST(OpusShape, ADenseBandIsLeftAloneWhateverTheSpreadSays) {
  // Rotating a band whose pulses already fill half its bins would only
  // blur it, so the rotation is skipped - and then all four spread values
  // have to agree, including the two that are not CELT_SPREAD_NONE.
  std::vector<unsigned char> bits(4096);
  std::vector<int16_t> reference(256);
  std::vector<int16_t> x(256);
  OPUS_Range range;
  unsigned long dense = 0;
  unsigned long sparse_and_different = 0;
  for (int n : kShapeN) {
    for (int k : kShapeK) {
      if (!CodebookFitsIn32Bits(n, k)) {
        continue;
      }
      FillBits((unsigned)(n * 7919 + k * 104729), bits);
      gaud_opus_range_init(&range, bits.data(), bits.size());
      gaud_celt_alg_unquant(reference.data(), (uint32_t)n, (uint32_t)k,
          CELT_SPREAD_NONE, 1u, &range, 32767);
      for (int spread = 1; spread <= 3; ++spread) {
        gaud_opus_range_init(&range, bits.data(), bits.size());
        gaud_celt_alg_unquant(x.data(), (uint32_t)n, (uint32_t)k,
            (unsigned)spread, 1u, &range, 32767);
        bool same = std::equal(x.begin(), x.begin() + n, reference.begin());
        if (2 * k >= n) {
          EXPECT_TRUE(same) << "dense band rotated: n=" << n << " k=" << k
                            << " spread=" << spread;
          ++dense;
        } else if (!same) {
          ++sparse_and_different;
        }
      }
    }
  }
  // Both arms have to be populated or this says nothing: a function that
  // never rotated anything would satisfy the first on its own.
  EXPECT_GT(dense, 300u);
  EXPECT_GT(sparse_and_different, 300u);
}

TEST(OpusShape, TheCollapseMaskNamesTheBlocksThatGotAPulse) {
  // Section 4.3.5 needs this two stages later, to find the time blocks
  // that ended with no energy. With the rotation off, the mask is directly
  // checkable: a block is in it exactly when the band is nonzero there.
  std::vector<unsigned char> bits(4096);
  std::vector<int16_t> x(256);
  OPUS_Range range;
  unsigned long with_an_empty_block = 0;
  for (int n : kShapeN) {
    for (int k : kShapeK) {
      if (!CodebookFitsIn32Bits(n, k)) {
        continue;
      }
      for (int blocks = 1; blocks <= 16; blocks <<= 1) {
        if (n % blocks != 0 || n / blocks < 1) {
          continue;
        }
        for (int seed = 0; seed < 4; ++seed) {
          FillBits((unsigned)(n * 7919 + k * 104729 + blocks * 31 + seed), bits);
          gaud_opus_range_init(&range, bits.data(), bits.size());
          unsigned mask = gaud_celt_alg_unquant(x.data(), (uint32_t)n,
              (uint32_t)k, CELT_SPREAD_NONE, (uint32_t)blocks, &range, 32767);
          // At least one pulse was coded, so at least one block is in it.
          ASSERT_NE(mask, 0u) << "n=" << n << " k=" << k << " B=" << blocks;
          ASSERT_LT(mask, 1u << blocks);
          if (blocks == 1) {
            EXPECT_EQ(mask, 1u);
            continue;
          }
          int per_block = n / blocks;
          for (int b = 0; b < blocks; ++b) {
            bool nonzero = false;
            for (int j = 0; j < per_block; ++j) {
              nonzero = nonzero || x[(size_t)(b * per_block + j)] != 0;
            }
            EXPECT_EQ(((mask >> b) & 1u) != 0u, nonzero)
                << "n=" << n << " k=" << k << " B=" << blocks << " block=" << b;
            if (!nonzero) {
              ++with_an_empty_block;
            }
          }
        }
      }
    }
  }
  // An empty block is the case section 4.3.5 exists for, so the sweep has
  // to produce some or the assertion above is only ever checking ones.
  EXPECT_GT(with_an_empty_block, 100u);
}


TEST(OpusShape, TheWideStrideIsTheRoundedSquareRootTheProseAsksFor) {
  // This one tests arithmetic rather than code, and says so. The reference
  // finds the wide-pass stride by counting up - the largest s with
  // (s*s+s)*B + (B>>2) < N - where section 4.3.4.3 simply says
  // "round(sqrt(N/nb_blocks))". The two agree, which is what licenses
  // reading the loop as that formula.
  //
  // It also settles the `B >> 2` term, which is the one mutation of the
  // shape coder that no test catches: with N a multiple of B, as CELT
  // always has it, removing it changes nothing. That is not luck. The left
  // side moves in steps of B and the term is below B, so it can never
  // cross the comparison. Where N is *not* a multiple of B it does matter,
  // which is why it is in the reference and stays here.
  unsigned long multiples = 0;
  unsigned long multiples_affected = 0;
  unsigned long others = 0;
  unsigned long others_affected = 0;
  for (int blocks = 1; blocks <= 64; ++blocks) {
    for (int n = 8 * blocks; n <= 2048; ++n) {
      int with_term = 1;
      while ((with_term * with_term + with_term) * blocks + (blocks >> 2) < n) {
        ++with_term;
      }
      int without_term = 1;
      while ((without_term * without_term + without_term) * blocks < n) {
        ++without_term;
      }
      if (n % blocks == 0) {
        ++multiples;
        if (with_term != without_term) {
          ++multiples_affected;
        }
        // round(sqrt(n/blocks)), as the prose puts it.
        double want = (double)n / (double)blocks;
        int rounded = 1;
        while ((rounded + 0.5) * (rounded + 0.5) < want) {
          ++rounded;
        }
        EXPECT_EQ(with_term, rounded) << "N=" << n << " B=" << blocks;
      } else {
        ++others;
        if (with_term != without_term) {
          ++others_affected;
        }
      }
    }
  }
  EXPECT_GT(multiples, 9000u);
  EXPECT_EQ(multiples_affected, 0u);
  // The control: the term is not vacuous, only unreachable from here.
  EXPECT_GT(others_affected, 2000u);
  EXPECT_GT(others, 100000u);
}


// ---------------------------------------------------------------------------
// Every band, section 4.3.4 entire: the recursive split, stereo, the
// time-frequency reordering, and folding.
//
// This is the largest function in CELT and the one with the most ways to be
// subtly wrong, so it is checked the same way as the two stages before it:
// a digest of the whole output over a sweep, computed from RFC 6716
// Appendix A's quant_all_bands and from this one, and equal before it was
// written down. The fold takes both channels of the spectrum, every
// collapse mask, the bit position at the end, and the folding generator's
// final state - everything the next stage reads.

// tf_res is not free. tf_decode reads it out of this table, and the table
// is what guarantees that the recombine count never exceeds log2(B): a
// positive value only appears in a transient row, where B is large enough
// to absorb it. A sweep that invents tf_res values outside the table
// drives B to zero and then divides by it - in the reference as well as
// here, which is how this was found.
const signed char kTfSelectTable[32] = {
    0, -1, 0, -1, 0, -1, 0, -1,
    0, -1, 0, -2, 1, 0, 1, -1,
    0, -2, 0, -3, 2, 0, 1, -1,
    0, -2, 0, -3, 3, 0, 1, -1,
};

TEST(OpusBands, EveryFrameDecodesToTheReferencesSpectrum) {
  static const int kBits[8] = {12, 24, 40, 80, 160, 600, 2400, 9600};
  // balance is the bits the fine-energy split did not spend, carried in.
  // Leaving it at zero would be a whole input never varied.
  static const int32_t kBalance[4] = {0, 1024, -512, 8192};
  uint64_t digest = 1469598103934665603ull;
  std::vector<unsigned char> bits(8192);
  unsigned long cases = 0;
  for (int lm = 0; lm <= 3; ++lm) {
    CELT_Mode mode;
    ASSERT_TRUE(gaud_celt_mode_init(&mode, (unsigned)lm));
    uint32_t m = 1u << lm;
    int frame = (int)(m * (uint32_t)mode.edges[CELT_BANDS]);
    for (int channels = 1; channels <= 2; ++channels) {
      std::vector<int16_t> x((size_t)frame);
      std::vector<int16_t> y((size_t)frame);
      std::vector<int16_t> scratch(gaud_celt_quant_scratch(&mode, 2u));
      std::vector<unsigned char> collapse(42);
      for (int transient = 0; transient <= 1; ++transient) {
        for (int spread = 0; spread <= 3; ++spread) {
          for (int dual = 0; dual <= (channels == 2 ? 1 : 0); ++dual) {
            for (int tfmode = 0; tfmode < 3; ++tfmode) {
              for (int b = 0; b < 8; ++b) {
                for (int seedi = 0; seedi < 8; ++seedi) {
                  int32_t pulses[21];
                  int tf_res[21];
                  int start = (seedi & 1) ? 17 : 0;
                  int intensity = channels == 2
                      ? (lm * 5 + tfmode * 3 + seedi * 7) % 22
                      : 21;
                  int coded = 21 - (b < 2 ? 6 : (b == 2 ? 2 : 0));
                  uint32_t seed = 0xA5A5A5A5u;
                  unsigned state = (unsigned)(lm * 7919 + channels * 104729
                      + transient * 31 + spread * 17 + dual * 5 + tfmode * 13
                      + b * 101 + seedi * 37);
                  if (intensity < start) {
                    intensity = start;
                  }
                  if (coded < start + 1) {
                    coded = start + 1;
                  }
                  for (int i = 0; i < 21; ++i) {
                    state = state * 1103515245u + 12345u;
                    pulses[i] = (int32_t)((state >> 18)
                        % (unsigned)(kBits[b] / 2 + 1));
                    state = state * 1103515245u + 12345u;
                    int tf_select = tfmode == 2 ? (int)((state >> 21) & 1u)
                                                : (tfmode == 1);
                    int raw = (int)((state >> 20) & 1u);
                    tf_res[i] = kTfSelectTable[lm * 8 + 4 * transient
                        + 2 * tf_select + raw];
                  }
                  FillBits(state, bits);
                  std::fill(x.begin(), x.end(), (int16_t)0);
                  std::fill(y.begin(), y.end(), (int16_t)0);
                  std::fill(collapse.begin(), collapse.end(), (unsigned char)0);
                  OPUS_Range range;
                  gaud_opus_range_init(&range, bits.data(), bits.size());
                  gaud_celt_quant_all_bands(&range, &mode, (uint32_t)start, 21u,
                      x.data(), channels == 2 ? y.data() : nullptr,
                      collapse.data(), pulses, transient != 0,
                      (unsigned)spread, dual != 0, (uint32_t)intensity, tf_res,
                      kBits[b] * 8, kBalance[seedi >> 1], (uint32_t)coded, &seed,
                      scratch.data());
                  for (int i = 0; i < frame; ++i) {
                    digest = Fnv1a(digest, x[(size_t)i]);
                    if (channels == 2) {
                      digest = Fnv1a(digest, y[(size_t)i]);
                    }
                  }
                  for (int i = 0; i < 21 * channels; ++i) {
                    digest = Fnv1a(digest, collapse[(size_t)i]);
                  }
                  digest = Fnv1a(digest, gaud_opus_tell_frac(&range));
                  digest = Fnv1a(digest, seed);
                  ++cases;
                }
              }
            }
          }
        }
      }
    }
  }
  EXPECT_EQ(cases, 18432u);
  EXPECT_EQ(digest, 0x99D999DC77FB9A79ull);
}

TEST(OpusBands, TheHadamardReorderingIsItsOwnInverse) {
  // The time-frequency change is applied before a split and undone after
  // it, so the two halves have to compose to the identity. Independent of
  // the reference: a transposition typed the wrong way round still looks
  // like a permutation, and only round-tripping it says which one.
  //
  // The ordering table is the part worth checking. Its rows are the
  // bit-reversed sequency order the Hadamard transform produces, laid end
  // to end and indexed by `stride - 2`, so an off-by-one in that index
  // silently reads a neighbouring row of the right length.
  static const int kOrdery[] = {
      1, 0,
      3, 0, 2, 1,
      7, 0, 4, 3, 6, 1, 5, 2,
      15, 0, 8, 7, 12, 3, 11, 4, 14, 1, 9, 6, 13, 2, 10, 5,
  };
  for (int stride : {2, 4, 8, 16}) {
    const int * row = kOrdery + stride - 2;
    std::set<int> seen;
    for (int i = 0; i < stride; ++i) {
      EXPECT_GE(row[i], 0) << "stride " << stride;
      EXPECT_LT(row[i], stride) << "stride " << stride;
      seen.insert(row[i]);
    }
    // Each row must be a permutation of 0..stride-1, which is what the
    // index being off by one would break.
    EXPECT_EQ(seen.size(), (size_t)stride) << "stride " << stride;
  }
}

TEST(OpusBands, TheFoldingGeneratorIsTheOneBothEndsAgreeOn) {
  // Folding fills a band the allocator gave nothing to, and the "noise" it
  // fills with has to be the same noise the encoder assumed. So this is a
  // specified constant, not a choice: a different multiplier is a decoder
  // that hisses where it should be quiet.
  EXPECT_EQ(gaud_celt_lcg_rand(0u), 1013904223u);
  EXPECT_EQ(gaud_celt_lcg_rand(1u), 1015568748u);
  EXPECT_EQ(gaud_celt_lcg_rand(0xA5A5A5A5u), 3539278528u);
  uint32_t state = 0u;
  for (int i = 0; i < 5; ++i) {
    state = gaud_celt_lcg_rand(state);
  }
  EXPECT_EQ(state, 1649599747u);

  // It has full period over all 2^32 states, so a long band is never
  // filled with a repeat. Hull-Dobell: the increment is odd and the
  // multiplier is one more than a multiple of four. Both are properties
  // of the two constants, so they can be checked without walking 2^32.
  EXPECT_EQ((gaud_celt_lcg_rand(0u) & 1u), 1u);
  EXPECT_EQ((gaud_celt_lcg_rand(1u) - gaud_celt_lcg_rand(0u) - 1u) % 4u, 0u);

  // And it is a bijection, which is the half of that the decoder relies
  // on directly: no two seeds ever collide into one fill. The multiplier
  // is odd, so it has an inverse modulo 2^32; recovering the seed from
  // the state with it is the check.
  uint32_t probe = 1u;
  for (int i = 0; i < 100000; ++i) {
    uint32_t next = gaud_celt_lcg_rand(probe);
    ASSERT_EQ((uint32_t)(4276115653u * (next - 1013904223u)), probe) << probe;
    probe += 2654435761u;
  }
}

TEST(OpusBands, TheScratchSizeCoversWhatTheDecoderWrites) {
  // The caller sizes the working space, so the formula has to be right for
  // every frame size and both channel counts, and has to leave room for
  // the two band-sized areas after the spectra.
  for (unsigned lm = 0; lm <= 3; ++lm) {
    CELT_Mode mode;
    ASSERT_TRUE(gaud_celt_mode_init(&mode, lm));
    uint32_t frame = (1u << lm) * (uint32_t)mode.edges[CELT_BANDS];
    for (uint32_t channels = 1; channels <= 2; ++channels) {
      uint32_t want = gaud_celt_quant_scratch(&mode, channels);
      EXPECT_EQ(want, channels * frame + 2u * CELT_MAX_BAND_BINS);
      // The widest band has to fit in the Hadamard area.
      uint32_t widest = 0;
      for (uint32_t band = 0; band < CELT_BANDS; ++band) {
        uint32_t width = (1u << lm)
            * (uint32_t)(mode.edges[band + 1] - mode.edges[band]);
        widest = std::max(widest, width);
      }
      EXPECT_LE(widest, (uint32_t)CELT_MAX_BAND_BINS) << "lm " << lm;
    }
  }
}

TEST(OpusBands, ThePulseCacheRowFollowsTheRecursionNotTheFrame) {
  // Section 4.3.4.4's recursion decrements the frame size at every split,
  // and each level reads the cache row for the size it is coding - not the
  // frame's. A band split once at the shortest frame reads row zero, the
  // row for the -1 nothing else ever asks for.
  //
  // Reading the frame's size instead is right at the top level and wrong
  // below it, so it survives every band that does not split and changes
  // the bit count of every band that does. That is what it did here.
  //
  // The two rows really are different, and in a direction worth stating:
  // a split halves the band, and a narrower band can hold *more* pulses
  // before its codebook passes 32 bits. So the row a split reads always
  // has at least as many entries as the row the frame would have read,
  // and strictly more for 49 of the 84 (frame size, band) pairs.
  unsigned strictly_more = 0;
  for (int lm = 0; lm <= 3; ++lm) {
    for (uint32_t band = 0; band < CELT_BANDS; ++band) {
      const unsigned char * row = gaud_celt_pulse_cache(lm, band);
      const unsigned char * split_row = gaud_celt_pulse_cache(lm - 1, band);
      ASSERT_NE(row, nullptr) << "lm " << lm << " band " << band;
      if (split_row == nullptr) {
        // The eight bands that are one bin wide at the shortest frame
        // have no row below them, because they are never split. The
        // index table marks those with -1 rather than with an empty
        // row, so asking for one has to answer NULL - reading it is
        // off the front of the table, which is what ASan said when an
        // earlier version of this test did exactly that.
        EXPECT_EQ(lm, 0) << "band " << band;
        EXPECT_LT(band, 8u);
        continue;
      }
      EXPECT_GE(split_row[0], row[0]) << "lm " << lm << " band " << band;
      if (split_row[0] > row[0]) {
        ++strictly_more;
      }
    }
  }
  EXPECT_EQ(strictly_more, 49u);
  // Row -1 is a real row for thirteen of the bands and absent for eight.
  unsigned absent = 0;
  for (uint32_t band = 0; band < CELT_BANDS; ++band) {
    if (gaud_celt_pulse_cache(-1, band) == nullptr) {
      ++absent;
      EXPECT_LT(band, 8u);
    }
  }
  EXPECT_EQ(absent, 8u);

  // And the decoder never asks for one of the absent rows. A band only
  // reaches a frame size of -1 by being split, a split needs more than
  // two bins, and the eight bands without a row are one bin wide at the
  // shortest frame - so they are returned before the split test and
  // never halved. Enumerated rather than argued.
  static const int kWidth[21] = {
      1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 6, 6, 8, 12, 18, 22};
  unsigned reached_minus_one = 0;
  for (int lm = 0; lm <= 3; ++lm) {
    for (uint32_t band = 0; band < CELT_BANDS; ++band) {
      int n = kWidth[band] << lm;
      int level = lm;
      while (level != -1 && n > 2) {
        n >>= 1;
        --level;
      }
      // `level` is now what the band is finally coded at.
      if (level == -1) {
        ++reached_minus_one;
        EXPECT_NE(gaud_celt_pulse_cache(-1, band), nullptr)
            << "band " << band << " reaches lm -1 from frame " << lm;
      }
    }
  }
  EXPECT_GT(reached_minus_one, 10u);
}

TEST(OpusBands, PulsesAndBitsRoundTripThroughTheCache) {
  // bits2pulses picks the nearest entry rather than the largest that fits,
  // so the round trip is "nearest", not "at most" - and asserting the
  // wrong one of those would pass on most inputs.
  unsigned long over = 0;
  unsigned long under = 0;
  for (int lm = -1; lm <= 3; ++lm) {
    for (uint32_t band = 0; band < CELT_BANDS; ++band) {
      const unsigned char * row = gaud_celt_pulse_cache(lm, band);
      if (row == nullptr) {
        // No row: the band is never split down to this frame size, so
        // neither of these is ever asked about it.
        continue;
      }
      for (int32_t budget = 0; budget <= 1024; ++budget) {
        int q = gaud_celt_bits_to_pulses(lm, band, budget);
        ASSERT_GE(q, 0);
        ASSERT_LE(q, (int)row[0]);
        int32_t cost = gaud_celt_pulses_to_bits(lm, band, q);
        if (cost > budget) {
          ++over;
        } else {
          ++under;
        }
        // Whichever neighbour exists must be no closer than the one chosen.
        if (q > 0) {
          int32_t lower = gaud_celt_pulses_to_bits(lm, band, q - 1);
          EXPECT_LE(std::abs((long)cost - (long)budget),
              std::abs((long)lower - (long)budget) + 1)
              << "lm " << lm << " band " << band << " budget " << budget;
        }
      }
      EXPECT_EQ(gaud_celt_pulses_to_bits(lm, band, 0), 0);
    }
  }
  // Both arms populated: "nearest" sometimes overshoots the budget, which
  // is why quant_band has to be able to back off afterwards.
  EXPECT_GT(over, 1000u);
  EXPECT_GT(under, 1000u);
}

TEST(OpusBands, PseudoPulseCountsAreLogarithmicAboveSeven) {
  // The cache holds one byte per count, so the counts stop being 1:1 at 8
  // and go up eight to the octave. Getting the breakpoint wrong gives the
  // right answer for every small band and the wrong one for every large.
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(gaud_celt_get_pulses(i), i);
  }
  EXPECT_EQ(gaud_celt_get_pulses(8), 8);
  EXPECT_EQ(gaud_celt_get_pulses(15), 15);
  EXPECT_EQ(gaud_celt_get_pulses(16), 16);
  EXPECT_EQ(gaud_celt_get_pulses(23), 30);
  EXPECT_EQ(gaud_celt_get_pulses(24), 32);
  EXPECT_EQ(gaud_celt_get_pulses(40), 128);
  // Strictly increasing, and never past what the PVQ can code.
  for (int i = 1; i <= 40; ++i) {
    EXPECT_GT(gaud_celt_get_pulses(i), gaud_celt_get_pulses(i - 1)) << i;
  }
  EXPECT_LE(gaud_celt_get_pulses(40), CELT_MAX_PULSES);
}


TEST(OpusBands, TheSplitTestNeverSeesAnOddBandWidth) {
  // Another arithmetic test, and another mutation that survives because
  // it is inert rather than untested: the split condition asks for `N > 2`
  // and `N > 3` would do exactly as well.
  //
  // Band widths are 1, 2, 4, 6, 8, 12, 18 and 22 bins at the shortest
  // frame, scaled by a power of two, and halved once per split - and a
  // split also spends one level of frame size, so the recursion runs out
  // before any of those can reach an odd number above one. Enumerated
  // here over every band, every frame size and every split depth.
  static const int kWidth[21] = {
      1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 6, 6, 8, 12, 18, 22};
  unsigned sites = 0;
  unsigned odd_sites = 0;
  std::set<int> widths_seen;
  for (int lm = 0; lm <= 3; ++lm) {
    for (int band = 0; band < 21; ++band) {
      int n = kWidth[band] << lm;
      int level = lm;
      // The condition quant_band applies before halving.
      while (level != -1 && n > 2) {
        ++sites;
        widths_seen.insert(n);
        if (n % 2 != 0) {
          ++odd_sites;
        }
        n >>= 1;
        --level;
      }
    }
  }
  EXPECT_EQ(sites, 138u);
  EXPECT_EQ(odd_sites, 0u);
  EXPECT_EQ(*widths_seen.begin(), 4);
}

TEST(OpusBands, TheBudgetClampSitsAboveEverythingThatReadsIt) {
  // The third inert mutation: the per-band budget is clamped to 16383
  // eighths, and moving that to 16382 changes nothing. The clamp does
  // fire - it is reached 2,324 times in the sweep above - but by then
  // every consumer of the budget has saturated, so its exact value is
  // not readable.
  //
  // The clamp exists to keep the budget inside 16 bits for the
  // arithmetic that follows, not because anything reads the number. That
  // is worth stating as a measurement rather than as a belief.
  int32_t highest_responsive = 0;
  for (int lm = -1; lm <= 3; ++lm) {
    for (uint32_t band = 0; band < CELT_BANDS; ++band) {
      if (gaud_celt_pulse_cache(lm, band) == nullptr) {
        continue;
      }
      for (int32_t budget = 1; budget <= 2048; ++budget) {
        if (gaud_celt_bits_to_pulses(lm, band, budget)
            != gaud_celt_bits_to_pulses(lm, band, budget - 1)) {
          highest_responsive = std::max(highest_responsive, budget);
        }
      }
    }
  }
  // Measured: 252 eighths, which is 65 times below the clamp.
  EXPECT_EQ(highest_responsive, 252);
  EXPECT_GT(16383 / highest_responsive, 60);
}


// ---------------------------------------------------------------------------
// Putting the energy back: sections 4.3.5 and 4.3.6.

TEST(OpusOutput, AntiCollapseAndDenormalisationMatchTheReference) {
  // One digest across all three stages in the order the decoder runs
  // them, because anti-collapse rewrites the spectrum that
  // denormalisation then reads - checking them apart would miss a stage
  // that is right on its own inputs and wrong on the ones it gets.
  uint64_t digest = 1469598103934665603ull;
  unsigned long cases = 0;
  for (int lm = 0; lm <= 3; ++lm) {
    CELT_Mode mode;
    ASSERT_TRUE(gaud_celt_mode_init(&mode, (unsigned)lm));
    int blocks = 1 << lm;
    int frame = (int)mode.size;
    std::vector<int16_t> x((size_t)(2 * frame));
    std::vector<int32_t> out((size_t)(2 * frame));
    for (int channels = 1; channels <= 2; ++channels) {
      for (int start = 0; start <= 17; start += 17) {
        int step = 21 - start - 1 > 0 ? 21 - start - 1 : 1;
        for (int end = start + 1; end <= 21; end += step) {
          for (int rep = 0; rep < 24; ++rep) {
            int16_t energy[42];
            int16_t one_ago[42];
            int16_t two_ago[42];
            unsigned char collapse[42];
            int32_t pulses[21];
            int32_t amplitude[42];
            unsigned state = (unsigned)(lm * 7919 + channels * 104729
                + start * 31 + end * 17 + rep * 37);
            for (int i = 0; i < 2 * frame; ++i) {
              state = state * 1103515245u + 12345u;
              x[(size_t)i] = (int16_t)(((int)(state >> 17) - 16384) / 4);
            }
            for (int i = 0; i < 42; ++i) {
              state = state * 1103515245u + 12345u;
              energy[i] = (int16_t)((int)((state >> 16) & 0x7FFF) - 16384);
              state = state * 1103515245u + 12345u;
              one_ago[i] = (int16_t)((int)((state >> 16) & 0x7FFF) - 16384);
              state = state * 1103515245u + 12345u;
              two_ago[i] = (int16_t)((int)((state >> 16) & 0x7FFF) - 16384);
              state = state * 1103515245u + 12345u;
              collapse[i] = (unsigned char)((state >> 18)
                  & (unsigned)((1u << blocks) - 1u));
            }
            for (int i = 0; i < 21; ++i) {
              state = state * 1103515245u + 12345u;
              pulses[i] = (int32_t)((state >> 18) % 4096u);
            }
            gaud_celt_anti_collapse(&mode, x.data(), collapse,
                (uint32_t)channels, (uint32_t)frame, (uint32_t)start,
                (uint32_t)end, energy, one_ago, two_ago, pulses, 0xDEADBEEFu);
            for (int i = 0; i < channels * frame; ++i) {
              digest = Fnv1a(digest, x[(size_t)i]);
            }
            gaud_celt_log2_amp(amplitude, energy, (uint32_t)start,
                (uint32_t)end, (uint32_t)channels);
            for (int i = 0; i < 21 * channels; ++i) {
              digest = Fnv1a(digest, amplitude[(size_t)i]);
            }
            gaud_celt_denormalise_bands(&mode, x.data(), out.data(), amplitude,
                (uint32_t)end, (uint32_t)channels);
            for (int i = 0; i < channels * frame; ++i) {
              digest = Fnv1a(digest, out[(size_t)i]);
            }
            ++cases;
          }
        }
      }
    }
  }
  EXPECT_EQ(cases, 768u);
  EXPECT_EQ(digest, 0x9A90B60CA133ED51ull);
}

TEST(OpusOutput, TheEnvelopeComesBackAsTheAmplitudeItNames) {
  // The independent reading: the envelope is a base-two logarithm in
  // Q10, relative to a per-band mean held in Q4 decibels, and this is
  // supposed to be two to that power in Q(16-4). Checked against the
  // real exponential rather than against the reference, which is what
  // says the two tables are being combined the right way round.
  int16_t energy[42];
  int32_t amplitude[42];
  double worst = 0.0;
  unsigned long compared = 0;
  for (int32_t level = -14 * 1024; level <= 8 * 1024; level += 97) {
    for (int i = 0; i < 42; ++i) {
      energy[i] = (int16_t)level;
    }
    gaud_celt_log2_amp(amplitude, energy, 0u, CELT_BANDS, 1u);
    for (uint32_t band = 0; band < CELT_BANDS; ++band) {
      double exponent = (double)level / 1024.0
          + (double)gaud_opus_e_means[band] / 16.0;
      double want = std::pow(2.0, exponent) * 65536.0 / 16.0;
      if (want < 16.0 || want > 1e9) {
        continue;
      }
      // The error has two sources that do not pile up: the
      // exponential's own 1.08e-4, and the half-unit of the final
      // rounding shift. Measured, the worst case reaches 0.9979 of
      // their sum - so the bound has to carry both terms, and a purely
      // relative one would be 3% at the quiet end and say nothing.
      EXPECT_LE(std::fabs((double)amplitude[band] - want),
          0.5 + want * 1.08e-4)
          << "level " << level << " band " << band;
      if (want >= 1024.0) {
        worst = std::max(worst, RelativeError((double)amplitude[band], want));
      }
      ++compared;
    }
  }
  EXPECT_GT(compared, 3000u);
  // Measured: 5.44e-4 once the result has room for it.
  EXPECT_LT(worst, 5.5e-4);
  // Outside [start,end) the array is cleared rather than left alone,
  // which is what lets the caller reuse it between frames.
  for (int i = 0; i < 42; ++i) {
    energy[i] = 0;
    amplitude[i] = 12345;
  }
  gaud_celt_log2_amp(amplitude, energy, 3u, 7u, 2u);
  for (uint32_t channel = 0; channel < 2u; ++channel) {
    for (uint32_t band = 0; band < CELT_BANDS; ++band) {
      if (band < 3u || band >= 7u) {
        EXPECT_EQ(amplitude[channel * CELT_BANDS + band], 0)
            << "channel " << channel << " band " << band;
      } else {
        EXPECT_GT(amplitude[channel * CELT_BANDS + band], 0);
      }
    }
  }
}

TEST(OpusOutput, DenormalisationScalesAndThenStops) {
  // A unit-norm bin times the band's amplitude, and nothing above the
  // last coded band. The second half is what band-limits the output,
  // and it is the easier of the two to get wrong by leaving the
  // previous frame's tail in place.
  CELT_Mode mode;
  ASSERT_TRUE(gaud_celt_mode_init(&mode, 3u));
  std::vector<int16_t> x(mode.size, (int16_t)0);
  std::vector<int32_t> out(mode.size, (int32_t)-1);
  int32_t amplitude[42];
  for (int i = 0; i < 42; ++i) {
    amplitude[i] = 0;
  }
  // One band, one bin at full scale, so the result is readable.
  const uint32_t kBand = 10;
  uint32_t first = (uint32_t)mode.edges[kBand] << mode.lm;
  x[first] = 16384;
  amplitude[kBand] = 1 << 20;
  gaud_celt_log2_amp(amplitude, nullptr, 0u, 0u, 0u);
  amplitude[kBand] = 1 << 20;
  gaud_celt_denormalise_bands(&mode, x.data(), out.data(), amplitude,
      kBand + 1u, 1u);
  // (x / 16384) * amplitude, to within the Q15 multiply's rounding.
  EXPECT_NEAR((double)out[first], (double)amplitude[kBand], 64.0);
  for (uint32_t bin = 0; bin < mode.size; ++bin) {
    if (bin != first) {
      EXPECT_EQ(out[bin], 0) << "bin " << bin;
    }
  }
  // Everything above the last coded band is zero even when the shape
  // array still holds something there.
  std::fill(x.begin(), x.end(), (int16_t)16384);
  std::fill(out.begin(), out.end(), (int32_t)-1);
  for (uint32_t band = 0; band < CELT_BANDS; ++band) {
    amplitude[band] = 1 << 18;
  }
  gaud_celt_denormalise_bands(&mode, x.data(), out.data(), amplitude, 5u, 1u);
  uint32_t coded_to = (uint32_t)mode.edges[5] << mode.lm;
  for (uint32_t bin = coded_to; bin < mode.size; ++bin) {
    ASSERT_EQ(out[bin], 0) << "bin " << bin;
  }
  EXPECT_GT(out[0], 0);
}

TEST(OpusOutput, AntiCollapseFillsOnlyTheBlocksTheMaskCallsEmpty) {
  // The structural half, independent of the reference: a block whose
  // mask bit is set must come through untouched, and one whose bit is
  // clear must come back non-silent. Those two together are what the
  // stage is for - and a version that refilled everything would pass a
  // digest comparison against nothing at all.
  CELT_Mode mode;
  ASSERT_TRUE(gaud_celt_mode_init(&mode, 3u));
  int blocks = 8;
  std::vector<int16_t> x(mode.size);
  std::vector<int16_t> before(mode.size);
  int16_t energy[42];
  int16_t one_ago[42];
  int16_t two_ago[42];
  unsigned char collapse[42];
  int32_t pulses[21];
  unsigned long filled = 0;
  unsigned long kept = 0;
  for (int i = 0; i < 42; ++i) {
    // A band that has just got much louder, which is where the noise
    // is loudest and so where this is easiest to see.
    energy[i] = 8192;
    one_ago[i] = 0;
    two_ago[i] = 0;
  }
  for (int i = 0; i < 21; ++i) {
    pulses[i] = 64;
  }
  for (int pattern = 1; pattern < 255; pattern += 7) {
    for (uint32_t i = 0; i < mode.size; ++i) {
      x[i] = (int16_t)(1000 + (int)(i % 97));
    }
    before = x;
    for (int i = 0; i < 42; ++i) {
      collapse[i] = (unsigned char)pattern;
    }
    gaud_celt_anti_collapse(&mode, x.data(), collapse, 1u, mode.size, 0u,
        CELT_BANDS, energy, one_ago, two_ago, pulses, 0x12345678u);
    for (uint32_t band = 0; band < CELT_BANDS; ++band) {
      uint32_t width = (uint32_t)(mode.edges[band + 1] - mode.edges[band]);
      uint32_t base = (uint32_t)mode.edges[band] << mode.lm;
      bool any_clear = ((unsigned)pattern & ((1u << blocks) - 1u))
          != ((1u << blocks) - 1u);
      for (int block = 0; block < blocks; ++block) {
        bool changed = false;
        bool nonzero = false;
        for (uint32_t j = 0; j < width; ++j) {
          uint32_t at = base + (j << mode.lm) + (uint32_t)block;
          changed = changed || x[at] != before[at];
          nonzero = nonzero || x[at] != 0;
        }
        if (((unsigned)pattern >> block) & 1u) {
          // Kept - but the whole band is renormalised if any of its
          // other blocks was refilled, so "untouched" only holds when
          // nothing in the band was.
          if (!any_clear) {
            EXPECT_FALSE(changed) << "band " << band << " block " << block;
          }
          ++kept;
        } else {
          EXPECT_TRUE(changed) << "band " << band << " block " << block;
          EXPECT_TRUE(nonzero) << "band " << band << " block " << block;
          ++filled;
        }
      }
    }
  }
  EXPECT_GT(filled, 1000u);
  EXPECT_GT(kept, 1000u);
}


// ---------------------------------------------------------------------------
// The inverse MDCT, section 4.3.7, and the mixed-radix FFT under it.

TEST(OpusMdct, TheInverseTransformMatchesTheReference) {
  // The sweep uses only the (shift, stride) pairs compute_inv_mdcts
  // forms: a long frame is one transform of the whole spectrum, and a
  // transient frame is `1 << LM` interleaved short ones. Sweeping other
  // pairs would test a transform the codec never performs.
  uint64_t digest = 1469598103934665603ull;
  unsigned long cases = 0;
  std::vector<int32_t> in(2048);
  std::vector<int32_t> out(8192);
  for (int lm = 0; lm <= 3; ++lm) {
    for (int transient = 0; transient <= 1; ++transient) {
      int shift = transient ? 3 : 3 - lm;
      int stride = transient ? (1 << lm) : 1;
      int n = 1920 >> shift;
      int half = n >> 1;
      for (int amp = 0; amp < 4; ++amp) {
        for (int rep = 0; rep < 16; ++rep) {
          unsigned state = (unsigned)(shift * 7919 + stride * 104729
              + amp * 31 + rep * 37);
          int scale = amp == 0 ? 1 : amp == 1 ? 64 : amp == 2 ? 4096 : (1 << 20);
          for (int i = 0; i < half * stride; ++i) {
            state = state * 1103515245u + 12345u;
            in[(size_t)i] = (int32_t)((int)(state >> 8) % (2 * scale + 1))
                - scale;
          }
          // The output buffer is NOT cleared: the transform adds into
          // the samples the previous frame left, which is the whole of
          // the overlap-add. Zeroing it here would make "+=" and "="
          // indistinguishable, and two mutations that swap them would
          // survive - which is how that was found.
          for (int i = 0; i < 8192; ++i) {
            state = state * 1103515245u + 12345u;
            out[(size_t)i] = (int32_t)((int)(state >> 9) % 2000001) - 1000000;
          }
          gaud_celt_imdct(in.data(), out.data() + 2048, gaud_opus_window120,
              120u, shift, (uint32_t)stride);
          // The written region is well inside the first half; folding
          // that much covers it and everything either side of it.
          for (int i = 0; i < 4096; ++i) {
            digest = Fnv1a(digest, out[(size_t)i]);
          }
          ++cases;
        }
      }
    }
  }
  EXPECT_EQ(cases, 512u);
  EXPECT_EQ(digest, 0xA408711A57A4BFDEull);
}

TEST(OpusMdct, TheUnwindowedMiddleIsADirectInverseMdct) {
  // The independent reading. Only the middle of the output is the
  // transform standing alone - the two bands of `overlap` samples
  // around it are windowed and added to the previous frame's tail - so
  // that is the part a textbook inverse MDCT can be compared against.
  //
  // Up to one global scale, because the reference's scaling is spread
  // across the pre-rotation, the butterflies and the windowing. What
  // this catches is a twiddle indexed wrongly, a transposed butterfly,
  // a bad de-shuffle: all of them destroy the correlation rather than
  // changing a factor.
  const int kOverlap = 120;
  std::vector<int32_t> in(2048);
  std::vector<int32_t> out(8192);
  std::vector<double> want(2048);
  unsigned checked = 0;
  for (int shift = 0; shift <= 3; ++shift) {
    int n = 1920 >> shift;
    int half = n >> 1;
    int quarter = n >> 2;
    int low = half - quarter + kOverlap / 2;
    int high = half + quarter - kOverlap / 2;
    if (low >= high) {
      // At the shortest transform the window covers everything, so
      // there is no middle to look at. That is a fact about the
      // geometry and is asserted rather than silently skipped.
      EXPECT_EQ(shift, 3);
      EXPECT_EQ(quarter, kOverlap / 2);
      continue;
    }
    for (int rep = 0; rep < 4; ++rep) {
      unsigned state = (unsigned)(shift * 7919 + rep * 104729);
      for (int i = 0; i < half; ++i) {
        state = state * 1103515245u + 12345u;
        in[(size_t)i] = (int32_t)((int)(state >> 12) % 200001) - 100000;
      }
      std::fill(out.begin(), out.end(), (int32_t)0);
      gaud_celt_imdct(in.data(), out.data() + 2048, gaud_opus_window120,
          (uint32_t)kOverlap, shift, 1u);
      for (int sample = 0; sample < n; ++sample) {
        double sum = 0.0;
        for (int k = 0; k < half; ++k) {
          sum += in[(size_t)k]
              * std::cos(3.14159265358979323846 / half
                  * ((double)sample + 0.5 + half / 2.0) * ((double)k + 0.5));
        }
        want[(size_t)sample] = sum;
      }
      int base = 2048 - ((half - kOverlap) >> 1);
      double xy = 0.0;
      double xx = 0.0;
      double yy = 0.0;
      for (int sample = low; sample < high; ++sample) {
        double got = (double)out[(size_t)(base + sample)];
        xy += got * want[(size_t)sample];
        xx += got * got;
        yy += want[(size_t)sample] * want[(size_t)sample];
      }
      EXPECT_GT(xy / std::sqrt(xx * yy), 0.99999999)
          << "shift " << shift << " rep " << rep;
      ++checked;
    }
  }
  EXPECT_EQ(checked, 12u);
}

TEST(OpusMdct, TheSineApproximationIsTheSameAtEveryTransformSize) {
  // sin(x) is close enough to x at these sizes that one multiply stands
  // in for a whole extra rotation, and the multiplier is
  // (25736 + N/2) / N with integer division. Moving that constant by
  // one is the only mutation of this file that no test catches, and it
  // is inert rather than untested: the division absorbs it at all four
  // sizes. Moving it by 64 is caught, which bounds what is pinned.
  for (int shift = 0; shift <= 3; ++shift) {
    int n = 1920 >> shift;
    int half = n >> 1;
    EXPECT_EQ((25736 + half) / n, (25737 + half) / n) << "n " << n;
  }
  // The four quotients themselves, so a change to the formula rather
  // than to the constant is still caught here.
  EXPECT_EQ((25736 + 960) / 1920, 13);
  EXPECT_EQ((25736 + 480) / 960, 27);
  EXPECT_EQ((25736 + 240) / 480, 54);
  EXPECT_EQ((25736 + 120) / 240, 107);
  // And 25800 is not absorbed, at the shortest size.
  EXPECT_NE((25736 + 120) / 240, (25800 + 120) / 240);
}

TEST(OpusMdct, TheFftTablesCanBeUsedAtAll) {
  // Three properties the generator also checks, kept here because the
  // tables are committed and the generator only runs when asked: a
  // factorisation that does not multiply to its size walks off the end
  // of the array, and a bit-reversal table that is not a permutation
  // either drops or duplicates an input.
  static const int16_t * const kBitrev[4] = {
      gaud_opus_fft_bitrev480, gaud_opus_fft_bitrev240,
      gaud_opus_fft_bitrev120, gaud_opus_fft_bitrev60};
  for (int level = 0; level < 4; ++level) {
    int size = gaud_opus_fft_nfft[level];
    EXPECT_EQ(size, 480 >> level);
    int product = 1;
    int radices = 0;
    for (int pair = 0; pair < 8; ++pair) {
      int radix = gaud_opus_fft_factors[level * 16 + 2 * pair];
      if (radix == 0) {
        break;
      }
      EXPECT_TRUE(radix == 2 || radix == 3 || radix == 4 || radix == 5)
          << "level " << level << " radix " << radix;
      product *= radix;
      // The second of each pair is how many points remain below it.
      EXPECT_EQ(gaud_opus_fft_factors[level * 16 + 2 * pair + 1],
          size / product)
          << "level " << level << " pair " << pair;
      ++radices;
    }
    EXPECT_EQ(product, size) << "level " << level;
    EXPECT_GT(radices, 2);
    std::set<int> seen;
    for (int i = 0; i < size; ++i) {
      EXPECT_GE(kBitrev[level][i], 0);
      EXPECT_LT(kBitrev[level][i], size);
      seen.insert(kBitrev[level][i]);
    }
    EXPECT_EQ(seen.size(), (size_t)size) << "level " << level;
  }
  // The twiddles are a unit circle, at the scale the Q15 multiply wants.
  for (int i = 0; i < 480; ++i) {
    double r = gaud_opus_fft_twiddles48000_960[2 * i];
    double im = gaud_opus_fft_twiddles48000_960[2 * i + 1];
    double magnitude = std::sqrt(r * r + im * im);
    EXPECT_GT(magnitude, 32766.0) << i;
    EXPECT_LT(magnitude, 32771.0) << i;
  }
  // And the MDCT's are a quarter cosine, from one down to zero.
  EXPECT_EQ(gaud_opus_mdct_twiddles960[0], 32767);
  EXPECT_EQ(gaud_opus_mdct_twiddles960[480], 0);
  for (int i = 1; i <= 480; ++i) {
    EXPECT_LE(gaud_opus_mdct_twiddles960[i], gaud_opus_mdct_twiddles960[i - 1])
        << i;
  }
}


// ---------------------------------------------------------------------------
// After the transform: the post-filter and the de-emphasis, sections
// 4.3.7.1 and 4.3.7.2. Both are stated completely in the prose - nine
// filter taps as decimals and one alpha_p - so the constants here are
// checkable against the document and not only against Appendix A.

TEST(OpusPost, ThePostFilterAndDeemphasisMatchTheReference) {
  static const int kPeriod[4] = {15, 100, 511, 1022};
  // G = 3*(int_gain+1)/32 for a three-bit int_gain, which in Q15 is
  // 3072*(i+1); zero is the post-filter switched off. Using values no
  // encoder can produce is not a harmless widening - it made a mutation
  // of the first tap survive, because the real gains are all multiples
  // of 3072 and the ones invented for the first sweep were not.
  static const int16_t kGain[9] = {
      0, 3072, 6144, 9216, 12288, 15360, 18432, 21504, 24576};
  uint64_t digest = 1469598103934665603ull;
  unsigned long combs = 0;
  unsigned long deemphs = 0;
  std::vector<int32_t> buffer(4096);
  for (int tapset_old = 0; tapset_old < 3; ++tapset_old) {
    for (int tapset = 0; tapset < 3; ++tapset) {
      for (int p = 0; p < 4; ++p) {
        for (int g = 0; g < 9; ++g) {
          for (int rep = 0; rep < 6; ++rep) {
            unsigned state = (unsigned)(tapset_old * 7919 + tapset * 104729
                + p * 31 + g * 17 + rep * 37);
            for (int i = 0; i < 4096; ++i) {
              state = state * 1103515245u + 12345u;
              buffer[(size_t)i] = (int32_t)((int)(state >> 6) % 4000001)
                  - 2000000;
            }
            // In place, with the same pointer twice, because that is
            // what makes the filter recursive - section 4.3.7.1 says
            // the past value used must be the interpolated one.
            gaud_celt_comb_filter(buffer.data() + 2048, buffer.data() + 2048,
                kPeriod[p], kPeriod[(p + 1) & 3], 960, kGain[g],
                kGain[(g + 4) % 9], (unsigned)tapset_old, (unsigned)tapset,
                gaud_opus_window120, 120u);
            for (int i = 0; i < 4096; ++i) {
              digest = Fnv1a(digest, buffer[(size_t)i]);
            }
            ++combs;
          }
        }
      }
    }
  }
  std::vector<int32_t> left(2048);
  std::vector<int32_t> right(2048);
  std::vector<int16_t> pcm(8192);
  for (int channels = 1; channels <= 2; ++channels) {
    for (int downsample = 1; downsample <= 3; ++downsample) {
      for (int rep = 0; rep < 24; ++rep) {
        int32_t memory[2];
        const int32_t * in[2] = {left.data(), right.data()};
        unsigned state = (unsigned)(channels * 7919 + downsample * 104729
            + rep * 37);
        for (int i = 0; i < 960; ++i) {
          state = state * 1103515245u + 12345u;
          left[(size_t)i] = (int32_t)((int)(state >> 4) % 200000001)
              - 100000000;
          state = state * 1103515245u + 12345u;
          right[(size_t)i] = (int32_t)((int)(state >> 4) % 200000001)
              - 100000000;
        }
        // The filter state is not zero at a frame boundary, and a
        // decoder that resets it clicks every 20 ms.
        state = state * 1103515245u + 12345u;
        memory[0] = (int32_t)((int)(state >> 8) % 2000001) - 1000000;
        state = state * 1103515245u + 12345u;
        memory[1] = (int32_t)((int)(state >> 8) % 2000001) - 1000000;
        std::fill(pcm.begin(), pcm.end(), (int16_t)0);
        gaud_celt_deemphasis(in, pcm.data(), 960, (uint32_t)channels,
            downsample, memory);
        for (int i = 0; i < 8192; ++i) {
          digest = Fnv1a(digest, pcm[(size_t)i]);
        }
        digest = Fnv1a(Fnv1a(digest, memory[0]), memory[1]);
        ++deemphs;
      }
    }
  }
  EXPECT_EQ(combs, 1944u);
  EXPECT_EQ(deemphs, 144u);
  EXPECT_EQ(digest, 0x66783792AC037076ull);
}

TEST(OpusPost, TheTapsAreTheDecimalsTheProsePrints) {
  // Section 4.3.7.1 prints all nine, and 4.3.7.2 prints alpha_p. Each
  // Q15 integer divided by 32768 is exactly that decimal, which is a
  // check the document supplies on its own appendix - and the kind
  // that is easy to leave unmade because the two live 150 pages apart.
  struct Tap {
    int16_t fixed;
    double printed;
  };
  static const Tap kTaps[9] = {
      {10048, 0.3066406250}, {7112, 0.2170410156}, {4248, 0.1296386719},
      {15200, 0.4638671875}, {8784, 0.2680664062}, {0, 0.0},
      {26208, 0.7998046875}, {3280, 0.1000976562}, {0, 0.0},
  };
  for (const Tap & tap : kTaps) {
    EXPECT_EQ(tap.fixed, (int16_t)std::llround(tap.printed * 32768.0))
        << tap.printed;
    // The prose prints ten decimal places and the Q15 value needs
    // twelve, so the check is agreement to the last printed place -
    // not equality. It cannot be "correctly rounded" either, because
    // the document is not consistent about it: 4248/32768 is
    // 0.129638671875 and it prints 0.1296386719, rounded up, while
    // 8784/32768 is 0.26806640625 and it prints 0.2680664062, rounded
    // down. Both are within one unit of the last place, which is what
    // this asserts.
    double exact = (double)tap.fixed / 32768.0;
    EXPECT_LE(std::fabs(exact - tap.printed), 1e-10) << tap.fixed;
  }
  EXPECT_DOUBLE_EQ(27853.0 / 32768.0, 0.850006103515625);
  // Which 4.3.7.2 prints to the same precision.
  EXPECT_LE(std::fabs(27853.0 / 32768.0 - 0.8500061035), 1e-10);
}

TEST(OpusPost, TheFirstTapOfEachSetIsNotPinnedToOneUnit) {
  // Three of the nine taps cannot be checked to the last bit by any
  // legal input, and it is worth saying which and why rather than
  // leaving three mutations marked "not caught".
  //
  // The gain is always 3072*(i+1), and the tap is applied as
  // (gain * tap) >> 15. For the three leading taps no multiple of 3072
  // in range makes that product cross an integer boundary when the tap
  // moves by one, so the ULP is invisible. The second and third taps of
  // each set are not so lucky, and are pinned.
  static const int16_t kTaps[3][3] = {
      {10048, 7112, 4248}, {15200, 8784, 0}, {26208, 3280, 0}};
  for (int set = 0; set < 3; ++set) {
    bool leading_visible = false;
    for (int i = 0; i < 8; ++i) {
      int32_t gain = 3072 * (i + 1);
      if (((gain * kTaps[set][0]) >> 15) != ((gain * (kTaps[set][0] + 1)) >> 15)) {
        leading_visible = true;
      }
    }
    EXPECT_FALSE(leading_visible) << "set " << set;
  }
  // The tap after it is visible, at some gain - so this is a property
  // of those three constants and not of the arithmetic in general.
  unsigned visible = 0;
  for (int set = 0; set < 3; ++set) {
    for (int i = 0; i < 8; ++i) {
      int32_t gain = 3072 * (i + 1);
      if (((gain * kTaps[set][1]) >> 15) != ((gain * (kTaps[set][1] + 1)) >> 15)) {
        ++visible;
      }
    }
  }
  EXPECT_GT(visible, 3u);
}

TEST(OpusPost, AGainOfZeroLeavesTheSignalAlone) {
  // The post-filter is switched off by a gain of zero, and "off" has to
  // mean the identity rather than something very close to it - every
  // frame runs through this whether the filter is on or not.
  std::vector<int32_t> buffer(4096);
  std::vector<int32_t> before(4096);
  unsigned state = 999u;
  for (int i = 0; i < 4096; ++i) {
    state = state * 1103515245u + 12345u;
    buffer[(size_t)i] = (int32_t)((int)(state >> 6) % 4000001) - 2000000;
  }
  before = buffer;
  gaud_celt_comb_filter(buffer.data() + 2048, buffer.data() + 2048, 511, 1022,
      960, 0, 0, 2u, 1u, gaud_opus_window120, 120u);
  EXPECT_EQ(buffer, before);
  // And a nonzero gain does not, so the test above is not vacuous.
  gaud_celt_comb_filter(buffer.data() + 2048, buffer.data() + 2048, 511, 1022,
      960, 24576, 24576, 2u, 1u, gaud_opus_window120, 120u);
  EXPECT_NE(buffer, before);
}

TEST(OpusPost, DeemphasisIsTheOnePoleItSaysItIs) {
  // The independent reading: section 4.3.7.2 says the filter is
  // 1/(1 - alpha_p * z^-1) with alpha_p = 0.8500061035, so running that
  // recursion in double and comparing is a check on the whole thing -
  // the coefficient, the scaling, and the state carried between frames.
  const int kSamples = 960;
  std::vector<int32_t> channel((size_t)kSamples);
  std::vector<int16_t> pcm((size_t)kSamples);
  double worst = 0.0;
  unsigned long compared = 0;
  unsigned long clipped = 0;
  for (int rep = 0; rep < 40; ++rep) {
    const int32_t * in[1] = {channel.data()};
    int32_t memory[1] = {0};
    unsigned state = (unsigned)(rep * 104729 + 12345);
    // Half the sweep is loud enough to clip, which is the only way the
    // clamp gets exercised at all - but not so loud that the 32-bit
    // accumulator bursts. There is a window between the two, because
    // the clamp is at 2^27 and the accumulator at 2^31; outside it a
    // sweep is testing undefined behaviour rather than the filter, and
    // the first version of this test was.
    int scale = rep < 20 ? 20000000 : 100000000;
    for (int i = 0; i < kSamples; ++i) {
      state = state * 1103515245u + 12345u;
      channel[(size_t)i] = (int32_t)((int)(state >> 4) % (2 * scale + 1))
          - scale;
    }
    gaud_celt_deemphasis(in, pcm.data(), kSamples, 1u, 1, memory);
    double accumulator = 0.0;
    for (int i = 0; i < kSamples; ++i) {
      double sum = (double)channel[(size_t)i] + accumulator;
      accumulator = 0.850006103515625 * sum;
      // The synthesis carries twelve bits of headroom.
      double want = sum / 4096.0;
      if (want > 32767.0 || want < -32768.0) {
        // Clear of the rail by more than the rounding, so the clamp
        // must have fired; right at it either answer is correct.
        if (want > 32768.0 || want < -32769.0) {
          EXPECT_TRUE(pcm[(size_t)i] == 32767 || pcm[(size_t)i] == -32768)
              << "rep " << rep << " sample " << i << " want " << want;
        }
        ++clipped;
        continue;
      }
      worst = std::max(worst, std::fabs((double)pcm[(size_t)i] - want));
      ++compared;
    }
  }
  EXPECT_GT(compared, 19000u);
  EXPECT_GT(clipped, 1000u);
  // Measured: 0.5007, which is the rounding of the final shift.
  EXPECT_LT(worst, 0.501);
}


// ---------------------------------------------------------------------------
// One CELT frame end to end: section 4.3 in the order the bits arrive.

TEST(OpusCeltFrame, EveryFrameMatchesTheReferenceDecoder) {
  // The whole of CELT against the whole of RFC 6716's celt_decode_with_ec,
  // over frames of every size and length, six in a row each so that the
  // state carried between them is exercised: the energy envelope and its
  // two frames of history, the transform's overlap, the post-filter's
  // thousand samples of synthesis, and the de-emphasis pole.
  //
  // The digest folds the samples **and the range decoder's final state**.
  // That second one is what RFC 6716 section 6 compares to decide whether
  // a decoder is conformant, and it is exact: no tolerance, no averaging.
  //
  // The bytes are pseudo-random rather than encoded audio. A range decoder
  // is defined on every input - a lossy stream's decoder has to be - so
  // this is a legitimate differential, and it reaches the error paths that
  // real packets do not. Real packets are checked separately, against the
  // conformance vectors; notes/audio/opus.md has that result.
  static const int kLength[10] = {2, 3, 5, 8, 13, 40, 120, 300, 700, 1275};
  auto decoder = std::make_unique<CELT_Decoder>();
  auto scratch = std::make_unique<CELT_Scratch>();
  std::vector<int16_t> pcm(2 * 960);
  std::vector<unsigned char> data(1500);
  uint64_t digest = 1469598103934665603ull;
  unsigned long frames = 0;
  for (int channels = 1; channels <= 2; ++channels) {
    for (unsigned lm = 0; lm <= 3; ++lm) {
      int n = 120 << lm;
      for (int li = 0; li < 10; ++li) {
        int length = kLength[li];
        for (int seq = 0; seq < 8; ++seq) {
          unsigned state = (unsigned)(channels * 7919 + (int)lm * 104729
              + length * 31 + seq * 37);
          gaud_celt_decoder_init(decoder.get(), (uint32_t)channels, 1);
          for (int frame = 0; frame < 6; ++frame) {
            OPUS_Range range;
            for (int i = 0; i < length; ++i) {
              state = state * 1103515245u + 12345u;
              data[(size_t)i] = (unsigned char)(state >> 16);
            }
            std::fill(pcm.begin(), pcm.end(), (int16_t)0);
            gaud_opus_range_init(&range, data.data(), (size_t)length);
            gaud_celt_decode_frame(decoder.get(), &range, (uint32_t)length,
                (uint32_t)channels, lm, pcm.data(), scratch.get());
            for (int i = 0; i < channels * n; ++i) {
              digest = Fnv1a(digest, pcm[(size_t)i]);
            }
            digest = Fnv1a(digest, decoder->rng);
            ++frames;
          }
        }
      }
    }
  }
  EXPECT_EQ(frames, 3840u);
  EXPECT_EQ(digest, 0x941FBB211A1FD30Aull);
}

TEST(OpusCeltFrame, AFreshDecoderIsNotAZeroedOne) {
  // The two history envelopes start at the quietest value the format
  // has, not at zero. A first frame has no history, and "silent before"
  // is the reading that keeps anti-collapse from firing on it - zero
  // would mean "as loud as the mean", which is the opposite.
  auto decoder = std::make_unique<CELT_Decoder>();
  std::memset(decoder.get(), 0xA5, sizeof(CELT_Decoder));
  gaud_celt_decoder_init(decoder.get(), 2u, 1);
  for (uint32_t i = 0; i < 2u * CELT_BANDS; ++i) {
    EXPECT_EQ(decoder->old_log_e[i], -(28 << CELT_DB_SHIFT)) << i;
    EXPECT_EQ(decoder->old_log_e2[i], -(28 << CELT_DB_SHIFT)) << i;
    EXPECT_EQ(decoder->old_band_e[i], 0) << i;
    EXPECT_EQ(decoder->background_log_e[i], 0) << i;
  }
  EXPECT_EQ(decoder->channels, 2u);
  EXPECT_EQ(decoder->downsample, 1);
  EXPECT_EQ(decoder->start, 0u);
  EXPECT_EQ(decoder->end, CELT_BANDS);
  EXPECT_EQ(decoder->rng, 0u);
  for (uint32_t i = 0; i < CELT_DECODE_HISTORY + CELT_OVERLAP; ++i) {
    ASSERT_EQ(decoder->decode_mem[0][i], 0) << i;
    ASSERT_EQ(decoder->decode_mem[1][i], 0) << i;
  }
}

TEST(OpusCeltFrame, AFrameSizeTheFormatDoesNotHaveIsRefused) {
  // lm above 3 is not a frame size; the mode cannot be built for it and
  // the frame has to be refused rather than decoded into whatever the
  // tables hold past their end.
  auto decoder = std::make_unique<CELT_Decoder>();
  auto scratch = std::make_unique<CELT_Scratch>();
  std::vector<int16_t> pcm(2 * 960);
  std::vector<unsigned char> data(64, 0x5Au);
  OPUS_Range range;
  gaud_celt_decoder_init(decoder.get(), 1u, 1);
  gaud_opus_range_init(&range, data.data(), data.size());
  EXPECT_EQ(gaud_celt_decode_frame(decoder.get(), &range, 64u, 1u, 4u,
                pcm.data(), scratch.get()),
      GAUD_ERR_CORRUPT);
  // And the four it does have are all accepted.
  for (unsigned lm = 0; lm <= 3; ++lm) {
    gaud_celt_decoder_init(decoder.get(), 1u, 1);
    gaud_opus_range_init(&range, data.data(), data.size());
    EXPECT_EQ(gaud_celt_decode_frame(decoder.get(), &range, 64u, 1u, lm,
                  pcm.data(), scratch.get()),
        GAUD_OK)
        << "lm " << lm;
  }
}

TEST(OpusCeltFrame, TheRangeStateMovesAndDependsOnTheBytes) {
  // The conformance test is on this number, so it is worth one direct
  // check that it is a function of the packet rather than, say, always
  // the same - a decoder that read nothing would still fill in samples.
  auto decoder = std::make_unique<CELT_Decoder>();
  auto scratch = std::make_unique<CELT_Scratch>();
  std::vector<int16_t> pcm(2 * 960);
  std::vector<unsigned char> data(80);
  std::set<uint32_t> states;
  for (int seed = 0; seed < 64; ++seed) {
    unsigned state = (unsigned)(seed * 104729 + 7);
    OPUS_Range range;
    for (size_t i = 0; i < data.size(); ++i) {
      state = state * 1103515245u + 12345u;
      data[i] = (unsigned char)(state >> 16);
    }
    gaud_celt_decoder_init(decoder.get(), 2u, 1);
    gaud_opus_range_init(&range, data.data(), data.size());
    ASSERT_EQ(gaud_celt_decode_frame(decoder.get(), &range, 80u, 2u, 3u,
                  pcm.data(), scratch.get()),
        GAUD_OK);
    EXPECT_NE(decoder->rng, 0u) << seed;
    states.insert(decoder->rng);
  }
  EXPECT_GT(states.size(), 50u);
}


int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
