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
 * Metadata: the container, the schemes, the round trip, and the four
 * picture states.
 *
 * The picture states are the reason this file reaches an internal header.
 * planning/audio.md 11.4 makes the verification a four-state answer, and
 * **neither container in phase 3 can produce more than one of those
 * states**: ID3v2's `APIC` states no dimensions at all, so every picture
 * read from a WAV or an AIFF is NOT_STATED. The first scheme that states
 * them is FLAC's `PICTURE` block in phase 4.
 *
 * Leaving the other three states untested until then would be leaving
 * written, shipped code that nothing has ever run - so the tests call
 * gaud_meta_picture_add_stated() and gaud_meta_verify_pictures()
 * directly, which are exactly the two functions the FLAC reader will
 * call. That reaches three states in a build with `image` and the fourth
 * in a build without, which is the arm the CI matrix exists to cover.
 */

#include "../../src/core/meta_internal.h"
#include "../../src/meta/scheme.h"
#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/audio/audio.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

/** A real 1x1 PNG, so the verifier has something it can actually decode. */
const unsigned char tiny_png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A,
    0x0A, 0x00, 0x00, 0x00, 0x0D, 'I', 'H', 'D', 'R', 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77,
    0x53, 0xDE, 0x00, 0x00, 0x00, 0x0C, 'I', 'D', 'A', 'T', 0x08, 0xD7,
    0x63, 0xF8, 0xCF, 0xC0, 0x00, 0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD,
    0x8D, 0xB0, 0x00, 0x00, 0x00, 0x00, 'I', 'E', 'N', 'D', 0xAE, 0x42,
    0x60, 0x82};

/** Write `meta` into a container in memory, then read it back. */
GAUD_Result round_trip(const char * codec, const GAUD_Meta * meta,
    GAUD_Meta_Policy policy, std::vector<unsigned char> & bytes) {
  GAUD_Stream * sink = nullptr;
  if (gaud_stream_create_memory_writer(nullptr, &sink) != GAUD_OK) {
    return GAUD_ERR_OOM;
  }
  GAUD_Encode_Params params;
  gaud_encode_params_default(&params);
  params.meta = meta;
  params.meta_policy = policy;

  GAUD_Encoder * encoder = nullptr;
  GAUD_Result result
      = gaud_encoder_create(codec, nullptr, sink, &params, &encoder);
  if (result != GAUD_OK) {
    gaud_stream_destroy(sink);
    return result;
  }
  GAUD_Buffer * buffer = nullptr;
  gaud_buffer_create(nullptr, params.format, params.layout,
      GAUD_LAYOUT_INTERLEAVED, 128, &buffer);
  gaud_buffer_set_frames(buffer, 128);
  gaud_encoder_write(encoder, buffer);
  result = gaud_encoder_finish(encoder);
  if (result == GAUD_OK) {
    const void * data = nullptr;
    size_t length = 0;
    gaud_stream_writer_bytes(sink, &data, &length);
    const unsigned char * p = (const unsigned char *)data;
    bytes.assign(p, p + length);
  }
  gaud_buffer_destroy(buffer);
  gaud_encoder_destroy(encoder);
  gaud_stream_destroy(sink);
  return result;
}

/** Load a document from bytes; the caller destroys both. */
GAUD_Result load(const std::vector<unsigned char> & bytes,
    GAUD_Stream ** out_stream, GAUD_Doc ** out_doc) {
  if (gaud_stream_create_memory(bytes.data(), bytes.size(), out_stream)
      != GAUD_OK) {
    return GAUD_ERR_OOM;
  }
  GAUD_Result result
      = gaud_doc_load(nullptr, *out_stream, nullptr, nullptr, out_doc);
  if (result != GAUD_OK) {
    gaud_stream_destroy(*out_stream);
    *out_stream = nullptr;
  }
  return result;
}

std::vector<std::string> values(const GAUD_Meta * meta, GAUD_Tag tag) {
  std::vector<std::string> out;
  for (size_t i = 0; i < gaud_meta_count(meta, tag); ++i) {
    out.push_back(gaud_meta_get(meta, tag, i));
  }
  return out;
}

} // namespace

TEST(Meta, EveryTagHasADistinctNameThatMapsBack) {
  std::vector<std::string> seen;
  for (int i = 0; i < GAUD_TAG_COUNT; ++i) {
    GAUD_Tag tag = (GAUD_Tag)i;
    const char * name = gaud_tag_name(tag);
    ASSERT_NE(name, nullptr);
    EXPECT_STRNE(name, "unknown") << "tag " << i << " has no name";
    EXPECT_EQ(std::find(seen.begin(), seen.end(), name), seen.end())
        << "two tags answer to " << name;
    seen.push_back(name);

    GAUD_Tag back;
    EXPECT_TRUE(gaud_tag_from_name(name, &back)) << name;
    EXPECT_EQ(back, tag) << name << " does not map back to itself";
  }
  GAUD_Tag ignored;
  EXPECT_FALSE(gaud_tag_from_name("not-a-tag", &ignored));
  EXPECT_STREQ(gaud_tag_name((GAUD_Tag)-1), "unknown");
  EXPECT_STREQ(gaud_tag_name((GAUD_Tag)GAUD_TAG_COUNT), "unknown");
}

TEST(Meta, AddKeepsOrderAndClearRemovesOnlyOneTag) {
  GAUD_Meta * meta = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &meta), GAUD_OK);
  EXPECT_EQ(gaud_meta_count(meta, GAUD_TAG_ARTIST), 0u);
  EXPECT_EQ(gaud_meta_get(meta, GAUD_TAG_ARTIST, 0), nullptr);

  ASSERT_EQ(gaud_meta_add(meta, GAUD_TAG_ARTIST, "first"), GAUD_OK);
  ASSERT_EQ(gaud_meta_add(meta, GAUD_TAG_ARTIST, "second"), GAUD_OK);
  ASSERT_EQ(gaud_meta_add(meta, GAUD_TAG_TITLE, "kept"), GAUD_OK);
  EXPECT_EQ(values(meta, GAUD_TAG_ARTIST),
      (std::vector<std::string>{"first", "second"}));

  /* The public add does NOT de-duplicate: a caller saying it twice has
   * said something. Only the scheme readers collapse duplicates, and
   * they use the internal adder. */
  ASSERT_EQ(gaud_meta_add(meta, GAUD_TAG_ARTIST, "first"), GAUD_OK);
  EXPECT_EQ(gaud_meta_count(meta, GAUD_TAG_ARTIST), 3u);
  ASSERT_EQ(gaud_meta_add_unique(meta, GAUD_TAG_ARTIST, "first"), GAUD_OK);
  EXPECT_EQ(gaud_meta_count(meta, GAUD_TAG_ARTIST), 3u)
      << "the internal adder must collapse an exact duplicate";

  gaud_meta_clear(meta, GAUD_TAG_ARTIST);
  EXPECT_EQ(gaud_meta_count(meta, GAUD_TAG_ARTIST), 0u);
  EXPECT_EQ(gaud_meta_count(meta, GAUD_TAG_TITLE), 1u)
      << "clearing one tag removed another";
  gaud_meta_destroy(meta);
}

TEST(Meta, ACopyOutlivesItsSourceAndKeepsEverything) {
  GAUD_Meta * source = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &source), GAUD_OK);
  gaud_meta_add(source, GAUD_TAG_TITLE, "t");
  gaud_meta_add(source, GAUD_TAG_ARTIST, "a1");
  gaud_meta_add(source, GAUD_TAG_ARTIST, "a2");
  gaud_meta_custom_add(source, "KEY", "value");
  gaud_meta_raw_attach(source, "id3v2", "PRIV", "\x01\x02\x03", 3);
  gaud_meta_picture_add(source, GAUD_PICTURE_FRONT_COVER, "image/png", "d",
      tiny_png, sizeof(tiny_png));

  GAUD_Meta * copy = nullptr;
  ASSERT_EQ(gaud_meta_copy(source, nullptr, &copy), GAUD_OK);
  gaud_meta_destroy(source); /* The copy must not point into it. */

  EXPECT_EQ(values(copy, GAUD_TAG_ARTIST),
      (std::vector<std::string>{"a1", "a2"}));
  EXPECT_STREQ(gaud_meta_get(copy, GAUD_TAG_TITLE, 0), "t");
  ASSERT_EQ(gaud_meta_custom_count(copy), 1u);
  const char * key = nullptr;
  const char * value = nullptr;
  ASSERT_EQ(gaud_meta_custom(copy, 0, &key, &value), GAUD_OK);
  EXPECT_STREQ(key, "KEY");
  EXPECT_STREQ(value, "value");
  ASSERT_EQ(gaud_meta_raw_count(copy), 1u);
  const void * raw = nullptr;
  size_t raw_size = 0;
  ASSERT_EQ(gaud_meta_raw(copy, 0, nullptr, nullptr, &raw, &raw_size),
      GAUD_OK);
  EXPECT_EQ(raw_size, 3u);
  EXPECT_EQ(std::memcmp(raw, "\x01\x02\x03", 3), 0);
  ASSERT_EQ(gaud_meta_picture_count(copy), 1u);
  EXPECT_EQ(gaud_meta_picture(copy, 0)->size, sizeof(tiny_png));
  EXPECT_EQ(std::memcmp(gaud_meta_picture(copy, 0)->data, tiny_png,
                sizeof(tiny_png)),
      0);
  gaud_meta_destroy(copy);
}

TEST(Meta, AddingManyPicturesDoesNotLeaveTheEarlierOnesDangling) {
  /* Each GAUD_Picture points into its own entry's buffers, and the entry
   * array is reallocated as it grows. If the views are not repointed
   * after a move, picture 0 is a dangling read the moment picture 5 is
   * added - and the bytes it returns look plausible. */
  GAUD_Meta * meta = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &meta), GAUD_OK);
  for (int i = 0; i < 40; ++i) {
    std::string description = "picture " + std::to_string(i);
    ASSERT_EQ(gaud_meta_picture_add(meta, GAUD_PICTURE_OTHER, "image/png",
                  description.c_str(), tiny_png, sizeof(tiny_png)),
        GAUD_OK);
  }
  ASSERT_EQ(gaud_meta_picture_count(meta), 40u);
  for (int i = 0; i < 40; ++i) {
    const GAUD_Picture * picture = gaud_meta_picture(meta, (size_t)i);
    ASSERT_NE(picture, nullptr);
    EXPECT_EQ(std::string(picture->description),
        "picture " + std::to_string(i))
        << "picture " << i << "'s description moved when the array grew";
    EXPECT_STREQ(picture->mime_type, "image/png");
    EXPECT_EQ(std::memcmp(picture->data, tiny_png, sizeof(tiny_png)), 0);
  }
  gaud_meta_destroy(meta);
}

TEST(Meta, TheFourPictureStatesAreAllReachable) {
  GAUD_Meta * meta = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &meta), GAUD_OK);

  /* 0: the container stated nothing. */
  ASSERT_EQ(gaud_meta_picture_add_stated(meta, GAUD_PICTURE_FRONT_COVER,
                "image/png", "", tiny_png, sizeof(tiny_png), 0, 0, 0, 0),
      GAUD_OK);
  /* 1: stated, and correct - the PNG really is 1x1. */
  ASSERT_EQ(gaud_meta_picture_add_stated(meta, GAUD_PICTURE_FRONT_COVER,
                "image/png", "", tiny_png, sizeof(tiny_png), 1, 1, 24, 0),
      GAUD_OK);
  /* 2: stated, and wrong. */
  ASSERT_EQ(gaud_meta_picture_add_stated(meta, GAUD_PICTURE_FRONT_COVER,
                "image/png", "", tiny_png, sizeof(tiny_png), 640, 480, 24, 0),
      GAUD_OK);
  /* 3: stated, and the payload is not an image at all. */
  const unsigned char rubbish[] = {'n', 'o', 't', ' ', 'p', 'n', 'g'};
  ASSERT_EQ(gaud_meta_picture_add_stated(meta, GAUD_PICTURE_FRONT_COVER,
                "image/png", "", rubbish, sizeof(rubbish), 8, 8, 24, 0),
      GAUD_OK);

  /* Before verification, a stated picture is UNVERIFIED and an unstated
   * one is NOT_STATED. That distinction is the whole of 11.4. */
  EXPECT_EQ(gaud_meta_picture(meta, 0)->status, GAUD_PICTURE_NOT_STATED);
  EXPECT_EQ(gaud_meta_picture(meta, 1)->status, GAUD_PICTURE_UNVERIFIED);

  gaud_meta_verify_pictures(meta);

  EXPECT_EQ(gaud_meta_picture(meta, 0)->status, GAUD_PICTURE_NOT_STATED)
      << "a container that stated nothing cannot be agreed or disagreed "
      << "with, and must not be reported as verified";

  if (gaud_have_image_validation()) {
    EXPECT_EQ(gaud_meta_picture(meta, 1)->status, GAUD_PICTURE_VERIFIED);
    EXPECT_EQ(gaud_meta_picture(meta, 1)->verified_width, 1u);
    EXPECT_EQ(gaud_meta_picture(meta, 1)->verified_height, 1u);
    EXPECT_EQ(gaud_meta_picture(meta, 2)->status, GAUD_PICTURE_MISMATCH)
        << "640x480 was stated for a 1x1 image";
    EXPECT_EQ(gaud_meta_picture(meta, 2)->verified_width, 1u);
    EXPECT_EQ(gaud_meta_picture(meta, 3)->status, GAUD_PICTURE_MISMATCH)
        << "a stated size for bytes that are not an image is a finding, "
        << "not an absence";
  }
  else {
    /* The other arm. Every stated picture stays UNVERIFIED, which says
     * "this build cannot tell you" - a different fact from NOT_STATED,
     * and the reason the enum has four values rather than three. */
    EXPECT_EQ(gaud_meta_picture(meta, 1)->status, GAUD_PICTURE_UNVERIFIED);
    EXPECT_EQ(gaud_meta_picture(meta, 2)->status, GAUD_PICTURE_UNVERIFIED);
    EXPECT_EQ(gaud_meta_picture(meta, 3)->status, GAUD_PICTURE_UNVERIFIED);
    for (size_t i = 1; i < 4; ++i) {
      EXPECT_EQ(gaud_meta_picture(meta, i)->verified_width, 0u)
          << "a build that cannot measure must not report a measurement";
    }
  }
  gaud_meta_destroy(meta);
}

TEST(Meta, TagsSurviveARoundTripThroughBothContainers) {
  GAUD_Meta * meta = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &meta), GAUD_OK);
  gaud_meta_add(meta, GAUD_TAG_TITLE, "A Title");
  gaud_meta_add(meta, GAUD_TAG_ARTIST, "First Artist");
  gaud_meta_add(meta, GAUD_TAG_ARTIST, "Second Artist");
  gaud_meta_add(meta, GAUD_TAG_ALBUM, "An Album");
  gaud_meta_add(meta, GAUD_TAG_DATE, "2026-09-30");
  gaud_meta_add(meta, GAUD_TAG_TRACK_NUMBER, "3");
  gaud_meta_add(meta, GAUD_TAG_TRACK_TOTAL, "12");
  gaud_meta_add(meta, GAUD_TAG_DISC_NUMBER, "1");
  gaud_meta_add(meta, GAUD_TAG_DISC_TOTAL, "2");
  gaud_meta_add(meta, GAUD_TAG_GENRE, "Ambient");
  gaud_meta_add(meta, GAUD_TAG_COMPOSER, "A Composer");
  gaud_meta_add(meta, GAUD_TAG_COMMENT, "caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac");
  gaud_meta_add(meta, GAUD_TAG_COPYRIGHT, "(c) 2026");
  gaud_meta_add(meta, GAUD_TAG_ISRC, "GBAYE0601498");
  gaud_meta_custom_add(meta, "REPLAYGAIN_TRACK_GAIN", "-3.21 dB");

  for (const char * codec : {"wav", "aiff"}) {
    std::vector<unsigned char> bytes;
    ASSERT_EQ(round_trip(codec, meta, GAUD_META_PRESERVE_ALL, bytes), GAUD_OK)
        << codec;
    GAUD_Stream * stream = nullptr;
    GAUD_Doc * doc = nullptr;
    ASSERT_EQ(load(bytes, &stream, &doc), GAUD_OK) << codec;
    const GAUD_Meta * back = gaud_doc_meta(doc);
    ASSERT_NE(back, nullptr);

    for (int t = 0; t < GAUD_TAG_COUNT; ++t) {
      GAUD_Tag tag = (GAUD_Tag)t;
      if (gaud_meta_count(meta, tag) == 0) {
        continue;
      }
      EXPECT_EQ(values(back, tag), values(meta, tag))
          << codec << ": " << gaud_tag_name(tag)
          << " did not survive the round trip";
    }
    /* The custom key too, and exactly once - a WAV carries its tags in
     * both LIST/INFO and id3, and a reader that appended rather than
     * merged would report everything twice. */
    ASSERT_EQ(gaud_meta_custom_count(back), 1u) << codec;
    const char * key = nullptr;
    const char * value = nullptr;
    ASSERT_EQ(gaud_meta_custom(back, 0, &key, &value), GAUD_OK);
    EXPECT_STREQ(key, "REPLAYGAIN_TRACK_GAIN") << codec;
    EXPECT_STREQ(value, "-3.21 dB") << codec;

    gaud_doc_destroy(doc);
    gaud_stream_destroy(stream);
  }
  gaud_meta_destroy(meta);
}

TEST(Meta, NonAsciiSurvivesInEveryScheme) {
  /* The encodings are where this goes wrong. A WAV carries the same
   * string through LIST/INFO, which states no encoding, and through
   * ID3v2, which states UTF-8 - and the first draft read INFO as Latin-1
   * and handed back doubled-up mojibake for its own output. */
  const char * const samples[] = {
      "caf\xc3\xa9",                 /* Latin-1 range, two bytes in UTF-8 */
      "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", /* outside Latin-1 entirely */
      "\xf0\x9f\x8e\xb5",            /* above the BMP: a surrogate pair */
      "plain ascii",
  };
  for (const char * sample : samples) {
    GAUD_Meta * meta = nullptr;
    ASSERT_EQ(gaud_meta_create(nullptr, &meta), GAUD_OK);
    gaud_meta_add(meta, GAUD_TAG_TITLE, sample);
    for (const char * codec : {"wav", "aiff"}) {
      std::vector<unsigned char> bytes;
      ASSERT_EQ(round_trip(codec, meta, GAUD_META_PRESERVE_ALL, bytes),
          GAUD_OK);
      GAUD_Stream * stream = nullptr;
      GAUD_Doc * doc = nullptr;
      ASSERT_EQ(load(bytes, &stream, &doc), GAUD_OK);
      EXPECT_STREQ(gaud_meta_get(gaud_doc_meta(doc), GAUD_TAG_TITLE, 0),
          sample)
          << codec << " mangled " << sample;
      gaud_doc_destroy(doc);
      gaud_stream_destroy(stream);
    }
    gaud_meta_destroy(meta);
  }
}

TEST(Meta, EveryPolicyWritesWhatItSaysAndNothingElse) {
  GAUD_Meta * meta = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &meta), GAUD_OK);
  gaud_meta_add(meta, GAUD_TAG_TITLE, "A Title");
  /* A raw block in this scheme's own vocabulary, so KEEP_RAW_ONLY has
   * something to keep. `PRIV` is a real ID3v2 frame with no mapping. */
  gaud_meta_raw_attach(meta, "id3v2", "PRIV", "\x01\x02\x03\x04", 4);

  struct {
    GAUD_Meta_Policy policy;
    bool expect_common;
    bool expect_raw;
    const char * name;
  } cases[] = {
      {GAUD_META_PRESERVE_ALL, true, true, "preserve-all"},
      {GAUD_META_DROP_ALL, false, false, "drop-all"},
      {GAUD_META_KEEP_COMMON_ONLY, true, false, "keep-common"},
      {GAUD_META_KEEP_RAW_ONLY, false, true, "keep-raw"},
  };
  for (const auto & twhich : cases) {
    std::vector<unsigned char> bytes;
    ASSERT_EQ(round_trip("wav", meta, twhich.policy, bytes), GAUD_OK)
        << twhich.name;
    GAUD_Stream * stream = nullptr;
    GAUD_Doc * doc = nullptr;
    ASSERT_EQ(load(bytes, &stream, &doc), GAUD_OK) << twhich.name;
    const GAUD_Meta * back = gaud_doc_meta(doc);

    EXPECT_EQ(gaud_meta_count(back, GAUD_TAG_TITLE) > 0, twhich.expect_common)
        << twhich.name << " got the common tags wrong";
    bool found_raw = false;
    for (size_t i = 0; i < gaud_meta_raw_count(back); ++i) {
      const char * id = nullptr;
      if (gaud_meta_raw(back, i, nullptr, &id, nullptr, nullptr) == GAUD_OK
          && std::strcmp(id, "PRIV") == 0) {
        found_raw = true;
      }
    }
    EXPECT_EQ(found_raw, twhich.expect_raw)
        << twhich.name << " got the raw blocks wrong";
    gaud_doc_destroy(doc);
    gaud_stream_destroy(stream);
  }
  gaud_meta_destroy(meta);
}

TEST(Meta, AFileWithNoTagsGivesAnEmptyMetaAndNotNull) {
  GAUD_Meta * empty = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &empty), GAUD_OK);
  std::vector<unsigned char> bytes;
  ASSERT_EQ(round_trip("wav", empty, GAUD_META_DROP_ALL, bytes), GAUD_OK);
  gaud_meta_destroy(empty);

  GAUD_Stream * stream = nullptr;
  GAUD_Doc * doc = nullptr;
  ASSERT_EQ(load(bytes, &stream, &doc), GAUD_OK);
  const GAUD_Meta * back = gaud_doc_meta(doc);
  ASSERT_NE(back, nullptr)
      << "an untagged file must still answer; a caller should never have to "
      << "tell 'no tags' from 'nothing to ask'";
  EXPECT_EQ(gaud_meta_count(back, GAUD_TAG_TITLE), 0u);
  EXPECT_EQ(gaud_meta_custom_count(back), 0u);
  EXPECT_EQ(gaud_meta_picture_count(back), 0u);
  gaud_doc_destroy(doc);
  gaud_stream_destroy(stream);
}

TEST(Meta, PicturesSurviveARoundTrip) {
  GAUD_Meta * meta = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &meta), GAUD_OK);
  ASSERT_EQ(gaud_meta_picture_add(meta, GAUD_PICTURE_FRONT_COVER,
                "image/png", "the cover", tiny_png, sizeof(tiny_png)),
      GAUD_OK);
  ASSERT_EQ(gaud_meta_picture_add(meta, GAUD_PICTURE_BACK_COVER,
                "image/png", "", tiny_png, sizeof(tiny_png)),
      GAUD_OK);

  for (const char * codec : {"wav", "aiff"}) {
    std::vector<unsigned char> bytes;
    ASSERT_EQ(round_trip(codec, meta, GAUD_META_PRESERVE_ALL, bytes), GAUD_OK);
    GAUD_Stream * stream = nullptr;
    GAUD_Doc * doc = nullptr;
    ASSERT_EQ(load(bytes, &stream, &doc), GAUD_OK) << codec;
    const GAUD_Meta * back = gaud_doc_meta(doc);
    ASSERT_EQ(gaud_meta_picture_count(back), 2u) << codec;

    const GAUD_Picture * front = gaud_meta_picture(back, 0);
    EXPECT_EQ(front->kind, GAUD_PICTURE_FRONT_COVER) << codec;
    EXPECT_STREQ(front->mime_type, "image/png") << codec;
    EXPECT_STREQ(front->description, "the cover") << codec;
    ASSERT_EQ(front->size, sizeof(tiny_png)) << codec;
    EXPECT_EQ(std::memcmp(front->data, tiny_png, sizeof(tiny_png)), 0)
        << codec << ": the image bytes changed";
    /* APIC states no dimensions, in either container, in every build. */
    EXPECT_EQ(front->status, GAUD_PICTURE_NOT_STATED) << codec;

    EXPECT_EQ(gaud_meta_picture(back, 1)->kind, GAUD_PICTURE_BACK_COVER)
        << codec;
    EXPECT_STREQ(gaud_meta_picture(back, 1)->description, "") << codec;
    gaud_doc_destroy(doc);
    gaud_stream_destroy(stream);
  }
  gaud_meta_destroy(meta);
}

namespace {

/** An ID3v2.4 tag holding one text frame, built by hand. */
std::vector<unsigned char> id3v24(const char * id, const std::string & body) {
  std::vector<unsigned char> frame;
  frame.insert(frame.end(), id, id + 4);
  size_t size = body.size() + 1u; /* the encoding byte */
  frame.push_back((unsigned char)((size >> 21) & 0x7F));
  frame.push_back((unsigned char)((size >> 14) & 0x7F));
  frame.push_back((unsigned char)((size >> 7) & 0x7F));
  frame.push_back((unsigned char)(size & 0x7F));
  frame.push_back(0);
  frame.push_back(0);
  frame.push_back(3); /* UTF-8 */
  frame.insert(frame.end(), body.begin(), body.end());

  std::vector<unsigned char> tag = {'I', 'D', '3', 4, 0, 0};
  size_t total = frame.size();
  tag.push_back((unsigned char)((total >> 21) & 0x7F));
  tag.push_back((unsigned char)((total >> 14) & 0x7F));
  tag.push_back((unsigned char)((total >> 7) & 0x7F));
  tag.push_back((unsigned char)(total & 0x7F));
  tag.insert(tag.end(), frame.begin(), frame.end());
  return tag;
}

std::string first_genre(const std::string & written) {
  GAUD_Meta * meta = nullptr;
  if (gaud_meta_create(nullptr, &meta) != GAUD_OK) {
    return "<oom>";
  }
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  std::vector<unsigned char> tag = id3v24("TCON", written);
  std::string out = "<none>";
  if (gaud_id3v2_parse(tag.data(), tag.size(), &limits, meta, nullptr)
      == GAUD_OK) {
    const char * value = gaud_meta_get(meta, GAUD_TAG_GENRE, 0);
    out = value ? value : "<none>";
  }
  gaud_meta_destroy(meta);
  return out;
}

} // namespace

TEST(Meta, GenreNormalisationReachesAFixedPoint) {
  /* A reader that normalises must be idempotent, or a round trip
   * changes the file a little each time. Both rows marked below were
   * found by `make fuzz-run-tags` as build/re-parse/re-build
   * instability, and neither is reachable from a file any tagger
   * writes - which is exactly why nothing else found them. */
  struct {
    const char * written;
    const char * expected;
    const char * why;
  } cases[] = {
      {"Ambient", "Ambient", "plain text passes through"},
      {"17", "Rock", "2.4 allows the bare number"},
      {"(17)", "Rock", "2.3's parenthesised form"},
      {"(17)Hard Rock", "Hard Rock", "the spelled-out text wins"},
      {"(0)2", "Country", "the refinement is itself a number"},
      {"(0)(666)x", "x", "the prefix may repeat"},
      {"(0)(17)", "Rock", "repeated, with nothing after the last"},
      {"(RX)", "(RX)", "not a number, so it is the text"},
      {"9999", "9999", "past the table, so it stays as it was"},
      {"", "<none>", "an empty value is not a genre"},
  };
  for (const auto & twhich : cases) {
    std::string once = first_genre(twhich.written);
    EXPECT_EQ(once, twhich.expected)
        << twhich.written << ": " << twhich.why;
    if (once == "<none>") {
      continue;
    }
    /* The fixed point: reading what we would write back returns the
     * same thing. This is the assertion, not the row above it. */
    EXPECT_EQ(first_genre(once), once)
        << twhich.written << " normalised to " << once
        << ", which normalises again to " << first_genre(once)
        << " - so every rewrite changes the genre";
  }
}

TEST(Meta, AMalformedKnownFrameIsKeptRatherThanDropped) {
  /* A one-byte COMM cannot be parsed - it needs an encoding byte, three
   * language bytes and a terminator - but it must not vanish. The rule
   * that an uninterpretable frame is kept applies to a malformed known
   * frame as much as to an unrecognised identifier, and the first draft
   * applied it only to the second. */
  GAUD_Meta * meta = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &meta), GAUD_OK);
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  std::vector<unsigned char> tag = id3v24("COMM", "");
  /* id3v24() writes an encoding byte, so the body is one byte long. */
  ASSERT_EQ(gaud_id3v2_parse(tag.data(), tag.size(), &limits, meta, nullptr),
      GAUD_OK);
  EXPECT_EQ(gaud_meta_count(meta, GAUD_TAG_COMMENT), 0u)
      << "a one-byte COMM holds no comment";
  ASSERT_EQ(gaud_meta_raw_count(meta), 1u)
      << "and it must still be there, kept raw";
  const char * id = nullptr;
  ASSERT_EQ(gaud_meta_raw(meta, 0, nullptr, &id, nullptr, nullptr), GAUD_OK);
  EXPECT_STREQ(id, "COMM");
  gaud_meta_destroy(meta);
}

TEST(Meta, TheEntryLimitCountsValuesAndNotFrames) {
  /* One ID3v2.4 text frame carries any number of NUL-separated values,
   * so a cap on "entries" that counts frames is not a cap at all. */
  GAUD_Meta * meta = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &meta), GAUD_OK);
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  limits.max_metadata_entries = 3;

  std::string many = "a";
  for (int i = 1; i < 20; ++i) {
    many += '\0';
    many += (char)('a' + i);
  }
  std::vector<unsigned char> tag = id3v24("TPE1", many);
  EXPECT_EQ(gaud_id3v2_parse(tag.data(), tag.size(), &limits, meta, nullptr),
      GAUD_ERR_LIMIT)
      << "twenty values under a cap of three were accepted";
  gaud_meta_destroy(meta);

  /* And the cap is not simply refusing everything: three fit. */
  GAUD_Meta * small = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &small), GAUD_OK);
  std::string few = "a";
  few += '\0';
  few += "b";
  std::vector<unsigned char> ok = id3v24("TPE1", few);
  EXPECT_EQ(gaud_id3v2_parse(ok.data(), ok.size(), &limits, small, nullptr),
      GAUD_OK);
  EXPECT_EQ(gaud_meta_count(small, GAUD_TAG_ARTIST), 2u);
  gaud_meta_destroy(small);
}

TEST(Meta, AFrameIdentifierOutsideAZ09StopsTheWalk) {
  /* The identifier reaches a caller as a custom key and as a raw
   * block's id, and every string this API returns is UTF-8 - so a high
   * byte there would break the promise. It also means the walk has lost
   * its place, and continuing invents frames out of whatever follows. */
  GAUD_Meta * meta = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &meta), GAUD_OK);
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  GAUD_Diagnostics diagnostics = {};
  std::vector<unsigned char> tag = id3v24("TÕNN", "value");
  EXPECT_EQ(
      gaud_id3v2_parse(tag.data(), tag.size(), &limits, meta, &diagnostics),
      GAUD_OK);
  EXPECT_EQ(gaud_meta_custom_count(meta), 0u);
  EXPECT_EQ(gaud_meta_raw_count(meta), 0u);
  EXPECT_GT(diagnostics.count, 0u)
      << "refusing a frame silently is worse than refusing it";
  gaud_diagnostics_destroy(&diagnostics);
  gaud_meta_destroy(meta);
}

TEST(Meta, RebuildingWhatWeBuiltProducesTheSameBytes) {
  /* The property `make fuzz-run-tags` asserts, as a test with named
   * cases: build, re-parse, re-build must be byte-identical. Six
   * defects in this parser were only ever visible as a difference here,
   * because each of them changed the value by a fixed amount per pass
   * and a single round trip looked correct. */
  GAUD_Meta * meta = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &meta), GAUD_OK);
  gaud_meta_add(meta, GAUD_TAG_TITLE, "A Title");
  gaud_meta_add(meta, GAUD_TAG_ARTIST, "First");
  gaud_meta_add(meta, GAUD_TAG_ARTIST, "Second");
  gaud_meta_add(meta, GAUD_TAG_GENRE, "Rock");
  gaud_meta_add(meta, GAUD_TAG_TRACK_NUMBER, "3");
  gaud_meta_add(meta, GAUD_TAG_TRACK_TOTAL, "12");
  gaud_meta_add(meta, GAUD_TAG_COMMENT, "caf\xc3\xa9");
  gaud_meta_custom_add(meta, "REPLAYGAIN_TRACK_GAIN", "-3.21 dB");
  gaud_meta_raw_attach(meta, "id3v2", "PRIV", "\x01\x02", 2);
  gaud_meta_picture_add(meta, GAUD_PICTURE_FRONT_COVER, "image/png", "c",
      tiny_png, sizeof(tiny_png));

  unsigned char * first = nullptr;
  size_t first_size = 0;
  ASSERT_EQ(gaud_id3v2_build(meta, GAUD_META_PRESERVE_ALL, nullptr, &first,
                &first_size),
      GAUD_OK);
  ASSERT_GT(first_size, 0u);
  gaud_meta_destroy(meta);

  GAUD_Meta * again = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &again), GAUD_OK);
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  ASSERT_EQ(gaud_id3v2_parse(first, first_size, &limits, again, nullptr),
      GAUD_OK);
  unsigned char * second = nullptr;
  size_t second_size = 0;
  ASSERT_EQ(gaud_id3v2_build(again, GAUD_META_PRESERVE_ALL, nullptr, &second,
                &second_size),
      GAUD_OK);

  ASSERT_EQ(second_size, first_size) << "the tag changed size on a rewrite";
  EXPECT_EQ(std::memcmp(first, second, first_size), 0)
      << "the tag changed content on a rewrite";
  gaud_meta_destroy(again);
  gcu_allocator_free(gaud_allocator_default(), first);
  gcu_allocator_free(gaud_allocator_default(), second);
}

/* ------------------------------------------- buffers that have to grow */

TEST(Meta, AVorbisCommentBlockOutgrowsTheBuildersFirstBuffer) {
  /* The builder starts at 256 bytes and doubles, copying what it already
   * holds into the new allocation. `make coverage` showed that copy had
   * never executed: it grew twenty-three times across the whole suite and
   * every one of those was the first allocation, where there is nothing
   * to copy. A reallocation path no test reaches is untested, not
   * working, and an off-by-one in the length copied would lose or
   * duplicate bytes in the middle of a block that still parses.
   *
   * Forty comments is several doublings, so the copy runs more than once
   * and at more than one size. */
  GAUD_Meta * meta = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &meta), GAUD_OK);
  std::vector<std::string> expected;
  for (int i = 0; i < 40; ++i) {
    expected.push_back(
        "comment number " + std::to_string(i) + ", long enough to matter");
    ASSERT_EQ(gaud_meta_add(meta, GAUD_TAG_COMMENT, expected.back().c_str()),
        GAUD_OK);
  }

  unsigned char * block = nullptr;
  size_t size = 0;
  ASSERT_EQ(gaud_vorbis_comment_build(
                meta, "ghoti.io test", nullptr, &block, &size),
      GAUD_OK);
  ASSERT_NE(block, nullptr);
  /* Asserted, because the point of the test is the growth: a builder
   * whose first buffer happened to be large enough would pass every
   * comparison below without ever copying anything. */
  ASSERT_GT(size, 256u) << "the block never outgrew the first allocation";
  gaud_meta_destroy(meta);

  GAUD_Meta * back = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &back), GAUD_OK);
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  ASSERT_EQ(
      gaud_vorbis_comment_parse(block, size, &limits, back, nullptr), GAUD_OK);
  ASSERT_EQ(gaud_meta_count(back, GAUD_TAG_COMMENT), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_STREQ(gaud_meta_get(back, GAUD_TAG_COMMENT, i), expected[i].c_str())
        << "comment " << i << " came back wrong, which is what a bad copy "
        << "length looks like once the buffer has moved";
  }
  gaud_meta_destroy(back);
  gcu_allocator_free(gaud_allocator_default(), block);
}

TEST(Meta, RiffInfoWritesACustomKeyOnlyWhenItIsAlreadyAFourCharacterId) {
  /* INFO has no user-defined entry, so a custom key can only be written
   * when it is already a four-character chunk id; anything longer has no
   * spelling and truncating it would invent an id that means something
   * else. Both arms are here because only the refusal had ever run - the
   * branch that writes one had been reached once and taken never. */
  GAUD_Meta * meta = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &meta), GAUD_OK);
  /* ITCH is a real INFO id - the technician - and deliberately one this
   * library has no tag for, so it stays a custom entry on the way back
   * instead of being mapped and proving nothing. */
  ASSERT_EQ(gaud_meta_custom_add(meta, "ITCH", "the technician"), GAUD_OK);
  ASSERT_EQ(gaud_meta_custom_add(meta, "ENGINEER", "no four-character id"),
      GAUD_OK);

  unsigned char * chunk = nullptr;
  size_t size = 0;
  ASSERT_EQ(gaud_riff_info_build(meta, nullptr, &chunk, &size), GAUD_OK);
  ASSERT_NE(chunk, nullptr);
  gaud_meta_destroy(meta);

  GAUD_Meta * back = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &back), GAUD_OK);
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  /* The chunk is "LIST", its length and "INFO" before the entries; the
   * parser takes the body after that header. */
  ASSERT_GT(size, 12u);
  ASSERT_EQ(gaud_riff_info_parse(
                chunk + 12u, size - 12u, &limits, back, nullptr),
      GAUD_OK);

  bool found_itch = false;
  bool found_engineer = false;
  for (size_t i = 0; i < gaud_meta_custom_count(back); ++i) {
    const char * key = nullptr;
    const char * value = nullptr;
    ASSERT_EQ(gaud_meta_custom(back, i, &key, &value), GAUD_OK);
    if (std::strcmp(key, "ITCH") == 0) {
      found_itch = true;
      EXPECT_STREQ(value, "the technician");
    }
    if (std::strcmp(key, "ENGINEER") == 0) {
      found_engineer = true;
    }
  }
  EXPECT_TRUE(found_itch) << "a four-character custom key was dropped";
  EXPECT_FALSE(found_engineer) << "a key INFO cannot spell was written anyway";
  gaud_meta_destroy(back);
  gcu_allocator_free(gaud_allocator_default(), chunk);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  gaud_register_builtin_codecs();
  return RUN_ALL_TESTS();
}
