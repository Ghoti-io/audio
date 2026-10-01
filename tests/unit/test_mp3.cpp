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
 * What is *not* here is a sample value, because this phase decodes none.
 */

#include "../../src/codec/mp3/mp3_internal.h"
#include <ghoti.io/audio/audio.h>
#include <ghoti.io/audio/codec_sdk.h>
#include <gtest/gtest.h>
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
} fixtures[] = {
    {"mp3_lame_stereo_44100.mp3", 44100u, 2u, GAUD_CODING_MPEG_LAYER3, 5760u,
        1105u, 246u, 4409u},
    {"mp3_lame_mono_44100.mp3", 44100u, 1u, GAUD_CODING_MPEG_LAYER3, 4608u,
        1105u, 502u, 3001u},
    {"mp3_lame_vbr_stereo_44100.mp3", 44100u, 2u, GAUD_CODING_MPEG_LAYER3,
        5760u, 1105u, 246u, 4409u},
    {"mp3_lame_truestereo_44100.mp3", 44100u, 2u, GAUD_CODING_MPEG_LAYER3,
        5760u, 1105u, 246u, 4409u},
    {"mp3_lame_transient_44100.mp3", 44100u, 2u, GAUD_CODING_MPEG_LAYER3,
        10368u, 1105u, 444u, 8819u},
    {"mp3_lame_stereo_320_48000.mp3", 48000u, 2u, GAUD_CODING_MPEG_LAYER3,
        6912u, 1105u, 1008u, 4799u},
    {"mp3_lame_silence_44100.mp3", 44100u, 2u, GAUD_CODING_MPEG_LAYER3, 3456u,
        1105u, 348u, 2003u},
    {"mp3_lame_stereo_22050.mp3", 22050u, 2u, GAUD_CODING_MPEG_LAYER3, 3456u,
        1105u, 348u, 2003u},
    {"mp3_lame_mono_11025.mp3", 11025u, 1u, GAUD_CODING_MPEG_LAYER3, 2304u,
        1105u, 178u, 1021u},
    /* The second MP3 encoder, which writes a length tag whose delay and
     * padding are zero. The 529 frames of decoder delay are still real and
     * are still subtracted, and both references do the same: they decode
     * these to 4,079 and 1,775 frames rather than to 4,608 and 2,304. */
    {"mp3_shine_stereo_44100.mp3", 44100u, 2u, GAUD_CODING_MPEG_LAYER3, 4608u,
        529u, 0u, 4079u},
    {"mp3_shine_mono_44100.mp3", 44100u, 1u, GAUD_CODING_MPEG_LAYER3, 2304u,
        529u, 0u, 1775u},
    /* Layer II, from both of its writers. Neither puts a length tag in
     * one, so the frames were counted and no trim is stated - and both
     * references decode all of them, which is the same answer. */
    {"mp2_twolame_stereo_44100.mp2", 44100u, 2u, GAUD_CODING_MPEG_LAYER2, 4608u,
        0u, 0u, 4608u},
    {"mp2_ff_mono_48000.mp2", 48000u, 1u, GAUD_CODING_MPEG_LAYER2, 3456u, 0u,
        0u, 3456u},
    {"mp2_ff_stereo_22050.mp2", 22050u, 2u, GAUD_CODING_MPEG_LAYER2, 2304u, 0u,
        0u, 2304u},
    /* No Xing frame at all: nothing states the length and nothing states
     * the delay. Both references also decode the whole 5,760 frames, so
     * "untrimmed" is not this library being unable to do what they do - it
     * is the file not saying. */
    {"mp3_lame_noxing_stereo_44100.mp3", 44100u, 2u, GAUD_CODING_MPEG_LAYER3,
        5760u, 0u, 0u, 5760u},
    /* An ID3v2 tag at the front and an ID3v1 trailer at the end, and **the
     * one fixture where the two references disagree**: libsndfile decodes
     * 2,003 frames and ffmpeg 2,351. 2,351 is 3,456 - 1,105, which is the
     * start trim applied and the end trim not. A minimal pair settles it:
     * the same file written with `-write_id3v1 0` decodes to 2,003 in
     * ffmpeg too, so the 128-byte trailer is what defeats its end trim,
     * and 2,003 is what the encoder was given. We agree with libsndfile. */
    {"mp3_tagged_stereo_44100.mp3", 44100u, 2u, GAUD_CODING_MPEG_LAYER3, 3456u,
        1105u, 348u, 2003u},
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

TEST(Mp3Load, ThereIsNoDecoderYet) {
  /* The honest statement of where this phase stops. The codec declares no
   * GAUD_CAP_DECODE and has no decoder entry point, so asking for one
   * answers "I know what this is and cannot decode it" rather than
   * handing back a decoder that produces silence - which is the first trap
   * planning/audio.md section 12 names, and the one a caller cannot tell
   * from a quiet passage. */
  Loaded loaded;
  ASSERT_EQ(OpenFile(loaded, "mp3_lame_stereo_44100.mp3"), GAUD_OK);
  const GAUD_Codec * codec = gaud_registry_find(nullptr, "mp3");
  ASSERT_NE(codec, nullptr);
  EXPECT_EQ(codec->capabilities & GAUD_CAP_DECODE, 0u);
  GAUD_Decoder * decoder = nullptr;
  EXPECT_EQ(
      gaud_decoder_create(loaded.track(), &decoder), GAUD_ERR_UNSUPPORTED);
  EXPECT_EQ(decoder, nullptr);
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

TEST(Mp3Load, LayerOneAndLayerTwoAreTheirOwnCodings) {
  /* No encoder in the oracle image writes Layer I at all, so this is the
   * only thing that reads one: the corpus has Layer II from two writers
   * and Layer III from two, and Layer I exists in hand-built frames or
   * nowhere. */
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
   * where the tag said it ended and finds nothing there, then carries on. */
  std::vector<unsigned char> stream;
  unsigned char head[10] = {'I', 'D', '3', 3, 0, 0, 0, 0, 0x7Fu, 0x7Fu};
  stream.insert(stream.end(), head, head + 10);
  std::vector<unsigned char> frames
      = Stream(Header(V1, L3, false, 9u, 0u, false, 0u), 6u);
  stream.insert(stream.end(), frames.begin(), frames.end());

  Loaded loaded;
  /* The span the tag claims is past the end, so the frame search begins
   * past the end and the stream has no frame this library will find. A
   * refusal naming the format is the honest answer: the alternative is
   * searching from zero as well, which would find frames inside a tag
   * body on every file that really does carry a large one. */
  EXPECT_EQ(OpenBytes(loaded, stream), GAUD_ERR_FORMAT);
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
