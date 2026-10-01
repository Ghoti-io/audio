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
