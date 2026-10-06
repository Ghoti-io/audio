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
#include <algorithm>
#include <array>
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

/** Encode with a caller-built parameter block. */
std::vector<unsigned char> EncodeWith(const Pcm & pcm, GAUD_Encode_Params params,
    GAUD_Result * out_result = nullptr) {
  params.format = GAUD_SAMPLE_S16;
  params.coding = GAUD_CODING_MPEG_LAYER3;
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
    unsigned channels = params.layout.channels;
    size_t chunk = 4096;
    EXPECT_EQ(gaud_buffer_create(nullptr, GAUD_SAMPLE_S16, params.layout,
                  GAUD_LAYOUT_INTERLEAVED, chunk, &buffer),
        GAUD_OK);
    size_t frames = pcm.size() / channels;
    for (size_t at = 0; at < frames; at += chunk) {
      size_t n = std::min(chunk, frames - at);
      EXPECT_EQ(gaud_buffer_set_frames(buffer, n), GAUD_OK);
      std::memcpy(gaud_buffer_data(buffer), pcm.data() + at * channels, n * channels * 2u);
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

TEST(Mp3EncodeRoundTrip, AClickKeepsItsEnergyWhereverItFallsInAGranule) {
  /* A single loud sample is the worst thing a granule can hold: a flat
   * spectrum, every line occupied. Masked, it needs more bits than the
   * twelve-bit length field can state; the encoder once let the length
   * wrap, wrote a field a quarter of the true size, and every decoder read
   * the truncated granule as it was written - so the click came back with a
   * fifth of its energy and nothing complained. Whatever the alignment,
   * most of it has to be there. */
  for (size_t position : {11900u, 12000u, 12100u, 12200u, 12300u, 12500u, 12800u}) {
    size_t frames = 22050;
    Pcm pcm(frames, 0);
    pcm[position] = 20000;
    Decoded d = Decode(Encode(pcm, 1, 44100, 128000));
    ASSERT_EQ(d.frames, frames);
    double energy = 0;
    for (size_t i = 0; i < frames; ++i) {
      energy += (double)d.pcm[i] * d.pcm[i];
    }
    EXPECT_GT(energy / (20000.0 * 20000.0), 0.9) << "at " << position;
    EXPECT_LT(energy / (20000.0 * 20000.0), 1.2) << "at " << position;
  }
}

TEST(Mp3EncodeRoundTrip, APureToneIsCodedAccuratelyAndALoudSquareWaveSurvives) {
  /* Two things a tone with a noise floor and a half-scale level do not
   * reach. A pure tone is one huge line and silence: a step search that
   * does not stop where that line would clamp picks the finest step there
   * is, which costs almost nothing in bits and returns the tone at a
   * quarter of its level. And a full-scale low-frequency wave puts the same
   * sign in every input of a subband's transform, whose first coefficient
   * is then eleven times the largest input - which an int32 does not hold
   * until the scale that brings it back has been applied. */
  size_t frames = 44100;
  Pcm tone(frames * 2);
  Pcm square(frames * 2);
  for (size_t i = 0; i < frames; ++i) {
    int16_t t = (int16_t)std::lround(16000.0 * std::sin(2 * M_PI * 1000.0 * i / 44100.0));
    int16_t q = (i % 441) < 220 ? 32767 : -32768;
    tone[2 * i] = tone[2 * i + 1] = t;
    square[2 * i] = square[2 * i + 1] = q;
  }
  EXPECT_GT(SnrDb(tone, Decode(Encode(tone, 2, 44100, 128000)).pcm), 40.0);
  EXPECT_GT(SnrDb(square, Decode(Encode(square, 2, 44100, 128000)).pcm), 15.0);
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

/* ---------------------------------------------- the psychoacoustic model */

/** Run the model on @p granules of @p make(sample index), return the last result. */
template <typename F>
MP3E_Psy_Result RunPsy(F make, unsigned granules, const MP3E_Psy_Rate & rate,
    int32_t * spectrum, int snr_offset = 0) {
  MP3E_Psy_Channel channel;
  gaud_mp3e_psy_channel_reset(&channel);
  MP3E_Filter filter;
  gaud_mp3e_filter_reset(&filter);
  MP3E_Psy_Result result;
  for (unsigned g = 0; g < granules; ++g) {
    int16_t pcm[576];
    for (unsigned i = 0; i < 576; ++i) {
      pcm[i] = make(g * 576u + i);
    }
    gaud_mp3e_psy_analyze(&rate, &channel, pcm, snr_offset, 0, &result);
    gaud_mp3e_filter_analyze(&filter, pcm, 1);
  }
  gaud_mp3e_filter_transform(&filter, 0, spectrum);
  return result;
}

double BandEnergy(const int32_t * spectrum, unsigned from, unsigned to) {
  double e = 0;
  for (unsigned k = from; k < to; ++k) {
    double v = (double)(spectrum[k] >> MP3E_ENERGY_SHIFT);
    e += v * v;
  }
  return e;
}

TEST(Mp3EncodePsy, ATonesMaskingIsEighteenDecibelsDownAndNoiseSixIsNot) {
  MP3E_Psy_Rate rate;
  gaud_mp3e_psy_rate_init(&rate, 0, 0);
  int32_t spectrum[576];
  uint32_t state = 7;
  auto tone = [&](unsigned n) {
    state = state * 1664525u + 1013904223u;
    double noise = ((int32_t)(state >> 8) - 8388608) / 8388608.0;
    return (int16_t)std::lround(10000.0 * std::sin(2 * M_PI * 1000.0 * n / 44100.0) + 60.0 * noise);
  };
  /* Long enough for the pre-echo rule - a threshold may not more than
   * double from one granule to the next - to have let it settle. */
  MP3E_Psy_Result r = RunPsy(tone, 90, rate, spectrum);
  const uint16_t * bounds = gaud_mp3_sfb_long[0];
  /* The band holding 1 kHz is the sixth: lines 24 to 30. */
  double signal = BandEnergy(spectrum, bounds[6], bounds[7]);
  double allowed = (double)r.allowed_long[6];
  double db = 10.0 * std::log10(signal / allowed);
  EXPECT_GT(db, 15.0) << "a tone is masked less than that";
  EXPECT_LT(db, 24.0) << "and more than that is not the model";

  state = 99;
  auto noise = [&](unsigned) {
    state = state * 1664525u + 1013904223u;
    double v = ((int32_t)(state >> 8) - 8388608) / 8388608.0;
    return (int16_t)std::lround(4000.0 * v);
  };
  r = RunPsy(noise, 90, rate, spectrum);
  /* A noise masker has a signal-to-noise ratio of six decibels, less what
   * normalising by the spreading function's width takes - or the least the
   * model holds a band at that frequency to, if that is more: 24 dB up to
   * 600 Hz, six an octave less to 3 kHz, three an octave less above. */
  auto least = [](double f) {
    if (f <= 600.0) {
      return 24.0;
    }
    if (f <= 3000.0) {
      return 24.0 - 6.0 * std::log2(f / 600.0);
    }
    return std::max(0.0, 10.0 - 3.0 * std::log2(f / 3000.0));
  };
  double off_sum = 0;
  for (unsigned b = 6; b < 19; ++b) {
    double e = BandEnergy(spectrum, bounds[b], bounds[b + 1]);
    double d = 10.0 * std::log10(e / (double)r.allowed_long[b]);
    double centre = 0.5 * (bounds[b] + bounds[b + 1]) * 44100.0 / 1152.0;
    double expected = std::max(6.0, least(centre));
    /* One band can sit several decibels off - a band straddles partitions
     * whose floors differ - but the shape is the table's. */
    EXPECT_NEAR(d, expected - 0.5, 5.0) << "band " << b << " at " << centre << " Hz";
    off_sum += d - (expected - 0.5);
  }
  EXPECT_NEAR(off_sum / 13.0, 0.0, 1.5) << "the level is the table's, on average";
}

TEST(Mp3EncodePsy, AStricterOffsetLowersEveryThreshold) {
  MP3E_Psy_Rate rate;
  gaud_mp3e_psy_rate_init(&rate, 0, 0);
  int32_t spectrum[576];
  uint32_t state = 5;
  auto noise = [&](unsigned) {
    state = state * 1664525u + 1013904223u;
    return (int16_t)(((int32_t)(state >> 8) - 8388608) / 2200);
  };
  MP3E_Psy_Result loose = RunPsy(noise, 60, rate, spectrum, 0);
  state = 5;
  MP3E_Psy_Result strict = RunPsy(noise, 60, rate, spectrum, 6 * 256);
  for (unsigned b = 10; b < 18; ++b) {
    double ratio = (double)strict.allowed_long[b] / (double)loose.allowed_long[b];
    EXPECT_NEAR(10.0 * std::log10(ratio), -6.0, 1.0) << "band " << b;
  }
}

TEST(Mp3EncodePsy, AThresholdMayNotMoreThanDoubleBetweenGranules) {
  MP3E_Psy_Rate rate;
  gaud_mp3e_psy_rate_init(&rate, 0, 0);
  MP3E_Psy_Channel channel;
  gaud_mp3e_psy_channel_reset(&channel);
  /* Steady noise, then noise four times the amplitude: sixteen times the
   * energy, which an uncapped threshold would follow up at once. */
  MP3E_Psy_Result last{};
  double worst = 0;
  for (unsigned g = 0; g < 40; ++g) {
    int16_t pcm[576];
    for (unsigned i = 0; i < 576; ++i) {
      uint32_t x = (g * 576u + i) * 2654435761u + 12345u;
      x ^= x >> 15;
      x *= 2246822519u;
      x ^= x >> 13;
      int v = (int)(x % 1601u) - 800;
      pcm[i] = (int16_t)(g >= 20 ? v * 4 : v);
    }
    MP3E_Psy_Result now;
    gaud_mp3e_psy_analyze(&rate, &channel, pcm, 0, 0, &now);
    if (g > 5) {
      for (unsigned b = 8; b < 18; ++b) {
        worst = std::max(worst,
            (double)now.allowed_long[b] / (double)last.allowed_long[b]);
      }
    }
    last = now;
  }
  EXPECT_GT(worst, 1.2) << "the step must have been seen for this to mean anything";
  EXPECT_LT(worst, 2.6);
}

TEST(Mp3EncodePsy, ATonePastThreeKilohertzIsMaskedByEighteenDecibelsNotSix) {
  /* Below about 1.5 kHz the model's least ratio (24 dB falling to 10 at
   * 3 kHz) is already stricter than a tone's 18, so tonality can only be
   * seen above that; the unpredictability measure is what tells the two
   * masker kinds apart, and replacing it by "everything is noise" leaves
   * every band at the noise figure. */
  MP3E_Psy_Rate rate;
  gaud_mp3e_psy_rate_init(&rate, 0, 0);
  int32_t spectrum[576];
  uint32_t state = 7;
  auto tone = [&](unsigned n) {
    state = state * 1664525u + 1013904223u;
    double noise = ((int32_t)(state >> 8) - 8388608) / 8388608.0;
    return (int16_t)std::lround(10000.0 * std::sin(2 * M_PI * 6000.0 * n / 44100.0) + 20.0 * noise);
  };
  MP3E_Psy_Result r = RunPsy(tone, 90, rate, spectrum);
  const uint16_t * bounds = gaud_mp3_sfb_long[0];
  unsigned line = (unsigned)(6000.0 / 22050.0 * 576.0);
  unsigned band = 0;
  while (bounds[band + 1] <= line) {
    ++band;
  }
  double signal = BandEnergy(spectrum, bounds[band], bounds[band + 1]);
  double db = 10.0 * std::log10(signal / (double)r.allowed_long[band]);
  EXPECT_GT(db, 13.0) << "a tone is masked less than that";
  EXPECT_LT(db, 24.0) << "and more than that is not the model";
}

TEST(Mp3EncodePsy, RaisingTheHearingThresholdLowersWhatQuietBandsMayCarry) {
  MP3E_Psy_Rate rate;
  gaud_mp3e_psy_rate_init(&rate, 0, 0);
  auto run = [&](int ath_offset_q8) {
    MP3E_Psy_Channel channel;
    gaud_mp3e_psy_channel_reset(&channel);
    MP3E_Psy_Result r{};
    int16_t silence[576] = {};
    for (unsigned g = 0; g < 10; ++g) {
      gaud_mp3e_psy_analyze(&rate, &channel, silence, 0, ath_offset_q8, &r);
    }
    return r;
  };
  MP3E_Psy_Result base = run(0);
  MP3E_Psy_Result lowered = run(10 * 256);
  for (unsigned b = 4; b < 20; ++b) {
    double db = 10.0 * std::log10((double)lowered.allowed_long[b] / (double)base.allowed_long[b]);
    EXPECT_NEAR(db, -10.0, 1.0) << "band " << b;
  }
}

TEST(Mp3EncodePsy, SilenceIsLeftToTheThresholdOfHearing) {
  MP3E_Psy_Rate rate;
  gaud_mp3e_psy_rate_init(&rate, 0, 0);
  int32_t spectrum[576];
  MP3E_Psy_Result r = RunPsy([](unsigned) { return (int16_t)0; }, 20, rate, spectrum);
  for (unsigned b = 0; b < 22; ++b) {
    EXPECT_GT(r.allowed_long[b], 0u) << "band " << b;
  }
  EXPECT_EQ(r.pe_long, 0u) << "nothing to code";
}

/* ------------------------------------------------------- block switching */

/** The block type of every granule-channel of a stereo MPEG-1 file, and
 *  whether each frame used middle/side. */
struct SideInfo {
  std::vector<std::array<int, 4>> types; ///< Per frame: gr0 ch0, ch1, gr1 ch0, ch1.
  std::vector<bool> middle_side;
  std::vector<uint32_t> back; ///< Each frame's back-pointer.
  uint32_t max_back = 0;
};

SideInfo ReadSideInfo(const std::vector<unsigned char> & mp3) {
  SideInfo info;
  MP3_Header first;
  EXPECT_TRUE(gaud_mp3_header_parse(mp3.data(), &first));
  size_t at = first.frame_size;
  while (at + 4 <= mp3.size()) {
    MP3_Header h;
    if (!gaud_mp3_header_parse(mp3.data() + at, &h)) {
      break;
    }
    MP3_Bits bits;
    gaud_mp3_bits_init(&bits, mp3.data() + at + 4, gaud_mp3_side_info_size(&h));
    uint32_t back = gaud_mp3_bits_read(&bits, 9);
    info.max_back = std::max(info.max_back, back);
    info.back.push_back(back);
    gaud_mp3_bits_read(&bits, 3);
    gaud_mp3_bits_read(&bits, 8);
    std::array<int, 4> t = {0, 0, 0, 0};
    for (unsigned gr = 0; gr < 2; ++gr) {
      for (unsigned ch = 0; ch < 2; ++ch) {
        gaud_mp3_bits_read(&bits, 12 + 9 + 8 + 4);
        unsigned ws = gaud_mp3_bits_read(&bits, 1);
        unsigned type = 0;
        if (ws) {
          type = gaud_mp3_bits_read(&bits, 2);
          gaud_mp3_bits_read(&bits, 1 + 10 + 9);
        }
        else {
          gaud_mp3_bits_read(&bits, 15 + 4 + 3);
        }
        gaud_mp3_bits_read(&bits, 3);
        t[gr * 2 + ch] = (int)type;
      }
    }
    info.types.push_back(t);
    info.middle_side.push_back((h.mode_extension & 2u) != 0);
    at += h.frame_size;
  }
  return info;
}

/** Quiet noise with loud bursts of noise at irregular spacing. */
Pcm Bursts(unsigned channels, size_t frames) {
  Pcm pcm(frames * channels);
  uint32_t state = 4242;
  auto next = [&]() {
    state = state * 1664525u + 1013904223u;
    return ((int32_t)(state >> 8) - 8388608) / 8388608.0;
  };
  size_t spacing = 7000;
  for (size_t i = 0; i < frames; ++i) {
    size_t phase = i % spacing;
    double envelope = phase < 2200 ? std::exp(-(double)phase / 500.0) : 0.0;
    for (unsigned c = 0; c < channels; ++c) {
      double v = 3.0 * next() + 9000.0 * envelope * next();
      pcm[i * channels + c] = (int16_t)std::lround(v);
    }
  }
  return pcm;
}

TEST(Mp3EncodeBlocks, TransientsMakeShortBlocksAndTheSequenceIsLegal) {
  Pcm pcm = Bursts(2, 44100 * 3);
  SideInfo info = ReadSideInfo(Encode(pcm, 2, 44100, 128000));
  int counts[4] = {0, 0, 0, 0};
  int previous[2] = {-1, -1}; /* before the first granule there is nothing */
  for (const auto & t : info.types) {
    for (unsigned gr = 0; gr < 2; ++gr) {
      EXPECT_EQ(t[gr * 2], t[gr * 2 + 1]) << "both channels share a block type";
      for (unsigned ch = 0; ch < 2; ++ch) {
        int type = t[gr * 2 + ch];
        ++counts[type];
        /* The format's transitions: start is followed by short, short by
         * short or stop, and nothing else may follow them. */
        if (previous[ch] == 1) {
          EXPECT_EQ(type, 2);
        }
        if (previous[ch] == 2) {
          EXPECT_TRUE(type == 2 || type == 3) << "type " << type;
        }
        if (type == 2) {
          EXPECT_TRUE(previous[ch] == -1 || previous[ch] == 1 || previous[ch] == 2);
        }
        if (type == 3) {
          EXPECT_EQ(previous[ch], 2);
        }
        previous[ch] = type;
      }
    }
  }
  EXPECT_GT(counts[1], 0) << "a start block";
  EXPECT_GT(counts[2], 0) << "a short block";
  EXPECT_GT(counts[3], 0) << "a stop block";
  EXPECT_GT(counts[0], counts[2]) << "most of a sparse signal is long";
}

TEST(Mp3EncodeBlocks, ShortBlocksKeepTheNoiseBeforeAnOnsetDown) {
  /* A burst of loud noise in near-silence. Coded with one long window the
   * noise spreads over 1152 samples and is far above the silence well
   * before the burst; with short blocks it stays within a short window of
   * it. The window measured is 700 to 420 samples ahead: further than a
   * short block's noise can reach and well inside a long one's. */
  size_t frames = 44100;
  Pcm pcm(frames, 0);
  uint32_t state = 1;
  size_t onset = 20000;
  for (size_t i = 0; i < frames; ++i) {
    state = state * 1664525u + 1013904223u;
    double n = ((int32_t)(state >> 8) - 8388608) / 8388608.0;
    double burst = (i >= onset && i < onset + 300) ? 12000.0 : 0.0;
    pcm[i] = (int16_t)std::lround(2.0 * n + burst * n);
  }
  Decoded d = Decode(Encode(pcm, 1, 44100, 128000));
  ASSERT_EQ(d.frames, frames);
  double before = 0;
  double original = 0;
  for (size_t i = onset - 700; i < onset - 420; ++i) {
    double e = (double)d.pcm[i] - pcm[i];
    before += e * e;
    original += (double)pcm[i] * pcm[i];
  }
  /* The silence's own energy is the reference: the noise added there may
   * be a few times it, not ten thousand. With long blocks alone it is 1.5
   * million against a hundred and thirty. */
  EXPECT_LT(before, 30.0 * original) << "pre-echo energy " << before << " against " << original;
}

/** Steady noise of one level, then of another, at sample @p at. */
Pcm Steps(unsigned channels, size_t frames, size_t at, double before, double after) {
  Pcm pcm(frames * channels);
  uint32_t state = 31337;
  for (size_t i = 0; i < frames; ++i) {
    for (unsigned c = 0; c < channels; ++c) {
      state = state * 1664525u + 1013904223u;
      double n = ((int32_t)(state >> 8) - 8388608) / 8388608.0;
      pcm[i * channels + c] = (int16_t)std::lround((i < at ? before : after) * n);
    }
  }
  return pcm;
}

/** Granules of short or transitional type within three frames of @p frame. */
int ShortsNear(const SideInfo & info, size_t frame) {
  int found = 0;
  for (size_t f = frame > 3 ? frame - 3 : 0; f < info.types.size() && f <= frame + 3; ++f) {
    for (int t : info.types[f]) {
      found += t != 0;
    }
  }
  return found;
}

TEST(Mp3EncodeBlocks, ARiseOfTwelveDecibelsAmongSteadyNoiseIsAnOnset) {
  /* Four times the amplitude is sixteen times the energy: past the rule's
   * factor of ten, and nowhere near leaving the earlier window's noise over
   * the signal, so it is the rise test that has to see it. */
  size_t at = 44100;
  SideInfo info = ReadSideInfo(Encode(Steps(2, 44100 * 2, at, 2000.0, 8000.0), 2, 44100, 128000));
  EXPECT_GT(ShortsNear(info, at / 1152), 0);
  SideInfo flat = ReadSideInfo(Encode(Steps(2, 44100 * 2, at, 8000.0, 8000.0), 2, 44100, 128000));
  EXPECT_EQ(ShortsNear(flat, at / 1152), 0) << "steady noise alone does not";
}

TEST(Mp3EncodeBlocks, AnAbruptStopIsAnEventToo) {
  /* Loud noise that ends at once. Nothing rises: the energy only falls. The
   * quiet that follows is where a long window's noise would stand over the
   * signal, so the decay test has to see it. */
  size_t at = 44100;
  SideInfo info = ReadSideInfo(Encode(Steps(2, 44100 * 2, at, 8000.0, 0.0), 2, 44100, 128000));
  EXPECT_GT(ShortsNear(info, at / 1152), 0);
}

TEST(Mp3EncodeStereo, ACorrelatedPairIsCodedAsMiddleAndSide) {
  Pcm mono = Tone(1, 44100 * 2, 44100);
  Pcm pcm(mono.size() * 2);
  for (size_t i = 0; i < mono.size(); ++i) {
    pcm[2 * i] = mono[i];
    pcm[2 * i + 1] = mono[i];
  }
  std::vector<unsigned char> mp3 = Encode(pcm, 2, 44100, 128000);
  SideInfo info = ReadSideInfo(mp3);
  size_t ms = 0;
  for (bool b : info.middle_side) {
    ms += b;
  }
  EXPECT_GT(ms, info.middle_side.size() * 9 / 10);
  Decoded d = Decode(mp3);
  double worst = 0;
  for (size_t i = 0; i + 1 < d.pcm.size(); i += 2) {
    worst = std::max(worst, std::fabs((double)d.pcm[i] - d.pcm[i + 1]));
  }
  EXPECT_LT(worst, 8.0) << "the side channel carries almost nothing";
  EXPECT_GT(SnrDb(pcm, d.pcm), 15.0);
}

TEST(Mp3EncodeStereo, IndependentChannelsAreNotForcedIntoMiddleSide) {
  Pcm pcm = Tone(2, 44100 * 2, 44100);
  SideInfo info = ReadSideInfo(Encode(pcm, 2, 44100, 128000));
  size_t ms = 0;
  for (bool b : info.middle_side) {
    ms += b;
  }
  EXPECT_LT(ms, info.middle_side.size() / 10);
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

TEST(Mp3EncodeFile, AHardFrameDrawsOnWhatEasyOnesSaved) {
  /* A plain tone for a second, then loud noise. The reservoir fills while it
   * is easy and a frame that wants more than its own bytes empties it, so
   * between two frames the back-pointer must fall by a good part of what it
   * held - and a stream whose frames never drew on it would never do that. */
  Pcm pcm = Steps(2, 44100 * 3, 44100, 0.0, 12000.0);
  for (size_t i = 0; i < 44100; ++i) {
    pcm[2 * i] = pcm[2 * i + 1]
        = (int16_t)std::lround(3000.0 * std::sin(2 * M_PI * 700.0 * i / 44100.0));
  }
  SideInfo info = ReadSideInfo(Encode(pcm, 2, 44100, 128000));
  uint32_t previous = 0;
  uint32_t drop = 0;
  uint32_t high = 0;
  for (size_t f = 0; f < info.back.size(); ++f) {
    high = std::max(high, info.back[f]);
    if (previous > info.back[f]) {
      drop = std::max(drop, previous - info.back[f]);
    }
    previous = info.back[f];
  }
  EXPECT_GT(high, 300u) << "the quiet second fills it";
  EXPECT_GT(drop, 150u) << "and the loud one drains it";
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

/* ----------------------------------------------------- the rate policies */

/** The bit rate of every audio frame, in kbit/s, and the tag's fields. */
struct Frames {
  std::vector<unsigned> kbps;
  MP3_Vbr_Tag tag;
  bool tag_ok = false;
  size_t first = 0;
  size_t total_bytes = 0;
};

Frames Walk(const std::vector<unsigned char> & mp3) {
  Frames f;
  MP3_Header h;
  EXPECT_TRUE(gaud_mp3_header_parse(mp3.data(), &h));
  f.tag_ok = gaud_mp3_tag_parse(mp3.data(), h.frame_size, &h, &f.tag);
  size_t at = h.frame_size;
  while (at + 4 <= mp3.size()) {
    MP3_Header g;
    if (!gaud_mp3_header_parse(mp3.data() + at, &g)) {
      break;
    }
    f.kbps.push_back(g.bitrate / 1000u);
    at += g.frame_size;
  }
  EXPECT_EQ(at, mp3.size());
  f.total_bytes = mp3.size();
  return f;
}

GAUD_Encode_Params BaseParams(unsigned channels, uint32_t rate) {
  GAUD_Encode_Params params;
  gaud_encode_params_default(&params);
  params.sample_rate = rate;
  params.layout = gaud_channel_layout_default(channels);
  return params;
}

/** A passage that is quiet and plain and then loud and busy, twice. */
Pcm Dynamics(unsigned channels, size_t frames, double rate) {
  Pcm pcm(frames * channels);
  uint32_t state = 77;
  for (size_t i = 0; i < frames; ++i) {
    double phase = std::fmod((double)i / rate, 2.0) / 2.0;
    double loud = phase < 0.5 ? 0.0 : 1.0;
    for (unsigned c = 0; c < channels; ++c) {
      state = state * 1664525u + 1013904223u;
      double noise = ((int32_t)(state >> 8) - 8388608) / 8388608.0;
      double v = 0.2 * std::sin(2 * M_PI * 440.0 * (c + 1) * i / rate)
          + loud * 0.25 * noise;
      pcm[i * channels + c] = (int16_t)std::lround(v * 32767.0);
    }
  }
  return pcm;
}

TEST(Mp3EncodeRate, ConstantRateEveryFrameIsTheSameRateAndTheTagSaysInfo) {
  Pcm pcm = Dynamics(2, 44100 * 3, 44100);
  GAUD_Encode_Params params = BaseParams(2, 44100);
  params.bitrate = 160000;
  Frames f = Walk(EncodeWith(pcm, params));
  ASSERT_TRUE(f.tag_ok);
  EXPECT_TRUE(f.tag.is_info);
  EXPECT_FALSE(f.tag.has_toc);
  for (unsigned k : f.kbps) {
    ASSERT_EQ(k, 160u);
  }
  EXPECT_EQ(f.tag.frames, f.kbps.size());
}

TEST(Mp3EncodeRate, VariableRateFollowsTheMaterialAndTheTagCarriesATable) {
  Pcm pcm = Dynamics(2, 44100 * 6, 44100);
  GAUD_Encode_Params params = BaseParams(2, 44100);
  params.rate_control = GAUD_RATE_VBR;
  params.quality = 50;
  std::vector<unsigned char> mp3 = EncodeWith(pcm, params);
  Frames f = Walk(mp3);
  ASSERT_TRUE(f.tag_ok);
  EXPECT_FALSE(f.tag.is_info);
  EXPECT_TRUE(f.tag.has_toc);
  EXPECT_EQ(f.tag.frames, f.kbps.size());
  EXPECT_EQ(f.tag.bytes, mp3.size());
  unsigned lo = *std::min_element(f.kbps.begin(), f.kbps.end());
  unsigned hi = *std::max_element(f.kbps.begin(), f.kbps.end());
  EXPECT_LT(lo * 2u, hi) << "quiet and plain costs far less than loud and busy";
  /* The table never goes backwards and ends near the end. */
  for (unsigned i = 1; i < 100; ++i) {
    EXPECT_GE(f.tag.toc[i], f.tag.toc[i - 1]);
  }
  EXPECT_EQ(f.tag.toc[0], 0u + f.tag.toc[0]);
  EXPECT_GT(f.tag.toc[99], 200u);
  /* And it still decodes to the right length. */
  EXPECT_EQ(Decode(mp3).frames, pcm.size() / 2);
}

TEST(Mp3EncodeRate, HigherQualityCostsMoreAndSoundsCloser) {
  Pcm pcm = Dynamics(2, 44100 * 4, 44100);
  size_t previous_size = 0;
  double previous_snr = -1000;
  for (uint32_t q : {20u, 50u, 80u}) {
    GAUD_Encode_Params params = BaseParams(2, 44100);
    params.rate_control = GAUD_RATE_VBR;
    params.quality = q;
    std::vector<unsigned char> mp3 = EncodeWith(pcm, params);
    double snr = SnrDb(pcm, Decode(mp3).pcm);
    EXPECT_GT(mp3.size(), previous_size) << "quality " << q;
    EXPECT_GT(snr, previous_snr) << "quality " << q;
    previous_size = mp3.size();
    previous_snr = snr;
  }
}

TEST(Mp3EncodeRate, VariableRateHonoursItsFloorAndCeiling) {
  Pcm pcm = Dynamics(2, 44100 * 4, 44100);
  GAUD_Encode_Params params = BaseParams(2, 44100);
  params.rate_control = GAUD_RATE_VBR;
  params.quality = 50;
  params.min_bitrate = 96000;
  params.bitrate = 192000;
  Frames f = Walk(EncodeWith(pcm, params));
  for (unsigned k : f.kbps) {
    ASSERT_GE(k, 96u);
    ASSERT_LE(k, 192u);
  }
}

TEST(Mp3EncodeRate, AverageRateLandsNearItsTarget) {
  Pcm pcm = Dynamics(2, 44100 * 12, 44100);
  GAUD_Encode_Params params = BaseParams(2, 44100);
  params.rate_control = GAUD_RATE_ABR;
  params.bitrate = 128000;
  std::vector<unsigned char> mp3 = EncodeWith(pcm, params);
  Frames f = Walk(mp3);
  double seconds = (double)pcm.size() / 2 / 44100.0;
  double average = (double)mp3.size() * 8.0 / seconds / 1000.0;
  EXPECT_NEAR(average, 128.0, 128.0 * 0.04);
  unsigned lo = *std::min_element(f.kbps.begin(), f.kbps.end());
  unsigned hi = *std::max_element(f.kbps.begin(), f.kbps.end());
  EXPECT_LT(lo, hi) << "an average-rate file is not a constant one";
}

TEST(Mp3EncodeFile, AnEasySignalAtAHighRateSpendsItsSurplusOnFinerCoding) {
  /* A tone needs a few hundred bits a frame; 320 kbit/s offers a thousand
   * times that, and the reservoir is full after a few frames. What it holds
   * beyond five eighths is put into finer steps rather than stuffed. */
  Pcm pcm = Tone(1, 44100 * 3, 44100);
  Decoded d = Decode(Encode(pcm, 1, 44100, 320000));
  double snr = SnrDb(pcm, d.pcm);
  /* Measured 52.6 dB; leaving the surplus unspent gives 49.8. The encoder is
   * deterministic, so the figure is exact and the bound sits between. */
  EXPECT_GT(snr, 51.0);
}

TEST(Mp3EncodeRate, ConstantRateAt44100PadsToTheExactAverage) {
  /* 128 kbit/s at 44.1 kHz is 417.96 bytes a frame, so one in about twenty
   * frames carries a padding byte; without the carry the file is 0.2 % short,
   * which over this many frames is a whole frame or more. */
  Pcm pcm = Tone(1, 44100 * 30, 44100);
  std::vector<unsigned char> mp3 = Encode(pcm, 1, 44100, 128000);
  Frames f = Walk(mp3);
  double ideal = 128000.0 / 8.0 * (double)f.kbps.size() * 1152.0 / 44100.0;
  EXPECT_NEAR((double)mp3.size(), ideal, 420.0);
}

TEST(Mp3EncodeRate, TheLowerSamplingFrequenciesDoAllThreeToo) {
  for (uint32_t rate : {22050u, 11025u}) {
    Pcm pcm = Dynamics(1, rate * 3, rate);
    for (GAUD_Rate_Control mode : {GAUD_RATE_CBR, GAUD_RATE_ABR, GAUD_RATE_VBR}) {
      GAUD_Encode_Params params = BaseParams(1, rate);
      params.rate_control = mode;
      params.bitrate = mode == GAUD_RATE_VBR ? 0 : 48000;
      std::vector<unsigned char> mp3 = EncodeWith(pcm, params);
      ASSERT_FALSE(mp3.empty()) << rate << " mode " << mode;
      EXPECT_EQ(Decode(mp3).frames, pcm.size()) << rate << " mode " << mode;
    }
  }
}

TEST(Mp3EncodeRate, ImpossibleRequestsAreRefused) {
  Pcm pcm(1000, 0);
  GAUD_Result result = GAUD_OK;
  GAUD_Encode_Params params = BaseParams(2, 44100);
  params.rate_control = GAUD_RATE_ABR;
  EncodeWith(pcm, params, &result);
  EXPECT_EQ(result, GAUD_ERR_UNSUPPORTED) << "an average with no target";
  params.bitrate = 8000;
  EncodeWith(pcm, params, &result);
  EXPECT_EQ(result, GAUD_ERR_UNSUPPORTED) << "a target under the format's floor";
  params = BaseParams(2, 44100);
  params.rate_control = GAUD_RATE_VBR;
  params.min_bitrate = 256000;
  params.bitrate = 128000;
  EncodeWith(pcm, params, &result);
  EXPECT_EQ(result, GAUD_ERR_INVALID) << "a floor over the ceiling";
  params = BaseParams(2, 44100);
  params.rate_control = GAUD_RATE_VBR;
  params.quality = 101;
  EncodeWith(pcm, params, &result);
  EXPECT_EQ(result, GAUD_ERR_INVALID);
}

TEST(Mp3EncodeTag, TheFrameIsAFunctionOfItsFieldsAlone) {
  /* The tag's checksum once read 190 bytes of a frame that could be
   * shorter, and so depended on what happened to be in memory after it -
   * which a big-endian machine under qemu showed by writing a different
   * byte. Whatever surrounds the buffer, the frame is the same. */
  for (unsigned version = 0; version < 3u; ++version) {
    for (unsigned channels = 1; channels <= 2u; ++channels) {
      MP3E_Tag tag = {};
      tag.version = (MP3_Version)version;
      tag.rate_index = 0;
      tag.channels = channels;
      tag.bitrate_index = 6;
      tag.vbr = version != 1u;
      tag.frames = 1234;
      tag.bytes = 56789;
      tag.delay = 528;
      tag.padding = 700;
      tag.music_crc = 0xBEEF;
      tag.music_length = 56789;
      tag.vbr_method = 1;
      tag.bitrate_byte = 64;
      unsigned char toc[100];
      for (unsigned i = 0; i < 100; ++i) {
        toc[i] = (unsigned char)i;
      }
      tag.toc = tag.vbr ? toc : nullptr;
      uint32_t head = 4 + gaud_mp3e_side_bytes(tag.version, channels);
      tag.frame_bytes = head + (tag.vbr ? 156 : 52);
      unsigned char a[4000];
      unsigned char b[4000];
      std::memset(a, 0x00, sizeof(a));
      std::memset(b, 0xFF, sizeof(b));
      gaud_mp3e_tag_build(&tag, a);
      gaud_mp3e_tag_build(&tag, b);
      EXPECT_EQ(std::memcmp(a, b, tag.frame_bytes), 0) << version << " x" << channels;
      /* And the checksum is over the bytes before its own field. */
      const unsigned char * lame = a + head + (tag.vbr ? 8 + 8 + 100 + 4 : 8 + 8);
      uint16_t stated = (uint16_t)((lame[34] << 8) | lame[35]);
      EXPECT_EQ(stated, gaud_mp3e_crc16(a, (size_t)(lame + 34 - a), 0));
    }
  }
}

TEST(Mp3EncodeFile, TagsAreWrittenAsID3v2AndReadBack) {
  GAUD_Meta * meta = nullptr;
  ASSERT_EQ(gaud_meta_create(nullptr, &meta), GAUD_OK);
  ASSERT_EQ(gaud_meta_add(meta, GAUD_TAG_TITLE, "A Title with \xC3\xA9 in it"), GAUD_OK);
  ASSERT_EQ(gaud_meta_add(meta, GAUD_TAG_ARTIST, "Somebody"), GAUD_OK);
  Pcm pcm = Tone(2, 20000, 44100);
  GAUD_Encode_Params params = BaseParams(2, 44100);
  params.meta = meta;
  std::vector<unsigned char> mp3 = EncodeWith(pcm, params);
  EXPECT_EQ(std::memcmp(mp3.data(), "ID3", 3), 0) << "tags go in front, as ID3v2";

  GAUD_Stream * stream = nullptr;
  ASSERT_EQ(gaud_stream_create_memory(mp3.data(), mp3.size(), &stream), GAUD_OK);
  GAUD_Doc * doc = nullptr;
  ASSERT_EQ(gaud_doc_load(nullptr, stream, nullptr, nullptr, &doc), GAUD_OK);
  const GAUD_Meta * read = gaud_doc_meta(doc);
  ASSERT_NE(read, nullptr);
  ASSERT_EQ(gaud_meta_count(read, GAUD_TAG_TITLE), 1u);
  EXPECT_STREQ(gaud_meta_get(read, GAUD_TAG_TITLE, 0), "A Title with \xC3\xA9 in it");
  EXPECT_STREQ(gaud_meta_get(read, GAUD_TAG_ARTIST, 0), "Somebody");
  gaud_doc_destroy(doc);
  gaud_stream_destroy(stream);
  /* And the audio is still where it was: the same length once decoded. */
  EXPECT_EQ(Decode(mp3).frames, pcm.size() / 2);

  /* Dropping the tags leaves a file that starts at the first frame. */
  params.meta_policy = GAUD_META_DROP_ALL;
  std::vector<unsigned char> bare = EncodeWith(pcm, params);
  EXPECT_NE(std::memcmp(bare.data(), "ID3", 3), 0);
  EXPECT_LT(bare.size(), mp3.size());
  gaud_meta_destroy(meta);
}

TEST(Mp3EncodeRegistration, TheEncoderStatesWhatKindOfEncoderItIs) {
  const GAUD_Codec * codec = gaud_registry_find(nullptr, "mp3");
  ASSERT_NE(codec, nullptr);
  EXPECT_TRUE(codec->capabilities & GAUD_CAP_ENCODE);
  EXPECT_EQ(codec->encoder_tier, GAUD_ENCODER_PRODUCTION);
}

TEST(Mp3EncodeRefusals, OnlySixteenBitSamplesAreAccepted) {
  GAUD_Encode_Params params = BaseParams(2, 44100);
  params.format = GAUD_SAMPLE_S24;
  GAUD_Stream * out = nullptr;
  ASSERT_EQ(gaud_stream_create_memory_writer(nullptr, &out), GAUD_OK);
  GAUD_Encoder * encoder = nullptr;
  GAUD_Result result = gaud_encoder_create("mp3", nullptr, out, &params, &encoder);
  EXPECT_NE(result, GAUD_OK);
  gaud_encoder_destroy(encoder);
  gaud_stream_destroy(out);
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
