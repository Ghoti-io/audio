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
 * FLAC: the properties that are exact, and that a differential would not
 * see.
 *
 * `make check-corpus` scores our decode against libFLAC and ffmpeg and
 * `make check-writer` scores our encode the same way, so the sample values
 * are not what is asserted here. What is here is everything a reference
 * cannot be asked about:
 *
 *   - the bit reader and writer are inverses at every width, including the
 *     widths no fixture happens to contain;
 *   - the two checksums match the published check values for their
 *     parameters, which is the only way to tell a transcribed polynomial
 *     from a plausible one;
 *   - the format's own refusals happen - a bit depth with no lossless
 *     landing place, a STREAMINFO the specification forbids;
 *   - a seek lands on the sample it was asked for, which a decode-from-the-
 *     start comparison cannot distinguish from a seek that silently did
 *     nothing;
 *   - a cuesheet of exactly the right length is accepted and one byte
 *     short is refused, which is the regression test for an off-by-one
 *     that dropped every real cuesheet while looking like a corrupt-block
 *     refusal.
 *
 * **Every round trip asserts the frame count before it compares a
 * sample.** A decoder that returns zero frames compares nothing and passes
 * any loop that walks what it was given, which is the first trap
 * planning/audio.md section 12 names.
 */

#include "../../src/codec/flac/flac_internal.h"
#include <ghoti.io/audio/audio.h>
#include <ghoti.io/cutil/allocator.h>
#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>

namespace {

/** Registers the codecs once, however many tests run. */
struct Registered {
  Registered() { gaud_register_builtin_codecs(); }
};
const Registered registered;

/* ------------------------------------------------------------ bit layer */

TEST(FlacBits, UnsignedReadsBackAtEveryWidth) {
  /* Every width from 1 to 32, each carrying its own largest value and a
   * value with alternating bits - so a reader that masked with the wrong
   * width fails on one of the two. */
  for (unsigned n = 1; n <= 32u; ++n) {
    uint64_t top = n == 32u ? 0xFFFFFFFFull : (((uint64_t)1 << n) - 1u);
    uint64_t alternating = 0xAAAAAAAAull & top;
    FLAC_Bit_Writer bw;
    gaud_flac_bitw_init(&bw, NULL);
    /* A three-bit lead-in, so nothing below is byte-aligned. A bit reader
     * tested only on aligned fields is tested on the easy half. */
    gaud_flac_bitw_write(&bw, 5u, 3u);
    gaud_flac_bitw_write(&bw, top, n);
    gaud_flac_bitw_write(&bw, alternating, n);
    gaud_flac_bitw_align(&bw);
    ASSERT_FALSE(bw.failed) << "width " << n;

    FLAC_Bits br;
    gaud_flac_bits_init(&br, bw.data, bw.size);
    EXPECT_EQ(gaud_flac_bits_read(&br, 3u), 5u) << "width " << n;
    EXPECT_EQ(gaud_flac_bits_read(&br, n), top) << "width " << n;
    EXPECT_EQ(gaud_flac_bits_read(&br, n), alternating) << "width " << n;
    EXPECT_FALSE(br.overrun) << "width " << n;
    gaud_flac_bitw_free(&bw);
  }
}

TEST(FlacBits, SignedReadsBackAtEveryWidth) {
  for (unsigned n = 1; n <= 64u; ++n) {
    /* The two values a sign-extension gets wrong: the most negative, where
     * negating the magnitude overflows, and minus one, where every bit is
     * set. */
    int64_t most_negative
        = n == 64u ? INT64_MIN : -((int64_t)1 << (n - 1u));
    int64_t values[3] = {most_negative, -1,
        n == 64u ? INT64_MAX : (((int64_t)1 << (n - 1u)) - 1)};
    FLAC_Bit_Writer bw;
    gaud_flac_bitw_init(&bw, NULL);
    gaud_flac_bitw_write(&bw, 1u, 1u);
    for (int64_t value : values) {
      gaud_flac_bitw_write(&bw, (uint64_t)value, n);
    }
    gaud_flac_bitw_align(&bw);
    ASSERT_FALSE(bw.failed) << "width " << n;

    FLAC_Bits br;
    gaud_flac_bits_init(&br, bw.data, bw.size);
    EXPECT_EQ(gaud_flac_bits_read(&br, 1u), 1u);
    for (int64_t value : values) {
      EXPECT_EQ(gaud_flac_bits_read_signed64(&br, n), value)
          << "width " << n;
    }
    EXPECT_FALSE(br.overrun) << "width " << n;
    gaud_flac_bitw_free(&bw);
  }
}

TEST(FlacBits, UnaryReadsBackIncludingLongRuns) {
  /* 0 and 1 are the ordinary cases; 63 and 64 straddle the reader's cache;
   * 300 is longer than any single refill. */
  const uint32_t runs[] = {0, 1u, 7u, 63u, 64u, 65u, 300u};
  FLAC_Bit_Writer bw;
  gaud_flac_bitw_init(&bw, NULL);
  for (uint32_t run : runs) {
    gaud_flac_bitw_write_unary(&bw, run);
  }
  gaud_flac_bitw_align(&bw);
  ASSERT_FALSE(bw.failed);

  FLAC_Bits br;
  gaud_flac_bits_init(&br, bw.data, bw.size);
  for (uint32_t run : runs) {
    EXPECT_EQ(gaud_flac_bits_read_unary(&br), run);
  }
  EXPECT_FALSE(br.overrun);
  gaud_flac_bitw_free(&bw);
}

TEST(FlacBits, CodedNumberReadsBackAtEveryLength) {
  /* Each boundary and the value either side of it, so a ladder that is one
   * step out fails rather than merely coding a value inefficiently. */
  const uint64_t boundaries[] = {0, 1u, 0x7Fu, 0x80u, 0x7FFu, 0x800u,
      0xFFFFu, 0x10000u, 0x1FFFFFu, 0x200000u, 0x3FFFFFFu, 0x4000000u,
      0x7FFFFFFFull, 0x80000000ull, 0xFFFFFFFFFull};
  for (uint64_t value : boundaries) {
    FLAC_Bit_Writer bw;
    gaud_flac_bitw_init(&bw, NULL);
    gaud_flac_bitw_write_coded_number(&bw, value);
    ASSERT_FALSE(bw.failed) << value;

    FLAC_Bits br;
    gaud_flac_bits_init(&br, bw.data, bw.size);
    uint64_t read = 0;
    ASSERT_TRUE(gaud_flac_bits_read_coded_number(&br, &read)) << value;
    EXPECT_EQ(read, value);
    gaud_flac_bitw_free(&bw);
  }
}

TEST(FlacBits, OverrunLatchesAndYieldsZero) {
  const unsigned char data[1] = {0xFF};
  FLAC_Bits br;
  gaud_flac_bits_init(&br, data, sizeof(data));
  EXPECT_EQ(gaud_flac_bits_read(&br, 8u), 0xFFu);
  EXPECT_FALSE(br.overrun);
  EXPECT_EQ(gaud_flac_bits_read(&br, 1u), 0u);
  EXPECT_TRUE(br.overrun);
  /* Sticky: a later read cannot clear it, which is what lets the frame
   * decoder ask once at the end instead of at every field. */
  EXPECT_EQ(gaud_flac_bits_read(&br, 1u), 0u);
  EXPECT_TRUE(br.overrun);
}

/* --------------------------------------------------------------- CRCs */

TEST(FlacCrc, MatchThePublishedCheckValues) {
  /*
   * The check value of a CRC is its result over the nine ASCII bytes
   * "123456789", and it is published for every catalogued parameter set.
   * Comparing against it is what distinguishes a correct transcription of
   * a polynomial from a plausible one - a reflected variant, a different
   * initial value or a final xor all produce a self-consistent checksum
   * that round-trips with itself and disagrees with every other
   * implementation.
   *
   * FLAC's CRC-8 is the catalogue's CRC-8/SMBUS: poly 0x07, init 0x00, no
   * reflection, no final xor, check 0xF4. Its CRC-16 is CRC-16/UMTS (also
   * called CRC-16/BUYPASS): poly 0x8005, init 0x0000, no reflection, no
   * final xor, check 0xFEE8.
   */
  const unsigned char check[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(gaud_flac_crc8(check, sizeof(check)), 0xF4u);
  EXPECT_EQ(gaud_flac_crc16(check, sizeof(check)), 0xFEE8u);
}

/* -------------------------------------------------------- STREAMINFO */

/** A STREAMINFO body that is valid, for a test to then break one field of. */
std::vector<unsigned char> good_streaminfo() {
  FLAC_Bit_Writer bw;
  gaud_flac_bitw_init(&bw, NULL);
  gaud_flac_bitw_write(&bw, 4096u, 16u);  /* min block */
  gaud_flac_bitw_write(&bw, 4096u, 16u);  /* max block */
  gaud_flac_bitw_write(&bw, 100u, 24u);   /* min frame */
  gaud_flac_bitw_write(&bw, 2000u, 24u);  /* max frame */
  gaud_flac_bitw_write(&bw, 44100u, 20u);
  gaud_flac_bitw_write(&bw, 1u, 3u);      /* channels - 1 */
  gaud_flac_bitw_write(&bw, 15u, 5u);     /* bits - 1 */
  gaud_flac_bitw_write(&bw, 1000u, 36u);
  std::vector<unsigned char> block(bw.data, bw.data + bw.size);
  block.resize(FLAC_STREAMINFO_SIZE, 0);
  gaud_flac_bitw_free(&bw);
  return block;
}

TEST(FlacStreaminfo, AcceptsAValidBlockAndRefusesWhatTheFormatForbids) {
  std::vector<unsigned char> block = good_streaminfo();
  FLAC_Streaminfo info;
  ASSERT_EQ(gaud_flac_parse_streaminfo(block.data(), block.size(), &info),
      GAUD_OK);
  EXPECT_EQ(info.sample_rate, 44100u);
  EXPECT_EQ(info.channels, 2u);
  EXPECT_EQ(info.bits_per_sample, 16u);
  EXPECT_EQ(info.total_samples, 1000u);
  EXPECT_EQ(info.max_block_size, 4096u);

  /* A block size below 16 is forbidden; the control above shows the only
   * difference is the field. */
  std::vector<unsigned char> small = block;
  small[0] = 0;
  small[1] = 8;
  small[2] = 0;
  small[3] = 8;
  EXPECT_EQ(gaud_flac_parse_streaminfo(small.data(), small.size(), &info),
      GAUD_ERR_CORRUPT);

  /* A minimum above the maximum describes no stream. */
  std::vector<unsigned char> inverted = block;
  inverted[0] = 0xFF;
  inverted[1] = 0xFF;
  EXPECT_EQ(
      gaud_flac_parse_streaminfo(inverted.data(), inverted.size(), &info),
      GAUD_ERR_CORRUPT);

  /* A rate of zero is a stream that is not audio. */
  std::vector<unsigned char> silent = block;
  silent[10] = 0;
  silent[11] = 0;
  silent[12] = (unsigned char)(silent[12] & 0x0F);
  EXPECT_EQ(gaud_flac_parse_streaminfo(silent.data(), silent.size(), &info),
      GAUD_ERR_UNSUPPORTED);

  /* Truncated. */
  EXPECT_EQ(gaud_flac_parse_streaminfo(block.data(), 33u, &info),
      GAUD_ERR_CORRUPT);
}

TEST(FlacStreaminfo, OnlyTheFourExactDepthsHaveASampleFormat) {
  /*
   * RFC 9639 allows 4 to 32 bits and the buffer holds 8, 16, 24 and 32.
   * For the rest there is no lossless landing place and this refuses
   * rather than scaling - see gaud_flac_format_for_depth(). The reference
   * encoder agrees to the extent that it can: `flac --bps=20` is refused
   * by the `flac` command line with "must be 8/16/24/32".
   */
  GAUD_Sample_Format format = GAUD_SAMPLE_FORMAT_COUNT;
  for (uint32_t bits = 1; bits <= 33u; ++bits) {
    bool exact = bits == 8u || bits == 16u || bits == 24u || bits == 32u;
    EXPECT_EQ(gaud_flac_format_for_depth(bits, &format), exact)
        << "depth " << bits;
  }
  EXPECT_TRUE(gaud_flac_format_for_depth(8u, &format));
  EXPECT_EQ(format, GAUD_SAMPLE_S8);
  EXPECT_TRUE(gaud_flac_format_for_depth(24u, &format));
  EXPECT_EQ(format, GAUD_SAMPLE_S24);
}

/* ----------------------------------------------------------- CUESHEET */

/** A cuesheet with @p tracks tracks, each carrying @p indices index points. */
std::vector<unsigned char> make_cuesheet(unsigned tracks, unsigned indices) {
  /* 128 of catalogue number, 8 of lead-in, 259 of flag and reserved, and
   * one of track count - 396, which is the number this got wrong. */
  std::vector<unsigned char> block(396u, 0);
  block[395] = (unsigned char)tracks;
  for (unsigned t = 0; t < tracks; ++t) {
    std::vector<unsigned char> entry(36u, 0);
    entry[8] = (unsigned char)(t + 1u);
    entry[35] = (unsigned char)indices;
    block.insert(block.end(), entry.begin(), entry.end());
    block.insert(block.end(), (size_t)indices * 12u, 0);
  }
  return block;
}

TEST(FlacCuesheet, AcceptsExactlyTheRightLengthAndRefusesOneByteShort) {
  GAUD_Limits limits;
  gaud_limits_default(&limits);

  std::vector<unsigned char> sheet = make_cuesheet(3u, 2u);
  uint32_t tracks = 0;
  ASSERT_EQ(gaud_flac_check_cuesheet(
                sheet.data(), sheet.size(), &limits, &tracks, nullptr),
      GAUD_OK);
  EXPECT_EQ(tracks, 3u);

  /*
   * The regression test. The fixed header is 396 bytes and was once
   * computed as 395, which read the track count from the last reserved
   * byte - always zero - and then found 396 bytes of trailing data the
   * counts did not explain. Every real cuesheet was refused, and because
   * this check exists to refuse corrupt blocks, that looked like the
   * blocks being corrupt rather than the check being wrong. A structural
   * check that is slightly off does not let bad input through; it rejects
   * good input, and nothing inside this library can tell the difference.
   */
  std::vector<unsigned char> truncated(sheet.begin(), sheet.end() - 1);
  EXPECT_EQ(gaud_flac_check_cuesheet(truncated.data(), truncated.size(),
                &limits, nullptr, nullptr),
      GAUD_ERR_CORRUPT);

  /* Trailing bytes the counts do not explain. */
  std::vector<unsigned char> padded = sheet;
  padded.push_back(0);
  EXPECT_EQ(gaud_flac_check_cuesheet(
                padded.data(), padded.size(), &limits, nullptr, nullptr),
      GAUD_ERR_CORRUPT);

  /* Shorter than the fixed header. */
  std::vector<unsigned char> stub(100u, 0);
  EXPECT_EQ(gaud_flac_check_cuesheet(
                stub.data(), stub.size(), &limits, nullptr, nullptr),
      GAUD_ERR_CORRUPT);

  /* An empty sheet - no tracks - is exactly the fixed header. */
  std::vector<unsigned char> empty = make_cuesheet(0, 0);
  EXPECT_EQ(empty.size(), 396u);
  EXPECT_EQ(gaud_flac_check_cuesheet(
                empty.data(), empty.size(), &limits, &tracks, nullptr),
      GAUD_OK);
  EXPECT_EQ(tracks, 0u);
}

TEST(FlacCuesheet, IndexPointsCountAgainstTheCueLimit) {
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  limits.max_cue_points = 3u;
  std::vector<unsigned char> sheet = make_cuesheet(2u, 4u);
  EXPECT_EQ(gaud_flac_check_cuesheet(
                sheet.data(), sheet.size(), &limits, nullptr, nullptr),
      GAUD_ERR_LIMIT);
  /* The control: the same sheet under a limit that admits it. */
  limits.max_cue_points = 100u;
  EXPECT_EQ(gaud_flac_check_cuesheet(
                sheet.data(), sheet.size(), &limits, nullptr, nullptr),
      GAUD_OK);
}

/* ------------------------------------------------------- round trips */

/** A signal with enough structure that the encoder picks real predictors. */
std::vector<int32_t> make_signal(size_t frames, uint32_t channels,
    uint32_t bits) {
  std::vector<int32_t> samples(frames * channels);
  int64_t span = (int64_t)1 << (bits - 1u);
  for (size_t f = 0; f < frames; ++f) {
    for (uint32_t ch = 0; ch < channels; ++ch) {
      /* A slow ramp, a fast one and a sprinkle of noise: the slow part is
       * what a fixed predictor handles well, and the noise is what stops
       * every subframe being CONSTANT. */
      int64_t value = (int64_t)((f * (7u + ch)) % 1024u) - 512;
      value += (int64_t)((f * 2654435761u + ch * 40503u) % 97u) - 48;
      value = value * span / 2048;
      samples[f * channels + ch] = (int32_t)value;
    }
  }
  return samples;
}

/** Encode @p samples with @p codec and return the bytes. */
std::vector<unsigned char> encode(const char * codec, GAUD_Sample_Format format,
    uint32_t channels, uint32_t rate, const std::vector<int32_t> & samples,
    size_t frames) {
  GAUD_Stream * out = nullptr;
  EXPECT_EQ(gaud_stream_create_memory_writer(nullptr, &out), GAUD_OK);
  GAUD_Encode_Params params;
  gaud_encode_params_default(&params);
  params.format = format;
  params.layout = gaud_channel_layout_default(channels);
  params.sample_rate = rate;

  GAUD_Encoder * encoder = nullptr;
  EXPECT_EQ(gaud_encoder_create(codec, nullptr, out, &params, &encoder),
      GAUD_OK);

  GAUD_Buffer * buffer = nullptr;
  /* 997 frames: not a divisor of the encoder's 4096-frame block, so the
   * block boundary falls inside a write every time. A buffer that lined up
   * with the block would never exercise the carry. */
  EXPECT_EQ(gaud_buffer_create(nullptr, format, params.layout,
                GAUD_LAYOUT_INTERLEAVED, 997u, &buffer),
      GAUD_OK);
  unsigned width = gaud_sample_format_bits(format) / 8u;
  size_t at = 0;
  while (at < frames) {
    size_t take = frames - at < 997u ? frames - at : 997u;
    unsigned char * data = (unsigned char *)gaud_buffer_data(buffer);
    for (size_t f = 0; f < take; ++f) {
      for (uint32_t ch = 0; ch < channels; ++ch) {
        uint32_t value = (uint32_t)samples[(at + f) * channels + ch];
        unsigned char * to = data + (f * channels + ch) * width;
        for (unsigned b = 0; b < width; ++b) {
          to[b] = (unsigned char)((value >> (8u * b)) & 0xFFu);
        }
      }
    }
    EXPECT_EQ(gaud_buffer_set_frames(buffer, take), GAUD_OK);
    EXPECT_EQ(gaud_encoder_write(encoder, buffer), GAUD_OK);
    at += take;
  }
  EXPECT_EQ(gaud_encoder_finish(encoder), GAUD_OK);
  gaud_buffer_destroy(buffer);

  const void * bytes = nullptr;
  size_t length = 0;
  EXPECT_EQ(gaud_stream_writer_bytes(out, &bytes, &length), GAUD_OK);
  std::vector<unsigned char> result(
      (const unsigned char *)bytes, (const unsigned char *)bytes + length);
  gaud_encoder_destroy(encoder);
  gaud_stream_destroy(out);
  return result;
}

/** Decode @p file wholly, asserting the frame count before anything else. */
std::vector<int32_t> decode(const std::vector<unsigned char> & file,
    uint32_t channels, size_t expect_frames, const char * expect_codec) {
  GAUD_Stream * in = nullptr;
  EXPECT_EQ(gaud_stream_create_memory(file.data(), file.size(), &in), GAUD_OK);
  GAUD_Doc * doc = nullptr;
  EXPECT_EQ(gaud_doc_load(nullptr, in, nullptr, nullptr, &doc), GAUD_OK);
  std::vector<int32_t> out;
  if (!doc) {
    gaud_stream_destroy(in);
    return out;
  }
  EXPECT_STREQ(gaud_doc_codec_name(doc), expect_codec);
  GAUD_Track * track = gaud_doc_track(doc, 0);
  /* The count the container states, before a single sample is looked at:
   * a decoder that produced silence of the right length would otherwise
   * pass every comparison below that walks what it was given. */
  EXPECT_EQ(gaud_track_frames(track), expect_frames);

  GAUD_Decoder * decoder = nullptr;
  EXPECT_EQ(gaud_decoder_create(track, &decoder), GAUD_OK);
  GAUD_Buffer * buffer = nullptr;
  EXPECT_EQ(
      gaud_decoder_buffer_create(decoder, nullptr, 1013u, &buffer), GAUD_OK);
  GAUD_Sample_Format format = gaud_track_format(track);
  unsigned width = gaud_sample_format_bits(format) / 8u;
  for (;;) {
    EXPECT_EQ(gaud_decoder_read(decoder, buffer), GAUD_OK);
    size_t got = gaud_buffer_frames(buffer);
    if (got == 0) {
      break;
    }
    const unsigned char * data
        = (const unsigned char *)gaud_buffer_data_const(buffer);
    for (size_t f = 0; f < got; ++f) {
      for (uint32_t ch = 0; ch < channels; ++ch) {
        const unsigned char * at = data + (f * channels + ch) * width;
        uint32_t raw = 0;
        for (unsigned b = 0; b < width; ++b) {
          raw |= (uint32_t)at[b] << (8u * b);
        }
        uint32_t sign = (uint32_t)1 << (width * 8u - 1u);
        out.push_back(raw & sign ? (int32_t)(raw | ~(sign * 2u - 1u))
                                 : (int32_t)raw);
      }
    }
  }
  gaud_buffer_destroy(buffer);
  gaud_decoder_destroy(decoder);
  gaud_doc_destroy(doc);
  gaud_stream_destroy(in);
  return out;
}

struct Shape {
  GAUD_Sample_Format format;
  uint32_t bits;
  uint32_t channels;
  uint32_t rate;
  const char * name;
};

/* Both stereo and not, every depth, and a channel count above the two the
 * decorrelation code paths handle - so independent coding is exercised as
 * well as the three stereo modes. */
const Shape shapes[] = {
    {GAUD_SAMPLE_S8, 8u, 1u, 8000u, "s8 mono"},
    {GAUD_SAMPLE_S16, 16u, 1u, 44100u, "s16 mono"},
    {GAUD_SAMPLE_S16, 16u, 2u, 44100u, "s16 stereo"},
    {GAUD_SAMPLE_S24, 24u, 2u, 48000u, "s24 stereo"},
    {GAUD_SAMPLE_S32, 32u, 2u, 96000u, "s32 stereo"},
    {GAUD_SAMPLE_S32, 32u, 1u, 192000u, "s32 mono"},
    {GAUD_SAMPLE_S16, 16u, 6u, 48000u, "s16 5.1"},
    {GAUD_SAMPLE_S16, 16u, 8u, 44100u, "s16 7.1"},
};

TEST(FlacRoundTrip, NativeIsLosslessForEveryShape) {
  /* 9001 frames: more than two of the encoder's 4096-frame blocks, and a
   * prime, so the last block is short and its own block-size field has to
   * be written rather than inherited. */
  const size_t frames = 9001u;
  for (const Shape & shape : shapes) {
    std::vector<int32_t> samples
        = make_signal(frames, shape.channels, shape.bits);
    std::vector<unsigned char> file = encode(
        "flac", shape.format, shape.channels, shape.rate, samples, frames);
    ASSERT_FALSE(file.empty()) << shape.name;
    std::vector<int32_t> back
        = decode(file, shape.channels, frames, "flac");
    ASSERT_EQ(back.size(), samples.size()) << shape.name;
    EXPECT_EQ(back, samples) << shape.name;
  }
}

TEST(FlacRoundTrip, OggIsLosslessForEveryShape) {
  const size_t frames = 9001u;
  for (const Shape & shape : shapes) {
    std::vector<int32_t> samples
        = make_signal(frames, shape.channels, shape.bits);
    std::vector<unsigned char> file = encode("ogg-flac", shape.format,
        shape.channels, shape.rate, samples, frames);
    ASSERT_FALSE(file.empty()) << shape.name;
    EXPECT_EQ(memcmp(file.data(), "OggS", 4), 0) << shape.name;
    std::vector<int32_t> back
        = decode(file, shape.channels, frames, "ogg-flac");
    ASSERT_EQ(back.size(), samples.size()) << shape.name;
    EXPECT_EQ(back, samples) << shape.name;
  }
}

/**
 * @brief Walk the page chain and count pages that continue a packet.
 *
 * Every page is required to start with `OggS` as it goes, so a walk that
 * lost the chain reports a failure rather than a count of zero.
 */
size_t pages_continuing_a_packet(const std::vector<unsigned char> & file) {
  size_t count = 0;
  size_t at = 0;
  while (at + 27u <= file.size()) {
    EXPECT_EQ(memcmp(file.data() + at, "OggS", 4), 0)
        << "lost the page chain at offset " << at;
    size_t segments = file[at + 26u];
    if (at + 27u + segments > file.size()) {
      break;
    }
    size_t body = 0;
    for (size_t i = 0; i < segments; ++i) {
      body += file[at + 27u + i];
    }
    if (file[at + 5u] & 0x01u) {
      ++count;
    }
    at += 27u + segments + body;
  }
  return count;
}

TEST(FlacRoundTrip, AnOggPacketLargerThanOnePageIsReassembled) {
  /* An Ogg page body is at most 255 lacing values of 255 bytes, so a
   * packet over 65,025 bytes is split and the pages after the first carry
   * the continuation flag. The reader then has to grow its packet buffer
   * while that buffer already holds the first part, and `make coverage`
   * showed that copy had never run: every packet every test and every
   * fixture had produced fitted in the first 4,096-byte allocation, or
   * arrived whole. An off-by-one in the length copied would have survived
   * the differential gates as well, because they decode our Ogg files
   * with libFLAC rather than with this reader.
   *
   * Eight channels of 32-bit noise is the shape that gets there: the
   * encoder can do nothing with it, so one 4,096-frame block is 131,072
   * bytes of subframe before any header. */
  const size_t frames = 9001u;
  const uint32_t channels = 8u;
  std::vector<int32_t> samples(frames * channels);
  uint32_t state = 0x9E3779B9u;
  for (auto & sample : samples) {
    state = state * 1664525u + 1013904223u;
    sample = (int32_t)state;
  }

  std::vector<unsigned char> file = encode(
      "ogg-flac", GAUD_SAMPLE_S32, channels, 48000u, samples, frames);
  ASSERT_FALSE(file.empty());

  /* Asserted rather than assumed. This test is about the carry, and a
   * packet that happened to fit in one page would exercise none of it
   * while still round-tripping perfectly - which is how a test quietly
   * stops testing what it was written for. */
  ASSERT_GT(pages_continuing_a_packet(file), 0u)
      << "no page continued a packet, so the reader never carried one";

  std::vector<int32_t> back = decode(file, channels, frames, "ogg-flac");
  ASSERT_EQ(back.size(), samples.size());
  EXPECT_EQ(back, samples);
}

TEST(FlacRoundTrip, ConstantAndVerbatimSubframesSurvive) {
  /* Silence picks CONSTANT for every subframe; white-ish noise with no
   * predictable structure pushes the planner towards VERBATIM or a
   * zero-order fixed predictor. Both are arms a signal with structure
   * never reaches. */
  const size_t frames = 5000u;
  for (int kind = 0; kind < 2; ++kind) {
    std::vector<int32_t> samples(frames * 2u, 0);
    if (kind == 1) {
      uint32_t state = 0x12345678u;
      for (auto & sample : samples) {
        state = state * 1664525u + 1013904223u;
        sample = (int32_t)(state >> 16) - 32768;
      }
    }
    std::vector<unsigned char> file
        = encode("flac", GAUD_SAMPLE_S16, 2u, 44100u, samples, frames);
    std::vector<int32_t> back = decode(file, 2u, frames, "flac");
    ASSERT_EQ(back.size(), samples.size()) << kind;
    EXPECT_EQ(back, samples) << kind;
  }
  /* Silence also has to compress: a file of zeroes that did not would mean
   * the escaped-partition arm never ran, and that arm is the only thing in
   * the encoder that emits an escape for the decoder to read back. */
  std::vector<int32_t> quiet(5000u * 2u, 0);
  std::vector<unsigned char> small
      = encode("flac", GAUD_SAMPLE_S16, 2u, 44100u, quiet, 5000u);
  EXPECT_LT(small.size(), 5000u * 2u * 2u / 8u);
}

TEST(FlacRoundTrip, WastedBitsSurvive) {
  /* Every sample a multiple of 256, which is what 8-bit material carried
   * in a 16-bit file looks like. The encoder shifts them out and the
   * decoder shifts them back; a decoder that did not would be quieter by
   * a factor of 256 and still produce the right number of frames. */
  const size_t frames = 4000u;
  std::vector<int32_t> samples(frames * 2u);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = (int32_t)(((i * 37u) % 200u) * 256u) - 25600;
  }
  std::vector<unsigned char> file
      = encode("flac", GAUD_SAMPLE_S16, 2u, 44100u, samples, frames);
  std::vector<int32_t> back = decode(file, 2u, frames, "flac");
  ASSERT_EQ(back.size(), samples.size());
  EXPECT_EQ(back, samples);
}

/* --------------------------------------------------------------- seeking */

TEST(FlacSeek, LandsOnTheFrameAskedFor) {
  /*
   * A ramp, so that a sample's value *is* its position: a seek that landed
   * anywhere else is caught by the value rather than by a count. Seeking
   * backwards as well as forwards, and to a position inside the frame
   * already decoded, which is the one case the decoder short-circuits.
   */
  const size_t frames = 20000u;
  std::vector<int32_t> samples(frames);
  for (size_t f = 0; f < frames; ++f) {
    samples[f] = (int32_t)(f % 30000u) - 15000;
  }
  for (const char * codec : {"flac", "ogg-flac"}) {
    std::vector<unsigned char> file
        = encode(codec, GAUD_SAMPLE_S16, 1u, 44100u, samples, frames);
    GAUD_Stream * in = nullptr;
    ASSERT_EQ(
        gaud_stream_create_memory(file.data(), file.size(), &in), GAUD_OK);
    GAUD_Doc * doc = nullptr;
    ASSERT_EQ(gaud_doc_load(nullptr, in, nullptr, nullptr, &doc), GAUD_OK);
    GAUD_Track * track = gaud_doc_track(doc, 0);
    GAUD_Decoder * decoder = nullptr;
    ASSERT_EQ(gaud_decoder_create(track, &decoder), GAUD_OK);
    GAUD_Buffer * buffer = nullptr;
    ASSERT_EQ(
        gaud_decoder_buffer_create(decoder, nullptr, 64u, &buffer), GAUD_OK);

    const uint64_t targets[] = {0, 4095u, 4096u, 4097u, 12345u, 100u,
        19999u, 8192u, 8200u};
    for (uint64_t target : targets) {
      uint64_t landed = UINT64_MAX;
      ASSERT_EQ(gaud_decoder_seek(decoder, target, &landed), GAUD_OK)
          << codec << " " << target;
      EXPECT_EQ(landed, target) << codec << " " << target;
      ASSERT_EQ(gaud_decoder_read(decoder, buffer), GAUD_OK);
      ASSERT_GT(gaud_buffer_frames(buffer), 0u) << codec << " " << target;
      const int16_t * data
          = (const int16_t *)gaud_buffer_data_const(buffer);
      EXPECT_EQ(data[0], samples[target]) << codec << " at " << target;
    }
    gaud_buffer_destroy(buffer);
    gaud_decoder_destroy(decoder);
    gaud_doc_destroy(doc);
    gaud_stream_destroy(in);
  }
}

TEST(FlacSeek, TheNativeWriterLeavesASeekTable) {
  /* Not only that seeking works, but that the table is there: a decoder
   * that scanned from the start would pass the test above and leave the
   * table's reader and writer both unexercised. */
  const size_t frames = 60000u;
  std::vector<int32_t> samples = make_signal(frames, 1u, 16u);
  std::vector<unsigned char> file
      = encode("flac", GAUD_SAMPLE_S16, 1u, 44100u, samples, frames);

  bool seen = false;
  size_t at = 4;
  while (at + FLAC_BLOCK_HEADER <= file.size()) {
    bool last = (file[at] & 0x80u) != 0;
    unsigned type = file[at] & 0x7Fu;
    size_t size = ((size_t)file[at + 1] << 16) | ((size_t)file[at + 2] << 8)
        | (size_t)file[at + 3];
    if (type == FLAC_BLOCK_SEEKTABLE) {
      seen = true;
      EXPECT_GT(size, 0u);
      EXPECT_EQ(size % 18u, 0u);
    }
    at += FLAC_BLOCK_HEADER + size;
    if (last) {
      break;
    }
  }
  EXPECT_TRUE(seen);
}

/* ------------------------------------------------- metadata blocks */

/** @brief Append a big-endian 32-bit value. */
void put_be32(std::vector<unsigned char> & out, uint32_t value) {
  out.push_back((unsigned char)(value >> 24));
  out.push_back((unsigned char)(value >> 16));
  out.push_back((unsigned char)(value >> 8));
  out.push_back((unsigned char)value);
}

/** @brief A PICTURE block body, as RFC 9639 section 8.7 lays one out. */
std::vector<unsigned char> picture_block(uint32_t kind, const char * mime,
    const char * description, uint32_t width, uint32_t height,
    const std::string & payload) {
  std::vector<unsigned char> out;
  put_be32(out, kind);
  put_be32(out, (uint32_t)strlen(mime));
  out.insert(out.end(), mime, mime + strlen(mime));
  put_be32(out, (uint32_t)strlen(description));
  out.insert(out.end(), description, description + strlen(description));
  put_be32(out, width);
  put_be32(out, height);
  put_be32(out, 24u);
  put_be32(out, 0);
  put_be32(out, (uint32_t)payload.size());
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

TEST(FlacPicture, KeepsWhatItStatedAndRefusesWhatRunsPastTheBlock) {
  GAUD_Limits limits;
  gaud_limits_default(&limits);

  std::vector<unsigned char> block
      = picture_block(3u, "image/png", "A cover", 640u, 480u, "notapng");
  GAUD_Meta * meta = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &meta), GAUD_OK);
  ASSERT_EQ(gaud_flac_parse_picture(block.data(), block.size(), &limits,
                meta, nullptr),
      GAUD_OK);
  ASSERT_EQ(gaud_meta_picture_count(meta), 1u);
  const GAUD_Picture * picture = gaud_meta_picture(meta, 0);
  ASSERT_NE(picture, nullptr);
  EXPECT_EQ(picture->kind, GAUD_PICTURE_FRONT_COVER);
  EXPECT_STREQ(picture->mime_type, "image/png");
  EXPECT_STREQ(picture->description, "A cover");
  EXPECT_EQ(picture->size, 7u);
  /* The stated dimensions are kept as stated, whatever the payload turns
   * out to be - the whole point of planning/audio.md 11.4's split. */
  EXPECT_EQ(picture->stated_width, 640u);
  EXPECT_EQ(picture->stated_height, 480u);
  EXPECT_EQ(picture->stated_depth, 24u);
  gaud_meta_destroy(meta);

  /* Every length in the block is attacker-controlled, so each one is a
   * place a reader can be walked off the end. Truncating at every byte
   * must give a refusal or a clean parse and never a crash - which under
   * ASan is what this actually tests. */
  for (size_t cut = 0; cut < block.size(); ++cut) {
    GAUD_Meta * partial = nullptr;
    ASSERT_EQ(gaud_meta_create(nullptr, &partial), GAUD_OK);
    GAUD_Result result = gaud_flac_parse_picture(
        block.data(), cut, &limits, partial, nullptr);
    EXPECT_TRUE(result == GAUD_OK || result == GAUD_ERR_CORRUPT)
        << "cut at " << cut << " gave " << gaud_result_string(result);
    gaud_meta_destroy(partial);
  }

  /* A payload larger than the cap is a limit, not a silent truncation. */
  limits.max_picture_bytes = 3u;
  GAUD_Meta * capped = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &capped), GAUD_OK);
  EXPECT_EQ(gaud_flac_parse_picture(block.data(), block.size(), &limits,
                capped, nullptr),
      GAUD_ERR_LIMIT);
  gaud_meta_destroy(capped);
}

TEST(FlacPicture, DescriptionThatIsNotUtf8ComesBackAsUtf8) {
  /* A description in Latin-1, which the specification says should be
   * UTF-8 and files are not. Whatever comes out must be valid UTF-8,
   * because a caller will print it. */
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  std::string payload = "x";
  std::vector<unsigned char> block
      = picture_block(0, "image/jpeg", "café", 1u, 1u, payload);
  GAUD_Meta * meta = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &meta), GAUD_OK);
  ASSERT_EQ(gaud_flac_parse_picture(block.data(), block.size(), &limits,
                meta, nullptr),
      GAUD_OK);
  const GAUD_Picture * picture = gaud_meta_picture(meta, 0);
  ASSERT_NE(picture, nullptr);
  /* 0xE9 in Latin-1 is U+00E9, which is C3 A9 in UTF-8. */
  EXPECT_STREQ(picture->description, "caf\xC3\xA9");
  gaud_meta_destroy(meta);
}

/** @brief Wrap @p body as a metadata block of @p type. */
void append_block(std::vector<unsigned char> & file, bool last, unsigned type,
    const std::vector<unsigned char> & body) {
  file.push_back((unsigned char)((last ? 0x80u : 0) | type));
  file.push_back((unsigned char)(body.size() >> 16));
  file.push_back((unsigned char)(body.size() >> 8));
  file.push_back((unsigned char)body.size());
  file.insert(file.end(), body.begin(), body.end());
}

TEST(FlacMetadata, CarriesWhatItDoesNotInterpretAndDropsPadding) {
  std::vector<unsigned char> file;
  file.insert(file.end(), {'f', 'L', 'a', 'C'});
  std::vector<unsigned char> info = good_streaminfo();
  append_block(file, false, FLAC_BLOCK_STREAMINFO, info);
  append_block(file, false, FLAC_BLOCK_APPLICATION,
      {'T', 'e', 's', 't', 1, 2, 3});
  /* A type no revision has defined yet. Kept, because a reader that
   * dropped it would silently strip a future block on every rewrite. */
  append_block(file, false, 42u, {9, 9, 9});
  append_block(file, true, FLAC_BLOCK_PADDING, std::vector<unsigned char>(16));

  GAUD_Limits limits;
  gaud_limits_default(&limits);
  GAUD_Stream * in = nullptr;
  ASSERT_EQ(gaud_stream_create_memory(file.data(), file.size(), &in),
      GAUD_OK);
  GAUD_Doc * doc = nullptr;
  /* Through the registry rather than straight into gaud_flac_open(): the
   * document this builds is a real one, and gaud_doc_create_internal()
   * needs the codec that will own it. The refusal tests above can pass a
   * null codec because they return before a document exists. */
  ASSERT_EQ(gaud_doc_load(nullptr, in, &limits, nullptr, &doc), GAUD_OK);
  GAUD_Meta * meta = gaud_doc_meta(doc);
  ASSERT_NE(meta, nullptr);

  bool seen_application = false;
  bool seen_unknown = false;
  bool seen_padding = false;
  for (size_t i = 0; i < gaud_meta_raw_count(meta); ++i) {
    const char * scheme = nullptr;
    const char * id = nullptr;
    const void * data = nullptr;
    size_t size = 0;
    ASSERT_EQ(gaud_meta_raw(meta, i, &scheme, &id, &data, &size), GAUD_OK);
    EXPECT_STREQ(scheme, "flac");
    if (std::string(id) == "APPLICATION") {
      seen_application = true;
      EXPECT_EQ(size, 7u);
    }
    else if (std::string(id) == "BLOCK_42") {
      seen_unknown = true;
      EXPECT_EQ(size, 3u);
    }
    else if (std::string(id).find("PADDING") != std::string::npos) {
      seen_padding = true;
    }
  }
  EXPECT_TRUE(seen_application);
  EXPECT_TRUE(seen_unknown);
  /* Padding is room an encoder left for a tagger; carrying it forward
   * would mean a file that gained a tag also kept the hole the tag was
   * meant to fill, and grew by both. */
  EXPECT_FALSE(seen_padding);
  gaud_doc_destroy(doc);
  gaud_stream_destroy(in);
}

TEST(FlacMetadata, RefusesASecondStreaminfoAndAnInvalidBlockType) {
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  std::vector<unsigned char> info = good_streaminfo();

  std::vector<unsigned char> twice;
  twice.insert(twice.end(), {'f', 'L', 'a', 'C'});
  append_block(twice, false, FLAC_BLOCK_STREAMINFO, info);
  append_block(twice, true, FLAC_BLOCK_STREAMINFO, info);
  GAUD_Stream * in = nullptr;
  ASSERT_EQ(gaud_stream_create_memory(twice.data(), twice.size(), &in),
      GAUD_OK);
  GAUD_Doc * doc = nullptr;
  EXPECT_EQ(gaud_flac_open(nullptr, in, &limits, nullptr, &doc),
      GAUD_ERR_CORRUPT);
  gaud_stream_destroy(in);
  in = nullptr;

  /* Type 127 is forbidden so that a block header cannot be mistaken for
   * a frame sync. */
  std::vector<unsigned char> invalid;
  invalid.insert(invalid.end(), {'f', 'L', 'a', 'C'});
  append_block(invalid, false, FLAC_BLOCK_STREAMINFO, info);
  append_block(invalid, true, FLAC_BLOCK_INVALID, {0});
  ASSERT_EQ(gaud_stream_create_memory(invalid.data(), invalid.size(), &in),
      GAUD_OK);
  EXPECT_EQ(gaud_flac_open(nullptr, in, &limits, nullptr, &doc),
      GAUD_ERR_CORRUPT);
  gaud_stream_destroy(in);
}

TEST(FlacMetadata, DropsASeekTableThatIsNotInOrder) {
  /* A table whose points do not ascend is one a binary search lands
   * anywhere in. Dropping it costs a slower seek; trusting it costs
   * correctness. The control is the same table sorted. */
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  std::vector<unsigned char> info = good_streaminfo();

  auto seek_point = [](std::vector<unsigned char> & out, uint64_t sample,
                        uint64_t offset) {
    put_be32(out, (uint32_t)(sample >> 32));
    put_be32(out, (uint32_t)sample);
    put_be32(out, (uint32_t)(offset >> 32));
    put_be32(out, (uint32_t)offset);
    out.push_back(0x10);
    out.push_back(0);
  };

  for (int ordered = 0; ordered < 2; ++ordered) {
    std::vector<unsigned char> table;
    seek_point(table, 0, 0);
    seek_point(table, ordered ? 4096u : 1u, 100u);
    seek_point(table, ordered ? 8192u : 1u, 200u);
    /* A placeholder, which an encoder writes to reserve room. It carries
     * no offset and must be dropped rather than kept. */
    seek_point(table, UINT64_MAX, 0);

    std::vector<unsigned char> file;
    file.insert(file.end(), {'f', 'L', 'a', 'C'});
    append_block(file, false, FLAC_BLOCK_STREAMINFO, info);
    append_block(file, true, FLAC_BLOCK_SEEKTABLE, table);

    GAUD_Stream * in = nullptr;
    ASSERT_EQ(gaud_stream_create_memory(file.data(), file.size(), &in),
        GAUD_OK);
    GAUD_Doc * doc = nullptr;
    GAUD_Diagnostics diagnostics;
    gaud_diagnostics_init(&diagnostics, nullptr);
    ASSERT_EQ(gaud_doc_load(nullptr, in, &limits, &diagnostics, &doc),
        GAUD_OK);
    GAUD_Track * track = gaud_doc_track(doc, 0);
    const FLAC_File * state = (const FLAC_File *)gaud_track_private(track);
    ASSERT_NE(state, nullptr);
    if (ordered) {
      /* Three points written, one a placeholder, so two survive. */
      EXPECT_EQ(state->seek_count, 3u);
      EXPECT_EQ(diagnostics.count, 0u);
    }
    else {
      EXPECT_EQ(state->seek_count, 0u);
      EXPECT_GT(diagnostics.count, 0u);
    }
    gaud_diagnostics_destroy(&diagnostics);
    gaud_doc_destroy(doc);
    gaud_stream_destroy(in);
  }
}

/* ------------------------------------------- frames nothing else writes */

/**
 * @brief Build one FLAC frame by hand, with the header spelled as asked.
 *
 * **Here because three arms of the frame-header reader are reachable from
 * no file in the corpus and from nothing in the oracle image.** An
 * instrumented decode of every fixture in `tests/data/` showed it:
 * neither libFLAC nor ffmpeg ever writes a variable-blocksize stream, and
 * neither ever defers the bit depth or the sample rate to STREAMINFO -
 * they always state them. Those are legal spellings that this library
 * must read, and the only honest way to test code no reference will
 * produce an input for is to produce the input.
 *
 * The frame carries one CONSTANT subframe per channel, which is the
 * smallest valid body there is, so what is being tested is entirely the
 * header.
 */
struct Header_Spelling {
  bool variable_block_size; ///< Coded number is a sample number.
  uint32_t block_code;      ///< 4 bits, as written.
  uint32_t rate_code;       ///< 4 bits; 0 defers to STREAMINFO.
  uint32_t depth_code;      ///< 3 bits; 0 defers to STREAMINFO.
  uint32_t channels;
  uint32_t block_size;      ///< What the frame actually holds.
  uint64_t number;
  int32_t value;            ///< The constant every sample takes.
  uint32_t depth;           ///< The bit depth the samples are written at.
};

std::vector<unsigned char> hand_built_frame(const Header_Spelling & how) {
  FLAC_Bit_Writer bw;
  gaud_flac_bitw_init(&bw, NULL);
  gaud_flac_bitw_write(&bw, 0x3FFEu, 14u);
  gaud_flac_bitw_write(&bw, 0, 1u);
  gaud_flac_bitw_write(&bw, how.variable_block_size ? 1u : 0u, 1u);
  gaud_flac_bitw_write(&bw, how.block_code, 4u);
  gaud_flac_bitw_write(&bw, how.rate_code, 4u);
  gaud_flac_bitw_write(&bw, how.channels - 1u, 4u);
  gaud_flac_bitw_write(&bw, how.depth_code, 3u);
  gaud_flac_bitw_write(&bw, 0, 1u);
  gaud_flac_bitw_write_coded_number(&bw, how.number);
  if (how.block_code == 6u) {
    gaud_flac_bitw_write(&bw, how.block_size - 1u, 8u);
  }
  else if (how.block_code == 7u) {
    gaud_flac_bitw_write(&bw, how.block_size - 1u, 16u);
  }
  if (how.rate_code == 0xDu) {
    gaud_flac_bitw_write(&bw, 44100u, 16u);
  }
  gaud_flac_bitw_write(&bw, gaud_flac_crc8(bw.data, bw.size), 8u);

  for (uint32_t ch = 0; ch < how.channels; ++ch) {
    gaud_flac_bitw_write(&bw, 0, 1u);           /* padding bit */
    gaud_flac_bitw_write(&bw, 0, 6u);           /* CONSTANT */
    gaud_flac_bitw_write(&bw, 0, 1u);           /* no wasted bits */
    gaud_flac_bitw_write(&bw, (uint64_t)how.value, how.depth);
  }
  gaud_flac_bitw_align(&bw);
  gaud_flac_bitw_write(&bw, gaud_flac_crc16(bw.data, bw.size), 16u);
  std::vector<unsigned char> out(bw.data, bw.data + bw.size);
  gaud_flac_bitw_free(&bw);
  return out;
}

TEST(FlacFrame, ReadsTheHeaderSpellingsNoEncoderHereWrites) {
  FLAC_Streaminfo info;
  memset(&info, 0, sizeof(info));
  info.min_block_size = 1024u;
  info.max_block_size = 1024u;
  info.sample_rate = 44100u;
  info.channels = 2u;
  info.bits_per_sample = 16u;

  struct Case {
    Header_Spelling how;
    const char * what;
  };
  const Case cases[] = {
      /* The ordinary spelling, as a control: if this one failed, a
       * failure below would say nothing about the spelling under test. */
      {{false, 7u, 0x9u, 4u, 2u, 1024u, 3u, -4000, 16u},
       "the ordinary spelling, as a control"},
      /* Variable blocksize: the coded number is a SAMPLE number, which
       * is the whole difference and is why it may be seven bytes long. */
      {{true, 7u, 0x9u, 4u, 2u, 1024u, 0xFFFFFFFFFull, -4000, 16u},
       "variable blocksize with a 36-bit sample number"},
      /* Rate deferred to STREAMINFO. */
      {{false, 7u, 0, 4u, 2u, 1024u, 3u, 1234, 16u},
       "sample rate deferred to STREAMINFO"},
      /* Depth deferred to STREAMINFO. */
      {{false, 7u, 0x9u, 0, 2u, 1024u, 3u, 1234, 16u},
       "bit depth deferred to STREAMINFO"},
      /* Both deferred at once. */
      {{false, 7u, 0, 0, 2u, 1024u, 3u, -1, 16u},
       "both deferred at once"},
      /* The rate as a 16-bit field at the end of the header. */
      {{false, 7u, 0xDu, 4u, 2u, 1024u, 3u, 32767, 16u},
       "sample rate as a 16-bit field after the coded number"},
      /* An 8-bit block size, which is the other end-of-header field. */
      {{false, 6u, 0x9u, 4u, 2u, 200u, 3u, -32768, 16u},
       "block size as an 8-bit field"},
      /* A tabulated block size, so neither extra field is present. */
      {{false, 0x1u, 0x9u, 4u, 2u, 192u, 3u, 7, 16u},
       "a tabulated block size of 192"},
  };

  FLAC_Frame frame;
  memset(&frame, 0, sizeof(frame));
  for (const Case & one : cases) {
    std::vector<unsigned char> bytes = hand_built_frame(one.how);
    size_t used = 0;
    ASSERT_EQ(
        gaud_flac_frame_decode(bytes.data(), bytes.size(), &info, &frame,
            &used),
        GAUD_OK)
        << one.what;
    EXPECT_EQ(used, bytes.size()) << one.what;
    EXPECT_EQ(frame.block_size, one.how.block_size) << one.what;
    EXPECT_EQ(frame.channels, one.how.channels) << one.what;
    EXPECT_EQ(frame.sample_rate, 44100u) << one.what;
    EXPECT_EQ(frame.bits_per_sample, 16u) << one.what;
    EXPECT_EQ(frame.variable_block_size, one.how.variable_block_size)
        << one.what;
    EXPECT_EQ(frame.number, one.how.number) << one.what;
    for (uint32_t ch = 0; ch < one.how.channels; ++ch) {
      const int64_t * samples
          = frame.samples + (size_t)ch * frame.capacity_frames;
      for (uint32_t i = 0; i < one.how.block_size; ++i) {
        ASSERT_EQ(samples[i], one.how.value)
            << one.what << " channel " << ch << " sample " << i;
      }
    }
  }
  gaud_flac_frame_free(&frame);
}

/**
 * @brief Build a two-channel frame whose channels are decorrelated.
 *
 * **Here because `make coverage` showed the inverse had never run.** The
 * three stereo modes are what nearly every real FLAC file uses, and
 * `check-corpus` reaches them on every fixture - but that gate needs the
 * pinned oracle container, and the unit suite, which is what runs
 * everywhere, decoded only independent-stereo and mono frames. The body
 * of `if (channels == 2 && assignment >= 8)` was executed zero times in
 * eighty-three frame decodes.
 *
 * One CONSTANT subframe per channel, so what is under test is entirely
 * the undecorrelation: the two constants that go in are the decorrelated
 * pair, and the two that come out must be the original left and right.
 * The channel carrying the difference is a bit deeper, and which one it
 * is depends on the assignment, so the width is computed here the same
 * way the reader computes it rather than being passed in - a reader and
 * a test that agreed on the wrong channel would still pass.
 */
std::vector<unsigned char> hand_built_stereo_frame(
    uint32_t assignment, int32_t first, int32_t second, uint32_t depth) {
  const uint32_t block_size = 64u;
  FLAC_Bit_Writer bw;
  gaud_flac_bitw_init(&bw, NULL);
  gaud_flac_bitw_write(&bw, 0x3FFEu, 14u);
  gaud_flac_bitw_write(&bw, 0, 1u);
  gaud_flac_bitw_write(&bw, 0, 1u);    /* fixed blocksize */
  gaud_flac_bitw_write(&bw, 7u, 4u);   /* block size in a 16-bit field */
  gaud_flac_bitw_write(&bw, 0x9u, 4u); /* 44100 */
  gaud_flac_bitw_write(&bw, assignment, 4u);
  gaud_flac_bitw_write(&bw, 4u, 3u);   /* 16 bits */
  gaud_flac_bitw_write(&bw, 0, 1u);
  gaud_flac_bitw_write_coded_number(&bw, 0);
  gaud_flac_bitw_write(&bw, block_size - 1u, 16u);
  gaud_flac_bitw_write(&bw, gaud_flac_crc8(bw.data, bw.size), 8u);

  const int32_t value[2] = {first, second};
  for (uint32_t ch = 0; ch < 2u; ++ch) {
    bool wider
        = ((assignment == 8u || assignment == 0xAu) && ch == 1u)
        || (assignment == 9u && ch == 0u);
    gaud_flac_bitw_write(&bw, 0, 1u);  /* padding bit */
    gaud_flac_bitw_write(&bw, 0, 6u);  /* CONSTANT */
    gaud_flac_bitw_write(&bw, 0, 1u);  /* no wasted bits */
    gaud_flac_bitw_write(&bw, (uint64_t)value[ch], depth + (wider ? 1u : 0u));
  }
  gaud_flac_bitw_align(&bw);
  gaud_flac_bitw_write(&bw, gaud_flac_crc16(bw.data, bw.size), 16u);
  std::vector<unsigned char> out(bw.data, bw.data + bw.size);
  gaud_flac_bitw_free(&bw);
  return out;
}

TEST(FlacFrame, TheThreeStereoDecorrelationsAreUndone) {
  FLAC_Streaminfo info;
  memset(&info, 0, sizeof(info));
  info.min_block_size = 64u;
  info.max_block_size = 64u;
  info.sample_rate = 44100u;
  info.channels = 2u;
  info.bits_per_sample = 16u;

  struct Case {
    int32_t left;
    int32_t right;
    const char * what;
  };
  /* The second pair sums to an odd number, which is the whole reason mid
   * and side is lossless: mid drops the sum's low bit and side carries it
   * back. A decoder that reconstructed the average instead would differ
   * by one here and agree on every even-summed pair, so a test with only
   * the first pair would not see it. */
  const Case pairs[] = {
      {1000, -200, "an even sum"},
      {1001, -200, "an odd sum, which is where the low bit matters"},
      {-32768, 32767, "the extremes of the depth"},
  };

  FLAC_Frame frame;
  memset(&frame, 0, sizeof(frame));
  for (const Case & pair : pairs) {
    const int32_t side = pair.left - pair.right;
    const int32_t mid
        = (int32_t)((((int64_t)pair.left + pair.right)) >> 1);
    struct Shape {
      uint32_t assignment;
      int32_t first;
      int32_t second;
      const char * name;
    };
    const Shape shapes[] = {
        /* Independent stereo, as a control: the same two values go in and
         * must come out untouched. If undecorrelation were a no-op the
         * three below would fail and this one would still pass, which is
         * what tells the two apart. */
        {1u, pair.left, pair.right, "independent stereo (control)"},
        {8u, pair.left, side, "left and side"},
        {9u, side, pair.right, "side and right"},
        {0xAu, mid, side, "mid and side"},
    };
    for (const Shape & shape : shapes) {
      std::vector<unsigned char> bytes = hand_built_stereo_frame(
          shape.assignment, shape.first, shape.second, 16u);
      size_t used = 0;
      ASSERT_EQ(gaud_flac_frame_decode(
                    bytes.data(), bytes.size(), &info, &frame, &used),
          GAUD_OK)
          << shape.name << ", " << pair.what;
      EXPECT_EQ(frame.channels, 2u) << shape.name;
      const int64_t * left = frame.samples;
      const int64_t * right = frame.samples + frame.capacity_frames;
      for (uint32_t i = 0; i < frame.block_size; ++i) {
        ASSERT_EQ(left[i], pair.left)
            << shape.name << ", " << pair.what << ", sample " << i;
        ASSERT_EQ(right[i], pair.right)
            << shape.name << ", " << pair.what << ", sample " << i;
      }
    }
  }
  gaud_flac_frame_free(&frame);
}

TEST(FlacFrame, RefusesTheValuesTheFormatForbids) {
  FLAC_Streaminfo info;
  memset(&info, 0, sizeof(info));
  info.min_block_size = 1024u;
  info.max_block_size = 1024u;
  info.sample_rate = 44100u;
  info.channels = 2u;
  info.bits_per_sample = 16u;

  FLAC_Frame frame;
  memset(&frame, 0, sizeof(frame));
  size_t used = 0;

  /* The control first: the same frame with legal codes decodes, so a
   * refusal below is about the field and not about the builder. */
  Header_Spelling legal = {false, 7u, 0x9u, 4u, 2u, 1024u, 3u, 0, 16u};
  std::vector<unsigned char> good = hand_built_frame(legal);
  ASSERT_EQ(gaud_flac_frame_decode(good.data(), good.size(), &info, &frame,
                &used),
      GAUD_OK);

  struct Bad {
    Header_Spelling how;
    const char * what;
  };
  const Bad bad[] = {
      /* 0b1111 is forbidden precisely so that the byte after a header
       * cannot complete a second sync code. */
      {{false, 7u, 0xFu, 4u, 2u, 1024u, 3u, 0, 16u}, "sample rate code 15"},
      /* Block size code 0 is reserved. */
      {{false, 0, 0x9u, 4u, 2u, 1024u, 3u, 0, 16u}, "block size code 0"},
      /* Bit depth code 3 is reserved. */
      {{false, 7u, 0x9u, 3u, 2u, 1024u, 3u, 0, 16u}, "bit depth code 3"},
  };
  for (const Bad & one : bad) {
    std::vector<unsigned char> bytes = hand_built_frame(one.how);
    EXPECT_EQ(gaud_flac_frame_decode(bytes.data(), bytes.size(), &info,
                  &frame, &used),
        GAUD_ERR_CORRUPT)
        << one.what;
  }

  /* A frame whose channel count disagrees with STREAMINFO is not a frame
   * of this stream, however well formed it is. This is what stops a
   * resync landing on a false sync inside the audio and decoding
   * thousands of samples of noise. */
  Header_Spelling wrong_channels
      = {false, 7u, 0x9u, 4u, 1u, 1024u, 3u, 0, 16u};
  std::vector<unsigned char> mismatched = hand_built_frame(wrong_channels);
  EXPECT_EQ(gaud_flac_frame_decode(mismatched.data(), mismatched.size(),
                &info, &frame, &used),
      GAUD_ERR_CORRUPT);

  /* A corrupted CRC-16, with nothing else changed. */
  std::vector<unsigned char> broken = good;
  broken[broken.size() - 1u] ^= 0x01u;
  EXPECT_EQ(gaud_flac_frame_decode(broken.data(), broken.size(), &info,
                &frame, &used),
      GAUD_ERR_CORRUPT);

  /* A corrupted header, which the CRC-8 catches before the subframes are
   * even read. */
  std::vector<unsigned char> header_broken = good;
  header_broken[4] ^= 0x01u;
  EXPECT_EQ(gaud_flac_frame_decode(header_broken.data(),
                header_broken.size(), &info, &frame, &used),
      GAUD_ERR_CORRUPT);

  /* Truncated: GAUD_ERR_IO and not CORRUPT, because "read more" and
   * "this is wrong" are different answers and the decoder's caller grows
   * its window on the first. */
  EXPECT_EQ(gaud_flac_frame_decode(good.data(), good.size() / 2u, &info,
                &frame, &used),
      GAUD_ERR_IO);
  gaud_flac_frame_free(&frame);
}

/* ------------------------------------------------------------ refusals */

TEST(FlacOpen, RefusesWhatIsNotAFlacStream) {
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  const unsigned char not_flac[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A',
      'V', 'E'};
  GAUD_Stream * in = nullptr;
  ASSERT_EQ(gaud_stream_create_memory(not_flac, sizeof(not_flac), &in),
      GAUD_OK);
  GAUD_Doc * doc = nullptr;
  EXPECT_EQ(gaud_flac_open(nullptr, in, &limits, nullptr, &doc),
      GAUD_ERR_FORMAT);
  gaud_stream_destroy(in);
}

TEST(FlacOpen, RefusesABitDepthWithNoLosslessFormat) {
  /* A STREAMINFO claiming 20 bits. The file is otherwise well formed, so
   * the refusal is about the depth and nothing else. */
  std::vector<unsigned char> block = good_streaminfo();
  /* Bits-per-sample is the five bits ending at bit 108 of the block. The
   * byte layout puts them across bytes 12 and 13. */
  block[12] = (unsigned char)((block[12] & 0xF0u) | ((19u >> 1) & 0x0Fu));
  block[13] = (unsigned char)((block[13] & 0x0Fu) | ((19u & 1u) << 7));

  std::vector<unsigned char> file;
  file.insert(file.end(), {'f', 'L', 'a', 'C'});
  file.insert(file.end(), {0x80, 0, 0, FLAC_STREAMINFO_SIZE});
  file.insert(file.end(), block.begin(), block.end());

  GAUD_Limits limits;
  gaud_limits_default(&limits);
  GAUD_Stream * in = nullptr;
  ASSERT_EQ(gaud_stream_create_memory(file.data(), file.size(), &in),
      GAUD_OK);
  GAUD_Doc * doc = nullptr;
  GAUD_Diagnostics diagnostics;
  gaud_diagnostics_init(&diagnostics, nullptr);
  EXPECT_EQ(gaud_flac_open(nullptr, in, &limits, &diagnostics, &doc),
      GAUD_ERR_UNSUPPORTED);
  EXPECT_GT(diagnostics.count, 0u);
  gaud_diagnostics_destroy(&diagnostics);
  gaud_stream_destroy(in);
}

TEST(FlacOpen, RefusesAFileWhoseFirstBlockIsNotStreaminfo) {
  std::vector<unsigned char> file;
  file.insert(file.end(), {'f', 'L', 'a', 'C'});
  /* A PADDING block first, which the format forbids. */
  file.insert(file.end(), {0x81, 0, 0, 4});
  file.insert(file.end(), {0, 0, 0, 0});
  GAUD_Limits limits;
  gaud_limits_default(&limits);
  GAUD_Stream * in = nullptr;
  ASSERT_EQ(gaud_stream_create_memory(file.data(), file.size(), &in),
      GAUD_OK);
  GAUD_Doc * doc = nullptr;
  EXPECT_EQ(gaud_flac_open(nullptr, in, &limits, nullptr, &doc),
      GAUD_ERR_CORRUPT);
  gaud_stream_destroy(in);
}

TEST(FlacEncode, RefusesFormatsFlacCannotCarry) {
  /* U8 is the tempting one: FLAC's 8-bit samples are signed, and writing
   * WAV's unsigned bytes into one would move every sample by half the
   * scale - silently, and losslessly, into the wrong file. */
  for (GAUD_Sample_Format format :
      {GAUD_SAMPLE_U8, GAUD_SAMPLE_F32, GAUD_SAMPLE_F64}) {
    GAUD_Stream * out = nullptr;
    ASSERT_EQ(gaud_stream_create_memory_writer(nullptr, &out), GAUD_OK);
    GAUD_Encode_Params params;
    gaud_encode_params_default(&params);
    params.format = format;
    GAUD_Encoder * encoder = nullptr;
    EXPECT_EQ(gaud_encoder_create("flac", nullptr, out, &params, &encoder),
        GAUD_ERR_UNSUPPORTED)
        << gaud_sample_format_string(format);
    gaud_stream_destroy(out);
  }
}

TEST(FlacCoding, FlacIsACodingWithNoFixedSampleFormat) {
  EXPECT_STREQ(gaud_sample_coding_name(GAUD_CODING_FLAC), "flac");
  EXPECT_FALSE(gaud_sample_coding_is_pcm(GAUD_CODING_FLAC));
  /* Unlike every coding before it, which all answer S16. A caller that
   * assumed a coded track's format could be had from its coding is wrong
   * about FLAC, and this is where that is written down. */
  EXPECT_EQ(gaud_sample_coding_format(GAUD_CODING_FLAC),
      GAUD_SAMPLE_FORMAT_COUNT);
  EXPECT_EQ(gaud_sample_coding_format(GAUD_CODING_G711_ULAW),
      GAUD_SAMPLE_S16);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
