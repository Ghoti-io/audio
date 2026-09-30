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
#include <string>
#include <vector>

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

TEST(Writer, GrowsAndReadsBackWhatWasWritten) {
  GAUD_Stream * w = nullptr;
  ASSERT_EQ(gaud_stream_create_memory_writer(nullptr, &w), GAUD_OK);
  EXPECT_TRUE(gaud_stream_writable(w));
  EXPECT_TRUE(gaud_stream_seekable(w));

  // More than the initial allocation, so the grow path runs.
  std::vector<unsigned char> blob(5000);
  for (size_t i = 0; i < blob.size(); ++i) {
    blob[i] = static_cast<unsigned char>(i * 7);
  }
  ASSERT_EQ(gaud_stream_write(w, blob.data(), blob.size()), GAUD_OK);

  const void * bytes = nullptr;
  size_t length = 0;
  ASSERT_EQ(gaud_stream_writer_bytes(w, &bytes, &length), GAUD_OK);
  EXPECT_EQ(length, blob.size());
  EXPECT_EQ(memcmp(bytes, blob.data(), blob.size()), 0);
  gaud_stream_destroy(w);
}

TEST(Writer, OverwritingTheMiddleDoesNotTruncate) {
  // Patching a header whose length was unknown at the time is exactly what
  // this supports; a write that truncated would destroy the samples.
  GAUD_Stream * w = nullptr;
  ASSERT_EQ(gaud_stream_create_memory_writer(nullptr, &w), GAUD_OK);
  const unsigned char first[] = {1, 2, 3, 4, 5, 6, 7, 8};
  ASSERT_EQ(gaud_stream_write(w, first, sizeof(first)), GAUD_OK);
  ASSERT_EQ(gaud_stream_seek(w, 2, GAUD_SEEK_SET), GAUD_OK);
  const unsigned char patch[] = {0xAA, 0xBB};
  ASSERT_EQ(gaud_stream_write(w, patch, sizeof(patch)), GAUD_OK);

  const void * bytes = nullptr;
  size_t length = 0;
  gaud_stream_writer_bytes(w, &bytes, &length);
  ASSERT_EQ(length, 8u) << "the tail was lost";
  const unsigned char expect[] = {1, 2, 0xAA, 0xBB, 5, 6, 7, 8};
  EXPECT_EQ(memcmp(bytes, expect, sizeof(expect)), 0);
  gaud_stream_destroy(w);
}

TEST(Writer, ANonWriterRefusesWritesAndTheQuery) {
  GAUD_Stream * r = OpenSample();
  EXPECT_FALSE(gaud_stream_writable(r));
  const unsigned char byte = 1;
  EXPECT_EQ(gaud_stream_write(r, &byte, 1), GAUD_ERR_INVALID);
  const void * bytes = nullptr;
  size_t length = 0;
  EXPECT_EQ(gaud_stream_writer_bytes(r, &bytes, &length),
      GAUD_ERR_UNSUPPORTED);
  gaud_stream_destroy(r);
}

TEST(Writer, AZeroLengthWriteIsFine) {
  GAUD_Stream * w = nullptr;
  ASSERT_EQ(gaud_stream_create_memory_writer(nullptr, &w), GAUD_OK);
  EXPECT_EQ(gaud_stream_write(w, nullptr, 0), GAUD_OK);
  gaud_stream_destroy(w);
}

TEST(Unseekable, ForwardsReadsAndRefusesEverythingElse) {
  // The wrapper exists so this path can be REACHED by a test. "A pipe is an
  // ordinary way to receive audio" describes code that never runs unless
  // something makes it run, and a gate that cannot reach a branch reports
  // success for it.
  GAUD_Stream * source = OpenSample();
  GAUD_Stream * blind = nullptr;
  ASSERT_EQ(gaud_stream_create_unseekable(source, &blind), GAUD_OK);

  EXPECT_FALSE(gaud_stream_seekable(blind));
  EXPECT_FALSE(gaud_stream_writable(blind));

  unsigned char out[4] = {};
  EXPECT_EQ(gaud_stream_read(blind, out, 4), 4u);
  EXPECT_EQ(memcmp(out, "RIFF", 4), 0);
  EXPECT_EQ(gaud_stream_tell(blind), 4u);

  EXPECT_EQ(gaud_stream_seek(blind, 0, GAUD_SEEK_SET), GAUD_ERR_UNSUPPORTED);
  uint64_t size = 0;
  EXPECT_EQ(gaud_stream_size(blind, &size), GAUD_ERR_UNSUPPORTED)
      << "a pipe does not know its own length, and a wrapper that passed "
         "the real one through would model a stream that exists nowhere";

  gaud_stream_destroy(blind);
  gaud_stream_destroy(source);
}

TEST(Unseekable, ReachesTheEndTheSameWayTheSourceDoes) {
  GAUD_Stream * source = OpenSample();
  GAUD_Stream * blind = nullptr;
  ASSERT_EQ(gaud_stream_create_unseekable(source, &blind), GAUD_OK);
  unsigned char out[64] = {};
  EXPECT_EQ(gaud_stream_read(blind, out, sizeof(out)), sizeof(kBytes));
  EXPECT_TRUE(gaud_stream_eof(blind));
  gaud_stream_destroy(blind);
  gaud_stream_destroy(source);
}

TEST(Unseekable, RefusesNullSource) {
  GAUD_Stream * blind = nullptr;
  EXPECT_EQ(gaud_stream_create_unseekable(nullptr, &blind), GAUD_ERR_INVALID);
  GAUD_Stream * source = OpenSample();
  EXPECT_EQ(gaud_stream_create_unseekable(source, nullptr), GAUD_ERR_INVALID);
  gaud_stream_destroy(source);
}

TEST(FileStream, ReadsAFileTheSameWayAMemoryStreamReadsItsBytes) {
  std::string path = std::string(GAUD_TEST_DATA) + "/wav_s16_stereo_44100.wav";
  GAUD_Stream * file = nullptr;
  ASSERT_EQ(gaud_stream_create_file(path.c_str(), &file), GAUD_OK);
  EXPECT_TRUE(gaud_stream_seekable(file));
  EXPECT_FALSE(gaud_stream_writable(file));

  uint64_t size = 0;
  ASSERT_EQ(gaud_stream_size(file, &size), GAUD_OK);
  EXPECT_GT(size, 12u);

  unsigned char head[12] = {};
  ASSERT_EQ(gaud_stream_read(file, head, sizeof(head)), sizeof(head));
  EXPECT_EQ(memcmp(head, "RIFF", 4), 0);
  EXPECT_EQ(memcmp(head + 8, "WAVE", 4), 0);
  EXPECT_EQ(gaud_stream_tell(file), 12u);

  ASSERT_EQ(gaud_stream_seek(file, 0, GAUD_SEEK_SET), GAUD_OK);
  EXPECT_EQ(gaud_stream_tell(file), 0u);
  EXPECT_FALSE(gaud_stream_eof(file));

  // Out of range is refused identically to the memory stream, rather than
  // being whatever fseek happens to permit - seeking past the end of a file
  // is legal in C and makes a hole on the next write.
  EXPECT_EQ(gaud_stream_seek(file, -1, GAUD_SEEK_SET), GAUD_ERR_INVALID);
  EXPECT_EQ(gaud_stream_seek(file, 1, GAUD_SEEK_END), GAUD_ERR_INVALID);
  gaud_stream_destroy(file);
}

TEST(FileStream, RefusesAFileThatIsNotThere) {
  GAUD_Stream * file = nullptr;
  EXPECT_EQ(gaud_stream_create_file("/nonexistent/nothing.wav", &file),
      GAUD_ERR_IO);
  EXPECT_EQ(file, nullptr);
  EXPECT_EQ(gaud_stream_create_file(nullptr, &file), GAUD_ERR_INVALID);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
