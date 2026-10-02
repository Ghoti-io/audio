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
#include <ghoti.io/audio/audio.h>
#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/audio/codecs.h>
#include <gtest/gtest.h>
#include <algorithm>
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
