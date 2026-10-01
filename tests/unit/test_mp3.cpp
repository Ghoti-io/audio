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
 * MPEG audio: the frame header, the length tags, and what the loader
 * concludes.
 *
 * Two kinds of test, and the difference matters:
 *
 * **Hand-built headers and frames**, for the arithmetic. Every reserved
 * encoding, every frame length the standard tabulates, and the three
 * fields whose encoding is inverted or offset. A corpus cannot test these:
 * no encoder writes a reserved value, and the frame lengths an encoder
 * does write are the handful its settings produce.
 *
 * **The fixtures, for the lengths.** The frame counts and trims asserted
 * below are not this library's own arithmetic written down twice. Every one
 * of them was measured against ffmpeg and libsndfile - which are two
 * genuinely separate MPEG decoders, unlike FLAC's four names for libFLAC -
 * by decoding each fixture and counting the samples that came out. Fifteen
 * of the sixteen agree with both; the sixteenth is named below with the
 * minimal pair that shows which reference is wrong.
 *
 * **Sample values are not asserted here.** `make check-mpeg` scores every
 * fixture's decode against ffmpeg and libsndfile - two separate MPEG
 * implementations - per channel, in level, in offset and in length, with
 * a control that must fail. What is here instead is everything a
 * reference cannot be asked about: that the frame count is the one the
 * loader stated, that the buffer size does not change the samples, that a
 * seek lands on the sample it was asked for, and that the generated
 * tables are still the standard's.
 */

#include "../../src/codec/mp3/mp3_internal.h"
#include "../../src/codec/mp3/mp3_tables.h"
#include <ghoti.io/audio/audio.h>
#include <ghoti.io/audio/codec_sdk.h>
#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

/** Registers the codecs once, however many tests run. */
struct Registered {
  Registered() {
    gaud_register_builtin_codecs();
  }
};
const Registered registered;

std::string Fixture(const char * name) {
  return std::string(GAUD_TEST_DATA) + "/" + name;
}

/* The header's own field encodings, by name, so that a test reads as the
 * standard's table rather than as four magic numbers. */
enum { V25 = 0, VRESERVED = 1, V2 = 2, V1 = 3 };
enum { LRESERVED = 0, L3 = 1, L2 = 2, L1 = 3 };

/** Assemble the four bytes of a frame header from its fields. */
std::vector<unsigned char> Header(unsigned version, unsigned layer, bool crc,
    unsigned bitrate_index, unsigned rate_index, bool pad, unsigned mode,
    unsigned mode_extension = 0, unsigned emphasis = 0) {
  std::vector<unsigned char> h(4, 0);
  h[0] = 0xFFu;
  /* The protection bit is set when there is NO cyclic redundancy check. */
  h[1] = (unsigned char)(0xE0u | (version << 3) | (layer << 1)
      | (crc ? 0u : 1u));
  h[2] = (unsigned char)((bitrate_index << 4) | (rate_index << 2)
      | (pad ? 2u : 0u));
  h[3] = (unsigned char)((mode << 6) | (mode_extension << 4) | emphasis);
  return h;
}

/** A whole frame of the shape @p header describes, zero-filled after it. */
std::vector<unsigned char> Frame(const std::vector<unsigned char> & header) {
  MP3_Header parsed;
  EXPECT_TRUE(gaud_mp3_header_parse(header.data(), &parsed));
  std::vector<unsigned char> frame(parsed.frame_size, 0);
  std::memcpy(frame.data(), header.data(), 4);
  return frame;
}

/**
 * Put a Xing or Info tag into @p frame, with all four optional fields and a
 * LAME extension.
 *
 * @param magic "Xing" or "Info".
 * @param encoder The nine characters of the extension's encoder string, or
 *   NULL to leave the extension's space as the zeros a writer that stopped
 *   after the Xing fields would leave.
 */
void PutXing(std::vector<unsigned char> & frame, const char * magic,
    uint32_t frames, uint32_t bytes, const char * encoder, uint32_t delay,
    uint32_t padding) {
  MP3_Header header;
  ASSERT_TRUE(gaud_mp3_header_parse(frame.data(), &header));
  size_t at
      = 4u + (header.crc_present ? 2u : 0u) + gaud_mp3_side_info_size(&header);
  ASSERT_LE(at + 156u + 36u, frame.size());
  std::memcpy(frame.data() + at, magic, 4);
  /* All four flags: frames, bytes, table of contents, quality. */
  frame[at + 4] = 0;
  frame[at + 5] = 0;
  frame[at + 6] = 0;
  frame[at + 7] = 0x0Fu;
  for (unsigned i = 0; i < 4u; ++i) {
    frame[at + 8 + i] = (unsigned char)(frames >> (24 - 8 * i));
    frame[at + 12 + i] = (unsigned char)(bytes >> (24 - 8 * i));
  }
  /* 100 bytes of table of contents and four of quality follow, left zero. */
  size_t lame = at + 8u + 4u + 4u + 100u + 4u;
  if (!encoder) {
    return;
  }
  std::memcpy(frame.data() + lame, encoder, 9);
  frame[lame + 21] = (unsigned char)(delay >> 4);
  frame[lame + 22]
      = (unsigned char)(((delay & 0x0Fu) << 4) | ((padding >> 8) & 0x0Fu));
  frame[lame + 23] = (unsigned char)(padding & 0xFFu);
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

GAUD_Result OpenBytes(Loaded & out, const std::vector<unsigned char> & bytes) {
  GAUD_Result result
      = gaud_stream_create_memory(bytes.data(), bytes.size(), &out.stream);
  if (result != GAUD_OK) {
    return result;
  }
  return gaud_doc_load(
      nullptr, out.stream, nullptr, &out.diagnostics, &out.doc);
}

/* --------------------------------------------------------- frame header */

TEST(Mp3Header, TheLayerFieldCountsDown) {
  /* `11` is Layer I and `01` is Layer III. A reader that treats the field
   * as the layer number decodes a Layer III file as Layer I, which is a
   * different frame length and a different number of samples. */
  const struct {
    unsigned field;
    unsigned layer;
  } cases[] = {{L1, 1u}, {L2, 2u}, {L3, 3u}};
  for (const auto & one : cases) {
    MP3_Header header;
    ASSERT_TRUE(gaud_mp3_header_parse(
        Header(V1, one.field, false, 9u, 0u, false, 0u).data(), &header))
        << "layer field " << one.field;
    EXPECT_EQ(header.layer, one.layer);
  }
  MP3_Header header;
  EXPECT_FALSE(gaud_mp3_header_parse(
      Header(V1, LRESERVED, false, 9u, 0u, false, 0u).data(), &header))
      << "the reserved layer encoding must be refused";
}

TEST(Mp3Header, TheVersionFieldsReservedValueIsInTheMiddle) {
  /* 00 is MPEG-2.5, 01 is reserved, 10 is MPEG-2, 11 is MPEG-1. Read as a
   * number the order is meaningless, and the reserved code sits between
   * two live ones rather than at either end. */
  MP3_Header header;
  ASSERT_TRUE(gaud_mp3_header_parse(
      Header(V25, L3, false, 5u, 0u, false, 3u).data(), &header));
  EXPECT_EQ(header.version, MP3_MPEG25);
  EXPECT_EQ(header.sample_rate, 11025u);
  EXPECT_EQ(header.samples, 576u);

  ASSERT_TRUE(gaud_mp3_header_parse(
      Header(V2, L3, false, 5u, 0u, false, 3u).data(), &header));
  EXPECT_EQ(header.version, MP3_MPEG2);
  EXPECT_EQ(header.sample_rate, 22050u);
  EXPECT_EQ(header.samples, 576u);

  ASSERT_TRUE(gaud_mp3_header_parse(
      Header(V1, L3, false, 5u, 0u, false, 3u).data(), &header));
  EXPECT_EQ(header.version, MP3_MPEG1);
  EXPECT_EQ(header.sample_rate, 44100u);
  EXPECT_EQ(header.samples, 1152u);

  EXPECT_FALSE(gaud_mp3_header_parse(
      Header(VRESERVED, L3, false, 5u, 0u, false, 3u).data(), &header))
      << "the reserved version encoding must be refused";
}

TEST(Mp3Header, TheProtectionBitIsNegativeLogic) {
  /* Set means no checksum, which is the common case - so a reader with the
   * sense inverted is wrong about nearly every file rather than about an
   * unusual one, and is wrong by the two bytes it then skips. */
  MP3_Header header;
  ASSERT_TRUE(gaud_mp3_header_parse(
      Header(V1, L3, false, 9u, 0u, false, 0u).data(), &header));
  EXPECT_FALSE(header.crc_present);
  ASSERT_TRUE(gaud_mp3_header_parse(
      Header(V1, L3, true, 9u, 0u, false, 0u).data(), &header));
  EXPECT_TRUE(header.crc_present);
}

TEST(Mp3Header, FrameLengthsAreTheOnesTheStandardTabulates) {
  /* The first row is the one that decides whether the Layer I arithmetic
   * was written correctly. Layer I is measured in four-byte slots, so the
   * slot count is floored and *then* scaled: 12 x 448000 / 44100 = 121.9,
   * which is 121 slots and 484 bytes. Folding the four into the numerator
   * gives 48 x 448000 / 44100 = 487.6, so 487 - a length three bytes too
   * long, on a file that otherwise decodes. Both readings are plausible
   * and only one is the format. */
  const struct {
    const char * what;
    unsigned version;
    unsigned layer;
    unsigned bitrate_index;
    unsigned rate_index;
    bool pad;
    uint32_t expect;
  } cases[] = {
      {"MPEG-1 Layer I, 448 kbit/s, 44.1 kHz", V1, L1, 14u, 0u, false, 484u},
      {"...the same, padded, so one more slot", V1, L1, 14u, 0u, true, 488u},
      {"MPEG-1 Layer I, 32 kbit/s, 32 kHz", V1, L1, 1u, 2u, false, 48u},
      {"MPEG-1 Layer III, 128 kbit/s, 44.1 kHz", V1, L3, 9u, 0u, false, 417u},
      {"...padded, so one byte more", V1, L3, 9u, 0u, true, 418u},
      {"MPEG-1 Layer III, 320 kbit/s, 48 kHz", V1, L3, 14u, 1u, false, 960u},
      {"MPEG-1 Layer II, 384 kbit/s, 32 kHz", V1, L2, 14u, 2u, false, 1728u},
      /* MPEG-2 and 2.5 Layer III have one granule, so the numerator is 72
       * and not 144. A reader that keeps 144 produces frames twice as long
       * as they are and loses sync on the second one. */
      {"MPEG-2 Layer III, 64 kbit/s, 22.05 kHz", V2, L3, 8u, 0u, false, 208u},
      {"MPEG-2.5 Layer III, 32 kbit/s, 11.025 kHz", V25, L3, 4u, 0u, false,
          208u},
      {"MPEG-2 Layer II, 160 kbit/s, 16 kHz", V2, L2, 14u, 2u, false, 1440u},
  };
  for (const auto & one : cases) {
    MP3_Header header;
    ASSERT_TRUE(gaud_mp3_header_parse(
        Header(one.version, one.layer, false, one.bitrate_index, one.rate_index,
            one.pad, 0u)
            .data(),
        &header))
        << one.what;
    EXPECT_EQ(header.frame_size, one.expect) << one.what;
  }
}

TEST(Mp3Header, ReservedAndImpossibleEncodingsAreRefused) {
  /* Finding a frame means testing every byte of a file as a candidate, so
   * each of these refusals is what keeps a false positive rare. */
  MP3_Header header;
  struct Case {
    const char * what;
    std::vector<unsigned char> bytes;
  };
  std::vector<Case> cases = {
      {"the sync word is eleven bits, not eight", {0xFFu, 0xC0u, 0x90u, 0x00u}},
      {"a first byte that is not 0xFF", {0xFEu, 0xFBu, 0x90u, 0x00u}},
      {"bitrate index 15 is reserved",
          Header(V1, L3, false, 15u, 0u, false, 0u)},
      {"sample rate index 3 is reserved",
          Header(V1, L3, false, 9u, 3u, false, 0u)},
  };
  for (const auto & one : cases) {
    EXPECT_FALSE(gaud_mp3_header_parse(one.bytes.data(), &header)) << one.what;
  }
  /* Bitrate index 0 is the free format: legal, and refused later by name
   * rather than here, because a header that parses is how the loader knows
   * to say which format it is declining. */
  ASSERT_TRUE(gaud_mp3_header_parse(
      Header(V1, L3, false, 0u, 0u, false, 0u).data(), &header));
  EXPECT_EQ(header.bitrate, 0u);
  EXPECT_EQ(header.frame_size, 0u);
}

TEST(Mp3Header, SideInformationFollowsTheVersionAndTheChannelCount) {
  const struct {
    unsigned version;
    unsigned layer;
    unsigned mode;
    uint32_t expect;
  } cases[] = {
      {V1, L3, 0u, 32u}, /* MPEG-1 stereo: two granules, two channels */
      {V1, L3, 3u, 17u}, /* MPEG-1 mono */
      {V2, L3, 0u, 17u}, /* MPEG-2 stereo: one granule */
      {V2, L3, 3u, 9u},  /* MPEG-2 mono */
      {V25, L3, 3u, 9u}, /* MPEG-2.5 mono */
      {V1, L2, 0u, 0u},  /* Layer II has no side information at all */
      {V1, L1, 0u, 0u},
  };
  for (const auto & one : cases) {
    MP3_Header header;
    ASSERT_TRUE(gaud_mp3_header_parse(
        Header(one.version, one.layer, false, 5u, 0u, false, one.mode).data(),
        &header));
    EXPECT_EQ(gaud_mp3_side_info_size(&header), one.expect)
        << "version field " << one.version << " layer field " << one.layer
        << " mode " << one.mode;
  }
}

TEST(Mp3Bands, EveryVersionAndRateHasABandRowAndNoneIsOutOfRange) {
  /* gaud_mp3_band_row() is the only place that knows which rows the
   * generated band tables have, so an index it returns past the end of
   * them is a read off the end of a table on every frame of a legal
   * file. Every (version, rate index) the header can carry is asked. */
  for (unsigned version : {0u, 2u, 3u}) {
    for (unsigned rate = 0; rate < 3u; ++rate) {
      MP3_Header header;
      ASSERT_TRUE(gaud_mp3_header_parse(
          Header(version, L3, false, 5u, rate, false, 0u).data(), &header))
          << "version " << version << " rate index " << rate;
      unsigned row = 99u;
      ASSERT_TRUE(gaud_mp3_band_row(&header, &row))
          << "no band row for version field " << version << " rate index "
          << rate;
      ASSERT_LT(row, 7u) << "row " << row << " is past the tables";
      /* And the row it chose is a partition of the spectrum. A row that
       * exists but stops short leaves lines with no band, which is the
       * defect phase 5 found; a row that overshoots indexes past 576. */
      unsigned longs = gaud_mp3_sfb_long_bands[row];
      unsigned shorts = gaud_mp3_sfb_short_bands[row];
      EXPECT_EQ(gaud_mp3_sfb_long[row][0], 0u);
      EXPECT_EQ(gaud_mp3_sfb_long[row][longs], 576u)
          << "the long row for " << header.sample_rate << " Hz ends at "
          << gaud_mp3_sfb_long[row][longs];
      EXPECT_EQ(gaud_mp3_sfb_short[row][0], 0u);
      EXPECT_EQ(gaud_mp3_sfb_short[row][shorts], 192u)
          << "the short row for " << header.sample_rate << " Hz ends at "
          << gaud_mp3_sfb_short[row][shorts];
      for (unsigned band = 0; band < longs; ++band) {
        EXPECT_LT(gaud_mp3_sfb_long[row][band],
            gaud_mp3_sfb_long[row][band + 1u])
            << "long band " << band << " of row " << row << " is empty or "
            << "runs backwards";
      }
      for (unsigned band = 0; band < shorts; ++band) {
        EXPECT_LT(gaud_mp3_sfb_short[row][band],
            gaud_mp3_sfb_short[row][band + 1u])
            << "short band " << band << " of row " << row;
      }
    }
  }
}

TEST(Mp3Bands, TheTwoRatesThatShareTheSixteenKilohertzRowReallyDo) {
  /* **This is the premise gaud_mp3_band_row() rests on**, and it is
   * asserted rather than assumed because it is a claim about a version no
   * standard describes: MPEG-2.5 at 11.025 and 12 kHz uses the same
   * scalefactor band tables as MPEG-2 at 16 kHz. The generator checks it
   * against minimp3 when the tables are built; this checks that the
   * mapping in the C actually lands on that row, which is a different
   * statement and the one a decode depends on.
   *
   * If this ever fails, 11.025 and 12 kHz need a row of their own - not a
   * different constant here. */
  MP3_Header sixteen;
  ASSERT_TRUE(gaud_mp3_header_parse(
      Header(2u, L3, false, 5u, 2u, false, 0u).data(), &sixteen));
  ASSERT_EQ(sixteen.sample_rate, 16000u);
  unsigned reference = 99u;
  ASSERT_TRUE(gaud_mp3_band_row(&sixteen, &reference));

  for (unsigned rate = 0; rate < 2u; ++rate) {
    MP3_Header header;
    ASSERT_TRUE(gaud_mp3_header_parse(
        Header(0u, L3, false, 5u, rate, false, 0u).data(), &header));
    ASSERT_TRUE(header.sample_rate == 11025u || header.sample_rate == 12000u);
    unsigned row = 99u;
    ASSERT_TRUE(gaud_mp3_band_row(&header, &row));
    EXPECT_EQ(row, reference)
        << header.sample_rate << " Hz does not use the 16 kHz band row";
  }

  /* And 8 kHz does not, which is the whole reason it has a row at all.
   * Without this the test above would pass on a mapping that sent every
   * MPEG-2.5 rate to row 5. */
  MP3_Header eight;
  ASSERT_TRUE(gaud_mp3_header_parse(
      Header(0u, L3, false, 5u, 2u, false, 0u).data(), &eight));
  ASSERT_EQ(eight.sample_rate, 8000u);
  unsigned row = 99u;
  ASSERT_TRUE(gaud_mp3_band_row(&eight, &row));
  EXPECT_NE(row, reference) << "8 kHz shares the 16 kHz band row, and its "
                               "tables are not the same numbers";
}

TEST(Mp3Header, TwoFramesAgreeAboutTheStreamAndNotAboutTheBitrate) {
  MP3_Header first;
  MP3_Header second;
  ASSERT_TRUE(gaud_mp3_header_parse(
      Header(V1, L3, false, 9u, 0u, false, 1u).data(), &first));

  /* A different bitrate is what a variable-rate file does every frame, so
   * comparing it would reject every VBR stream at its second frame. */
  ASSERT_TRUE(gaud_mp3_header_parse(
      Header(V1, L3, false, 5u, 0u, true, 0u).data(), &second));
  EXPECT_TRUE(gaud_mp3_headers_compatible(&first, &second));

  /* A different sample rate, version, layer or channel count is a
   * different stream, and at that point a matching sync word is a
   * coincidence. */
  ASSERT_TRUE(gaud_mp3_header_parse(
      Header(V1, L3, false, 9u, 1u, false, 1u).data(), &second));
  EXPECT_FALSE(gaud_mp3_headers_compatible(&first, &second));
  ASSERT_TRUE(gaud_mp3_header_parse(
      Header(V2, L3, false, 9u, 0u, false, 1u).data(), &second));
  EXPECT_FALSE(gaud_mp3_headers_compatible(&first, &second));
  ASSERT_TRUE(gaud_mp3_header_parse(
      Header(V1, L2, false, 9u, 0u, false, 1u).data(), &second));
  EXPECT_FALSE(gaud_mp3_headers_compatible(&first, &second));
  ASSERT_TRUE(gaud_mp3_header_parse(
      Header(V1, L3, false, 9u, 0u, false, 3u).data(), &second));
  EXPECT_FALSE(gaud_mp3_headers_compatible(&first, &second));
}

/* ----------------------------------------------------------- length tags */

TEST(Mp3Tag, AnInfoTagIsFoundPastTheSideInformation) {
  std::vector<unsigned char> frame
      = Frame(Header(V1, L3, false, 9u, 0u, false, 0u));
  PutXing(frame, "Info", 1234u, 56789u, "LAME3.100", 576u, 1404u);

  MP3_Header header;
  ASSERT_TRUE(gaud_mp3_header_parse(frame.data(), &header));
  MP3_Vbr_Tag tag;
  ASSERT_TRUE(gaud_mp3_tag_parse(frame.data(), frame.size(), &header, &tag));
  EXPECT_TRUE(tag.present);
  EXPECT_TRUE(tag.is_info);
  EXPECT_FALSE(tag.vbri);
  EXPECT_TRUE(tag.has_frames);
  EXPECT_EQ(tag.frames, 1234u);
  EXPECT_TRUE(tag.has_bytes);
  EXPECT_EQ(tag.bytes, 56789u);
  EXPECT_TRUE(tag.has_toc);
  EXPECT_TRUE(tag.has_quality);
  EXPECT_TRUE(tag.lame_present);
  EXPECT_STREQ(tag.encoder, "LAME3.100");
  EXPECT_EQ(tag.lame_delay, 576u);
  EXPECT_EQ(tag.lame_padding, 1404u);
}

TEST(Mp3Tag, AMonoTagSitsFifteenBytesEarlier) {
  /* The whole reason the side information's length has to be right: a mono
   * frame's is 17 bytes rather than 32, so a reader that assumed stereo
   * looks for the magic fifteen bytes past it and finds nothing. */
  std::vector<unsigned char> frame
      = Frame(Header(V1, L3, false, 9u, 0u, false, 3u));
  PutXing(frame, "Xing", 7u, 8u, "LAME3.100", 576u, 1000u);
  MP3_Header header;
  ASSERT_TRUE(gaud_mp3_header_parse(frame.data(), &header));
  EXPECT_EQ(gaud_mp3_side_info_size(&header), 17u);
  MP3_Vbr_Tag tag;
  ASSERT_TRUE(gaud_mp3_tag_parse(frame.data(), frame.size(), &header, &tag));
  EXPECT_FALSE(tag.is_info);
  EXPECT_EQ(tag.frames, 7u);
}

TEST(Mp3Tag, ATagInAProtectedFrameIsTwoBytesFurtherOn) {
  /* A frame carrying a checksum puts its main data two bytes later, and
   * most writers of these tags put the tag where the main data would be
   * without one. Both offsets are tried, so both kinds of file are read -
   * which is the only way to be right about the writers that are
   * consistent and the ones that are not. */
  std::vector<unsigned char> frame
      = Frame(Header(V1, L3, true, 9u, 0u, false, 0u));
  MP3_Header header;
  ASSERT_TRUE(gaud_mp3_header_parse(frame.data(), &header));
  ASSERT_TRUE(header.crc_present);
  PutXing(frame, "Xing", 11u, 12u, "LAME3.100", 576u, 1000u);
  MP3_Vbr_Tag tag;
  ASSERT_TRUE(gaud_mp3_tag_parse(frame.data(), frame.size(), &header, &tag));
  EXPECT_EQ(tag.frames, 11u);

  /* And the other spelling: the same tag at the offset a writer that
   * ignored the checksum would use. */
  std::vector<unsigned char> other
      = Frame(Header(V1, L3, true, 9u, 0u, false, 0u));
  std::memcpy(other.data() + 4u + 32u, "Xing", 4);
  other[4u + 32u + 7u] = 0x01u; /* the frames flag alone */
  other[4u + 32u + 11u] = 99u;
  MP3_Vbr_Tag loose;
  ASSERT_TRUE(gaud_mp3_tag_parse(other.data(), other.size(), &header, &loose));
  EXPECT_EQ(loose.frames, 99u);
  EXPECT_FALSE(loose.lame_present) << "nothing printable follows the fields";
}

TEST(Mp3Tag, TheLameExtensionIsBelievedOnlyWhenItIsThere) {
  /* The control for the delay. A writer that filled in the Xing fields and
   * stopped leaves the frame's own data where the extension would be, and
   * for a silent first frame that is zeros - so reading a delay out of it
   * would trim a track by whatever those bytes happened to be. */
  std::vector<unsigned char> frame
      = Frame(Header(V1, L3, false, 9u, 0u, false, 0u));
  PutXing(frame, "Xing", 100u, 200u, nullptr, 0u, 0u);
  MP3_Header header;
  ASSERT_TRUE(gaud_mp3_header_parse(frame.data(), &header));
  MP3_Vbr_Tag tag;
  ASSERT_TRUE(gaud_mp3_tag_parse(frame.data(), frame.size(), &header, &tag));
  EXPECT_TRUE(tag.present);
  EXPECT_EQ(tag.frames, 100u);
  EXPECT_FALSE(tag.lame_present);
  GAUD_Trim trim = gaud_mp3_tag_trim(&tag);
  EXPECT_FALSE(trim.stated);
  EXPECT_EQ(trim.encoder_delay, 0u);
  EXPECT_EQ(trim.padding, 0u);

  /* And the arm that must pass: three different encoder strings, all of
   * which ffmpeg writes depending on its flags, and all of which both
   * reference decoders read a delay out of. The first draft of this
   * reader had a list of two names and this corpus is written with the
   * third. */
  for (const char * encoder : {"LAME3.100", "Lavc61.19", "Lavf lame"}) {
    std::vector<unsigned char> named
        = Frame(Header(V1, L3, false, 9u, 0u, false, 0u));
    PutXing(named, "Info", 10u, 20u, encoder, 576u, 1404u);
    MP3_Vbr_Tag one;
    ASSERT_TRUE(gaud_mp3_tag_parse(named.data(), named.size(), &header, &one))
        << encoder;
    EXPECT_TRUE(one.lame_present) << encoder;
    EXPECT_EQ(one.lame_delay, 576u) << encoder;
  }
}

TEST(Mp3Tag, TheDecoderDelayIsAddedToWhatTheEncoderStated) {
  /* 529 frames the decoder itself introduces - the two filterbanks in
   * series - which is why LAME's number is short by exactly that much and
   * why every player that does gapless playback adds it back. */
  MP3_Vbr_Tag tag;
  std::memset(&tag, 0, sizeof(tag));
  tag.lame_present = true;
  tag.lame_delay = 576u;
  tag.lame_padding = 1404u;
  GAUD_Trim trim = gaud_mp3_tag_trim(&tag);
  EXPECT_TRUE(trim.stated);
  EXPECT_EQ(trim.encoder_delay, 1105u);
  EXPECT_EQ(trim.padding, 875u);

  /* A padding smaller than the constant floors at zero rather than
   * wrapping, which on an unsigned subtraction would be a trim of
   * eighteen million million frames. */
  tag.lame_padding = 100u;
  trim = gaud_mp3_tag_trim(&tag);
  EXPECT_EQ(trim.padding, 0u);
  EXPECT_EQ(trim.encoder_delay, 1105u);
}

TEST(Mp3Tag, AVbriTagIsAtAFixedOffsetWhereXingsIsNot) {
  /* Fraunhofer's tag, which is not Xing's under another name: it is 36
   * bytes from the frame's own start whatever the side information's
   * length, and it has no flags - the frame count and byte count are
   * always there. A reader that looks only for Xing loses the length of
   * every file this encoder wrote. */
  std::vector<unsigned char> frame
      = Frame(Header(V1, L3, false, 9u, 0u, false, 0u));
  std::memcpy(frame.data() + 36u, "VBRI", 4);
  frame[36u + 8u] = 0x00u; /* quality */
  frame[36u + 9u] = 0x64u;
  frame[36u + 10u] = 0x00u; /* bytes */
  frame[36u + 11u] = 0x01u;
  frame[36u + 12u] = 0x00u;
  frame[36u + 13u] = 0x00u;
  frame[36u + 14u] = 0x00u; /* frames */
  frame[36u + 15u] = 0x00u;
  frame[36u + 16u] = 0x02u;
  frame[36u + 17u] = 0x00u;

  MP3_Header header;
  ASSERT_TRUE(gaud_mp3_header_parse(frame.data(), &header));
  MP3_Vbr_Tag tag;
  ASSERT_TRUE(gaud_mp3_tag_parse(frame.data(), frame.size(), &header, &tag));
  EXPECT_TRUE(tag.vbri);
  EXPECT_TRUE(tag.present);
  EXPECT_EQ(tag.frames, 512u);
  EXPECT_EQ(tag.bytes, 65536u);
  EXPECT_EQ(tag.quality, 100u);
  EXPECT_FALSE(tag.lame_present) << "VBRI's writer does not write one";
}

TEST(Mp3Tag, AFrameWithNoTagIsNotAnError) {
  /* Most MPEG audio ever encoded carries none of these. */
  std::vector<unsigned char> frame
      = Frame(Header(V1, L3, false, 9u, 0u, false, 0u));
  MP3_Header header;
  ASSERT_TRUE(gaud_mp3_header_parse(frame.data(), &header));
  MP3_Vbr_Tag tag;
  EXPECT_FALSE(gaud_mp3_tag_parse(frame.data(), frame.size(), &header, &tag));
  EXPECT_FALSE(tag.present);
}

/* --------------------------------------------------------- the fixtures */

/**
 * What the corpus is, and what two independent decoders say it is.
 *
 * `frames` is the file's own content: the frame count the stream states or
 * that counting its frames gives, times the samples a frame decodes to.
 * `recording` is that minus the delay and padding, and it is the number
 * **ffmpeg and libsndfile both produce** when asked to decode the file -
 * measured, not derived. For twelve of these it is also exactly the number
 * of samples the encoder was given, which is the strongest form the check
 * takes: the generator asked for 4,409 frames and the file says so.
 */
const struct Fixtures {
  const char * name;
  uint32_t rate;
  unsigned channels;
  GAUD_Sample_Coding coding;
  uint64_t frames;
  uint64_t delay;
  uint64_t padding;
  uint64_t recording;
  /** Whether it is silence by construction, so that the energy check
   *  below does not demand a signal from a file that has none. */
  bool silent;
} fixtures[] = {
    {"mp3_lame_stereo_44100.mp3", 44100u, 2u, GAUD_CODING_MPEG_LAYER3, 5760u,
        1105u, 246u, 4409u, false},
    {"mp3_lame_mono_44100.mp3", 44100u, 1u, GAUD_CODING_MPEG_LAYER3, 4608u,
        1105u, 502u, 3001u, false},
    {"mp3_lame_vbr_stereo_44100.mp3", 44100u, 2u, GAUD_CODING_MPEG_LAYER3,
        5760u, 1105u, 246u, 4409u, false},
    {"mp3_lame_truestereo_44100.mp3", 44100u, 2u, GAUD_CODING_MPEG_LAYER3,
        5760u, 1105u, 246u, 4409u, false},
    {"mp3_lame_transient_44100.mp3", 44100u, 2u, GAUD_CODING_MPEG_LAYER3,
        10368u, 1105u, 444u, 8819u, false},
    {"mp3_lame_stereo_320_48000.mp3", 48000u, 2u, GAUD_CODING_MPEG_LAYER3,
        6912u, 1105u, 1008u, 4799u, false},
    {"mp3_lame_silence_44100.mp3", 44100u, 2u, GAUD_CODING_MPEG_LAYER3, 3456u,
        1105u, 348u, 2003u, true},
    {"mp3_lame_stereo_22050.mp3", 22050u, 2u, GAUD_CODING_MPEG_LAYER3, 3456u,
        1105u, 348u, 2003u, false},
    {"mp3_lame_mono_11025.mp3", 11025u, 1u, GAUD_CODING_MPEG_LAYER3, 2304u,
        1105u, 178u, 1021u, false},
    /* Broadband at an MPEG-2 rate, which the rest of the corpus has only
     * at MPEG-1 rates; it is check_mpeg_input.py's low-rate calibration. */
    {"mp3_lame_noise_22050.mp3", 22050u, 2u, GAUD_CODING_MPEG_LAYER3, 3456u,
        1105u, 348u, 2003u, false},
    /* **MPEG-2.5, all three of its sampling frequencies.** 11.025 kHz is
     * the file above; these are the other two. 12 kHz uses the same band
     * tables as MPEG-2 at 16 kHz, which is what gaud_mp3_band_row() maps
     * it onto; 8 kHz has a row of its own that is in neither standard.
     * Both 8 kHz files, because LAME chose short blocks for the mono one
     * and long for the stereo one - and the short-block region boundary
     * is the one place where 8 kHz needed arithmetic of its own, so a
     * corpus with only the stereo file would have passed with it wrong. */
    {"mp3_lame_mpeg25_12000.mp3", 12000u, 2u, GAUD_CODING_MPEG_LAYER3, 2880u,
        1105u, 574u, 1201u, false},
    {"mp3_lame_mpeg25_mono_8000.mp3", 8000u, 1u, GAUD_CODING_MPEG_LAYER3,
        2880u, 1105u, 174u, 1601u, false},
    {"mp3_lame_mpeg25_stereo_8000.mp3", 8000u, 2u, GAUD_CODING_MPEG_LAYER3,
        2880u, 1105u, 174u, 1601u, false},
    /* The second MP3 encoder, which writes a length tag whose delay and
     * padding are zero. The 529 frames of decoder delay are still real and
     * are still subtracted, and both references do the same: they decode
     * these to 4,079 and 1,775 frames rather than to 4,608 and 2,304. */
    {"mp3_shine_stereo_44100.mp3", 44100u, 2u, GAUD_CODING_MPEG_LAYER3, 4608u,
        529u, 0u, 4079u, false},
    {"mp3_shine_mono_44100.mp3", 44100u, 1u, GAUD_CODING_MPEG_LAYER3, 2304u,
        529u, 0u, 1775u, false},
    /* Layer II, from both of its writers. Neither puts a length tag in
     * one, so the frames were counted and no trim is stated - and both
     * references decode all of them, which is the same answer. */
    {"mp2_twolame_stereo_44100.mp2", 44100u, 2u, GAUD_CODING_MPEG_LAYER2, 4608u,
        0u, 0u, 4608u, false},
    {"mp2_ff_mono_48000.mp2", 48000u, 1u, GAUD_CODING_MPEG_LAYER2, 3456u, 0u,
        0u, 3456u, false},
    {"mp2_ff_stereo_22050.mp2", 22050u, 2u, GAUD_CODING_MPEG_LAYER2, 2304u, 0u,
        0u, 2304u, false},
    /* No Xing frame at all: nothing states the length and nothing states
     * the delay. Both references also decode the whole 5,760 frames, so
     * "untrimmed" is not this library being unable to do what they do - it
     * is the file not saying. */
    {"mp3_lame_noxing_stereo_44100.mp3", 44100u, 2u, GAUD_CODING_MPEG_LAYER3,
        5760u, 0u, 0u, 5760u, false},
    /* An ID3v2 tag at the front and an ID3v1 trailer at the end, and **the
     * one fixture where the two references disagree**: libsndfile decodes
     * 2,003 frames and ffmpeg 2,351. 2,351 is 3,456 - 1,105, which is the
     * start trim applied and the end trim not. A minimal pair settles it:
     * the same file written with `-write_id3v1 0` decodes to 2,003 in
     * ffmpeg too, so the 128-byte trailer is what defeats its end trim,
     * and 2,003 is what the encoder was given. We agree with libsndfile. */
    {"mp3_tagged_stereo_44100.mp3", 44100u, 2u, GAUD_CODING_MPEG_LAYER3, 3456u,
        1105u, 348u, 2003u, false},
};

TEST(Mp3Load, EveryFixtureIdentifiesAsTheStreamItIs) {
  for (const auto & one : fixtures) {
    Loaded loaded;
    ASSERT_EQ(OpenFile(loaded, one.name), GAUD_OK) << one.name;
    GAUD_Track * track = loaded.track();
    ASSERT_NE(track, nullptr) << one.name;
    EXPECT_STREQ(gaud_doc_codec_name(loaded.doc), "mp3") << one.name;
    EXPECT_EQ(gaud_track_sample_rate(track), one.rate) << one.name;
    EXPECT_EQ(gaud_track_layout(track).channels, one.channels) << one.name;
    EXPECT_EQ(gaud_track_coding(track), one.coding) << one.name;
    /* Every layer decodes to 16-bit, which is a choice of this library's
     * and not a property of a format that carries no depth at all. */
    EXPECT_EQ(gaud_track_format(track), GAUD_SAMPLE_S16) << one.name;
  }
}

TEST(Mp3Load, TheStatedLengthMinusTheStatedTrimIsTheRecording) {
  for (const auto & one : fixtures) {
    Loaded loaded;
    ASSERT_EQ(OpenFile(loaded, one.name), GAUD_OK) << one.name;
    GAUD_Track * track = loaded.track();
    EXPECT_EQ(gaud_track_frames(track), one.frames) << one.name;
    GAUD_Trim trim = gaud_track_trim(track);
    EXPECT_EQ(trim.encoder_delay, one.delay) << one.name;
    EXPECT_EQ(trim.padding, one.padding) << one.name;
    EXPECT_EQ(one.frames - one.delay - one.padding, one.recording)
        << one.name << ": the table above is inconsistent with itself";
    /* The duration is the recording's, not the file's contents', which is
     * the difference between a gapless library and a clicking one. */
    EXPECT_NEAR(gaud_track_duration(track),
        (double)one.recording / (double)one.rate, 1e-9)
        << one.name;
  }
}

TEST(Mp3Load, WhetherTheDelayWasStatedIsItselfReported) {
  /* Zero delay that was stated and zero delay that was never mentioned are
   * different facts, and a caller doing gapless playback can act on the
   * first and not the second. */
  Loaded stated;
  ASSERT_EQ(OpenFile(stated, "mp3_lame_stereo_44100.mp3"), GAUD_OK);
  EXPECT_TRUE(gaud_track_trim(stated.track()).stated);

  Loaded silent;
  ASSERT_EQ(OpenFile(silent, "mp3_lame_noxing_stereo_44100.mp3"), GAUD_OK);
  EXPECT_FALSE(gaud_track_trim(silent.track()).stated);
}

TEST(Mp3Load, AStreamWithNoLengthTagIsCountedAndSaysSo) {
  /* Short enough to walk, so the count is exact rather than estimated -
   * and the diagnostic is absent for that reason. The estimate's own
   * diagnostic is asserted below on a stream too long to walk. */
  Loaded loaded;
  ASSERT_EQ(OpenFile(loaded, "mp3_lame_noxing_stereo_44100.mp3"), GAUD_OK);
  MP3_File * file = static_cast<MP3_File *>(gaud_doc_private(loaded.doc));
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->length_source, MP3_LENGTH_COUNTED);
  EXPECT_EQ(gaud_track_frames(loaded.track()), 5760u);
  EXPECT_EQ(loaded.diagnostics.count, 0u);
}

TEST(Mp3Load, ALongStreamWithNoLengthTagIsEstimatedAndSaysSo) {
  /* More frames than the walk will count, so the length comes from the
   * data size and one bitrate. That is exact for a constant-rate file and
   * wrong for a variable-rate one with no tag, which is why it is reported
   * as an estimate rather than as a measurement.
   *
   * 200 identical frames, which is past MP3_WALK_LIMIT. */
  std::vector<unsigned char> stream;
  std::vector<unsigned char> frame
      = Frame(Header(V1, L3, false, 9u, 0u, false, 0u));
  for (unsigned i = 0; i < 200u; ++i) {
    stream.insert(stream.end(), frame.begin(), frame.end());
  }
  Loaded loaded;
  ASSERT_EQ(OpenBytes(loaded, stream), GAUD_OK);
  MP3_File * file = static_cast<MP3_File *>(gaud_doc_private(loaded.doc));
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->length_source, MP3_LENGTH_ESTIMATED);
  /* 200 frames of 417 bytes at 128 kbit/s and 44.1 kHz: the estimate is
   * the byte count converted to time, so it is within a frame of
   * 200 x 1152 rather than exactly it. */
  uint64_t frames = gaud_track_frames(loaded.track());
  EXPECT_GT(frames, 200u * 1152u - 1152u);
  EXPECT_LT(frames, 200u * 1152u + 1152u);
  ASSERT_GE(loaded.diagnostics.count, 1u);
  EXPECT_NE(std::string(loaded.diagnostics.items[0].recommended_action)
                .find("derived from the data size"),
      std::string::npos);
}

TEST(Mp3Load, AVariableRateStreamWithNoTagReportsNoLengthAtAll) {
  /* The arm that must not guess. A bounded look at the first frames shows
   * the bitrate changing, so neither the stated length nor the
   * constant-rate estimate is available - and an estimate taken anyway
   * would be wrong by however much the rate varies. */
  std::vector<unsigned char> stream;
  for (unsigned i = 0; i < 200u; ++i) {
    std::vector<unsigned char> frame
        = Frame(Header(V1, L3, false, i % 2u ? 9u : 5u, 0u, false, 0u));
    stream.insert(stream.end(), frame.begin(), frame.end());
  }
  Loaded loaded;
  ASSERT_EQ(OpenBytes(loaded, stream), GAUD_OK);
  MP3_File * file = static_cast<MP3_File *>(gaud_doc_private(loaded.doc));
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->length_source, MP3_LENGTH_UNKNOWN);
  EXPECT_EQ(gaud_track_frames(loaded.track()), UINT64_MAX);
  EXPECT_LT(gaud_track_duration(loaded.track()), 0.0);
  ASSERT_GE(loaded.diagnostics.count, 1u);
}

TEST(Mp3Load, TheTagsOnBothEndsAreReadAndNeitherIsAudio) {
  Loaded loaded;
  ASSERT_EQ(OpenFile(loaded, "mp3_tagged_stereo_44100.mp3"), GAUD_OK);
  const GAUD_Meta * meta = gaud_doc_meta(loaded.doc);
  ASSERT_NE(meta, nullptr);
  EXPECT_STREQ(gaud_meta_get(meta, GAUD_TAG_TITLE, 0), "A Title");
  EXPECT_STREQ(gaud_meta_get(meta, GAUD_TAG_ARTIST, 0), "An Artist");
  EXPECT_STREQ(gaud_meta_get(meta, GAUD_TAG_ALBUM, 0), "An Album");

  /* The 128-byte ID3v1 trailer is not audio. A reader that counted it
   * would leave the decoder resynchronising inside a tag at the end of
   * every file an ordinary tagger has touched. */
  MP3_File * file = static_cast<MP3_File *>(gaud_doc_private(loaded.doc));
  ASSERT_NE(file, nullptr);
  uint64_t size = 0;
  ASSERT_EQ(gaud_stream_size(loaded.stream, &size), GAUD_OK);
  EXPECT_EQ(file->audio_offset + file->audio_length, size - 128u);
  /* And the first frame is not audio either, because the length tag is
   * in it. */
  EXPECT_GT(file->audio_offset, file->first_frame_offset);
}

TEST(Mp3Load, LeadingBytesThatAreNotAFrameAreSkippedAndNoted) {
  /* A stream cut out of something else, which is the common shape of a
   * damaged MP3. The frame search finds the first real frame and the
   * document says in a diagnostic that it did. */
  std::vector<unsigned char> stream(1000u, 0x55u);
  std::vector<unsigned char> frame
      = Frame(Header(V1, L3, false, 9u, 0u, false, 0u));
  for (unsigned i = 0; i < 6u; ++i) {
    stream.insert(stream.end(), frame.begin(), frame.end());
  }
  Loaded loaded;
  ASSERT_EQ(OpenBytes(loaded, stream), GAUD_OK);
  MP3_File * file = static_cast<MP3_File *>(gaud_doc_private(loaded.doc));
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->first_frame_offset, 1000u);
  ASSERT_GE(loaded.diagnostics.count, 1u);
  EXPECT_NE(std::string(loaded.diagnostics.items[0].recommended_action)
                .find("not a frame and not a tag"),
      std::string::npos);
}

TEST(Mp3Load, OneSyncWordAloneIsNotAFrame) {
  /* The control for the frame search: a byte pair that matches eleven bits
   * of sync and a plausible header, with nothing after it. Two bytes in
   * every few hundred of any compressed data look like this, so a reader
   * that believed them would claim half the files it was shown. */
  std::vector<unsigned char> stream(4096u, 0x00u);
  std::vector<unsigned char> header = Header(V1, L3, false, 9u, 0u, false, 0u);
  std::memcpy(stream.data() + 100u, header.data(), 4);
  Loaded loaded;
  EXPECT_EQ(OpenBytes(loaded, stream), GAUD_ERR_FORMAT);
}

TEST(Mp3Load, AFreeFormatStreamIsRefusedByName) {
  /* Bitrate index 0: the frame states no rate, so its length is the
   * distance to the next sync word. Legal, rare, and not implemented -
   * and the refusal says which of those it is rather than answering
   * "not this format". */
  std::vector<unsigned char> frame(417u, 0u);
  std::vector<unsigned char> header = Header(V1, L3, false, 0u, 0u, false, 0u);
  std::memcpy(frame.data(), header.data(), 4);
  std::vector<unsigned char> stream;
  for (unsigned i = 0; i < 6u; ++i) {
    stream.insert(stream.end(), frame.begin(), frame.end());
  }
  Loaded loaded;
  EXPECT_EQ(OpenBytes(loaded, stream), GAUD_ERR_UNSUPPORTED);
  ASSERT_GE(loaded.diagnostics.count, 1u);
  EXPECT_NE(std::string(loaded.diagnostics.items[0].recommended_action)
                .find("free format"),
      std::string::npos);
}

TEST(Mp3Load, AnUnseekableStreamIsRefusedWithAReason) {
  /* Every other loader here reads its container forwards. This one cannot,
   * and the reason is the format: the length comes from the file's size
   * and from trailers at the end. */
  std::vector<unsigned char> bytes;
  std::vector<unsigned char> frame
      = Frame(Header(V1, L3, false, 9u, 0u, false, 0u));
  for (unsigned i = 0; i < 6u; ++i) {
    bytes.insert(bytes.end(), frame.begin(), frame.end());
  }
  GAUD_Stream * seekable = nullptr;
  ASSERT_EQ(gaud_stream_create_memory(bytes.data(), bytes.size(), &seekable),
      GAUD_OK);
  GAUD_Stream * pipe = nullptr;
  ASSERT_EQ(gaud_stream_create_unseekable(seekable, &pipe), GAUD_OK);
  GAUD_Diagnostics diagnostics;
  gaud_diagnostics_init(&diagnostics, nullptr);
  GAUD_Doc * doc = nullptr;
  const GAUD_Codec * codec = gaud_registry_find(nullptr, "mp3");
  ASSERT_NE(codec, nullptr);
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  EXPECT_EQ(codec->open(codec, pipe, &limits, &diagnostics, &doc),
      GAUD_ERR_UNSUPPORTED);
  ASSERT_GE(diagnostics.count, 1u);
  EXPECT_NE(
      std::string(diagnostics.items[0].recommended_action).find("seekable"),
      std::string::npos);
  gaud_diagnostics_destroy(&diagnostics);
  gaud_stream_destroy(pipe);
  gaud_stream_destroy(seekable);
}

/* --------------------------------------------- what the loader refuses */

/** A stream of @p count identical frames of the shape @p header describes. */
std::vector<unsigned char> Stream(
    const std::vector<unsigned char> & header, unsigned count) {
  std::vector<unsigned char> frame = Frame(header);
  std::vector<unsigned char> out;
  for (unsigned i = 0; i < count; ++i) {
    out.insert(out.end(), frame.begin(), frame.end());
  }
  return out;
}

TEST(Mp3Load, AnApeTrailerIsExcludedFromTheAudioAndNotInterpreted) {
  /* APE is the third tag an MPEG stream can carry and this library has no
   * reader for it. Counting its bytes as audio would leave a decoder
   * resynchronising inside a tag, so its stated length is subtracted and
   * a diagnostic says it was found and not read. */
  std::vector<unsigned char> stream
      = Stream(Header(V1, L3, false, 9u, 0u, false, 0u), 6u);
  size_t audio = stream.size();
  /* A footer: the magic, a version, the tag's own size at offset 12, the
   * item count, and flags whose top bit says a header precedes the items.
   * Left clear here, so the span is the footer's stated size alone. */
  std::vector<unsigned char> footer(32u, 0u);
  std::memcpy(footer.data(), "APETAGEX", 8);
  footer[8] = 0xD0u; /* version 2000 */
  footer[9] = 0x07u;
  uint32_t span = 32u + 16u; /* the footer, and sixteen bytes of items */
  footer[12] = (unsigned char)(span & 0xFFu);
  footer[13] = (unsigned char)((span >> 8) & 0xFFu);
  stream.insert(stream.end(), 16u, 0x41u);
  stream.insert(stream.end(), footer.begin(), footer.end());

  Loaded loaded;
  ASSERT_EQ(OpenBytes(loaded, stream), GAUD_OK);
  MP3_File * file = static_cast<MP3_File *>(gaud_doc_private(loaded.doc));
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->audio_offset + file->audio_length, audio);
  bool said = false;
  for (size_t i = 0; i < loaded.diagnostics.count; ++i) {
    said = said
        || std::string(loaded.diagnostics.items[i].recommended_action)
                .find("APE tag")
            != std::string::npos;
  }
  EXPECT_TRUE(said);
}

TEST(Mp3Load, AnId3v2FooterIsPartOfTheTagsSpan) {
  /* ID3v2.4 allows a ten-byte footer after the tag's body, and a flag in
   * the header says so. A reader that ignores the flag starts looking for
   * the first frame ten bytes early, lands in the footer, and either finds
   * no frame or finds one by accident further on. */
  std::vector<unsigned char> stream;
  unsigned char head[10] = {'I', 'D', '3', 4, 0, 0x10u, 0, 0, 0, 20};
  stream.insert(stream.end(), head, head + 10);
  stream.insert(stream.end(), 20u, 0u); /* the body */
  stream.insert(stream.end(), 10u, 0u); /* the footer the flag promised */
  std::vector<unsigned char> frames
      = Stream(Header(V1, L3, false, 9u, 0u, false, 0u), 6u);
  stream.insert(stream.end(), frames.begin(), frames.end());

  Loaded loaded;
  ASSERT_EQ(OpenBytes(loaded, stream), GAUD_OK);
  MP3_File * file = static_cast<MP3_File *>(gaud_doc_private(loaded.doc));
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->first_frame_offset, 40u);
  /* And no "bytes before the first frame were not a frame" warning, which
   * is what a reader that mislaid the footer would produce. */
  EXPECT_EQ(loaded.diagnostics.count, 0u);
}

TEST(Mp3Load, ALengthTagClaimingMoreThanTheBytesCouldHoldIsNotBelieved) {
  /* A Xing frame count is a number an encoder wrote once and nothing has
   * checked since - and a file that has been cut keeps the original's.
   * Believing it produces a duration a player will scrub against and a
   * frame count a caller will allocate from, so it is bounded by what the
   * bytes could hold at the lowest bitrate the format defines. */
  std::vector<unsigned char> header = Header(V1, L3, false, 9u, 0u, false, 0u);
  std::vector<unsigned char> tag_frame = Frame(header);
  PutXing(tag_frame, "Info", 100000u, 1000u, "LAME3.100", 576u, 1404u);
  std::vector<unsigned char> stream = tag_frame;
  std::vector<unsigned char> rest = Stream(header, 6u);
  stream.insert(stream.end(), rest.begin(), rest.end());

  Loaded loaded;
  ASSERT_EQ(OpenBytes(loaded, stream), GAUD_OK);
  MP3_File * file = static_cast<MP3_File *>(gaud_doc_private(loaded.doc));
  ASSERT_NE(file, nullptr);
  EXPECT_NE(file->length_source, MP3_LENGTH_STATED);
  EXPECT_EQ(file->length_source, MP3_LENGTH_COUNTED);
  EXPECT_EQ(gaud_track_frames(loaded.track()), 6u * 1152u);
  bool said = false;
  for (size_t i = 0; i < loaded.diagnostics.count; ++i) {
    said = said
        || std::string(loaded.diagnostics.items[i].recommended_action)
                .find("more frames than this many bytes")
            != std::string::npos;
  }
  EXPECT_TRUE(said);

  /* The control: the same tag with a believable count is believed. */
  std::vector<unsigned char> honest = Frame(header);
  PutXing(honest, "Info", 6u, 1000u, "LAME3.100", 576u, 1404u);
  std::vector<unsigned char> second = honest;
  second.insert(second.end(), rest.begin(), rest.end());
  Loaded believed;
  ASSERT_EQ(OpenBytes(believed, second), GAUD_OK);
  MP3_File * trusted = static_cast<MP3_File *>(gaud_doc_private(believed.doc));
  ASSERT_NE(trusted, nullptr);
  EXPECT_EQ(trusted->length_source, MP3_LENGTH_STATED);
  EXPECT_EQ(gaud_track_frames(believed.track()), 6u * 1152u);
}

TEST(Mp3Load, AStreamThatEndsInsideItsOwnTagFrameIsRefused) {
  /* Found by the fuzz harness, and what it found was not a crash: it was a
   * document whose audio began past the end of its own file, because the
   * tag frame is skipped as "not audio" and the skip ran off the end.
   * Every caller would then have had to defend against a data offset
   * outside the stream. */
  std::vector<unsigned char> header = Header(V1, L3, false, 9u, 0u, false, 0u);
  std::vector<unsigned char> frame = Frame(header);
  PutXing(frame, "Info", 5u, 1000u, "LAME3.100", 576u, 1404u);
  /* Cut it just past the tag, so the tag is wholly present and the frame
   * is not. */
  frame.resize(200u);
  Loaded loaded;
  EXPECT_EQ(OpenBytes(loaded, frame), GAUD_ERR_CORRUPT);
  ASSERT_GE(loaded.diagnostics.count, 1u);
  bool said = false;
  for (size_t i = 0; i < loaded.diagnostics.count; ++i) {
    said = said
        || std::string(loaded.diagnostics.items[i].recommended_action)
                .find("ends inside its first frame")
            != std::string::npos;
  }
  EXPECT_TRUE(said);
}

TEST(Mp3Load, AFrameWhoseBodyIsMissingIsNotCounted) {
  /* The count is multiplied by the samples a frame decodes to, so counting
   * a frame whose data is not there states a duration the file does not
   * contain. A partial download ends exactly this way. */
  std::vector<unsigned char> header = Header(V1, L3, false, 9u, 0u, false, 0u);
  std::vector<unsigned char> stream = Stream(header, 4u);
  /* A fifth frame's header and a third of its body. */
  std::vector<unsigned char> partial = Frame(header);
  partial.resize(140u);
  stream.insert(stream.end(), partial.begin(), partial.end());

  Loaded loaded;
  ASSERT_EQ(OpenBytes(loaded, stream), GAUD_OK);
  MP3_File * file = static_cast<MP3_File *>(gaud_doc_private(loaded.doc));
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->length_source, MP3_LENGTH_COUNTED);
  EXPECT_EQ(gaud_track_frames(loaded.track()), 4u * 1152u)
      << "the fifth frame's header is there and its body is not";
}

TEST(Mp3Load, EachLayerIsItsOwnCoding) {
  /* No encoder in the oracle image writes Layer I at all, so the corpus
   * has Layer II from two writers, Layer III from two, and Layer I only
   * as a fixture the generator constructs. These are hand-built frames,
   * which is the cheapest way to check the coding each layer reports. */
  const struct {
    unsigned layer;
    GAUD_Sample_Coding coding;
    uint32_t samples;
  } cases[] = {
      {L1, GAUD_CODING_MPEG_LAYER1, 384u},
      {L2, GAUD_CODING_MPEG_LAYER2, 1152u},
      {L3, GAUD_CODING_MPEG_LAYER3, 1152u},
  };
  for (const auto & one : cases) {
    std::vector<unsigned char> stream
        = Stream(Header(V1, one.layer, false, 9u, 0u, false, 0u), 8u);
    Loaded loaded;
    ASSERT_EQ(OpenBytes(loaded, stream), GAUD_OK) << one.coding;
    EXPECT_EQ(gaud_track_coding(loaded.track()), one.coding);
    EXPECT_EQ(gaud_track_frames(loaded.track()), 8u * one.samples);
  }
}

TEST(Mp3Load, TheLimitsRefuseBeforeAnythingIsAllocated) {
  /* A ::GAUD_Limits is the caller's policy and it has to bite on a format
   * whose header states the rate and the channel count per frame. */
  std::vector<unsigned char> stream
      = Stream(Header(V1, L3, false, 9u, 0u, false, 0u), 8u);
  GAUD_Stream * memory = nullptr;
  ASSERT_EQ(gaud_stream_create_memory(stream.data(), stream.size(), &memory),
      GAUD_OK);
  const GAUD_Codec * codec = gaud_registry_find(nullptr, "mp3");
  ASSERT_NE(codec, nullptr);

  struct Case {
    const char * what;
    uint32_t max_sample_rate;
    uint32_t max_channels;
    uint64_t max_frames;
  };
  const Case cases[] = {
      {"a rate above the cap", 22050u, 8u, 1u << 30},
      {"more channels than the cap", 96000u, 1u, 1u << 30},
      {"more frames than the cap", 96000u, 8u, 100u},
  };
  for (const auto & one : cases) {
    GAUD_Limits limits;
    gaud_limits_default(&limits);
    limits.max_sample_rate = one.max_sample_rate;
    limits.max_channels = one.max_channels;
    limits.max_frames = one.max_frames;
    GAUD_Doc * doc = nullptr;
    ASSERT_EQ(gaud_stream_seek(memory, 0, GAUD_SEEK_SET), GAUD_OK);
    EXPECT_EQ(
        codec->open(codec, memory, &limits, nullptr, &doc), GAUD_ERR_LIMIT)
        << one.what;
    EXPECT_EQ(doc, nullptr) << one.what;
  }
  /* The control: the same stream inside the defaults loads. */
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  GAUD_Doc * doc = nullptr;
  ASSERT_EQ(gaud_stream_seek(memory, 0, GAUD_SEEK_SET), GAUD_OK);
  EXPECT_EQ(codec->open(codec, memory, &limits, nullptr, &doc), GAUD_OK);
  gaud_doc_destroy(doc);
  gaud_stream_destroy(memory);
}

TEST(Mp3Load, AnId3v2TagLongerThanTheStreamIsIgnoredAndTheFramesAreRead) {
  /* The tag's own header states its length, and a truncated file states a
   * length the file does not have. The frames are a separate question and
   * may be perfectly good, so this is a diagnostic rather than a refusal -
   * but the frames have to be found, which means the search starts after
   * where the tag said it ended and finds nothing there. A refusal naming
   * the format is the honest answer: the alternative is searching from
   * zero as well, which would find frames inside a tag body on every file
   * that really does carry a large one - and a partial download, which is
   * the common case, has no frames yet anyway. */
  std::vector<unsigned char> stream;
  unsigned char head[10] = {'I', 'D', '3', 3, 0, 0, 0, 0, 0x7Fu, 0x7Fu};
  stream.insert(stream.end(), head, head + 10);
  std::vector<unsigned char> frames
      = Stream(Header(V1, L3, false, 9u, 0u, false, 0u), 6u);
  stream.insert(stream.end(), frames.begin(), frames.end());

  Loaded loaded;
  EXPECT_EQ(OpenBytes(loaded, stream), GAUD_ERR_FORMAT);
}

/* ------------------------------------------------------------ decoding */

/** Decode a whole track into one vector of 16-bit samples. */
std::vector<int16_t> DecodeAll(GAUD_Track * track, size_t block = 97) {
  std::vector<int16_t> out;
  GAUD_Decoder * decoder = nullptr;
  EXPECT_EQ(gaud_decoder_create(track, &decoder), GAUD_OK);
  if (!decoder) {
    return out;
  }
  GAUD_Buffer * buffer = nullptr;
  EXPECT_EQ(
      gaud_decoder_buffer_create(decoder, nullptr, block, &buffer), GAUD_OK);
  unsigned channels = gaud_track_layout(track).channels;
  for (;;) {
    EXPECT_EQ(gaud_decoder_read(decoder, buffer), GAUD_OK);
    size_t frames = gaud_buffer_frames(buffer);
    if (frames == 0) {
      break;
    }
    const int16_t * data
        = static_cast<const int16_t *>(gaud_buffer_data_const(buffer));
    out.insert(out.end(), data, data + frames * channels);
  }
  gaud_buffer_destroy(buffer);
  gaud_decoder_destroy(decoder);
  return out;
}

TEST(Mp3Decode, EveryFixtureDecodesTheFrameCountItStated) {
  /* The first trap planning/audio.md section 12 names, asserted without
   * any reference: a decoder that returns silence, or stops early, or
   * runs long, produces the wrong number of frames - and the loader has
   * already said what the right number is. `make check-mpeg` compares the
   * samples; this compares the count, and it runs on a machine with no
   * container. */
  for (const auto & one : fixtures) {
    Loaded loaded;
    ASSERT_EQ(OpenFile(loaded, one.name), GAUD_OK) << one.name;
    GAUD_Track * track = loaded.track();
    GAUD_Decoder * decoder = nullptr;
    /* **Every fixture decodes.** There used to be a `decodes` flag here
     * and one file that was false: MPEG-2.5 Layer III, refused because
     * its band tables are in no standard. They are generated now, so the
     * flag described nothing and the branch it guarded could not be
     * taken - which is worse than no branch, because it reads as a
     * tested refusal path and is not one. The refusals this library does
     * make are asserted on constructed input instead, where the input
     * can be made to have the property. */
    ASSERT_EQ(gaud_decoder_create(track, &decoder), GAUD_OK) << one.name;
    gaud_decoder_destroy(decoder);

    std::vector<int16_t> samples = DecodeAll(track);
    unsigned channels = gaud_track_layout(track).channels;
    ASSERT_EQ(samples.size() % channels, 0u) << one.name;
    EXPECT_EQ(samples.size() / channels, one.frames) << one.name;

    /* And it is not silence. The count alone would pass for a decoder
     * that produced the right number of zeros, which is exactly the
     * failure that sounds like a quiet passage. */
    if (!one.silent) {
      int64_t energy = 0;
      for (int16_t value : samples) {
        energy += (int64_t)value * value;
      }
      EXPECT_GT(energy / (int64_t)samples.size(), 1000)
          << one.name
          << " decoded to something far too quiet to be the "
             "signal the generator put in it";
    }
  }
}

TEST(Mp3Decode, TheBlockSizeDoesNotChangeTheSamples) {
  /* A frame is 1,152 samples and a caller's buffer is whatever it chose,
   * so the decoder holds a frame and drains it across calls. Three block
   * sizes: one smaller than a frame, one that is not a divisor of it, and
   * one much larger. */
  Loaded loaded;
  ASSERT_EQ(OpenFile(loaded, "mp3_lame_stereo_44100.mp3"), GAUD_OK);
  std::vector<int16_t> reference = DecodeAll(loaded.track(), 1152u);
  ASSERT_FALSE(reference.empty());
  for (size_t block : {1u, 97u, 577u, 4096u}) {
    Loaded again;
    ASSERT_EQ(OpenFile(again, "mp3_lame_stereo_44100.mp3"), GAUD_OK);
    EXPECT_EQ(DecodeAll(again.track(), block), reference)
        << "block size " << block;
  }
}

TEST(Mp3Decode, ASeekLandsOnTheSampleItWasAskedFor) {
  /* The property no reference can be asked about, and the one a
   * decode-from-the-start comparison cannot distinguish from a seek that
   * silently did nothing. There is no index in an MPEG stream, so the
   * seek counts frames from the start and then runs up to the target
   * through the frames whose state it needs; what is checked here is that
   * the samples after it are the samples that were there before. */
  Loaded loaded;
  ASSERT_EQ(OpenFile(loaded, "mp3_lame_stereo_44100.mp3"), GAUD_OK);
  GAUD_Track * track = loaded.track();
  std::vector<int16_t> whole = DecodeAll(track, 1152u);
  unsigned channels = gaud_track_layout(track).channels;
  ASSERT_GT(whole.size(), 4000u * channels);

  GAUD_Decoder * decoder = nullptr;
  ASSERT_EQ(gaud_decoder_create(track, &decoder), GAUD_OK);
  GAUD_Buffer * buffer = nullptr;
  ASSERT_EQ(
      gaud_decoder_buffer_create(decoder, nullptr, 500u, &buffer), GAUD_OK);
  /* Targets on and off a frame boundary, forwards and backwards, and one
   * at the very start - which is the case that has to reset the
   * filterbank rather than carry it. */
  for (uint64_t target : {2304u, 2305u, 1000u, 3456u, 0u, 4000u}) {
    uint64_t landed = UINT64_MAX;
    ASSERT_EQ(gaud_decoder_seek(decoder, target, &landed), GAUD_OK) << target;
    EXPECT_EQ(landed, target) << "a seek must report where it landed";
    ASSERT_EQ(gaud_decoder_read(decoder, buffer), GAUD_OK);
    size_t frames = gaud_buffer_frames(buffer);
    ASSERT_GT(frames, 0u) << target;
    const int16_t * data
        = static_cast<const int16_t *>(gaud_buffer_data_const(buffer));
    /* The run-up means the decoder's filterbank state is warm but not
     * identical to a decode from the start, so the samples are compared
     * with a tolerance - and the tolerance is tight enough that a seek
     * landing on the wrong frame could not pass it. One frame out is
     * 1,152 samples of completely different audio. */
    int64_t error = 0;
    int64_t energy = 0;
    for (size_t i = 0; i < frames * channels; ++i) {
      int64_t want = whole[(target * channels) + i];
      int64_t got = data[i];
      error += (want - got) * (want - got);
      energy += want * want;
    }
    double relative = energy > 0 ? std::sqrt((double)error / (double)energy)
                                 : (double)error;
    EXPECT_LT(relative, 0.01)
        << "seek to " << target << " landed somewhere else";
  }
  gaud_buffer_destroy(buffer);
  gaud_decoder_destroy(decoder);
}

TEST(Mp3Decode, ALayerIStreamDecodesAtEveryQuantiserWidth) {
  /* Nothing in the oracle image writes Layer I, so this fixture is
   * built by tools/oracle/make_corpus.py rather than by an encoder - and
   * it allocates a different number of bits to each of fourteen
   * subbands, so one file exercises every sample width the layer has.
   * `make check-mpeg` scores its samples against both references; what is
   * asserted here is that the fourteen widths are all present, which is
   * the property that makes the fixture worth having. */
  Loaded loaded;
  ASSERT_EQ(OpenFile(loaded, "mp1_handbuilt_stereo_32000.mp1"), GAUD_OK);
  EXPECT_EQ(gaud_track_coding(loaded.track()), GAUD_CODING_MPEG_LAYER1);
  std::vector<int16_t> samples = DecodeAll(loaded.track());
  EXPECT_EQ(samples.size() / 2u, 4608u);

  unsigned widths = 0;
  uint64_t at = gaud_track_data_offset(loaded.track());
  unsigned char frame[4];
  ASSERT_EQ(
      gaud_stream_seek(loaded.stream, (int64_t)at, GAUD_SEEK_SET), GAUD_OK);
  ASSERT_EQ(gaud_stream_read(loaded.stream, frame, 4), 4u);
  MP3_Header header;
  ASSERT_TRUE(gaud_mp3_header_parse(frame, &header));
  EXPECT_EQ(header.layer, 1u);
  /* The allocation fields are the 256 bits after the header. */
  std::vector<unsigned char> allocations(32);
  ASSERT_EQ(gaud_stream_read(loaded.stream, allocations.data(), 32), 32u);
  MP3_Bits bits;
  gaud_mp3_bits_init(&bits, allocations.data(), allocations.size());
  bool seen[17] = {false};
  for (unsigned sb = 0; sb < 32u; ++sb) {
    for (unsigned ch = 0; ch < 2u; ++ch) {
      unsigned value = gaud_mp3_bits_read(&bits, 4u);
      ASSERT_LT(value, 15u) << "allocation 15 is forbidden";
      if (value) {
        seen[value + 1u] = true;
      }
    }
  }
  for (unsigned width = 2; width <= 15u; ++width) {
    EXPECT_TRUE(seen[width]) << "no subband uses " << width
                             << "-bit samples, so this fixture no longer "
                                "covers every Layer I quantiser";
    ++widths;
  }
  EXPECT_EQ(widths, 14u);
}

TEST(Mp3Decode, AFrameWhoseReservoirIsMissingIsSilenceAndIsCounted) {
  /* A Layer III frame's main data starts before its own header, so the
   * first frames of a stream point at bytes that do not exist. Every
   * decoder emits silence for those and so does this one - the point of
   * the test is that it is **counted**, so that silence this library
   * could not avoid and silence the file contains are different facts
   * from outside. */
  std::vector<unsigned char> header = Header(V1, L3, false, 9u, 0u, false, 3u);
  std::vector<unsigned char> frame = Frame(header);
  /* A side information whose main_data_begin is the largest the field can
   * state: nine bits of ones, at the very start of the frame's data. */
  frame[4] = 0xFFu;
  frame[5] = 0x80u;
  std::vector<unsigned char> stream;
  for (unsigned i = 0; i < 8u; ++i) {
    stream.insert(stream.end(), frame.begin(), frame.end());
  }
  Loaded loaded;
  ASSERT_EQ(OpenBytes(loaded, stream), GAUD_OK);
  std::vector<int16_t> samples = DecodeAll(loaded.track());
  /* Every frame is ungrounded, because every frame points back 511 bytes
   * and the frames are 417 long - so the whole decode is silence, and the
   * frame count is still exact. */
  EXPECT_EQ(samples.size(), 8u * 1152u);
  for (int16_t value : samples) {
    EXPECT_EQ(value, 0);
  }
}

/** Bits into a byte buffer, most significant first, as the format is. */
class Writer {
public:
  void put(uint32_t value, unsigned width) {
    for (int shift = (int)width - 1; shift >= 0; --shift) {
      if (at_ == 0) {
        bytes_.push_back(0);
      }
      if ((value >> shift) & 1u) {
        bytes_.back() |= (unsigned char)(0x80u >> at_);
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

/**
 * A side information like SideInfo()'s but with a chosen global_gain.
 *
 * global_gain is eight bits and every one of its 256 values is legal, so
 * 255 is an ordinary frame and not a malformed one. It scales the whole
 * spectrum by 2^((gain - 210 - 8*subblock)/4), which at the top of the
 * range asks for values far outside what an int32 of Q28 can hold - so
 * this is how a file reaches the saturating arithmetic without being
 * corrupt in any way a decoder is entitled to refuse.
 */
std::vector<unsigned char> SideInfoWithGain(unsigned gain) {
  Writer w;
  w.put(0, 9);
  w.put(0, 3);
  w.put(0, 4);
  w.put(0, 4);
  for (unsigned gr = 0; gr < 2u; ++gr) {
    for (unsigned ch = 0; ch < 2u; ++ch) {
      w.put(500, 12);
      w.put(100, 9);
      w.put(gain, 8);
      w.put(0, 4);
      w.put(0, 1);
      w.put(1, 5);
      w.put(1, 5);
      w.put(1, 5);
      w.put(7, 4);
      w.put(7, 3);
      w.put(0, 1);
      w.put(0, 1);
      w.put(0, 1);
    }
  }
  std::vector<unsigned char> out = w.bytes();
  out.resize(32u, 0u);
  return out;
}

TEST(Mp3Decode, AFrameAtTheTopOfTheGainRangeDecodesWithoutOverflowing) {
  /* **Every addition in the Q28 pipeline can overflow and none of them is
   * bounded by the format.** A granule states its own global_gain, all
   * 256 values of which are legal, and at the top of that range the
   * requantised spectrum sits near the end of int32 - where mid/side, the
   * alias-reduction butterflies, the overlap-add between granules and the
   * frequency inversion were all signed overflow, which is undefined.
   *
   * Found by the fuzzer, on a corpus that had grown since the decoder
   * landed; the same eight sites reproduce on the commit that wrote them,
   * so the saturation is a fix and not a consequence of MPEG-2.5.
   *
   * **What proves the arithmetic is defined is the sanitizer build, not
   * this test.** `make test-asan` compiles with -fsanitize=undefined and
   * `make fuzz-run-mpeg` runs the same code over
   * tests/fuzz/corpus/mpeg; either one reports every site above on the
   * unfixed decoder. What this test does is keep an input that reaches
   * those sites inside `make test`, so that the sanitizer has something
   * to look at without the fuzzer having to rediscover it - and assert
   * the part that is observable from outside: the decode finishes and
   * produces the frames it claimed.
   *
   * The two gains are the control pair. 180 is the ordinary frame
   * SideInfo() uses and saturates nothing; 255 is the same frame with
   * only that field changed. They must decode *differently*, or the gain
   * never took effect and this input reaches none of the arithmetic it
   * was written for. */
  std::vector<int16_t> decoded[2];
  unsigned gains[2] = {180u, 255u};
  for (int which = 0; which < 2; ++which) {
    std::vector<unsigned char> side = SideInfoWithGain(gains[which]);
    std::vector<unsigned char> frame
        = Frame(Header(V1, L3, false, 9u, 0u, false, 0u));
    std::memcpy(frame.data() + 4u, side.data(), side.size());
    size_t at = 4u + side.size();
    for (size_t k = 0; at + k < frame.size(); ++k) {
      frame[at + k] = (unsigned char)(0x80u + (k * 37u) % 0x7Fu);
    }
    std::vector<unsigned char> stream;
    for (unsigned i = 0; i < 6u; ++i) {
      stream.insert(stream.end(), frame.begin(), frame.end());
    }
    Loaded loaded;
    ASSERT_EQ(OpenBytes(loaded, stream), GAUD_OK) << gains[which];
    decoded[which] = DecodeAll(loaded.track());
    EXPECT_EQ(decoded[which].size(), 6u * 1152u * 2u) << gains[which];
    bool any = false;
    for (int16_t value : decoded[which]) {
      if (value != 0) {
        any = true;
        break;
      }
    }
    EXPECT_TRUE(any) << "gain " << gains[which] << " decoded to pure "
                     << "silence, so this exercised none of the arithmetic";
  }
  EXPECT_NE(decoded[0], decoded[1])
      << "the two gains decoded identically, so global_gain had no effect "
      << "and the loud frame is not reaching the arithmetic this test is "
      << "about";
}

/**
 * A joint-stereo frame whose every intensity position is @p position.
 *
 * Built field by field rather than by filling the main data with a byte
 * pattern, because the test below needs two frames that differ in the
 * intensity positions and in **nothing else**: a byte pattern that
 * changed those four-bit fields would change the bits after them too,
 * and two decodes that differed would prove nothing.
 *
 * The shape, and why each part is what it is:
 *
 *   - `scalefac_compress` 14 selects four-bit scalefactors for bands 0 to
 *     10 and two-bit ones for 11 to 20. Four bits is what lets a position
 *     above 6 exist at all; two bits cannot reach one, so the upper bands
 *     are legal positions in both frames and contribute the same thing to
 *     each.
 *   - **The right channel's `part2_3_length` covers its scalefactors and
 *     nothing else**, so its spectrum is empty and `nonzero[1]` is zero -
 *     which puts the intensity bound at the bottom of the spectrum and
 *     makes every band an intensity band. That is the arrangement a real
 *     encoder produces at the top of the spectrum and this produces
 *     everywhere.
 *   - The left channel's is longer, so its count1 region fills the low
 *     spectrum with the plus and minus ones that region codes. Without
 *     that there is nothing for an intensity weight to be applied to and
 *     every frame decodes to silence.
 */
std::vector<unsigned char> IntensityFrame(unsigned position) {
  /* 11 bands of four bits and 10 of two: 64 bits of scalefactors. */
  const unsigned scalefactor_bits = 11u * 4u + 10u * 2u;
  const unsigned count1_bits = 120u;

  Writer side;
  side.put(0, 9); /* main_data_begin: each frame stands alone */
  side.put(0, 3); /* private bits */
  side.put(0, 4); /* scfsi, channel 0: both granules carry their own */
  side.put(0, 4); /* scfsi, channel 1 */
  for (unsigned gr = 0; gr < 2u; ++gr) {
    for (unsigned ch = 0; ch < 2u; ++ch) {
      side.put(ch == 0 ? scalefactor_bits + count1_bits : scalefactor_bits,
          12);                         /* part2_3_length */
      side.put(0, 9);                  /* big_values: none */
      side.put(180, 8);                /* global_gain */
      side.put(14, 4);                 /* scalefac_compress */
      side.put(0, 1);                  /* no window switching */
      side.put(0, 5);                  /* table_select 0 */
      side.put(0, 5);
      side.put(0, 5);
      side.put(7, 4);                  /* region0_count */
      side.put(7, 3);                  /* region1_count */
      side.put(0, 1);                  /* preflag */
      side.put(0, 1);                  /* scalefac_scale */
      side.put(0, 1);                  /* count1table_select */
    }
  }
  std::vector<unsigned char> side_bytes = side.bytes();
  side_bytes.resize(32u, 0u);

  Writer main;
  for (unsigned gr = 0; gr < 2u; ++gr) {
    for (unsigned ch = 0; ch < 2u; ++ch) {
      /* The left channel's own scalefactors are a legal position in both
       * frames: only the right channel's are read as intensity
       * positions, and varying the left's would change its
       * requantisation and confound the comparison. */
      for (unsigned band = 0; band < 11u; ++band) {
        main.put(ch == 1u ? position : 4u, 4);
      }
      for (unsigned band = 0; band < 10u; ++band) {
        main.put(1, 2);
      }
      if (ch == 0u) {
        /* Count1 quadruples, table A. A run of ones codes the shortest
         * codeword repeatedly, which fills the low spectrum. */
        for (unsigned bit = 0; bit < count1_bits; ++bit) {
          main.put((bit % 3u) == 0u ? 1u : 0u, 1);
        }
      }
    }
  }

  /* Joint stereo with both switches on: mode 1, mode extension 3. */
  std::vector<unsigned char> frame
      = Frame(Header(V1, L3, false, 9u, 0u, false, 1u, 3u));
  std::memcpy(frame.data() + 4u, side_bytes.data(), side_bytes.size());
  const std::vector<unsigned char> & body = main.bytes();
  size_t at = 4u + side_bytes.size();
  EXPECT_LE(at + body.size(), frame.size());
  std::memcpy(frame.data() + at, body.data(),
      std::min(body.size(), frame.size() - at));
  return frame;
}

TEST(Mp3Decode, AnIntensityPositionWithNoWeightIsNotUsedAsAnIndex) {
  /*
   * **An out-of-bounds read of a global, found by the Ogg fuzzer.** The
   * intensity position *is* the right channel's scalefactor, read with
   * whatever width `scalefac_compress` selected - four bits here, so 0
   * to 15. ISO/IEC 11172-3 gives a weight for 0 to 6 and calls 7
   * illegal, and says nothing at all about 8 to 15 because no encoder
   * writes them. The decoder tested exactly for 7, so a frame stating 12
   * indexed a seven-row table five rows past its end.
   *
   * **The assertion is that 12 and 15 decode identically**, and that is
   * what makes this a test rather than a crash reproducer. Both are
   * positions the standard defines no weight for, so a correct decoder
   * treats both the same way - as though intensity stereo were off for
   * that band, which here means the middle/side matrix applies instead.
   * The two frames differ in those four-bit fields and in nothing else.
   * Before the fix, 15 took the illegal path and 12 read a weight from
   * whatever follows the table, so the two differed.
   *
   * **And the control**: position 5, which is legal and must decode
   * *differently* from both. Without it the test above would pass on a
   * decoder that had intensity stereo switched off entirely, which is the
   * other way to make 12 and 15 agree.
   *
   * `make test-asan` is what proves the read is gone; this keeps an
   * input that reaches it inside `make test`.
   */
  std::vector<int16_t> decoded[3];
  const unsigned positions[3] = {12u, 15u, 5u};
  for (int which = 0; which < 3; ++which) {
    std::vector<unsigned char> frame = IntensityFrame(positions[which]);
    std::vector<unsigned char> stream;
    for (unsigned i = 0; i < 6u; ++i) {
      stream.insert(stream.end(), frame.begin(), frame.end());
    }
    Loaded loaded;
    ASSERT_EQ(OpenBytes(loaded, stream), GAUD_OK) << positions[which];
    decoded[which] = DecodeAll(loaded.track());
    ASSERT_EQ(decoded[which].size(), 6u * 1152u * 2u) << positions[which];
    bool any = false;
    for (int16_t value : decoded[which]) {
      if (value != 0) {
        any = true;
        break;
      }
    }
    EXPECT_TRUE(any) << "position " << positions[which] << " decoded to "
                     << "silence, so no weight was applied to anything";
  }
  EXPECT_EQ(decoded[0], decoded[1])
      << "positions 12 and 15 decoded differently, and the standard "
      << "defines a weight for neither: one of them is being used as an "
      << "index into a table that does not have that row";
  EXPECT_NE(decoded[0], decoded[2])
      << "a legal position decoded the same as an illegal one, so "
      << "intensity stereo is not reached at all and the comparison "
      << "above is vacuous";
}

/**
 * A stereo MPEG-1 Layer III side information that decodes.
 *
 * Every field the format requires, with values chosen to be legal rather
 * than musical: a hundred pairs of big values in three regions coded with
 * table 1, no window switching, no preflag. The main data that follows it
 * is whatever the caller puts there - what matters for the test below is
 * that the *structure* is valid, so that a reader which found it two
 * bytes from where it should be would fail rather than disagree.
 */
std::vector<unsigned char> SideInfo() {
  Writer w;
  w.put(0, 9); /* main_data_begin: this frame stands alone */
  w.put(0, 3); /* private bits, stereo */
  w.put(0, 4); /* scfsi, channel 0 */
  w.put(0, 4); /* scfsi, channel 1 */
  for (unsigned gr = 0; gr < 2u; ++gr) {
    for (unsigned ch = 0; ch < 2u; ++ch) {
      w.put(500, 12); /* part2_3_length */
      w.put(100, 9);  /* big_values */
      w.put(180, 8);  /* global_gain */
      w.put(0, 4);    /* scalefac_compress */
      w.put(0, 1);    /* no window switching */
      w.put(1, 5);    /* table_select[0] */
      w.put(1, 5);    /* table_select[1] */
      w.put(1, 5);    /* table_select[2] */
      w.put(7, 4);    /* region0_count */
      w.put(7, 3);    /* region1_count */
      w.put(0, 1);    /* preflag */
      w.put(0, 1);    /* scalefac_scale */
      w.put(0, 1);    /* count1table_select */
    }
  }
  std::vector<unsigned char> out = w.bytes();
  out.resize(32u, 0u);
  return out;
}

TEST(Mp3Decode, AProtectedFrameDecodesTheSameAsAnUnprotectedOne) {
  /* The protection bit moves the side information and the main data two
   * bytes later, and nothing in the corpus sets it: no encoder in the
   * oracle image writes a CRC. So the only check on that offset is this
   * one, and it is an equivalence rather than a value - the same side
   * information and the same main data, in a frame with a checksum and a
   * frame without, must decode to the same samples. A reader that
   * mislaid the two bytes would parse the side information off by
   * sixteen bits, which these values are chosen to make fatal rather
   * than merely different: a table_select of 4 or 14 and a big_values
   * past 288 are both refusals.
   *
   * The main data is a run of non-zero bytes rather than zeros, because
   * two frames of silence are equal whatever the offset was. */
  std::vector<unsigned char> side = SideInfo();
  std::vector<int16_t> decoded[2];
  for (int which = 0; which < 2; ++which) {
    bool crc = which != 0;
    std::vector<unsigned char> frame
        = Frame(Header(V1, L3, crc, 9u, 0u, false, 0u));
    size_t at = 4u + (crc ? 2u : 0u);
    if (crc) {
      /* A checksum the reader does not verify and must skip. */
      frame[4] = 0x12u;
      frame[5] = 0x34u;
    }
    std::memcpy(frame.data() + at, side.data(), side.size());
    /* The main data, indexed from its own start rather than from the
     * frame's: the two frames put it two bytes apart, and a fill that
     * depended on the absolute offset would make them carry *different*
     * data, which is the mistake the first draft of this test made. The
     * length is the shorter of the two for the same reason. */
    size_t payload = frame.size() - (4u + 2u + side.size());
    for (size_t k = 0; k < payload; ++k) {
      frame[at + side.size() + k] = (unsigned char)(0x80u + (k * 37u) % 0x7Fu);
    }
    std::vector<unsigned char> stream;
    for (unsigned i = 0; i < 6u; ++i) {
      stream.insert(stream.end(), frame.begin(), frame.end());
    }
    Loaded loaded;
    ASSERT_EQ(OpenBytes(loaded, stream), GAUD_OK) << which;
    decoded[which] = DecodeAll(loaded.track());
    EXPECT_EQ(decoded[which].size(), 6u * 1152u * 2u) << which;
  }
  EXPECT_EQ(decoded[0], decoded[1])
      << "the two frames carry the same side information and the same "
         "main data, so the checksum's two bytes are the only difference "
         "and they are not data";
  /* And the payload really did reach the decoder: all-zero output would
   * make this test pass for the wrong reason. */
  int64_t energy = 0;
  for (int16_t value : decoded[0]) {
    energy += (int64_t)value * value;
  }
  EXPECT_GT(energy, 0)
      << "both decodes are silence, so this proved nothing about the "
         "offset";
}

/* ------------------------------------------------- the generated tables */

/*
 * What these are for.
 *
 * `src/codec/mp3/mp3_tables.c` is generated from the tables in ISO/IEC
 * 11172-3 and 13818-3 by `tools/tables/gen_mp3_tables.py`, which
 * validates everything it extracts - that every Huffman table is a
 * complete prefix code, that every window coefficient is a multiple of
 * 2^-16, and a dozen more. **Those checks run in the generator and the
 * generator does not run in the build.** What is below is the half of
 * that which can be re-checked from the generated file alone, so that a
 * corrupted or hand-edited table is caught by `make test` rather than by
 * a differential that needs a container.
 */

TEST(Mp3Tables, EveryHuffmanTreeIsCompleteAndWellFormed) {
  for (unsigned number = 0; number < 32u; ++number) {
    const MP3_Huff * table = &gaud_mp3_huff[number];
    if (table->unused) {
      EXPECT_EQ(table->width, 0u) << number;
      continue;
    }
    if (table->width == 0u) {
      continue; /* Table 0 codes nothing. */
    }
    /* Walk every node reachable from the root. A tree with a hole in it
     * would decode some bit pattern into a node index that is not a
     * node, which is the failure that cannot be caught at run time
     * without a test on every lookup. */
    std::vector<unsigned> pending = {0u};
    std::vector<bool> visited;
    unsigned leaves = 0;
    std::vector<bool> covered((size_t)table->width * table->width, false);
    while (!pending.empty()) {
      unsigned node = pending.back();
      pending.pop_back();
      if (visited.size() <= node) {
        visited.resize(node + 1u, false);
      }
      if (visited[node]) {
        FAIL() << "table " << number << " revisits node " << node
               << ", so its tree has a cycle";
      }
      visited[node] = true;
      for (unsigned bit = 0; bit < 2u; ++bit) {
        int16_t entry = gaud_mp3_huff_nodes[table->offset + 2u * node + bit];
        if (entry < 0) {
          unsigned payload = (unsigned)(-(int)entry - 1);
          unsigned x = payload >> 4;
          unsigned y = payload & 15u;
          ASSERT_LT(x, table->width) << "table " << number;
          ASSERT_LT(y, table->width) << "table " << number;
          size_t index = (size_t)x * table->width + y;
          EXPECT_FALSE(covered[index])
              << "table " << number << " codes (" << x << "," << y << ") twice";
          covered[index] = true;
          ++leaves;
        }
        else {
          pending.push_back((unsigned)entry);
        }
      }
    }
    EXPECT_EQ(leaves, (unsigned)table->width * table->width)
        << "table " << number << " has " << leaves << " leaves for a "
        << (unsigned)table->width << "x" << (unsigned)table->width << " grid";
    for (size_t index = 0; index < covered.size(); ++index) {
      EXPECT_TRUE(covered[index])
          << "table " << number << " does not code the pair at index " << index;
    }
  }
}

TEST(Mp3Tables, TheSynthesisWindowIsTheStandardsExactValues) {
  /* Every coefficient of Table 3-B.3 is an exact multiple of 2^-16 - the
   * standard prints them to nine decimal places and 0.000015259 is
   * 1/65536 - so in Q28 every one of them is a multiple of 4096. Nothing
   * a mis-read digit or a hand edit produced would be. */
  bool any = false;
  for (unsigned i = 0; i < 512u; ++i) {
    EXPECT_EQ(gaud_mp3_window[i] % 4096, 0)
        << "window coefficient " << i << " is " << gaud_mp3_window[i]
        << ", which is not a multiple of 2^-16 in Q28";
    if (gaud_mp3_window[i] != 0) {
      any = true;
    }
  }
  EXPECT_TRUE(any) << "the window is all zeros, which would decode every "
                      "file to silence";
  /* Two values from the standard, by hand: D[0] is zero and D[1] is
   * -1/65536. A table shifted by one entry fails this. */
  EXPECT_EQ(gaud_mp3_window[0], 0);
  EXPECT_EQ(gaud_mp3_window[1], -(1 << MP3_Q) / 65536);
}

TEST(Mp3Tables, RequantisationIsMonotonicAndExactAtOne) {
  /* |is|^(4/3) for every value the Huffman stage can produce, as a
   * mantissa and an exponent. Monotonic because the function is, and
   * exactly 1.0 at 1 because 1^(4/3) is 1 - which is the entry every
   * frame with a quiet passage uses. */
  EXPECT_EQ(gaud_mp3_pow43[0], 0u);
  EXPECT_EQ(gaud_mp3_pow43[1] & 0xFFFFFFu, 1u << 23);
  EXPECT_EQ(gaud_mp3_pow43[1] >> 24, 0u);
  double previous = 0.0;
  for (unsigned value = 1; value < 8207u; ++value) {
    uint32_t packed = gaud_mp3_pow43[value];
    double here = (double)(packed & 0xFFFFFFu)
        * std::pow(2.0, (double)(packed >> 24) - 23.0);
    EXPECT_GT(here, previous) << "pow43 is not monotonic at " << value;
    /* And it is the right function, to the 24 bits the mantissa has. */
    double want = std::pow((double)value, 4.0 / 3.0);
    EXPECT_LT(std::fabs(here - want) / want, 1e-6)
        << "pow43[" << value << "] is " << here << " and " << value
        << "^(4/3) is " << want;
    previous = here;
  }
}

TEST(Mp3Tables, TheBandTablesPartitionTheWholeSpectrum) {
  /* Boundaries, so band b covers [b] to [b+1). The generator checks the
   * widths against the standard's printed columns; what is checkable here
   * is that the result covers every line exactly once and reaches the top
   * - which is the property the padding band was added for, and the
   * defect it fixed was a third of the spectrum decoded with the wrong
   * scalefactor. */
  for (unsigned row = 0; row < 6u; ++row) {
    unsigned bands = gaud_mp3_sfb_long_bands[row];
    ASSERT_GT(bands, 0u) << row;
    ASSERT_LT(bands, 24u) << row;
    EXPECT_EQ(gaud_mp3_sfb_long[row][0], 0u) << row;
    EXPECT_EQ(gaud_mp3_sfb_long[row][bands], 576u)
        << "row " << row << "'s long bands stop at "
        << gaud_mp3_sfb_long[row][bands] << " of 576";
    for (unsigned band = 1; band <= bands; ++band) {
      EXPECT_GT(gaud_mp3_sfb_long[row][band], gaud_mp3_sfb_long[row][band - 1u])
          << "row " << row << " band " << band;
    }
    unsigned shorts = gaud_mp3_sfb_short_bands[row];
    EXPECT_EQ(gaud_mp3_sfb_short[row][0], 0u) << row;
    EXPECT_EQ(gaud_mp3_sfb_short[row][shorts], 192u)
        << "row " << row << "'s short bands stop at "
        << gaud_mp3_sfb_short[row][shorts] << " of 192";
    for (unsigned band = 1; band <= shorts; ++band) {
      EXPECT_GT(
          gaud_mp3_sfb_short[row][band], gaud_mp3_sfb_short[row][band - 1u])
          << "row " << row << " short band " << band;
    }
  }
}

TEST(Mp3Tables, LayerIsQuantiserWidthsFindTheirClass) {
  /* Layer I's allocation of n means n+1 bits and so 2^(n+1)-1 levels,
   * and the class table's rows are 3, 5, 7, 9, 15, 31 ... - so only the
   * rows from 15 upwards line up with the index. The generated map is the
   * correspondence and this is the check that it is one. */
  for (unsigned width = 2; width <= 16u; ++width) {
    unsigned index = gaud_mp3_class_for_bits[width];
    ASSERT_LT(index, 17u) << width;
    EXPECT_EQ(gaud_mp3_classes[index].steps, (1u << width) - 1u)
        << width << "-bit samples are mapped to a class of "
        << gaud_mp3_classes[index].steps << " levels";
    EXPECT_EQ(gaud_mp3_classes[index].sample_bits, width) << width;
  }
}

/* -------------------------------------------------------------- probing */

TEST(Mp3Probe, AFrameAtTheStartIsCertainAndOneFurtherInIsNot) {
  std::vector<unsigned char> frame
      = Frame(Header(V1, L3, false, 9u, 0u, false, 0u));
  std::vector<unsigned char> clean;
  for (unsigned i = 0; i < 6u; ++i) {
    clean.insert(clean.end(), frame.begin(), frame.end());
  }
  GAUD_Stream * stream = nullptr;
  ASSERT_EQ(
      gaud_stream_create_memory(clean.data(), clean.size(), &stream), GAUD_OK);
  GAUD_Probe_Result result;
  ASSERT_EQ(gaud_probe(nullptr, stream, &result), GAUD_OK);
  EXPECT_STREQ(result.codec_name, "mp3");
  EXPECT_EQ(result.confidence, (unsigned)GAUD_CONFIDENCE_CERTAIN);
  /* A probe must leave the stream where it found it. */
  EXPECT_EQ(gaud_stream_tell(stream), 0u);
  gaud_stream_destroy(stream);

  std::vector<unsigned char> junked(1000u, 0x55u);
  junked.insert(junked.end(), clean.begin(), clean.end());
  GAUD_Stream * second = nullptr;
  ASSERT_EQ(gaud_stream_create_memory(junked.data(), junked.size(), &second),
      GAUD_OK);
  ASSERT_EQ(gaud_probe(nullptr, second, &result), GAUD_OK);
  EXPECT_STREQ(result.codec_name, "mp3");
  EXPECT_EQ(result.confidence, (unsigned)GAUD_CONFIDENCE_LIKELY)
      << "a frame found past the start is a claim another codec's "
         "certainty should beat";
  gaud_stream_destroy(second);
}

TEST(Mp3Probe, TheOtherCodecsFilesAreStillTheirs) {
  /* The real risk of a content-sniffing codec with no signature: an MPEG
   * sync word occurs by chance in the samples of any file, so this probe
   * must not outbid a container that knows what it is looking at. */
  const char * theirs[] = {
      "wav_s16_stereo_44100.wav",
      "aiff_s16_stereo_44100.aiff",
      "flac_lib_noise_44100.flac",
      "oggflac_ff_s16_stereo_44100.oga",
      "wav_imaadpcm_loud_8000.wav",
      "wav_msadpcm_coefs_8000.wav",
  };
  for (const char * name : theirs) {
    GAUD_Stream * stream = nullptr;
    ASSERT_EQ(gaud_stream_create_file(Fixture(name).c_str(), &stream), GAUD_OK)
        << name;
    GAUD_Probe_Result result;
    ASSERT_EQ(gaud_probe(nullptr, stream, &result), GAUD_OK) << name;
    EXPECT_STRNE(result.codec_name, "mp3") << name;
    gaud_stream_destroy(stream);
  }
}

TEST(Mp3Probe, NoiseIsNotThisFormat) {
  /* 64 KiB of a cheap repeatable sequence, and the test asserts first that
   * it is a hard case rather than assuming it. It contains twenty byte
   * pairs that pass every validity check in the frame header - a reserved
   * layer, a reserved version, bitrate index 15 and rate index 3 all
   * refused - and two of those twenty state the free format, which has no
   * length to confirm. None of the twenty has a second frame where its own
   * length says one should be.
   *
   * **That measurement is what this test is for.** Before it, the frame
   * search accepted an unconfirmable candidate from anywhere in a stream
   * and this probe claimed the noise as MPEG audio. A test that only
   * checked the verdict would have passed as soon as the sequence changed
   * and stopped containing candidates, so the candidate count is asserted
   * too. */
  std::vector<unsigned char> noise(65536u);
  uint32_t state = 0x12345678u;
  for (auto & byte : noise) {
    state = state * 1664525u + 1013904223u;
    byte = (unsigned char)(state >> 24);
  }
  unsigned candidates = 0;
  unsigned free_format = 0;
  for (size_t i = 0; i + 4u <= noise.size(); ++i) {
    MP3_Header header;
    if (gaud_mp3_header_parse(noise.data() + i, &header)) {
      ++candidates;
      if (header.frame_size == 0u) {
        ++free_format;
      }
    }
  }
  EXPECT_GT(candidates, 0u) << "this sequence no longer contains a single "
                               "candidate header, so it is not testing the "
                               "frame search any more";
  EXPECT_GT(free_format, 0u) << "and none of them states the free format, "
                                "which is the case that cannot be confirmed "
                                "by arithmetic";

  GAUD_Stream * stream = nullptr;
  ASSERT_EQ(
      gaud_stream_create_memory(noise.data(), noise.size(), &stream), GAUD_OK);
  GAUD_Probe_Result result;
  ASSERT_EQ(gaud_probe(nullptr, stream, &result), GAUD_OK);
  EXPECT_STRNE(result.codec_name, "mp3")
      << candidates << " candidate headers in the noise, " << free_format
      << " of them free format, and one of them was believed";
  gaud_stream_destroy(stream);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
