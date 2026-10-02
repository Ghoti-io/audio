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
#include "../../src/codec/opus/opus_tables.h"
#include <ghoti.io/audio/audio.h>
#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/audio/codecs.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
