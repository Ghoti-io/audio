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
 * The memory stream.
 */

#include <ghoti.io/audio/audio.h>
#include <gtest/gtest.h>
#include <string.h>

namespace {

const unsigned char kBytes[] = {'R', 'I', 'F', 'F', 4, 3, 2, 1, 'W', 'A', 'V',
    'E'};

GAUD_Stream * OpenSample() {
  GAUD_Stream * stream = nullptr;
  EXPECT_EQ(
      gaud_stream_create_memory(kBytes, sizeof(kBytes), &stream), GAUD_OK);
  return stream;
}

} // namespace

TEST(Stream, CreateRejectsNullOutput) {
  EXPECT_EQ(gaud_stream_create_memory(kBytes, sizeof(kBytes), nullptr),
      GAUD_ERR_INVALID);
}

TEST(Stream, CreateRejectsNullBytesWithNonZeroLength) {
  GAUD_Stream * stream = nullptr;
  EXPECT_EQ(gaud_stream_create_memory(nullptr, 8, &stream), GAUD_ERR_INVALID);
  EXPECT_EQ(stream, nullptr) << "nothing is written to out on failure";
}

TEST(Stream, AnEmptyStreamIsValidAndAtEnd) {
  GAUD_Stream * stream = nullptr;
  ASSERT_EQ(gaud_stream_create_memory(nullptr, 0, &stream), GAUD_OK);
  ASSERT_NE(stream, nullptr);
  EXPECT_TRUE(gaud_stream_eof(stream));
  unsigned char byte = 0;
  EXPECT_EQ(gaud_stream_read(stream, &byte, 1), 0u);
  gaud_stream_destroy(stream);
}

TEST(Stream, ReadsSequentiallyAndTracksPosition) {
  GAUD_Stream * stream = OpenSample();
  unsigned char out[4] = {};

  EXPECT_EQ(gaud_stream_tell(stream), 0u);
  ASSERT_EQ(gaud_stream_read(stream, out, 4), 4u);
  EXPECT_EQ(memcmp(out, "RIFF", 4), 0);
  EXPECT_EQ(gaud_stream_tell(stream), 4u);
  EXPECT_FALSE(gaud_stream_eof(stream));

  gaud_stream_destroy(stream);
}

TEST(Stream, AShortReadIsNotAnError) {
  GAUD_Stream * stream = OpenSample();
  unsigned char out[32] = {};
  EXPECT_EQ(gaud_stream_read(stream, out, sizeof(out)), sizeof(kBytes));
  EXPECT_TRUE(gaud_stream_eof(stream));
  gaud_stream_destroy(stream);
}

TEST(Stream, ReadingTheLastByteSetsEof) {
  // Documented: eof is set on reaching the end, not on a later short read.
  // A parser that loops "while not eof" would otherwise run one iteration
  // too many on every file.
  GAUD_Stream * stream = OpenSample();
  unsigned char out[sizeof(kBytes)] = {};
  ASSERT_EQ(gaud_stream_read(stream, out, sizeof(kBytes)), sizeof(kBytes));
  EXPECT_TRUE(gaud_stream_eof(stream));
  gaud_stream_destroy(stream);
}

TEST(Stream, SeekingBackClearsEof) {
  // The bug this guards: a stream read to the end and rewound still reports
  // eof, so the second of two passes over a seekable stream ends at once.
  GAUD_Stream * stream = OpenSample();
  unsigned char out[sizeof(kBytes)] = {};
  ASSERT_EQ(gaud_stream_read(stream, out, sizeof(kBytes)), sizeof(kBytes));
  ASSERT_TRUE(gaud_stream_eof(stream));

  ASSERT_EQ(gaud_stream_seek(stream, 0, GAUD_SEEK_SET), GAUD_OK);
  EXPECT_FALSE(gaud_stream_eof(stream));
  EXPECT_EQ(gaud_stream_read(stream, out, 4), 4u);

  gaud_stream_destroy(stream);
}

TEST(Stream, SeeksFromEveryOrigin) {
  GAUD_Stream * stream = OpenSample();
  unsigned char out[4] = {};

  ASSERT_EQ(gaud_stream_seek(stream, 8, GAUD_SEEK_SET), GAUD_OK);
  ASSERT_EQ(gaud_stream_read(stream, out, 4), 4u);
  EXPECT_EQ(memcmp(out, "WAVE", 4), 0);

  ASSERT_EQ(gaud_stream_seek(stream, -4, GAUD_SEEK_CUR), GAUD_OK);
  EXPECT_EQ(gaud_stream_tell(stream), 8u);

  ASSERT_EQ(gaud_stream_seek(stream, -12, GAUD_SEEK_END), GAUD_OK);
  EXPECT_EQ(gaud_stream_tell(stream), 0u);

  gaud_stream_destroy(stream);
}

TEST(Stream, SeekingToTheEndIsAllowedAndReadingThereIsNot) {
  GAUD_Stream * stream = OpenSample();
  EXPECT_EQ(gaud_stream_seek(stream, 0, GAUD_SEEK_END), GAUD_OK);
  EXPECT_EQ(gaud_stream_tell(stream), sizeof(kBytes));
  unsigned char byte = 0;
  EXPECT_EQ(gaud_stream_read(stream, &byte, 1), 0u);
  gaud_stream_destroy(stream);
}

TEST(Stream, SeekingOutsideTheStreamIsRefused) {
  GAUD_Stream * stream = OpenSample();
  EXPECT_EQ(gaud_stream_seek(stream, -1, GAUD_SEEK_SET), GAUD_ERR_INVALID);
  EXPECT_EQ(gaud_stream_seek(stream, 1, GAUD_SEEK_END), GAUD_ERR_INVALID);
  EXPECT_EQ(gaud_stream_seek(stream, 9999, GAUD_SEEK_SET), GAUD_ERR_INVALID);
  // A negative offset past the start must be caught rather than wrapping
  // into an enormous unsigned position.
  EXPECT_EQ(gaud_stream_seek(stream, INT64_MIN, GAUD_SEEK_CUR),
      GAUD_ERR_INVALID);
  EXPECT_EQ(gaud_stream_seek(stream, INT64_MAX, GAUD_SEEK_CUR),
      GAUD_ERR_INVALID);
  // A refused seek leaves the position alone.
  EXPECT_EQ(gaud_stream_tell(stream), 0u);
  gaud_stream_destroy(stream);
}

TEST(Stream, RejectsAnUnknownOrigin) {
  GAUD_Stream * stream = OpenSample();
  EXPECT_EQ(gaud_stream_seek(stream, 0, static_cast<GAUD_Seek_Origin>(99)),
      GAUD_ERR_INVALID);
  gaud_stream_destroy(stream);
}

TEST(Stream, ReportsItsSizeAndThatItCanSeek) {
  GAUD_Stream * stream = OpenSample();
  uint64_t size = 0;
  ASSERT_EQ(gaud_stream_size(stream, &size), GAUD_OK);
  EXPECT_EQ(size, sizeof(kBytes));
  EXPECT_TRUE(gaud_stream_seekable(stream));
  gaud_stream_destroy(stream);
}

TEST(Stream, BorrowsRatherThanCopies) {
  // Documented contract, and the reason a fuzz harness can hand over its
  // input without a copy per iteration. If this ever starts copying, the
  // contract in stream.h is wrong and callers relying on it are too.
  unsigned char mutable_bytes[4] = {'a', 'b', 'c', 'd'};
  GAUD_Stream * stream = nullptr;
  ASSERT_EQ(gaud_stream_create_memory(mutable_bytes, 4, &stream), GAUD_OK);

  mutable_bytes[0] = 'z';
  unsigned char out[4] = {};
  ASSERT_EQ(gaud_stream_read(stream, out, 4), 4u);
  EXPECT_EQ(out[0], 'z') << "the stream copied its input";

  gaud_stream_destroy(stream);
}

TEST(Stream, AccessorsRefuseNullSafely) {
  EXPECT_EQ(gaud_stream_read(nullptr, nullptr, 0), 0u);
  EXPECT_EQ(gaud_stream_tell(nullptr), static_cast<uint64_t>(-1));
  EXPECT_EQ(gaud_stream_seek(nullptr, 0, GAUD_SEEK_SET), GAUD_ERR_INVALID);
  uint64_t size = 0;
  EXPECT_EQ(gaud_stream_size(nullptr, &size), GAUD_ERR_INVALID);
  EXPECT_TRUE(gaud_stream_eof(nullptr));
  EXPECT_FALSE(gaud_stream_seekable(nullptr));
  gaud_stream_destroy(nullptr);
}

TEST(Stream, ReadRejectsNullDestination) {
  GAUD_Stream * stream = OpenSample();
  EXPECT_EQ(gaud_stream_read(stream, nullptr, 4), 0u);
  EXPECT_EQ(gaud_stream_tell(stream), 0u) << "a refused read moved on";
  gaud_stream_destroy(stream);
}

TEST(Stream, SizeRejectsNullOutput) {
  GAUD_Stream * stream = OpenSample();
  EXPECT_EQ(gaud_stream_size(stream, nullptr), GAUD_ERR_INVALID);
  gaud_stream_destroy(stream);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
