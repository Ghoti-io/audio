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
 * The Ogg page layer on its own: packet assembly, the page scan, the list
 * of logical streams, the last granule position, and the bisection.
 *
 * **Hand-built files, because a real one cannot ask these questions.** An
 * encoder writes one logical stream, flushes a page per packet, and never
 * puts `OggS` where it is not a page - so a corpus of real Ogg files
 * exercises the easy half of every function here and leaves the half that
 * goes wrong untouched. What is built below instead is the awkward half:
 * a packet spanning three pages, a packet whose length is an exact
 * multiple of 255, two multiplexed streams, and a page body containing
 * four bytes that spell a page header.
 *
 * Every assertion about the bisection names a byte offset, and that is on
 * purpose. A bisection that gave up and returned the start of the file
 * would still decode the right samples - slowly - and would pass a test
 * that only checked the audio. So the offsets are asserted to move.
 */

#include "../../src/container/ogg/ogg.h"
#include <ghoti.io/audio/audio.h>
#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>

namespace {

/** A file built by paging the given packets, one page each unless told. */
struct Built {
  std::vector<unsigned char> bytes;
};

/** A packet to write: its contents, its granule, and whether to flush. */
struct Packet {
  std::vector<unsigned char> data;
  uint64_t granule;
  bool flush;
};

Built build(const std::vector<Packet> & packets, uint32_t serial = 0x1234u) {
  GAUD_Stream * out = nullptr;
  EXPECT_EQ(gaud_stream_create_memory_writer(nullptr, &out), GAUD_OK);
  OGG_Writer writer;
  gaud_ogg_writer_init(&writer, out, serial);
  for (size_t i = 0; i < packets.size(); ++i) {
    bool eos = i + 1 == packets.size();
    EXPECT_EQ(gaud_ogg_writer_packet(&writer, packets[i].data.data(),
                  packets[i].data.size(), packets[i].granule,
                  packets[i].flush, eos),
        GAUD_OK);
  }
  const void * bytes = nullptr;
  size_t length = 0;
  EXPECT_EQ(gaud_stream_writer_bytes(out, &bytes, &length), GAUD_OK);
  Built built;
  built.bytes.assign((const unsigned char *)bytes,
      (const unsigned char *)bytes + length);
  gaud_stream_destroy(out);
  return built;
}

/** A packet of @p size bytes whose contents are a function of @p seed. */
std::vector<unsigned char> filler(size_t size, unsigned seed) {
  std::vector<unsigned char> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = (unsigned char)((i * 31u + seed * 17u) & 0xFFu);
  }
  return data;
}

/** A readable stream over @p bytes; the caller destroys it. */
GAUD_Stream * over(const std::vector<unsigned char> & bytes) {
  GAUD_Stream * stream = nullptr;
  EXPECT_EQ(
      gaud_stream_create_memory(bytes.data(), bytes.size(), &stream), GAUD_OK);
  return stream;
}

/** Read every packet of @p bytes back, as sizes and granules. */
struct Drained {
  std::vector<size_t> sizes;
  std::vector<uint64_t> granules;
  std::vector<unsigned char> first_byte;
  GAUD_Result ended;
};

Drained drain(GAUD_Stream * stream, uint64_t from_offset = UINT64_MAX) {
  OGG_Reader reader;
  gaud_ogg_reader_init(&reader, stream, gaud_allocator_default());
  Drained drained;
  drained.ended = GAUD_OK;
  if (from_offset != UINT64_MAX) {
    /* A serial has to be chosen before a seek, because a seek keeps it. */
    reader.serial = 0x1234u;
    reader.have_serial = true;
    EXPECT_EQ(gaud_ogg_reader_seek(&reader, from_offset), GAUD_OK);
  }
  for (;;) {
    const unsigned char * data = nullptr;
    size_t size = 0;
    uint64_t granule = 0;
    GAUD_Result result
        = gaud_ogg_reader_packet(&reader, &data, &size, &granule, nullptr);
    if (result != GAUD_OK) {
      drained.ended = result;
      break;
    }
    drained.sizes.push_back(size);
    drained.granules.push_back(granule);
    drained.first_byte.push_back(size ? data[0] : 0u);
  }
  gaud_ogg_reader_free(&reader);
  return drained;
}

} // namespace

/* ------------------------------------------------------ packet assembly */

TEST(OggPackets, APacketSpanningThreePagesComesBackWhole) {
  /*
   * 140,000 bytes is more than two pages can hold (65,025 each), so the
   * packet occupies one full page, a second full page, and part of a
   * third. A reader that treated a page as a packet - which is the first
   * thing anyone writes, and which works on every file small enough to
   * fit in one page - reports three packets here.
   */
  std::vector<Packet> packets
      = {{filler(140000u, 1u), 1000u, true}, {filler(10u, 2u), 2000u, true}};
  Built built = build(packets);
  GAUD_Stream * stream = over(built.bytes);
  Drained drained = drain(stream);
  ASSERT_EQ(drained.sizes.size(), 2u);
  EXPECT_EQ(drained.sizes[0], 140000u);
  EXPECT_EQ(drained.sizes[1], 10u);
  gaud_stream_destroy(stream);
}

TEST(OggPackets, APacketAMultipleOf255LongEndsWhereItSays) {
  /*
   * The classic Ogg framing defect, in both directions. A packet of
   * exactly 255 bytes is one lacing value of 255 - which means "continues"
   * - followed by an explicit zero. A writer that omitted the zero would
   * join this packet to the next, and a reader that stopped at the 255
   * would split the next one. Both failures are invisible on any packet
   * whose length is not a multiple of 255, which is almost all of them.
   */
  for (size_t length : {255u, 510u, 65025u}) {
    std::vector<Packet> packets = {{filler(length, 3u), 100u, true},
        {filler(7u, 4u), 200u, true}};
    Built built = build(packets);
    GAUD_Stream * stream = over(built.bytes);
    Drained drained = drain(stream);
    ASSERT_EQ(drained.sizes.size(), 2u) << length;
    EXPECT_EQ(drained.sizes[0], length) << length;
    EXPECT_EQ(drained.sizes[1], 7u) << length;
    gaud_stream_destroy(stream);
  }
}

TEST(OggPackets, AZeroLengthPacketIsAPacket) {
  /* Opus has none, but Vorbis's floor-0 arm and a silent Opus frame can
   * both be a packet of no bytes, and Ogg spells it as a lacing value of
   * zero. A reader that skipped it loses the packet *count*, which is
   * what a granule position is reconciled against. */
  std::vector<Packet> packets = {{filler(5u, 5u), 10u, true},
      {{}, 20u, true}, {filler(6u, 6u), 30u, true}};
  Built built = build(packets);
  GAUD_Stream * stream = over(built.bytes);
  Drained drained = drain(stream);
  ASSERT_EQ(drained.sizes.size(), 3u);
  EXPECT_EQ(drained.sizes[0], 5u);
  EXPECT_EQ(drained.sizes[1], 0u);
  EXPECT_EQ(drained.sizes[2], 6u);
  gaud_stream_destroy(stream);
}

TEST(OggPackets, ASeekPastTheStartOfAPacketDropsItsTail) {
  /*
   * The defect gaud_ogg_reader_seek()'s drop_continued exists to prevent,
   * and it needs a hand-built file because no encoder writes a packet long
   * enough for a seek to land inside it.
   *
   * The first packet spans two pages. Seeking to the second page lands on
   * a page whose first packet began where the reader never looked, so what
   * finishes there is a fragment. A decoder handed that fragment has no
   * way to know: the tail of a compressed packet is bytes, and bytes
   * decode to something. So the first packet the reader yields after the
   * seek must be the *next* whole one - identified here by its first byte,
   * which the filler makes distinct per packet.
   */
  std::vector<unsigned char> spanning = filler(100000u, 7u);
  std::vector<unsigned char> after = filler(40u, 8u);
  std::vector<Packet> packets
      = {{spanning, 1000u, true}, {after, 2000u, true}};
  Built built = build(packets);

  /* Where the second page begins: the first page is 27 + 255 + 65,025. */
  const uint64_t second_page = 27u + 255u + 65025u;
  ASSERT_LT(second_page, built.bytes.size());
  ASSERT_EQ(memcmp(&built.bytes[second_page], "OggS", 4), 0);

  GAUD_Stream * stream = over(built.bytes);
  Drained drained = drain(stream, second_page);
  ASSERT_EQ(drained.sizes.size(), 1u);
  EXPECT_EQ(drained.sizes[0], 40u);
  EXPECT_EQ(drained.first_byte[0], after[0]);
  gaud_stream_destroy(stream);
}

/* ----------------------------------------------------------- the scan */

TEST(OggScan, AFalseMagicInAPageBodyIsNotAPage) {
  /*
   * **The control for the whole seek layer.** A page body is compressed
   * audio and may contain `OggS`; a scan that believed the magic would
   * find a page that is not there, read a nonsense segment table, and
   * return an offset in the middle of real data. The checksum is what
   * rules it out.
   *
   * The planted bytes are not random: they are a *plausible* page header -
   * correct magic, version zero, a segment count, a serial - so that
   * everything except the CRC agrees with it. That is the only version of
   * this test that proves the CRC is what rejected it, rather than some
   * earlier field check.
   */
  std::vector<unsigned char> body = filler(4000u, 9u);
  const size_t plant = 1000u;
  memcpy(&body[plant], "OggS", 4);
  body[plant + 4] = 0u;    /* version */
  body[plant + 5] = 0u;    /* flags: a continuation of nothing */
  memset(&body[plant + 6], 0, 8);  /* granule 0 */
  body[plant + 14] = 0x34u;        /* the same serial, little-endian */
  body[plant + 15] = 0x12u;
  body[plant + 16] = 0u;
  body[plant + 17] = 0u;
  memset(&body[plant + 18], 0, 4); /* sequence 0 */
  memset(&body[plant + 22], 0, 4); /* and a checksum of zero, which is the
                                    * only field that can be wrong once the
                                    * rest is right */
  body[plant + 26] = 1u;           /* one segment */
  body[plant + 27] = 10u;          /* ...of ten bytes */

  std::vector<Packet> packets = {{body, 500u, true}};
  Built built = build(packets);
  GAUD_Stream * stream = over(built.bytes);

  /* The real page is at zero. The scan from byte 1 must find nothing,
   * because the only other `OggS` in the file is the planted one. */
  ASSERT_NE(memcmp(&built.bytes[0], "OggS", 4), 0u + 1u); /* it is there */
  OGG_Page_Info page;
  EXPECT_EQ(gaud_ogg_find_page(stream, gaud_allocator_default(), 0u,
                built.bytes.size(), &page),
      GAUD_OK);
  EXPECT_EQ(page.offset, 0u);
  EXPECT_EQ(page.serial, 0x1234u);
  EXPECT_EQ(page.granule, 500u);

  EXPECT_EQ(gaud_ogg_find_page(stream, gaud_allocator_default(), 1u,
                built.bytes.size(), &page),
      GAUD_ERR_FORMAT);

  /* And the control for the control: with the planted checksum corrected,
   * the scan *does* find it - which is what shows the rejection above was
   * the CRC and not the scan failing to look. */
  std::vector<unsigned char> doctored = built.bytes;
  /* The real page's own header: 27 bytes plus one lacing value per 255
   * body bytes, plus the terminator. A 4,000-byte body is sixteen. */
  const size_t table = body.size() / 255u + 1u;
  size_t at = 27u + table + plant;
  ASSERT_EQ(memcmp(&doctored[at], "OggS", 4), 0);
  uint32_t crc = gaud_ogg_crc32(&doctored[at], 27u + 1u + 10u);
  doctored[at + 22] = (unsigned char)(crc & 0xFFu);
  doctored[at + 23] = (unsigned char)((crc >> 8) & 0xFFu);
  doctored[at + 24] = (unsigned char)((crc >> 16) & 0xFFu);
  doctored[at + 25] = (unsigned char)((crc >> 24) & 0xFFu);
  GAUD_Stream * second = over(doctored);
  EXPECT_EQ(gaud_ogg_find_page(second, gaud_allocator_default(), 1u,
                doctored.size(), &page),
      GAUD_OK);
  EXPECT_EQ(page.offset, at);
  gaud_stream_destroy(second);
  gaud_stream_destroy(stream);
}

TEST(OggScan, AMagicStraddlingTheWindowBoundaryIsStillFound) {
  /*
   * The scan reads 65,536 bytes at a time and searches all but the last
   * three, so a magic beginning in those three bytes is found by the next
   * window rather than lost between them.
   *
   * **The overlap cannot be reached from a valid Ogg file**, and that is
   * worth writing down because it is why this test looks artificial. One
   * page is at most 27 + 255 + 65,025 = 65,307 bytes, so two consecutive
   * page magics are never more than that far apart and a magic can never
   * fall in the last three bytes of a window whose start was a page. The
   * overlap matters when the scan crosses a stretch that is *not* Ogg -
   * which is every scan of data not yet identified, and which
   * gaud_ogg_last_granule() does on purpose when its window lands in the
   * middle of a page body.
   *
   * So: 65,534 zero bytes, then a page. Without the three-byte overlap the
   * page is invisible and the file reads as containing none.
   */
  std::vector<Packet> packets = {{filler(9u, 12u), 200u, true}};
  Built built = build(packets);
  const uint64_t pad = 65534u;
  std::vector<unsigned char> padded(pad, 0u);
  padded.insert(padded.end(), built.bytes.begin(), built.bytes.end());
  ASSERT_EQ(memcmp(&padded[pad], "OggS", 4), 0);

  GAUD_Stream * stream = over(padded);
  OGG_Page_Info page;
  ASSERT_EQ(gaud_ogg_find_page(
                stream, gaud_allocator_default(), 0u, padded.size(), &page),
      GAUD_OK);
  EXPECT_EQ(page.offset, pad);
  EXPECT_EQ(page.granule, 200u);
  gaud_stream_destroy(stream);
}

/* ------------------------------------------------------ logical streams */

TEST(OggLogical, OneStreamIsReportedWithItsCodecSignature) {
  std::vector<unsigned char> head = {0x01u, 'v', 'o', 'r', 'b', 'i', 's'};
  std::vector<Packet> packets
      = {{head, 0u, true}, {filler(30u, 13u), 100u, true}};
  Built built = build(packets, 0xABCDEF01u);
  GAUD_Stream * stream = over(built.bytes);
  OGG_Logical streams[OGG_MAX_LOGICAL];
  size_t count = 0;
  ASSERT_EQ(gaud_ogg_scan_logical(stream, gaud_allocator_default(), streams,
                OGG_MAX_LOGICAL, &count),
      GAUD_OK);
  ASSERT_EQ(count, 1u);
  EXPECT_EQ(streams[0].serial, 0xABCDEF01u);
  EXPECT_EQ(streams[0].offset, 0u);
  ASSERT_EQ(streams[0].head_size, head.size());
  EXPECT_EQ(memcmp(streams[0].head, head.data(), head.size()), 0);
  gaud_stream_destroy(stream);
}

TEST(OggLogical, TwoMultiplexedStreamsAreBothReported) {
  /*
   * Ogg's own requirement is that every logical stream's BOS page precede
   * any other page, so a multiplexed file begins with all of them. Built
   * by hand because nothing in this tree writes one: a multiplexed Ogg
   * file is what a video file is, and the reason to read one here is that
   * a reader which took the first serial it saw and stopped would decode
   * a video file's *subtitles* as audio on some files and work on others.
   */
  Built first = build({{{0x01u, 'v', 'o', 'r', 'b', 'i', 's'}, 0u, true}},
      0x11111111u);
  Built second = build({{{'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'}, 0u, true}},
      0x22222222u);
  std::vector<unsigned char> both = first.bytes;
  both.insert(both.end(), second.bytes.begin(), second.bytes.end());

  GAUD_Stream * stream = over(both);
  OGG_Logical streams[OGG_MAX_LOGICAL];
  size_t count = 0;
  ASSERT_EQ(gaud_ogg_scan_logical(stream, gaud_allocator_default(), streams,
                OGG_MAX_LOGICAL, &count),
      GAUD_OK);
  ASSERT_EQ(count, 2u);
  EXPECT_EQ(streams[0].serial, 0x11111111u);
  EXPECT_EQ(streams[1].serial, 0x22222222u);
  EXPECT_EQ(memcmp(streams[1].head, "OpusHead", 8), 0);
  gaud_stream_destroy(stream);
}

/* --------------------------------------------------- the last granule */

TEST(OggGranule, TheLastGranuleIsTheOneTheLastPageStates) {
  std::vector<Packet> packets = {{filler(10u, 14u), 1000u, true},
      {filler(10u, 15u), 2000u, true}, {filler(10u, 16u), 3000u, true}};
  Built built = build(packets);
  GAUD_Stream * stream = over(built.bytes);
  uint64_t granule = 0;
  ASSERT_EQ(gaud_ogg_last_granule(
                stream, gaud_allocator_default(), 0x1234u, &granule),
      GAUD_OK);
  EXPECT_EQ(granule, 3000u);

  /* Another stream's number is not an error in the file; it is a question
   * the file does not answer. */
  EXPECT_EQ(gaud_ogg_last_granule(
                stream, gaud_allocator_default(), 0x9999u, &granule),
      GAUD_ERR_FORMAT);
  gaud_stream_destroy(stream);
}

TEST(OggGranule, APageStatingNoPositionIsNotTheAnswer) {
  /*
   * A page that finishes no packet states all-ones, and a reader that took
   * the last page's granule position verbatim would report a length of
   * 2^64 - 1 for any file whose final packet spans two pages. That is not
   * a hypothetical shape: it is what a large final Vorbis packet does.
   */
  std::vector<Packet> packets = {{filler(10u, 17u), 4000u, true},
      {filler(100000u, 18u), 9000u, true}};
  Built built = build(packets);
  GAUD_Stream * stream = over(built.bytes);

  /* The middle page of the spanning packet states no position. */
  OGG_Page_Info page;
  ASSERT_EQ(gaud_ogg_find_page(stream, gaud_allocator_default(),
                27u + 1u + 10u, built.bytes.size(), &page),
      GAUD_OK);
  EXPECT_EQ(page.granule, OGG_NO_GRANULE);

  uint64_t granule = 0;
  ASSERT_EQ(gaud_ogg_last_granule(
                stream, gaud_allocator_default(), 0x1234u, &granule),
      GAUD_OK);
  EXPECT_EQ(granule, 9000u);
  gaud_stream_destroy(stream);
}

/* ------------------------------------------------------- the bisection */

TEST(OggBisect, TheOffsetMovesWithTheTarget) {
  /*
   * **The assertion that the bisection is doing anything.** It is allowed
   * to return the start of the file whenever it finds nothing better, and
   * that fallback is correct - a caller decodes forward from there and
   * gets the right samples. Which means a bisection that always returned
   * zero would pass every test that only looked at the audio. So what is
   * checked here is the offset, and that it increases.
   *
   * 400 pages of 160 bytes each, with a granule position of 1,000 per
   * page, so that a target's expected page is arithmetic rather than
   * measured: the page whose granule is at or below the target.
   */
  std::vector<Packet> packets;
  const uint64_t pages = 400u;
  for (uint64_t i = 0; i < pages; ++i) {
    packets.push_back({filler(160u, (unsigned)i), (i + 1u) * 1000u, true});
  }
  Built built = build(packets);
  GAUD_Stream * stream = over(built.bytes);
  OGG_Reader reader;
  gaud_ogg_reader_init(&reader, stream, gaud_allocator_default());
  reader.serial = 0x1234u;
  reader.have_serial = true;

  uint64_t previous_offset = 0;
  uint64_t previous_granule = 0;
  for (uint64_t target : {0u, 1u, 1000u, 1001u, 50000u, 199999u, 200000u,
           399999u, 400000u, 500000u}) {
    uint64_t offset = 0;
    uint64_t granule = 0;
    ASSERT_EQ(gaud_ogg_bisect(&reader, target, 0u, &offset, &granule),
        GAUD_OK)
        << target;
    /* Never past the target: a caller decodes forward, never backward. */
    EXPECT_LE(granule, target) << target;
    /* And never behind an earlier target's answer. */
    EXPECT_GE(offset, previous_offset) << target;
    EXPECT_GE(granule, previous_granule) << target;
    previous_offset = offset;
    previous_granule = granule;
    /* The granule must be a multiple of the per-page step, i.e. a real
     * page boundary rather than an interpolation. */
    EXPECT_EQ(granule % 1000u, 0u) << target;
  }
  /* The last target is past the end of the stream, so the answer is the
   * last page - which is the far end a bisection gets wrong. */
  EXPECT_EQ(previous_granule, pages * 1000u);

  /* And the near end: a target inside the first page has nothing before
   * it, so the answer is the floor with a granule of zero. */
  uint64_t offset = UINT64_MAX;
  uint64_t granule = UINT64_MAX;
  ASSERT_EQ(gaud_ogg_bisect(&reader, 1u, 0u, &offset, &granule), GAUD_OK);
  EXPECT_EQ(offset, 0u);
  EXPECT_EQ(granule, 0u);

  /* The middle of the file must not be answered with the start of it. */
  ASSERT_EQ(gaud_ogg_bisect(&reader, 200000u, 0u, &offset, &granule),
      GAUD_OK);
  EXPECT_GT(offset, built.bytes.size() / 4u);
  EXPECT_EQ(granule, 200000u);

  gaud_ogg_reader_free(&reader);
  gaud_stream_destroy(stream);
}

TEST(OggBisect, ThePageItLandsOnYieldsWholePackets) {
  /*
   * The bisection's result is fed to gaud_ogg_reader_seek(), and what
   * comes back has to be a packet and not the tail of one. Built with
   * packets of 700 bytes and no flush, so that pages hold several packets
   * and some packets straddle a page boundary - which is the packing a
   * real Vorbis encoder uses and the one Ogg FLAC never produces.
   */
  std::vector<Packet> packets;
  const uint64_t count = 600u;
  for (uint64_t i = 0; i < count; ++i) {
    packets.push_back({filler(700u, (unsigned)(i + 100u)),
        (i + 1u) * 100u, false});
  }
  Built built = build(packets);
  GAUD_Stream * stream = over(built.bytes);
  OGG_Reader reader;
  gaud_ogg_reader_init(&reader, stream, gaud_allocator_default());
  reader.serial = 0x1234u;
  reader.have_serial = true;

  uint64_t offset = 0;
  uint64_t granule = 0;
  ASSERT_EQ(gaud_ogg_bisect(&reader, 30000u, 0u, &offset, &granule), GAUD_OK);
  EXPECT_GT(offset, 0u);
  gaud_ogg_reader_free(&reader);

  Drained drained = drain(stream, offset);
  ASSERT_GT(drained.sizes.size(), 0u);
  /* Every packet whole: the filler makes 700 the only legal length, so a
   * fragment is caught by its size. */
  for (size_t i = 0; i < drained.sizes.size(); ++i) {
    EXPECT_EQ(drained.sizes[i], 700u) << i;
  }
  gaud_stream_destroy(stream);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
