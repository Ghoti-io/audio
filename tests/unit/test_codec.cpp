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
 * The codec registry and the probe over it.
 *
 * Every codec here is built the way an out-of-tree one would be - a
 * file-scope GAUD_Codec, handed to gaud_registry_register(). That is the
 * point: phase 1 adds a codec compiled against the *installed* headers, and
 * these tests are what says the interface it will use works at all.
 */

#include <ghoti.io/audio/audio.h>
#include <gtest/gtest.h>
#include <stdio.h>
#include <string.h>

namespace {

const unsigned char kRiff[] = {'R', 'I', 'F', 'F'};
const unsigned char kWave[] = {'W', 'A', 'V', 'E'};
const unsigned char kFlaC[] = {'f', 'L', 'a', 'C'};

const GAUD_Codec_Magic kWavMagics[] = {
    {0, kRiff, 4},
    {8, kWave, 4},
};
const GAUD_Codec_Magic kFlacMagics[] = {{0, kFlaC, 4}};

/** A decode-only codec, so encoder_tier must be NONE. */
GAUD_Codec MakeWav() {
  GAUD_Codec codec = {};
  codec.abi_version = GAUD_CODEC_ABI_VERSION;
  codec.size = sizeof(GAUD_Codec);
  codec.name = "wav";
  codec.capabilities = GAUD_CAP_DECODE;
  codec.encoder_tier = GAUD_ENCODER_NONE;
  codec.magics = kWavMagics;
  codec.magic_count = 2;
  return codec;
}

GAUD_Codec MakeFlac() {
  GAUD_Codec codec = {};
  codec.abi_version = GAUD_CODEC_ABI_VERSION;
  codec.size = sizeof(GAUD_Codec);
  codec.name = "flac";
  codec.capabilities = GAUD_CAP_DECODE | GAUD_CAP_ENCODE;
  codec.encoder_tier = GAUD_ENCODER_EXACT;
  codec.magics = kFlacMagics;
  codec.magic_count = 1;
  return codec;
}

struct Fixture {
  GAUD_Registry * registry = nullptr;
  Fixture() { EXPECT_EQ(gaud_registry_create(nullptr, &registry), GAUD_OK); }
  ~Fixture() { gaud_registry_destroy(registry); }
};

GAUD_Stream * Open(const unsigned char * bytes, size_t length) {
  GAUD_Stream * stream = nullptr;
  EXPECT_EQ(gaud_stream_create_memory(bytes, length, &stream), GAUD_OK);
  return stream;
}

} // namespace

TEST(Registry, StartsEmptyAndAccepts) {
  Fixture f;
  EXPECT_EQ(gaud_registry_count(f.registry), 0u);

  GAUD_Codec wav = MakeWav();
  ASSERT_EQ(gaud_registry_register(f.registry, &wav), GAUD_OK);
  EXPECT_EQ(gaud_registry_count(f.registry), 1u);
  EXPECT_EQ(gaud_registry_by_index(f.registry, 0), &wav);
  EXPECT_EQ(gaud_registry_find(f.registry, "wav"), &wav);
  EXPECT_EQ(gaud_registry_find(f.registry, "flac"), nullptr);
}

TEST(Registry, BorrowsTheCodecRatherThanCopyingIt) {
  // codec.h documents this, and it is what lets a plugin's file-scope
  // constant be the registered object. A registry that copied would make
  // a later ctx change invisible.
  Fixture f;
  GAUD_Codec wav = MakeWav();
  ASSERT_EQ(gaud_registry_register(f.registry, &wav), GAUD_OK);
  EXPECT_EQ(gaud_registry_find(f.registry, "wav"), &wav)
      << "the registry copied the codec";
}

TEST(Registry, GrowsPastItsInitialCapacity) {
  // More codecs than the first allocation holds, so the realloc path runs
  // and the earlier entries are checked to have survived it.
  Fixture f;
  static GAUD_Codec codecs[64];
  // Wide enough for "codec" plus any int, because the compiler cannot see
  // that i stays under 64 and -Werror=format-truncation is right to say so.
  static char names[64][24];
  for (int i = 0; i < 64; ++i) {
    snprintf(names[i], sizeof(names[i]), "codec%d", i);
    codecs[i] = MakeWav();
    codecs[i].name = names[i];
    ASSERT_EQ(gaud_registry_register(f.registry, &codecs[i]), GAUD_OK) << i;
  }
  ASSERT_EQ(gaud_registry_count(f.registry), 64u);
  for (int i = 0; i < 64; ++i) {
    EXPECT_EQ(gaud_registry_find(f.registry, names[i]), &codecs[i]) << i;
  }
}

TEST(Registry, RefusesADuplicateName) {
  // Two codecs under one name would make find() depend on load order, which
  // for a plugin is whatever the dynamic linker decided.
  Fixture f;
  GAUD_Codec first = MakeWav();
  GAUD_Codec second = MakeWav();
  ASSERT_EQ(gaud_registry_register(f.registry, &first), GAUD_OK);
  EXPECT_EQ(gaud_registry_register(f.registry, &second), GAUD_ERR_INVALID);
  EXPECT_EQ(gaud_registry_count(f.registry), 1u);
  EXPECT_EQ(gaud_registry_find(f.registry, "wav"), &first);
}

TEST(Registry, RefusesAnUnknownAbiVersion) {
  // The whole reason abi_version exists. A codec built against a header
  // this library does not know must be refused at registration, not stored
  // and then called through a pointer its compiler never wrote.
  Fixture f;
  GAUD_Codec codec = MakeWav();
  codec.abi_version = GAUD_CODEC_ABI_VERSION + 1;
  EXPECT_EQ(gaud_registry_register(f.registry, &codec), GAUD_ERR_INVALID);

  codec.abi_version = 0;
  EXPECT_EQ(gaud_registry_register(f.registry, &codec), GAUD_ERR_INVALID);
  EXPECT_EQ(gaud_registry_count(f.registry), 0u);
}

TEST(Registry, RefusesAStructSmallerThanTheFixedPart) {
  Fixture f;
  GAUD_Codec codec = MakeWav();
  codec.size = sizeof(GAUD_Codec) - 1;
  EXPECT_EQ(gaud_registry_register(f.registry, &codec), GAUD_ERR_INVALID);
}

TEST(Registry, RefusesAMissingOrEmptyName) {
  Fixture f;
  GAUD_Codec codec = MakeWav();
  codec.name = nullptr;
  EXPECT_EQ(gaud_registry_register(f.registry, &codec), GAUD_ERR_INVALID);
  codec.name = "";
  EXPECT_EQ(gaud_registry_register(f.registry, &codec), GAUD_ERR_INVALID);
  EXPECT_EQ(gaud_registry_register(f.registry, nullptr), GAUD_ERR_INVALID);
}

TEST(Registry, RefusesInconsistentMagics) {
  Fixture f;
  GAUD_Codec codec = MakeWav();
  codec.magics = nullptr;
  codec.magic_count = 2;
  EXPECT_EQ(gaud_registry_register(f.registry, &codec), GAUD_ERR_INVALID);

  GAUD_Codec_Magic empty[] = {{0, nullptr, 4}};
  codec = MakeWav();
  codec.magics = empty;
  codec.magic_count = 1;
  EXPECT_EQ(gaud_registry_register(f.registry, &codec), GAUD_ERR_INVALID);

  GAUD_Codec_Magic zero_length[] = {{0, kRiff, 0}};
  codec = MakeWav();
  codec.magics = zero_length;
  codec.magic_count = 1;
  EXPECT_EQ(gaud_registry_register(f.registry, &codec), GAUD_ERR_INVALID);
}

TEST(Registry, TheEncoderTierMustMatchTheEncodeCapability) {
  // planning/audio.md 11.3 made the tier the machine-readable half of
  // "stubs are labelled". A codec that declares an encoder and no tier, or
  // a tier and no encoder, would put that back into prose.
  Fixture f;

  GAUD_Codec encodes_untiered = MakeFlac();
  encodes_untiered.encoder_tier = GAUD_ENCODER_NONE;
  EXPECT_EQ(gaud_registry_register(f.registry, &encodes_untiered),
      GAUD_ERR_INVALID);

  GAUD_Codec tiered_but_decode_only = MakeWav();
  tiered_but_decode_only.encoder_tier = GAUD_ENCODER_STUB;
  EXPECT_EQ(gaud_registry_register(f.registry, &tiered_but_decode_only),
      GAUD_ERR_INVALID);

  GAUD_Codec out_of_range = MakeFlac();
  out_of_range.encoder_tier
      = static_cast<GAUD_Encoder_Tier>(GAUD_ENCODER_TIER_COUNT);
  EXPECT_EQ(
      gaud_registry_register(f.registry, &out_of_range), GAUD_ERR_INVALID);

  EXPECT_EQ(gaud_registry_count(f.registry), 0u);
}

TEST(Registry, TheDefaultRegistryRefusesToBeDestroyed) {
  GAUD_Registry * def = gaud_registry_default();
  ASSERT_NE(def, nullptr);
  gaud_registry_destroy(def); // must be a no-op, not a free
  EXPECT_EQ(gaud_registry_default(), def);
}

TEST(Registry, NullMeansTheDefault) {
  EXPECT_EQ(gaud_registry_count(nullptr),
      gaud_registry_count(gaud_registry_default()));
}

TEST(Registry, ByIndexRefusesOutOfRange) {
  Fixture f;
  EXPECT_EQ(gaud_registry_by_index(f.registry, 0), nullptr);
  GAUD_Codec wav = MakeWav();
  ASSERT_EQ(gaud_registry_register(f.registry, &wav), GAUD_OK);
  EXPECT_EQ(gaud_registry_by_index(f.registry, 1), nullptr);
  EXPECT_EQ(gaud_registry_find(f.registry, nullptr), nullptr);
}

TEST(Probe, IdentifiesByMagic) {
  Fixture f;
  GAUD_Codec wav = MakeWav();
  GAUD_Codec flac = MakeFlac();
  ASSERT_EQ(gaud_registry_register(f.registry, &wav), GAUD_OK);
  ASSERT_EQ(gaud_registry_register(f.registry, &flac), GAUD_OK);

  const unsigned char bytes[] = {'f', 'L', 'a', 'C', 0, 0, 0, 34};
  GAUD_Stream * stream = Open(bytes, sizeof(bytes));
  GAUD_Probe_Result result = {};
  ASSERT_EQ(gaud_probe(f.registry, stream, &result), GAUD_OK);
  ASSERT_NE(result.codec_name, nullptr);
  EXPECT_STREQ(result.codec_name, "flac");
  EXPECT_GT(result.confidence, 0u);
  gaud_stream_destroy(stream);
}

TEST(Probe, MatchesASignatureAtANonZeroOffset) {
  // WAV's second signature is at byte 8, which is why GAUD_Codec_Magic
  // carries an offset at all.
  Fixture f;
  GAUD_Codec wav = MakeWav();
  ASSERT_EQ(gaud_registry_register(f.registry, &wav), GAUD_OK);

  const unsigned char bytes[] = {'X', 'X', 'X', 'X', 0, 0, 0, 0, 'W', 'A', 'V',
      'E'};
  GAUD_Stream * stream = Open(bytes, sizeof(bytes));
  GAUD_Probe_Result result = {};
  ASSERT_EQ(gaud_probe(f.registry, stream, &result), GAUD_OK);
  ASSERT_NE(result.codec_name, nullptr);
  EXPECT_STREQ(result.codec_name, "wav");
  gaud_stream_destroy(stream);
}

TEST(Probe, ReportsNothingForAnUnknownFormat) {
  // Not an error: "no codec recognised this" is an answer.
  Fixture f;
  GAUD_Codec wav = MakeWav();
  ASSERT_EQ(gaud_registry_register(f.registry, &wav), GAUD_OK);

  const unsigned char bytes[] = {'n', 'o', 'p', 'e', '!', '!', '!', '!'};
  GAUD_Stream * stream = Open(bytes, sizeof(bytes));
  GAUD_Probe_Result result = {};
  EXPECT_EQ(gaud_probe(f.registry, stream, &result), GAUD_OK);
  EXPECT_EQ(result.codec_name, nullptr);
  EXPECT_EQ(result.confidence, 0u);
  gaud_stream_destroy(stream);
}

TEST(Probe, LeavesTheStreamWhereItFoundIt) {
  // The next codec's probe is entitled to this, and so is the caller who is
  // about to hand the stream to a loader.
  Fixture f;
  GAUD_Codec wav = MakeWav();
  GAUD_Codec flac = MakeFlac();
  ASSERT_EQ(gaud_registry_register(f.registry, &wav), GAUD_OK);
  ASSERT_EQ(gaud_registry_register(f.registry, &flac), GAUD_OK);

  const unsigned char bytes[] = {'R', 'I', 'F', 'F', 1, 0, 0, 0, 'W', 'A', 'V',
      'E'};
  GAUD_Stream * stream = Open(bytes, sizeof(bytes));
  ASSERT_EQ(gaud_stream_seek(stream, 0, GAUD_SEEK_SET), GAUD_OK);

  GAUD_Probe_Result result = {};
  ASSERT_EQ(gaud_probe(f.registry, stream, &result), GAUD_OK);
  EXPECT_EQ(gaud_stream_tell(stream), 0u);
  EXPECT_STREQ(result.codec_name, "wav");
  gaud_stream_destroy(stream);
}

TEST(Probe, ATruncatedStreamDoesNotMatchOnAPrefix) {
  // The bytes present agree with WAV's first signature, but the second one
  // runs off the end. A short read must not count as a match.
  Fixture f;
  GAUD_Codec flac = MakeFlac();
  ASSERT_EQ(gaud_registry_register(f.registry, &flac), GAUD_OK);

  const unsigned char bytes[] = {'f', 'L'};
  GAUD_Stream * stream = Open(bytes, sizeof(bytes));
  GAUD_Probe_Result result = {};
  ASSERT_EQ(gaud_probe(f.registry, stream, &result), GAUD_OK);
  EXPECT_EQ(result.codec_name, nullptr);
  gaud_stream_destroy(stream);
}

TEST(Probe, AProbeFunctionCanOutrankASignature) {
  Fixture f;
  static bool called;
  called = false;

  GAUD_Codec codec = {};
  codec.abi_version = GAUD_CODEC_ABI_VERSION;
  codec.size = sizeof(GAUD_Codec);
  codec.name = "probed";
  codec.capabilities = GAUD_CAP_DECODE;
  codec.probe = [](const GAUD_Codec *, GAUD_Stream * stream,
                    unsigned * out) -> GAUD_Result {
    called = true;
    unsigned char head[4] = {};
    if (gaud_stream_read(stream, head, 4) == 4
        && memcmp(head, "deep", 4) == 0) {
      *out = GAUD_CONFIDENCE_CERTAIN;
    }
    else {
      *out = GAUD_CONFIDENCE_NONE;
    }
    return GAUD_OK;
  };
  ASSERT_EQ(gaud_registry_register(f.registry, &codec), GAUD_OK);

  const unsigned char bytes[] = {'d', 'e', 'e', 'p', '!'};
  GAUD_Stream * stream = Open(bytes, sizeof(bytes));
  GAUD_Probe_Result result = {};
  ASSERT_EQ(gaud_probe(f.registry, stream, &result), GAUD_OK);
  EXPECT_TRUE(called);
  EXPECT_STREQ(result.codec_name, "probed");
  EXPECT_EQ(result.confidence, GAUD_CONFIDENCE_CERTAIN);
  // The probe deliberately did NOT restore the position. The registry must
  // do it anyway - a third-party probe getting this wrong should not corrupt
  // the probe of the codec registered after it.
  EXPECT_EQ(gaud_stream_tell(stream), 0u);
  gaud_stream_destroy(stream);
}

TEST(Probe, TheMoreConfidentCodecWins) {
  Fixture f;
  GAUD_Codec weak = MakeWav();
  weak.name = "weak";
  weak.magics = nullptr;
  weak.magic_count = 0;
  weak.probe = [](const GAUD_Codec *, GAUD_Stream *,
                   unsigned * out) -> GAUD_Result {
    *out = GAUD_CONFIDENCE_WEAK;
    return GAUD_OK;
  };
  GAUD_Codec flac = MakeFlac();
  ASSERT_EQ(gaud_registry_register(f.registry, &weak), GAUD_OK);
  ASSERT_EQ(gaud_registry_register(f.registry, &flac), GAUD_OK);

  const unsigned char bytes[] = {'f', 'L', 'a', 'C'};
  GAUD_Stream * stream = Open(bytes, sizeof(bytes));
  GAUD_Probe_Result result = {};
  ASSERT_EQ(gaud_probe(f.registry, stream, &result), GAUD_OK);
  EXPECT_STREQ(result.codec_name, "flac")
      << "the weaker answer, registered first, won";
  gaud_stream_destroy(stream);
}

TEST(Probe, AFailingProbeIsNotAMatch) {
  Fixture f;
  GAUD_Codec codec = MakeWav();
  codec.name = "broken";
  codec.magics = nullptr;
  codec.magic_count = 0;
  codec.probe = [](const GAUD_Codec *, GAUD_Stream *,
                    unsigned * out) -> GAUD_Result {
    *out = GAUD_CONFIDENCE_CERTAIN; // written, but the result says no
    return GAUD_ERR_IO;
  };
  ASSERT_EQ(gaud_registry_register(f.registry, &codec), GAUD_OK);

  const unsigned char bytes[] = {'a', 'b', 'c', 'd'};
  GAUD_Stream * stream = Open(bytes, sizeof(bytes));
  GAUD_Probe_Result result = {};
  ASSERT_EQ(gaud_probe(f.registry, stream, &result), GAUD_OK);
  EXPECT_EQ(result.codec_name, nullptr);
  gaud_stream_destroy(stream);
}

TEST(Probe, RefusesNullArguments) {
  Fixture f;
  GAUD_Probe_Result result = {};
  const unsigned char bytes[] = {'a'};
  GAUD_Stream * stream = Open(bytes, 1);
  EXPECT_EQ(gaud_probe(f.registry, nullptr, &result), GAUD_ERR_INVALID);
  EXPECT_EQ(gaud_probe(f.registry, stream, nullptr), GAUD_ERR_INVALID);
  gaud_stream_destroy(stream);
}

TEST(Probe, AnEmptyRegistryMatchesNothing) {
  Fixture f;
  const unsigned char bytes[] = {'f', 'L', 'a', 'C'};
  GAUD_Stream * stream = Open(bytes, sizeof(bytes));
  GAUD_Probe_Result result = {};
  EXPECT_EQ(gaud_probe(f.registry, stream, &result), GAUD_OK);
  EXPECT_EQ(result.codec_name, nullptr);
  gaud_stream_destroy(stream);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
