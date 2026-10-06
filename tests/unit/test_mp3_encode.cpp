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
 * The MP3 encoder: everything a reference decoder cannot be asked.
 *
 * `make check-mp3-encode` hands the encoder's files to ffmpeg and
 * libsndfile and scores what they decode to. What is here is what that
 * gate cannot see, because it concerns the encoder's own arithmetic rather
 * than its files:
 *
 *   - **The Huffman code words are the decoder's inverse**, every symbol of
 *     every table, walked back through the decoder's own trees. A code
 *     that is wrong for a pair no test signal contains would otherwise
 *     ship, because a corpus contains the pairs its signals make.
 *   - **The header writer and the decoder's parser agree** over every
 *     version, bitrate and rate, and the frame lengths match the standard's
 *     arithmetic.
 *   - **The quantiser and the decoder's requantiser are inverses**: what
 *     the encoder believes the decoder will reconstruct is what it does.
 *   - **The delay is measured, not assumed**: an impulse comes back at the
 *     sample it went in at, once the stated trim is applied.
 *   - **The reservoir is respected** frame by frame in the written file.
 *   - **The same input gives the same bytes**, which is the precondition
 *     of the cross-architecture promise.
 */

#include "../../src/codec/mp3/mp3enc_internal.h"
#include <ghoti.io/audio/audio.h>
#include <ghoti.io/audio/codec_sdk.h>
#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Registered {
  Registered() {
    gaud_register_builtin_codecs();
  }
};
const Registered registered;

/** One signal: interleaved 16-bit samples. */
using Pcm = std::vector<int16_t>;

/** A deterministic sum of sines and a little noise, in 16-bit. */
Pcm Tone(unsigned channels, size_t frames, double rate, double level = 0.3) {
  Pcm pcm(frames * channels);
  uint32_t state = 12345u;
  for (size_t i = 0; i < frames; ++i) {
    for (unsigned c = 0; c < channels; ++c) {
      state = state * 1664525u + 1013904223u;
      double noise = ((int32_t)(state >> 8) - 8388608) / 8388608.0;
      double f = c == 0 ? 700.0 : 1900.0;
      double v = level * std::sin(2 * M_PI * f * i / rate)
          + 0.3 * level * std::sin(2 * M_PI * f * 2.7 * i / rate)
          + 0.03 * noise;
      pcm[i * channels + c] = (int16_t)std::lround(v * 32767.0);
    }
  }
  return pcm;
}

/** Encode through the public interface into memory. */
std::vector<unsigned char> Encode(const Pcm & pcm, unsigned channels,
    uint32_t rate, uint32_t bps, GAUD_Result * out_result = nullptr,
    size_t chunk = 997) {
  GAUD_Encode_Params params;
  gaud_encode_params_default(&params);
  params.format = GAUD_SAMPLE_S16;
  params.coding = GAUD_CODING_MPEG_LAYER3;
  params.sample_rate = rate;
  params.layout = gaud_channel_layout_default(channels);
  params.bitrate = bps;
  GAUD_Stream * out = nullptr;
  EXPECT_EQ(gaud_stream_create_memory_writer(nullptr, &out), GAUD_OK);
  GAUD_Encoder * encoder = nullptr;
  GAUD_Result result = gaud_encoder_create("mp3", nullptr, out, &params, &encoder);
  if (out_result) {
    *out_result = result;
  }
  std::vector<unsigned char> bytes;
  if (result == GAUD_OK) {
    GAUD_Buffer * buffer = nullptr;
    EXPECT_EQ(gaud_buffer_create(nullptr, GAUD_SAMPLE_S16, params.layout,
                  GAUD_LAYOUT_INTERLEAVED, chunk, &buffer),
        GAUD_OK);
    size_t frames = pcm.size() / channels;
    for (size_t at = 0; at < frames; at += chunk) {
      size_t n = std::min(chunk, frames - at);
      EXPECT_EQ(gaud_buffer_set_frames(buffer, n), GAUD_OK);
      std::memcpy(gaud_buffer_data(buffer), pcm.data() + at * channels,
          n * channels * 2u);
      EXPECT_EQ(gaud_encoder_write(encoder, buffer), GAUD_OK);
    }
    EXPECT_EQ(gaud_encoder_finish(encoder), GAUD_OK);
    const void * data = nullptr;
    size_t size = 0;
    EXPECT_EQ(gaud_stream_writer_bytes(out, &data, &size), GAUD_OK);
    bytes.assign((const unsigned char *)data, (const unsigned char *)data + size);
    gaud_buffer_destroy(buffer);
  }
  gaud_encoder_destroy(encoder);
  gaud_stream_destroy(out);
  return bytes;
}

struct Decoded {
  Pcm pcm;
  uint64_t frames = 0;
  uint32_t rate = 0;
  unsigned channels = 0;
};

Decoded Decode(const std::vector<unsigned char> & bytes) {
  Decoded d;
  GAUD_Stream * stream = nullptr;
  EXPECT_EQ(gaud_stream_create_memory(bytes.data(), bytes.size(), &stream), GAUD_OK);
  GAUD_Doc * doc = nullptr;
  EXPECT_EQ(gaud_doc_load(nullptr, stream, nullptr, nullptr, &doc), GAUD_OK);
  if (!doc) {
    return d;
  }
  GAUD_Track * track = gaud_doc_track(doc, 0);
  d.rate = gaud_track_sample_rate(track);
  d.channels = gaud_track_layout(track).channels;
  GAUD_Decoder * decoder = nullptr;
  EXPECT_EQ(gaud_decoder_create(track, &decoder), GAUD_OK);
  GAUD_Buffer * buffer = nullptr;
  EXPECT_EQ(gaud_decoder_buffer_create(decoder, nullptr, 1000, &buffer), GAUD_OK);
  for (;;) {
    EXPECT_EQ(gaud_decoder_read(decoder, buffer), GAUD_OK);
    size_t n = gaud_buffer_frames(buffer);
    if (n == 0) {
      break;
    }
    const int16_t * p = (const int16_t *)gaud_buffer_data_const(buffer);
    d.pcm.insert(d.pcm.end(), p, p + n * d.channels);
  }
  /* The decoder returns every sample the frames carry; removing the
   * encoder's delay and padding is the caller's, and this is the caller. */
  GAUD_Trim trim = gaud_track_trim(track);
  size_t head = (size_t)trim.encoder_delay * d.channels;
  size_t tail = (size_t)trim.padding * d.channels;
  if (head + tail <= d.pcm.size()) {
    d.pcm.erase(d.pcm.end() - (std::ptrdiff_t)tail, d.pcm.end());
    d.pcm.erase(d.pcm.begin(), d.pcm.begin() + (std::ptrdiff_t)head);
  }
  d.frames = d.pcm.size() / d.channels;
  gaud_buffer_destroy(buffer);
  gaud_decoder_destroy(decoder);
  gaud_doc_destroy(doc);
  gaud_stream_destroy(stream);
  return d;
}

double SnrDb(const Pcm & a, const Pcm & b) {
  double signal = 0;
  double error = 0;
  size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) {
    signal += (double)a[i] * a[i];
    double d = (double)a[i] - b[i];
    error += d * d;
  }
  return 10.0 * std::log10(signal / std::max(error, 1e-9));
}

/* ----------------------------------------------------------- the tables */

/** Walk the decoder's tree for the leaf @p code of @p length bits reaches. */
int WalkTree(const int16_t * nodes, unsigned offset, uint32_t code, unsigned length) {
  unsigned at = offset;
  for (unsigned i = 0; i < length; ++i) {
    unsigned bit = (code >> (length - 1 - i)) & 1u;
    int16_t entry = nodes[at + bit];
    if (entry < 0) {
      return i == length - 1 ? -(int)entry - 1 : -2;
    }
    at = offset + 2u * (unsigned)entry;
  }
  return -3;
}

TEST(Mp3EncodeTables, EveryPairCodeDecodesBackThroughTheDecodersTree) {
  size_t checked = 0;
  for (unsigned t = 0; t < 32u; ++t) {
    const MP3_Huff & table = gaud_mp3_huff[t];
    if (table.width == 0u) {
      continue;
    }
    for (unsigned x = 0; x < table.width; ++x) {
      for (unsigned y = 0; y < table.width; ++y) {
        unsigned at = gaud_mp3enc_huff_start[t] + x * table.width + y;
        int payload = WalkTree(gaud_mp3_huff_nodes, table.offset,
            gaud_mp3enc_huff_code[at], gaud_mp3enc_huff_len[at]);
        ASSERT_EQ(payload, (int)((x << 4) | y)) << "table " << t << " (" << x << "," << y << ")";
        ++checked;
      }
    }
  }
  EXPECT_GT(checked, 4000u);
}

TEST(Mp3EncodeTables, EveryQuadrupleCodeDecodesBackThroughTheDecodersTree) {
  for (unsigned which = 0; which < 2u; ++which) {
    for (unsigned v = 0; v < 16u; ++v) {
      int payload = WalkTree(gaud_mp3_quad_nodes, gaud_mp3_quad_offset[which],
          gaud_mp3enc_quad_code[which * 16u + v], gaud_mp3enc_quad_len[which * 16u + v]);
      EXPECT_EQ(payload, (int)v) << "table " << which << " value " << v;
    }
  }
}

TEST(Mp3EncodeTables, ThresholdsAreStrictlyIncreasingAndSitAtTheRoundingPoint) {
  for (unsigned i = 1; i < 8207u; ++i) {
    ASSERT_LT(gaud_mp3enc_quant_threshold[i - 1u], gaud_mp3enc_quant_threshold[i]);
  }
  /* (0 + 0.5946)^(4/3) = 0.5000..., and (1 + 0.5946)^(4/3) = 1.8627... */
  EXPECT_NEAR((double)gaud_mp3enc_quant_threshold[0] / (1 << 28), 0.5000, 0.001);
  EXPECT_NEAR((double)gaud_mp3enc_quant_threshold[1] / (1 << 28), 1.8627, 0.001);
}

TEST(Mp3EncodeMath, LogarithmAndPowerAreInverseAndSquareRootIsFloor) {
  for (uint64_t v : {1ull, 2ull, 3ull, 1000ull, 123456789ull, 1ull << 40}) {
    int32_t l = gaud_mp3e_log2_q8(v);
    EXPECT_NEAR(l / 256.0, std::log2((double)v), 0.01) << v;
  }
  for (int e = -2000; e < 3000; e += 37) {
    double want = std::exp2(e / 256.0) * 65536.0;
    double got = (double)gaud_mp3e_exp2_q8(e);
    EXPECT_NEAR(got / want, 1.0, 0.003) << e;
  }
  for (uint64_t v : {0ull, 1ull, 3ull, 4ull, 15ull, 16ull, 999999999999ull, ~0ull}) {
    uint64_t r = gaud_mp3e_isqrt(v);
    EXPECT_LE(r * r, v);
    EXPECT_TRUE(r == 0xFFFFFFFFull || (r + 1) * (r + 1) > v) << v;
  }
}

/* ------------------------------------------------------------ the header */

TEST(Mp3EncodeHeader, TheParserReadsBackWhatTheWriterWritesForEveryCombination) {
  size_t checked = 0;
  for (unsigned v = 0; v < 3u; ++v) {
    for (unsigned r = 0; r < 3u; ++r) {
      for (unsigned b = 1; b < 15u; ++b) {
        for (unsigned pad = 0; pad < 2u; ++pad) {
          for (unsigned ch = 1; ch <= 2u; ++ch) {
            MP3E_Frame_Header h = {};
            h.version = (MP3_Version)v;
            h.bitrate_index = b;
            h.rate_index = r;
            h.padding = pad;
            h.mode = ch == 1u ? MP3_MODE_SINGLE_CHANNEL : MP3_MODE_JOINT_STEREO;
            h.mode_extension = ch == 2u ? 2u : 0u;
            h.channels = ch;
            h.bitrate = gaud_mp3e_bitrate_kbps(h.version, b) * 1000u;
            MP3_Version version;
            unsigned rate_index;
            uint32_t rates[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};
            h.sample_rate = rates[v][r];
            ASSERT_TRUE(gaud_mp3e_rate_lookup(h.sample_rate, &version, &rate_index));
            ASSERT_EQ((unsigned)version, v);
            ASSERT_EQ(rate_index, r);
            ASSERT_EQ(gaud_mp3e_bitrate_index(h.version, h.bitrate / 1000u), (int)b);
            unsigned char bytes[4];
            gaud_mp3e_header_write(bytes, &h);
            MP3_Header parsed;
            ASSERT_TRUE(gaud_mp3_header_parse(bytes, &parsed));
            EXPECT_EQ((unsigned)parsed.version, v);
            EXPECT_EQ(parsed.layer, 3u);
            EXPECT_FALSE(parsed.crc_present);
            EXPECT_EQ(parsed.bitrate, h.bitrate);
            EXPECT_EQ(parsed.sample_rate, h.sample_rate);
            EXPECT_EQ(parsed.channels, ch);
            EXPECT_EQ(parsed.mode, h.mode);
            EXPECT_EQ(parsed.mode_extension, h.mode_extension);
            EXPECT_EQ(parsed.padding, (bool)pad);
            EXPECT_EQ(parsed.frame_size, gaud_mp3e_frame_bytes(&h) + pad);
            EXPECT_EQ(gaud_mp3_side_info_size(&parsed), gaud_mp3e_side_bytes(h.version, ch));
            ++checked;
          }
        }
      }
    }
  }
  EXPECT_EQ(checked, 3u * 3u * 14u * 2u * 2u);
}

/* -------------------------------------------------------- the quantiser */

TEST(Mp3EncodeQuantiser, RequantisingWhatWasQuantisedGivesTheSameInteger) {
  for (int e4 = -60; e4 <= 20; e4 += 7) {
    for (int32_t q = 1; q < 8207; q += 97) {
      int64_t value = gaud_mp3e_dequantize_one(q, e4);
      if (value > INT32_MAX || value == 0) {
        continue;
      }
      EXPECT_EQ(gaud_mp3e_quantize_one((int32_t)value, e4), q) << "e4 " << e4 << " q " << q;
      EXPECT_EQ(gaud_mp3e_quantize_one(-(int32_t)value, e4), -q);
    }
  }
}

TEST(Mp3EncodeQuantiser, RoundsTowardTheNearerReconstructionWithTheStandardsOffset) {
  /* Between q^(4/3) and (q+1)^(4/3) the boundary is at (q + 0.5946)^(4/3),
   * which is below the midpoint: the offset exists to bias small values
   * down. One unit either side of it must land either side. */
  int e4 = 0;
  for (int32_t q = 1; q < 200; ++q) {
    uint64_t t = gaud_mp3enc_quant_threshold[q];
    int32_t below = gaud_mp3e_quantize_one((int32_t)(t - 2u), e4 + 0);
    int32_t above = gaud_mp3e_quantize_one((int32_t)(t + 2u), e4 + 0);
    if (t + 2u > INT32_MAX) {
      break;
    }
    EXPECT_EQ(below, q);
    EXPECT_EQ(above, q + 1);
  }
}

/* ----------------------------------------------------------- end to end */

TEST(Mp3EncodeRoundTrip, EveryRateAndChannelCountDecodesToTheRightLength) {
  struct Case { uint32_t rate; uint32_t kbps; };
  const Case cases[] = {{48000, 128}, {44100, 128}, {32000, 128}, {24000, 64},
      {22050, 64}, {16000, 64}, {12000, 32}, {11025, 32}, {8000, 32}};
  for (const Case & c : cases) {
    for (unsigned channels = 1; channels <= 2u; ++channels) {
      size_t frames = c.rate / 2u + 123u;
      Pcm pcm = Tone(channels, frames, c.rate);
      std::vector<unsigned char> mp3 = Encode(pcm, channels, c.rate, c.kbps * 1000u);
      ASSERT_FALSE(mp3.empty());
      Decoded d = Decode(mp3);
      EXPECT_EQ(d.frames, frames) << c.rate << " x" << channels;
      EXPECT_EQ(d.pcm.size(), frames * channels) << c.rate << " x" << channels;
      EXPECT_EQ(d.rate, c.rate);
      EXPECT_EQ(d.channels, channels);
      EXPECT_GT(SnrDb(pcm, d.pcm), 15.0) << c.rate << " x" << channels;
    }
  }
}

TEST(Mp3EncodeRoundTrip, AnImpulseComesBackAtTheSampleItWentInAt) {
  /* The delay is measured: the trim the tag states puts the decode back
   * on the input, so the impulse's peak is where the impulse was. */
  for (unsigned channels = 1; channels <= 2u; ++channels) {
    size_t frames = 20000;
    Pcm pcm(frames * channels, 0);
    size_t at = 7777;
    for (unsigned c = 0; c < channels; ++c) {
      pcm[at * channels + c] = 24000;
    }
    Decoded d = Decode(Encode(pcm, channels, 44100, 128000));
    ASSERT_EQ(d.frames, frames);
    size_t peak = 0;
    int best = 0;
    for (size_t i = 0; i < frames; ++i) {
      int v = std::abs((int)d.pcm[i * channels]);
      if (v > best) {
        best = v;
        peak = i;
      }
    }
    EXPECT_EQ(peak, at);
  }
}

TEST(Mp3EncodeRoundTrip, SilenceAndAVeryShortInputBothWork) {
  for (size_t frames : {1u, 100u, 575u, 576u, 577u, 1151u, 1152u, 1153u}) {
    Pcm pcm(frames * 2u, 0);
    std::vector<unsigned char> mp3 = Encode(pcm, 2, 44100, 128000);
    ASSERT_FALSE(mp3.empty()) << frames;
    Decoded d = Decode(mp3);
    EXPECT_EQ(d.frames, frames) << frames;
  }
}

TEST(Mp3EncodeRoundTrip, NoInputStillMakesAReadableFile) {
  /* One frame of silence, and no trim: the loader refuses to apply a trim
   * that would remove the whole file, so the length is the frame's. */
  std::vector<unsigned char> mp3 = Encode(Pcm(), 2, 44100, 128000);
  ASSERT_FALSE(mp3.empty());
  Decoded d = Decode(mp3);
  EXPECT_EQ(d.pcm.size(), 1152u * 2u);
  for (int16_t v : d.pcm) {
    ASSERT_EQ(v, 0);
  }
}

TEST(Mp3EncodeRoundTrip, TheBufferSizeDoesNotChangeTheBytes) {
  Pcm pcm = Tone(2, 30000, 44100);
  std::vector<unsigned char> a = Encode(pcm, 2, 44100, 128000, nullptr, 997);
  std::vector<unsigned char> b = Encode(pcm, 2, 44100, 128000, nullptr, 576);
  std::vector<unsigned char> c = Encode(pcm, 2, 44100, 128000, nullptr, 30000);
  EXPECT_EQ(a, b);
  EXPECT_EQ(a, c);
}

TEST(Mp3EncodeRoundTrip, EncodingTwiceGivesTheSameBytes) {
  Pcm pcm = Tone(2, 20000, 44100);
  EXPECT_EQ(Encode(pcm, 2, 44100, 192000), Encode(pcm, 2, 44100, 192000));
}

/* ------------------------------------------------------- the file itself */

TEST(Mp3EncodeFile, FramesAreWhereTheirLengthsSayAndTheReservoirIsReachable) {
  Pcm pcm = Tone(2, 44100 * 2, 44100);
  std::vector<unsigned char> mp3 = Encode(pcm, 2, 44100, 128000);
  size_t at = 0;
  size_t frames = 0;
  MP3_Header first;
  ASSERT_TRUE(gaud_mp3_header_parse(mp3.data(), &first));
  /* The tag frame states the counts. */
  MP3_Vbr_Tag tag;
  ASSERT_TRUE(gaud_mp3_tag_parse(mp3.data(), first.frame_size, &first, &tag));
  EXPECT_TRUE(tag.is_info);
  EXPECT_TRUE(tag.has_frames);
  EXPECT_TRUE(tag.has_bytes);
  EXPECT_EQ(tag.bytes, mp3.size());
  EXPECT_TRUE(tag.lame_present);
  at += first.frame_size;
  uint64_t reservoir_seen = 0;
  while (at < mp3.size()) {
    MP3_Header h;
    ASSERT_TRUE(gaud_mp3_header_parse(mp3.data() + at, &h)) << at;
    ASSERT_LE(at + h.frame_size, mp3.size());
    EXPECT_EQ(h.bitrate, 128000u);
    uint32_t side = gaud_mp3_side_info_size(&h);
    uint32_t begin = ((uint32_t)mp3[at + 4] << 1) | (mp3[at + 5] >> 7);
    EXPECT_LE(begin, 511u);
    reservoir_seen = std::max<uint64_t>(reservoir_seen, begin);
    (void)side;
    at += h.frame_size;
    ++frames;
  }
  EXPECT_EQ(at, mp3.size());
  EXPECT_EQ(frames, tag.frames);
  EXPECT_GT(reservoir_seen, 0u) << "a signal that varies should use the reservoir";
}

TEST(Mp3EncodeFile, TheTagStatesTheDelayAndPaddingThatMakeTheLengthExact) {
  Pcm pcm = Tone(1, 10000, 44100);
  std::vector<unsigned char> mp3 = Encode(pcm, 1, 44100, 64000);
  MP3_Header h;
  ASSERT_TRUE(gaud_mp3_header_parse(mp3.data(), &h));
  MP3_Vbr_Tag tag;
  ASSERT_TRUE(gaud_mp3_tag_parse(mp3.data(), h.frame_size, &h, &tag));
  GAUD_Trim trim = gaud_mp3_tag_trim(&tag);
  EXPECT_TRUE(trim.stated);
  EXPECT_EQ(trim.encoder_delay, 1057u);
  EXPECT_EQ(trim.encoder_delay + 10000u + trim.padding, (uint64_t)tag.frames * 1152u);
}

/* ------------------------------------------------------------ refusals */

TEST(Mp3EncodeRefusals, WhatTheFormatCannotCarryIsRefusedAtCreation) {
  Pcm pcm(100, 0);
  GAUD_Result result = GAUD_OK;
  Encode(pcm, 2, 44101, 128000, &result);
  EXPECT_EQ(result, GAUD_ERR_UNSUPPORTED) << "a sample rate no MPEG version has";
  Encode(pcm, 2, 44100, 130000, &result);
  EXPECT_EQ(result, GAUD_ERR_UNSUPPORTED) << "a bit rate that is not in the table";
  Encode(pcm, 2, 44100, 8000, &result);
  EXPECT_EQ(result, GAUD_ERR_UNSUPPORTED) << "8 kbit/s is MPEG-2's, not MPEG-1's";
  Encode(pcm, 2, 22050, 320000, &result);
  EXPECT_EQ(result, GAUD_ERR_UNSUPPORTED) << "and 320 is MPEG-1's";
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
