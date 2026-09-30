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
 * Sample-format conversion, dither, and the three measurements the decode
 * gate rests on.
 */

#include <ghoti.io/audio/audio.h>
#include <gtest/gtest.h>
#include <math.h>
#include <string.h>

namespace {

GAUD_Buffer * Filled(GAUD_Sample_Format format, uint32_t channels,
    size_t frames, double value) {
  GAUD_Buffer * b = nullptr;
  EXPECT_EQ(gaud_buffer_create(nullptr, format,
                gaud_channel_layout_default(channels),
                GAUD_LAYOUT_INTERLEAVED, frames, &b),
      GAUD_OK);
  for (size_t f = 0; f < frames; ++f) {
    for (uint32_t c = 0; c < channels; ++c) {
      unsigned char * p = static_cast<unsigned char *>(gaud_buffer_data(b))
          + gaud_buffer_offset(b, f, c);
      switch (format) {
      case GAUD_SAMPLE_U8: p[0] = (unsigned char)(value * 127.0 + 128.0); break;
      case GAUD_SAMPLE_S8: p[0] = (unsigned char)(signed char)(value * 127.0);
        break;
      case GAUD_SAMPLE_S16: {
        int16_t v = (int16_t)(value * 32767.0);
        memcpy(p, &v, 2);
        break;
      }
      case GAUD_SAMPLE_S24: {
        int32_t v = (int32_t)(value * 8388607.0);
        p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF;
        break;
      }
      case GAUD_SAMPLE_S32: {
        int32_t v = (int32_t)(value * 2147483000.0);
        memcpy(p, &v, 4);
        break;
      }
      case GAUD_SAMPLE_F32: {
        float v = (float)value;
        memcpy(p, &v, 4);
        break;
      }
      case GAUD_SAMPLE_F64: memcpy(p, &value, 8); break;
      default: break;
      }
    }
  }
  gaud_buffer_set_frames(b, frames);
  return b;
}

} // namespace

TEST(Convert, WideningIsLosslessAndRoundTripsBack) {
  // s16 -> s32 -> s16 must return the original exactly. Anything else means
  // the scaling is not by full scale.
  GAUD_Buffer * src = Filled(GAUD_SAMPLE_S16, 2, 64, 0.5);
  GAUD_Buffer * wide = nullptr;
  ASSERT_EQ(gaud_ops_convert_format(nullptr, src, GAUD_SAMPLE_S32, nullptr,
                &wide),
      GAUD_OK);
  GAUD_Convert_Options opts;
  gaud_convert_options_default(&opts);
  opts.dither = GAUD_DITHER_NONE; // or the noise would move a sample
  GAUD_Buffer * back = nullptr;
  ASSERT_EQ(gaud_ops_convert_format(nullptr, wide, GAUD_SAMPLE_S16, &opts,
                &back),
      GAUD_OK);
  EXPECT_EQ(memcmp(gaud_buffer_data_const(src), gaud_buffer_data_const(back),
                gaud_buffer_size_bytes(src)),
      0);
  gaud_buffer_destroy(src);
  gaud_buffer_destroy(wide);
  gaud_buffer_destroy(back);
}

TEST(Convert, TheFrameCountCarriesOverRatherThanTheCapacity) {
  GAUD_Buffer * src = Filled(GAUD_SAMPLE_S16, 2, 64, 0.25);
  gaud_buffer_set_frames(src, 10);
  GAUD_Buffer * out = nullptr;
  ASSERT_EQ(
      gaud_ops_convert_format(nullptr, src, GAUD_SAMPLE_F32, nullptr, &out),
      GAUD_OK);
  EXPECT_EQ(gaud_buffer_frames(out), 10u);
  EXPECT_EQ(gaud_buffer_capacity(out), 64u);
  gaud_buffer_destroy(src);
  gaud_buffer_destroy(out);
}

TEST(Convert, FullScaleMapsToFullScale) {
  GAUD_Buffer * src = Filled(GAUD_SAMPLE_F32, 1, 4, 1.0);
  GAUD_Convert_Options opts;
  gaud_convert_options_default(&opts);
  opts.dither = GAUD_DITHER_NONE;
  GAUD_Buffer * out = nullptr;
  ASSERT_EQ(
      gaud_ops_convert_format(nullptr, src, GAUD_SAMPLE_S16, &opts, &out),
      GAUD_OK);
  int16_t v;
  memcpy(&v, gaud_buffer_data_const(out), 2);
  // +1.0 becomes +32767 and not a wrap to -32768: two's complement is
  // asymmetric and the clamp is what keeps the positive extreme positive.
  EXPECT_EQ(v, 32767);
  gaud_buffer_destroy(src);
  gaud_buffer_destroy(out);

  GAUD_Buffer * neg = Filled(GAUD_SAMPLE_F32, 1, 4, -1.0);
  ASSERT_EQ(
      gaud_ops_convert_format(nullptr, neg, GAUD_SAMPLE_S16, &opts, &out),
      GAUD_OK);
  memcpy(&v, gaud_buffer_data_const(out), 2);
  EXPECT_EQ(v, -32768) << "the negative extreme does reach full scale";
  gaud_buffer_destroy(neg);
  gaud_buffer_destroy(out);
}

TEST(Convert, AFloatOverFullScaleIsClampedRatherThanWrapping) {
  GAUD_Buffer * src = Filled(GAUD_SAMPLE_F64, 1, 4, 4.0);
  GAUD_Convert_Options opts;
  gaud_convert_options_default(&opts);
  opts.dither = GAUD_DITHER_NONE;
  GAUD_Buffer * out = nullptr;
  ASSERT_EQ(
      gaud_ops_convert_format(nullptr, src, GAUD_SAMPLE_S16, &opts, &out),
      GAUD_OK);
  int16_t v;
  memcpy(&v, gaud_buffer_data_const(out), 2);
  EXPECT_EQ(v, 32767) << "wrapping here would be loud and wrong";
  gaud_buffer_destroy(src);
  gaud_buffer_destroy(out);
}

TEST(Convert, TheEightBitFormatsDifferByTheirSilence) {
  // WAV's 8-bit is unsigned and AIFF's is signed. Converting between them
  // must move every sample by 128, and a library that treated them as one
  // format would produce a full-scale DC offset.
  GAUD_Buffer * u = Filled(GAUD_SAMPLE_U8, 1, 4, 0.0);
  EXPECT_EQ(static_cast<const unsigned char *>(gaud_buffer_data_const(u))[0],
      128u);
  GAUD_Convert_Options opts;
  gaud_convert_options_default(&opts);
  opts.dither = GAUD_DITHER_NONE;
  GAUD_Buffer * s = nullptr;
  ASSERT_EQ(gaud_ops_convert_format(nullptr, u, GAUD_SAMPLE_S8, &opts, &s),
      GAUD_OK);
  EXPECT_EQ(static_cast<const unsigned char *>(gaud_buffer_data_const(s))[0],
      0u);
  gaud_buffer_destroy(u);
  gaud_buffer_destroy(s);
}

TEST(Convert, DitherIsReproducibleFromItsSeed) {
  // A conversion that cannot be repeated cannot be gated by a golden file.
  GAUD_Buffer * src = Filled(GAUD_SAMPLE_F32, 2, 256, 0.3333);
  GAUD_Convert_Options a, b;
  gaud_convert_options_default(&a);
  a.dither_seed = 12345;
  b = a;
  GAUD_Buffer * first = nullptr;
  GAUD_Buffer * second = nullptr;
  ASSERT_EQ(gaud_ops_convert_format(nullptr, src, GAUD_SAMPLE_S16, &a, &first),
      GAUD_OK);
  ASSERT_EQ(gaud_ops_convert_format(nullptr, src, GAUD_SAMPLE_S16, &b,
                &second),
      GAUD_OK);
  EXPECT_EQ(memcmp(gaud_buffer_data_const(first),
                gaud_buffer_data_const(second),
                gaud_buffer_size_bytes(first)),
      0);
  gaud_buffer_destroy(first);
  gaud_buffer_destroy(second);
  gaud_buffer_destroy(src);
}

TEST(Convert, ADifferentSeedGivesDifferentNoise) {
  GAUD_Buffer * src = Filled(GAUD_SAMPLE_F32, 2, 256, 0.3333);
  GAUD_Convert_Options a, b;
  gaud_convert_options_default(&a);
  a.dither_seed = 1;
  b = a;
  b.dither_seed = 2;
  GAUD_Buffer * first = nullptr;
  GAUD_Buffer * second = nullptr;
  ASSERT_EQ(gaud_ops_convert_format(nullptr, src, GAUD_SAMPLE_S16, &a, &first),
      GAUD_OK);
  ASSERT_EQ(gaud_ops_convert_format(nullptr, src, GAUD_SAMPLE_S16, &b,
                &second),
      GAUD_OK);
  EXPECT_NE(memcmp(gaud_buffer_data_const(first),
                gaud_buffer_data_const(second),
                gaud_buffer_size_bytes(first)),
      0);
  gaud_buffer_destroy(first);
  gaud_buffer_destroy(second);
  gaud_buffer_destroy(src);
}

TEST(Convert, DitherIsNotAppliedWhenNothingIsDiscarded) {
  // Widening loses nothing, so adding noise would be adding noise. A
  // dithered s16 -> s32 must be identical to an undithered one.
  GAUD_Buffer * src = Filled(GAUD_SAMPLE_S16, 1, 128, 0.2);
  GAUD_Convert_Options dithered, plain;
  gaud_convert_options_default(&dithered);
  gaud_convert_options_default(&plain);
  plain.dither = GAUD_DITHER_NONE;
  GAUD_Buffer * a = nullptr;
  GAUD_Buffer * b = nullptr;
  ASSERT_EQ(
      gaud_ops_convert_format(nullptr, src, GAUD_SAMPLE_S32, &dithered, &a),
      GAUD_OK);
  ASSERT_EQ(gaud_ops_convert_format(nullptr, src, GAUD_SAMPLE_S32, &plain, &b),
      GAUD_OK);
  EXPECT_EQ(memcmp(gaud_buffer_data_const(a), gaud_buffer_data_const(b),
                gaud_buffer_size_bytes(a)),
      0);
  gaud_buffer_destroy(src);
  gaud_buffer_destroy(a);
  gaud_buffer_destroy(b);
}

TEST(Convert, DitherRaisesTheNoiseFloorOfASilentSignal) {
  // The observable effect, so that "dither happened" is a measured fact
  // rather than a flag that was set. Converting silence with dither must
  // produce something above zero; without it, exactly zero.
  GAUD_Buffer * quiet = Filled(GAUD_SAMPLE_F32, 1, 4096, 0.0);
  GAUD_Convert_Options tri, none;
  gaud_convert_options_default(&tri);
  tri.dither_seed = 99;
  gaud_convert_options_default(&none);
  none.dither = GAUD_DITHER_NONE;

  GAUD_Buffer * with = nullptr;
  GAUD_Buffer * without = nullptr;
  ASSERT_EQ(gaud_ops_convert_format(nullptr, quiet, GAUD_SAMPLE_S16, &tri,
                &with),
      GAUD_OK);
  ASSERT_EQ(gaud_ops_convert_format(nullptr, quiet, GAUD_SAMPLE_S16, &none,
                &without),
      GAUD_OK);
  double rms_with = 0, rms_without = 0;
  ASSERT_EQ(gaud_ops_rms(with, &rms_with), GAUD_OK);
  ASSERT_EQ(gaud_ops_rms(without, &rms_without), GAUD_OK);
  EXPECT_EQ(rms_without, 0.0);
  EXPECT_GT(rms_with, 0.0);
  EXPECT_LT(rms_with, 0.001) << "a whole LSB or so, not an audible amount";
  gaud_buffer_destroy(quiet);
  gaud_buffer_destroy(with);
  gaud_buffer_destroy(without);
}

TEST(Convert, RectangularAndTriangularDiffer) {
  GAUD_Buffer * quiet = Filled(GAUD_SAMPLE_F32, 1, 8192, 0.0);
  GAUD_Convert_Options rect, tri;
  gaud_convert_options_default(&rect);
  rect.dither = GAUD_DITHER_RECTANGULAR;
  rect.dither_seed = 7;
  tri = rect;
  tri.dither = GAUD_DITHER_TRIANGULAR;
  GAUD_Buffer * a = nullptr;
  GAUD_Buffer * b = nullptr;
  ASSERT_EQ(gaud_ops_convert_format(nullptr, quiet, GAUD_SAMPLE_S16, &rect, &a),
      GAUD_OK);
  ASSERT_EQ(gaud_ops_convert_format(nullptr, quiet, GAUD_SAMPLE_S16, &tri, &b),
      GAUD_OK);
  double ra = 0, rb = 0;
  gaud_ops_rms(a, &ra);
  gaud_ops_rms(b, &rb);
  // Triangular is the sum of two uniforms, so it is the noisier of the two
  // - that is the 3 dB it trades for removing noise modulation.
  EXPECT_GT(rb, ra);
  gaud_buffer_destroy(quiet);
  gaud_buffer_destroy(a);
  gaud_buffer_destroy(b);
}

TEST(Convert, NonPcmIsRefusedAtEveryEntryPoint) {
  GAUD_Buffer * dsd = nullptr;
  ASSERT_EQ(gaud_buffer_create(nullptr, GAUD_SAMPLE_DSD1,
                gaud_channel_layout_default(1), GAUD_LAYOUT_INTERLEAVED, 64,
                &dsd),
      GAUD_OK);
  GAUD_Buffer * out = nullptr;
  double value = 0;
  EXPECT_EQ(
      gaud_ops_convert_format(nullptr, dsd, GAUD_SAMPLE_S16, nullptr, &out),
      GAUD_ERR_UNSUPPORTED);
  EXPECT_EQ(gaud_ops_convert_sample_layout(nullptr, dsd, GAUD_LAYOUT_PLANAR,
                &out),
      GAUD_ERR_UNSUPPORTED);
  EXPECT_EQ(gaud_ops_peak(dsd, &value), GAUD_ERR_UNSUPPORTED);
  EXPECT_EQ(gaud_ops_rms(dsd, &value), GAUD_ERR_UNSUPPORTED);
  EXPECT_EQ(gaud_ops_dc_offset(dsd, &value), GAUD_ERR_UNSUPPORTED);

  // And converting TO a non-PCM format is refused too, which is the arm a
  // guard on the source alone would miss.
  GAUD_Buffer * pcm = Filled(GAUD_SAMPLE_S16, 1, 8, 0.1);
  EXPECT_EQ(
      gaud_ops_convert_format(nullptr, pcm, GAUD_SAMPLE_OPAQUE, nullptr, &out),
      GAUD_ERR_UNSUPPORTED);
  gaud_buffer_destroy(dsd);
  gaud_buffer_destroy(pcm);
}

TEST(Convert, LayoutConversionMovesSamplesAndRoundTrips) {
  GAUD_Buffer * src = Filled(GAUD_SAMPLE_S16, 3, 16, 0.4);
  // Make the channels differ, or a transpose that dropped one would pass.
  for (size_t f = 0; f < 16; ++f) {
    for (uint32_t c = 0; c < 3; ++c) {
      int16_t v = (int16_t)(f * 10 + c);
      memcpy(static_cast<unsigned char *>(gaud_buffer_data(src))
              + gaud_buffer_offset(src, f, c),
          &v, 2);
    }
  }
  GAUD_Buffer * planar = nullptr;
  ASSERT_EQ(gaud_ops_convert_sample_layout(nullptr, src, GAUD_LAYOUT_PLANAR,
                &planar),
      GAUD_OK);
  EXPECT_EQ(gaud_buffer_sample_layout(planar), GAUD_LAYOUT_PLANAR);
  for (size_t f = 0; f < 16; ++f) {
    for (uint32_t c = 0; c < 3; ++c) {
      int16_t v;
      memcpy(&v,
          static_cast<const unsigned char *>(gaud_buffer_data_const(planar))
              + gaud_buffer_offset(planar, f, c),
          2);
      EXPECT_EQ(v, (int16_t)(f * 10 + c)) << f << "," << c;
    }
  }
  GAUD_Buffer * back = nullptr;
  ASSERT_EQ(gaud_ops_convert_sample_layout(nullptr, planar,
                GAUD_LAYOUT_INTERLEAVED, &back),
      GAUD_OK);
  EXPECT_EQ(memcmp(gaud_buffer_data_const(src), gaud_buffer_data_const(back),
                gaud_buffer_size_bytes(src)),
      0);
  gaud_buffer_destroy(src);
  gaud_buffer_destroy(planar);
  gaud_buffer_destroy(back);
}

TEST(Convert, ConvertingToTheSameLayoutIsACopy) {
  // Documented, so that a caller normalising an input does not have to
  // check first.
  GAUD_Buffer * src = Filled(GAUD_SAMPLE_S16, 2, 8, 0.5);
  GAUD_Buffer * out = nullptr;
  ASSERT_EQ(gaud_ops_convert_sample_layout(nullptr, src,
                GAUD_LAYOUT_INTERLEAVED, &out),
      GAUD_OK);
  EXPECT_NE(out, src);
  EXPECT_EQ(memcmp(gaud_buffer_data_const(src), gaud_buffer_data_const(out),
                gaud_buffer_size_bytes(src)),
      0);
  gaud_buffer_destroy(src);
  gaud_buffer_destroy(out);
}

TEST(Measure, PeakRmsAndDcSeeWhatTheOthersCannot) {
  // The three together are the decode gate. Each is here because one of the
  // others would miss the defect it catches.
  GAUD_Buffer * half = Filled(GAUD_SAMPLE_S16, 1, 1000, 0.5);
  double peak = 0, rms = 0, dc = 0;
  ASSERT_EQ(gaud_ops_peak(half, &peak), GAUD_OK);
  ASSERT_EQ(gaud_ops_rms(half, &rms), GAUD_OK);
  ASSERT_EQ(gaud_ops_dc_offset(half, &dc), GAUD_OK);
  EXPECT_NEAR(peak, 0.5, 0.001);
  EXPECT_NEAR(rms, 0.5, 0.001) << "constant, so RMS equals the level";
  EXPECT_NEAR(dc, 0.5, 0.001) << "constant, so the mean equals it too";
  gaud_buffer_destroy(half);
}

TEST(Measure, SilenceIsZeroOnAllThree) {
  GAUD_Buffer * quiet = Filled(GAUD_SAMPLE_S16, 2, 100, 0.0);
  double peak = 1, rms = 1, dc = 1;
  gaud_ops_peak(quiet, &peak);
  gaud_ops_rms(quiet, &rms);
  gaud_ops_dc_offset(quiet, &dc);
  EXPECT_EQ(peak, 0.0);
  EXPECT_EQ(rms, 0.0);
  EXPECT_EQ(dc, 0.0);
  gaud_buffer_destroy(quiet);
}

TEST(Measure, TheMeasurementsWalkFramesAndNotCapacity) {
  // A half-filled buffer's tail is silence; including it would dilute every
  // answer by however much went unused.
  GAUD_Buffer * b = Filled(GAUD_SAMPLE_S16, 1, 100, 0.8);
  gaud_buffer_set_frames(b, 100);
  double full = 0;
  gaud_ops_rms(b, &full);
  gaud_buffer_set_frames(b, 50);
  double half = 0;
  gaud_ops_rms(b, &half);
  EXPECT_NEAR(full, half, 1e-9) << "the samples are constant, so RMS must not "
                                   "change with how many are counted";
  gaud_buffer_destroy(b);
}

TEST(Measure, AnEmptyBufferIsZeroRatherThanADivisionByZero) {
  GAUD_Buffer * b = Filled(GAUD_SAMPLE_S16, 1, 8, 0.5);
  gaud_buffer_set_frames(b, 0);
  double v = 42;
  EXPECT_EQ(gaud_ops_rms(b, &v), GAUD_OK);
  EXPECT_EQ(v, 0.0);
  gaud_buffer_destroy(b);
}

TEST(Measure, RefusesNullSafely) {
  double v = 0;
  EXPECT_EQ(gaud_ops_peak(nullptr, &v), GAUD_ERR_INVALID);
  GAUD_Buffer * b = Filled(GAUD_SAMPLE_S16, 1, 4, 0.1);
  EXPECT_EQ(gaud_ops_rms(b, nullptr), GAUD_ERR_INVALID);
  gaud_buffer_destroy(b);
  gaud_convert_options_default(nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
