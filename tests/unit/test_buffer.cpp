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
 * Sample formats, channel layouts, and the buffer.
 */

#include <ghoti.io/audio/audio.h>
// gaud_frame_size lives in the codec SDK, which a test is entitled to
// reach: the SDK is public API and is as much under test as the rest.
#include <ghoti.io/audio/codec_sdk.h>
#include <gtest/gtest.h>
#include <string.h>

TEST(SampleFormat, PcmPredicateSeparatesTheTwoThatAreNot) {
  // ops.h guards every entry point on this, so a format landing on the
  // wrong side of it would make arithmetic run over coded bytes.
  EXPECT_TRUE(gaud_sample_format_is_pcm(GAUD_SAMPLE_U8));
  EXPECT_TRUE(gaud_sample_format_is_pcm(GAUD_SAMPLE_S8));
  EXPECT_TRUE(gaud_sample_format_is_pcm(GAUD_SAMPLE_S16));
  EXPECT_TRUE(gaud_sample_format_is_pcm(GAUD_SAMPLE_S24));
  EXPECT_TRUE(gaud_sample_format_is_pcm(GAUD_SAMPLE_S32));
  EXPECT_TRUE(gaud_sample_format_is_pcm(GAUD_SAMPLE_F32));
  EXPECT_TRUE(gaud_sample_format_is_pcm(GAUD_SAMPLE_F64));
  EXPECT_FALSE(gaud_sample_format_is_pcm(GAUD_SAMPLE_DSD1));
  EXPECT_FALSE(gaud_sample_format_is_pcm(GAUD_SAMPLE_OPAQUE));
  EXPECT_FALSE(gaud_sample_format_is_pcm(
      static_cast<GAUD_Sample_Format>(GAUD_SAMPLE_FORMAT_COUNT)));
}

TEST(SampleFormat, EveryFormatHasBitsAndAName) {
  for (int i = 0; i < GAUD_SAMPLE_FORMAT_COUNT; ++i) {
    auto f = static_cast<GAUD_Sample_Format>(i);
    EXPECT_GT(gaud_sample_format_bits(f), 0u) << i;
    EXPECT_STRNE(gaud_sample_format_string(f), "unknown") << i;
  }
  EXPECT_EQ(gaud_sample_format_bits(
                static_cast<GAUD_Sample_Format>(GAUD_SAMPLE_FORMAT_COUNT)),
      0u);
}

TEST(SampleFormat, OnlyTheTwoFloatsAreFloat) {
  EXPECT_TRUE(gaud_sample_format_is_float(GAUD_SAMPLE_F32));
  EXPECT_TRUE(gaud_sample_format_is_float(GAUD_SAMPLE_F64));
  EXPECT_FALSE(gaud_sample_format_is_float(GAUD_SAMPLE_S32));
  EXPECT_FALSE(gaud_sample_format_is_float(GAUD_SAMPLE_DSD1));
}

TEST(ChannelLayout, UnstatedIsNotTheSameAsMono) {
  // The distinction GAUD_Channel_Layout exists for. A file that said
  // nothing and a file that said "one centre channel" are different facts,
  // and a caller has to be able to tell them apart.
  GAUD_Channel_Layout unstated = gaud_channel_layout_unspecified(1);
  GAUD_Channel_Layout mono = gaud_channel_layout_default(1);
  EXPECT_EQ(unstated.mask, 0u);
  EXPECT_EQ(mono.mask, static_cast<uint32_t>(GAUD_CH_FRONT_CENTER));
  EXPECT_EQ(unstated.channels, mono.channels);
  EXPECT_TRUE(gaud_channel_layout_valid(unstated));
  EXPECT_TRUE(gaud_channel_layout_valid(mono));
}

TEST(ChannelLayout, FivePointOneNamesTheLfe) {
  // The defect this type exists to prevent: a 5.1 file whose centre and LFE
  // are swapped sounds wrong, is attributable to nothing, and passes every
  // test that compares total energy.
  GAUD_Channel_Layout layout = gaud_channel_layout_default(6);
  ASSERT_TRUE(gaud_channel_layout_valid(layout));
  EXPECT_EQ(gaud_channel_layout_at(layout, 0), GAUD_CH_FRONT_LEFT);
  EXPECT_EQ(gaud_channel_layout_at(layout, 1), GAUD_CH_FRONT_RIGHT);
  EXPECT_EQ(gaud_channel_layout_at(layout, 2), GAUD_CH_FRONT_CENTER);
  EXPECT_EQ(gaud_channel_layout_at(layout, 3), GAUD_CH_LOW_FREQUENCY);
  EXPECT_EQ(gaud_channel_layout_at(layout, 4), GAUD_CH_BACK_LEFT);
  EXPECT_EQ(gaud_channel_layout_at(layout, 5), GAUD_CH_BACK_RIGHT);
}

TEST(ChannelLayout, AMaskDisagreeingWithTheCountIsInvalid) {
  GAUD_Channel_Layout bad = {GAUD_CH_FRONT_LEFT | GAUD_CH_FRONT_RIGHT, 6};
  EXPECT_FALSE(gaud_channel_layout_valid(bad));
  GAUD_Channel_Layout zero = {0, 0};
  EXPECT_FALSE(gaud_channel_layout_valid(zero)) << "no channels at all";
}

TEST(ChannelLayout, AnUnconventionalCountGetsNoInventedMask) {
  // Stating a mask for a count with no convention would be asserting
  // something the format never said.
  GAUD_Channel_Layout nine = gaud_channel_layout_default(9);
  EXPECT_EQ(nine.mask, 0u);
  EXPECT_EQ(nine.channels, 9u);
  EXPECT_EQ(gaud_channel_layout_at(nine, 0), 0);
}

TEST(ChannelLayout, EveryPositionHasAName) {
  const GAUD_Channel all[] = {GAUD_CH_FRONT_LEFT, GAUD_CH_FRONT_RIGHT,
      GAUD_CH_FRONT_CENTER, GAUD_CH_LOW_FREQUENCY, GAUD_CH_BACK_LEFT,
      GAUD_CH_BACK_RIGHT, GAUD_CH_FRONT_LEFT_OF_CENTER,
      GAUD_CH_FRONT_RIGHT_OF_CENTER, GAUD_CH_BACK_CENTER, GAUD_CH_SIDE_LEFT,
      GAUD_CH_SIDE_RIGHT, GAUD_CH_TOP_CENTER, GAUD_CH_TOP_FRONT_LEFT,
      GAUD_CH_TOP_FRONT_CENTER, GAUD_CH_TOP_FRONT_RIGHT,
      GAUD_CH_TOP_BACK_LEFT, GAUD_CH_TOP_BACK_CENTER, GAUD_CH_TOP_BACK_RIGHT};
  for (GAUD_Channel c : all) {
    EXPECT_STRNE(gaud_channel_string(c), "?") << c;
  }
}

namespace {
GAUD_Buffer * Make(GAUD_Sample_Format format, uint32_t channels,
    size_t frames, GAUD_Sample_Layout sl = GAUD_LAYOUT_INTERLEAVED) {
  GAUD_Buffer * b = nullptr;
  EXPECT_EQ(gaud_buffer_create(nullptr, format,
                gaud_channel_layout_default(channels), sl, frames, &b),
      GAUD_OK);
  return b;
}
} // namespace

TEST(Buffer, ReportsWhatItWasMadeWith) {
  GAUD_Buffer * b = Make(GAUD_SAMPLE_S16, 2, 100);
  EXPECT_EQ(gaud_buffer_capacity(b), 100u);
  EXPECT_EQ(gaud_buffer_frames(b), 0u) << "nothing is meaningful yet";
  EXPECT_EQ(gaud_buffer_format(b), GAUD_SAMPLE_S16);
  EXPECT_EQ(gaud_buffer_channels(b), 2u);
  EXPECT_EQ(gaud_buffer_frame_size(b), 4u);
  EXPECT_EQ(gaud_buffer_size_bytes(b), 400u);
  EXPECT_EQ(gaud_buffer_sample_layout(b), GAUD_LAYOUT_INTERLEAVED);
  gaud_buffer_destroy(b);
}

TEST(Buffer, AFreshBufferIsSilenceRatherThanZeroedBytes) {
  // For u8 these differ: silence is 128, and a buffer handed on after a
  // short read would otherwise carry a tail of full-negative samples.
  GAUD_Buffer * b = Make(GAUD_SAMPLE_U8, 1, 8);
  const unsigned char * p
      = static_cast<const unsigned char *>(gaud_buffer_data_const(b));
  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(p[i], 0x80u) << i;
  }
  gaud_buffer_destroy(b);

  GAUD_Buffer * s = Make(GAUD_SAMPLE_S16, 1, 8);
  const unsigned char * q
      = static_cast<const unsigned char *>(gaud_buffer_data_const(s));
  for (size_t i = 0; i < 16; ++i) {
    EXPECT_EQ(q[i], 0x00u) << i;
  }
  gaud_buffer_destroy(s);
}

TEST(Buffer, FrameCountIsCappedByTheCapacity) {
  GAUD_Buffer * b = Make(GAUD_SAMPLE_S16, 2, 10);
  EXPECT_EQ(gaud_buffer_set_frames(b, 10), GAUD_OK);
  EXPECT_EQ(gaud_buffer_frames(b), 10u);
  EXPECT_EQ(gaud_buffer_set_frames(b, 11), GAUD_ERR_INVALID);
  EXPECT_EQ(gaud_buffer_frames(b), 10u) << "a refused set changed it";
  gaud_buffer_destroy(b);
}

TEST(Buffer, BytesUsedFollowsTheFrameCountAndNotTheCapacity) {
  // What a caller writing a decoded block out needs; size_bytes would write
  // the unused tail as well.
  GAUD_Buffer * b = Make(GAUD_SAMPLE_S16, 2, 100);
  gaud_buffer_set_frames(b, 7);
  EXPECT_EQ(gaud_buffer_bytes_used(b), 28u);
  EXPECT_EQ(gaud_buffer_size_bytes(b), 400u);
  gaud_buffer_destroy(b);
}

TEST(Buffer, InterleavedAndPlanarAddressDifferently) {
  GAUD_Buffer * i = Make(GAUD_SAMPLE_S16, 2, 4, GAUD_LAYOUT_INTERLEAVED);
  // Frame-major: L R L R.
  EXPECT_EQ(gaud_buffer_offset(i, 0, 0), 0u);
  EXPECT_EQ(gaud_buffer_offset(i, 0, 1), 2u);
  EXPECT_EQ(gaud_buffer_offset(i, 1, 0), 4u);
  gaud_buffer_destroy(i);

  GAUD_Buffer * p = Make(GAUD_SAMPLE_S16, 2, 4, GAUD_LAYOUT_PLANAR);
  // Channel-major: L L L L R R R R.
  EXPECT_EQ(gaud_buffer_offset(p, 0, 0), 0u);
  EXPECT_EQ(gaud_buffer_offset(p, 1, 0), 2u);
  EXPECT_EQ(gaud_buffer_offset(p, 0, 1), 8u);
  gaud_buffer_destroy(p);
}

TEST(Buffer, OffsetRefusesOutOfRange) {
  GAUD_Buffer * b = Make(GAUD_SAMPLE_S16, 2, 4);
  EXPECT_EQ(gaud_buffer_offset(b, 4, 0), static_cast<size_t>(-1));
  EXPECT_EQ(gaud_buffer_offset(b, 0, 2), static_cast<size_t>(-1));
  EXPECT_EQ(gaud_buffer_offset(nullptr, 0, 0), static_cast<size_t>(-1));
  gaud_buffer_destroy(b);
}

TEST(Buffer, TwentyFourBitIsThreePackedBytes) {
  // Not a 32-bit value with a spare byte: that is what the file holds, and
  // storing it padded would stop a 24-bit file round-tripping byte for byte.
  EXPECT_EQ(gaud_frame_size(GAUD_SAMPLE_S24, 1), 3u);
  EXPECT_EQ(gaud_frame_size(GAUD_SAMPLE_S24, 2), 6u);
  GAUD_Buffer * b = Make(GAUD_SAMPLE_S24, 2, 10);
  EXPECT_EQ(gaud_buffer_size_bytes(b), 60u);
  EXPECT_EQ(gaud_buffer_offset(b, 1, 0), 6u);
  gaud_buffer_destroy(b);
}

TEST(Buffer, NonPcmFormatsAreSizedTheirOwnWay) {
  // DSD is one bit per sample and an opaque buffer is a byte count with no
  // channel dimension. gaud_frame_size answers 0 for both, deliberately.
  EXPECT_EQ(gaud_frame_size(GAUD_SAMPLE_DSD1, 2), 0u);
  EXPECT_EQ(gaud_frame_size(GAUD_SAMPLE_OPAQUE, 1), 0u);

  GAUD_Buffer * d = Make(GAUD_SAMPLE_DSD1, 2, 64);
  EXPECT_EQ(gaud_buffer_size_bytes(d), 16u) << "64 frames x 2 ch / 8 bits";
  EXPECT_EQ(gaud_buffer_offset(d, 0, 0), static_cast<size_t>(-1))
      << "not byte-addressable";
  gaud_buffer_destroy(d);

  GAUD_Buffer * o = Make(GAUD_SAMPLE_OPAQUE, 2, 100);
  EXPECT_EQ(gaud_buffer_size_bytes(o), 100u) << "a byte count, not per channel";
  EXPECT_EQ(gaud_buffer_offset(o, 5, 0), 5u);
  gaud_buffer_destroy(o);
}

TEST(Buffer, DsdSilenceIsTheIdlePattern) {
  // 0x69 rather than 0x00: an all-zero DSD stream is full-negative DC, not
  // silence.
  GAUD_Buffer * d = Make(GAUD_SAMPLE_DSD1, 1, 64);
  const unsigned char * p
      = static_cast<const unsigned char *>(gaud_buffer_data_const(d));
  EXPECT_EQ(p[0], 0x69u);
  gaud_buffer_destroy(d);
}

TEST(Buffer, CreateRefusesNonsense) {
  GAUD_Buffer * b = nullptr;
  GAUD_Channel_Layout ok = gaud_channel_layout_default(2);
  EXPECT_EQ(gaud_buffer_create(nullptr, GAUD_SAMPLE_S16, ok,
                GAUD_LAYOUT_INTERLEAVED, 4, nullptr),
      GAUD_ERR_INVALID);
  GAUD_Channel_Layout none = {0, 0};
  EXPECT_EQ(gaud_buffer_create(nullptr, GAUD_SAMPLE_S16, none,
                GAUD_LAYOUT_INTERLEAVED, 4, &b),
      GAUD_ERR_INVALID);
  EXPECT_EQ(gaud_buffer_create(nullptr,
                static_cast<GAUD_Sample_Format>(GAUD_SAMPLE_FORMAT_COUNT), ok,
                GAUD_LAYOUT_INTERLEAVED, 4, &b),
      GAUD_ERR_INVALID);
  EXPECT_EQ(gaud_buffer_create(nullptr, GAUD_SAMPLE_S16, ok,
                static_cast<GAUD_Sample_Layout>(9), 4, &b),
      GAUD_ERR_INVALID);
}

TEST(Buffer, AnEnormousRequestIsRefusedRatherThanOverflowing) {
  // The whole reason this is not a malloc the caller does themselves.
  GAUD_Buffer * b = nullptr;
  EXPECT_EQ(gaud_buffer_create(nullptr, GAUD_SAMPLE_F64,
                gaud_channel_layout_default(8), GAUD_LAYOUT_INTERLEAVED,
                SIZE_MAX / 8, &b),
      GAUD_ERR_OOM);
}

TEST(Buffer, ZeroFramesIsValid) {
  GAUD_Buffer * b = Make(GAUD_SAMPLE_S16, 2, 0);
  EXPECT_EQ(gaud_buffer_capacity(b), 0u);
  EXPECT_EQ(gaud_buffer_size_bytes(b), 0u);
  EXPECT_EQ(gaud_buffer_data(b), nullptr);
  gaud_buffer_destroy(b);
}

TEST(Buffer, SilenceResetsBothTheBytesAndTheCount) {
  GAUD_Buffer * b = Make(GAUD_SAMPLE_U8, 1, 4);
  memset(gaud_buffer_data(b), 0x00, 4);
  gaud_buffer_set_frames(b, 4);
  gaud_buffer_silence(b);
  EXPECT_EQ(gaud_buffer_frames(b), 0u);
  EXPECT_EQ(static_cast<const unsigned char *>(gaud_buffer_data_const(b))[0],
      0x80u);
  gaud_buffer_destroy(b);
}

TEST(Buffer, AccessorsRefuseNullSafely) {
  EXPECT_EQ(gaud_buffer_capacity(nullptr), 0u);
  EXPECT_EQ(gaud_buffer_frames(nullptr), 0u);
  EXPECT_EQ(gaud_buffer_channels(nullptr), 0u);
  EXPECT_EQ(gaud_buffer_data(nullptr), nullptr);
  EXPECT_EQ(gaud_buffer_data_const(nullptr), nullptr);
  EXPECT_EQ(gaud_buffer_size_bytes(nullptr), 0u);
  EXPECT_EQ(gaud_buffer_frame_size(nullptr), 0u);
  EXPECT_EQ(gaud_buffer_bytes_used(nullptr), 0u);
  EXPECT_EQ(gaud_buffer_set_frames(nullptr, 0), GAUD_ERR_INVALID);
  gaud_buffer_destroy(nullptr);
  gaud_buffer_silence(nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
