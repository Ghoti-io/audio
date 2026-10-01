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

TEST(VorbisLoad, AskingForADecoderIsRefusedAndTheCapabilityBitSaysWhy) {
  /*
   * planning/audio.md section 11.18's argument, as an assertion. The
   * identification half shipping on its own is only honest if a program
   * can find out: a caller asks the codec what it can do, and asks the
   * track for a decoder, and both answers agree.
   *
   * **This test is expected to be deleted.** When the decoder lands,
   * GAUD_CAP_DECODE is declared and gaud_decoder_create() succeeds, and
   * this becomes the test that must be removed rather than relaxed.
   */
  const GAUD_Codec * codec = gaud_registry_find(NULL, "vorbis");
  ASSERT_NE(codec, nullptr);
  EXPECT_TRUE((codec->capabilities & GAUD_CAP_METADATA_READ) != 0);
  EXPECT_FALSE((codec->capabilities & GAUD_CAP_DECODE) != 0);

  Loaded loaded;
  ASSERT_EQ(OpenFile(loaded, "vorbis_lib_stereo_44100.ogg"), GAUD_OK);
  GAUD_Decoder * decoder = nullptr;
  EXPECT_EQ(gaud_decoder_create(loaded.track(), &decoder),
      GAUD_ERR_UNSUPPORTED);
  EXPECT_EQ(decoder, nullptr);
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
