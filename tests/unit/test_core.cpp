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
 * Result codes, diagnostics, limits, and what this build says about itself.
 */

#include <ghoti.io/audio/audio.h>
#include <gtest/gtest.h>
#include <string.h>

TEST(Result, EveryCodeHasItsOwnString) {
  // The point is not that each is non-empty but that none was left to fall
  // through to "unknown", which is what happens when a code is added and the
  // table is not.
  for (int i = 0; i < GAUD_RESULT_COUNT; ++i) {
    const char * s = gaud_result_string(static_cast<GAUD_Result>(i));
    ASSERT_NE(s, nullptr) << "result " << i;
    EXPECT_STRNE(s, "unknown") << "result " << i << " has no string";
    EXPECT_GT(strlen(s), 0u) << "result " << i;
  }
}

TEST(Result, StringsAreDistinct) {
  for (int i = 0; i < GAUD_RESULT_COUNT; ++i) {
    for (int j = i + 1; j < GAUD_RESULT_COUNT; ++j) {
      EXPECT_STRNE(gaud_result_string(static_cast<GAUD_Result>(i)),
          gaud_result_string(static_cast<GAUD_Result>(j)))
          << "results " << i << " and " << j << " share a string";
    }
  }
}

TEST(Result, OutOfRangeIsUnknown) {
  EXPECT_STREQ(gaud_result_string(static_cast<GAUD_Result>(GAUD_RESULT_COUNT)),
      "unknown");
  EXPECT_STREQ(gaud_result_string(static_cast<GAUD_Result>(-1)), "unknown");
}

TEST(Result, OkIsZero) {
  // Callers write `if (gaud_...() != GAUD_OK)`. Worth one assertion.
  EXPECT_EQ(GAUD_OK, 0);
}

TEST(Limits, DefaultsAreAllNonZero) {
  // Zero does not mean "unlimited" in this struct, so a field left unset by
  // gaud_limits_default() would refuse everything rather than allow
  // everything - a failure that looks like corrupt input.
  GAUD_Limits limits;
  memset(&limits, 0, sizeof(limits));
  gaud_limits_default(&limits);

  EXPECT_GT(limits.max_tracks, 0u);
  EXPECT_GT(limits.max_channels, 0u);
  EXPECT_GT(limits.max_sample_rate, 0u);
  EXPECT_GT(limits.max_frames, 0u);
  EXPECT_GT(limits.max_decoded_bytes, 0u);
  EXPECT_GT(limits.max_element_size, 0u);
  EXPECT_GT(limits.max_nesting_depth, 0u);
  EXPECT_GT(limits.max_metadata_bytes, 0u);
  EXPECT_GT(limits.max_metadata_entries, 0u);
  EXPECT_GT(limits.max_picture_bytes, 0u);
  EXPECT_GT(limits.max_cue_points, 0u);
}

TEST(Limits, DefaultSampleRateAdmitsDsd) {
  // planning/audio.md 11.6 puts DSD in core. A default that refused the one
  // non-PCM rate the library plans to carry would be found much later, by
  // someone with no way to know the number was not deliberate.
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  EXPECT_GE(limits.max_sample_rate, 2822400u) << "DSD64 would not load";
}

TEST(Limits, DefaultIgnoresNull) {
  gaud_limits_default(nullptr); // must not crash
}

TEST(Diagnostics, ZeroInitialisedListAccepts) {
  // Documented: a zeroed struct is usable and uses the default allocator.
  GAUD_Diagnostics diagnostics;
  memset(&diagnostics, 0, sizeof(diagnostics));

  GAUD_Diagnostic entry = {};
  entry.codec_name = "wav";
  entry.severity = GAUD_DIAG_WARNING;

  ASSERT_EQ(gaud_diagnostics_append(&diagnostics, &entry), GAUD_OK);
  ASSERT_EQ(diagnostics.count, 1u);
  EXPECT_STREQ(diagnostics.items[0].codec_name, "wav");

  gaud_diagnostics_destroy(&diagnostics);
  EXPECT_EQ(diagnostics.count, 0u);
  EXPECT_EQ(diagnostics.items, nullptr);
}

TEST(Diagnostics, GrowsPastItsInitialCapacity) {
  // More entries than the first allocation holds, so that the realloc path
  // runs. A single append would leave it untested.
  GAUD_Diagnostics diagnostics = {};
  gaud_diagnostics_init(&diagnostics, nullptr);

  for (uint64_t i = 0; i < 200; ++i) {
    GAUD_Diagnostic entry = {};
    entry.codec_name = "flac";
    entry.offset = i;
    ASSERT_EQ(gaud_diagnostics_append(&diagnostics, &entry), GAUD_OK) << i;
  }
  ASSERT_EQ(diagnostics.count, 200u);
  // Order is preserved; a grow that copied wrongly would show here.
  for (uint64_t i = 0; i < 200; ++i) {
    EXPECT_EQ(diagnostics.items[i].offset, i);
  }
  gaud_diagnostics_destroy(&diagnostics);
}

TEST(Diagnostics, ClearKeepsTheAllocatorAndDestroyDoesNot) {
  GAUD_Diagnostics diagnostics = {};
  const GAUD_Allocator * allocator = gaud_allocator_default();
  gaud_diagnostics_init(&diagnostics, allocator);

  GAUD_Diagnostic entry = {};
  entry.codec_name = "mp3";
  ASSERT_EQ(gaud_diagnostics_append(&diagnostics, &entry), GAUD_OK);

  gaud_diagnostics_clear(&diagnostics);
  EXPECT_EQ(diagnostics.count, 0u);
  EXPECT_EQ(diagnostics.allocator, allocator) << "clear dropped the allocator";
  // Documented as reusable after a clear.
  ASSERT_EQ(gaud_diagnostics_append(&diagnostics, &entry), GAUD_OK);

  gaud_diagnostics_destroy(&diagnostics);
  EXPECT_EQ(diagnostics.allocator, nullptr);
}

TEST(Diagnostics, RejectsNullArguments) {
  GAUD_Diagnostics diagnostics = {};
  GAUD_Diagnostic entry = {};
  EXPECT_EQ(gaud_diagnostics_append(nullptr, &entry), GAUD_ERR_INVALID);
  EXPECT_EQ(gaud_diagnostics_append(&diagnostics, nullptr), GAUD_ERR_INVALID);
  gaud_diagnostics_clear(nullptr);
  gaud_diagnostics_destroy(nullptr);
  gaud_diagnostics_init(nullptr, nullptr);
}

TEST(Build, ImageValidationAgreesWithHowTheLibraryWasCompiled) {
  // This is not the tautology it looks like. The function is compiled into
  // the library and the #ifdef here is evaluated in the test, so the two
  // answers come from two separate compilations. They differ whenever the
  // feature define reaches one and not the other - which is exactly what
  // happens if -DGAUD_HAVE_IMAGE is appended to CFLAGS after LIB_CFLAGS has
  // already captured it. That bug compiles, links, and silently gives the
  // library and its callers different ideas about the build.
#ifdef GAUD_HAVE_IMAGE
  EXPECT_TRUE(gaud_have_image_validation())
      << "the tests were built against image and the library was not";
#else
  EXPECT_FALSE(gaud_have_image_validation())
      << "the library was built against image and the tests were not";
#endif
}

TEST(Version, StringIsNotEmpty) {
  const char * v = gaud_version_string();
  ASSERT_NE(v, nullptr);
  EXPECT_GT(strlen(v), 0u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
