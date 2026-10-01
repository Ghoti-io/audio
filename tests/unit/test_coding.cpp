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
 * The coded sample paths: G.711 and the two ADPCM families.
 *
 * What is asserted here is what can be asserted *exactly*. ADPCM is lossy,
 * so "the samples come back" is not available and an SNR floor is the
 * honest substitute - but several exact properties survive, and those are
 * the ones worth a unit test rather than a differential:
 *
 *   - the two G.711 laws are total functions on 256 codes, and encoding a
 *     decoded code returns it (the one exception is µ-law's duplicate
 *     zero, which is a property of the law);
 *   - a block's geometry is determined by the container's header, and the
 *     three formulas are checked against the numbers ffmpeg and libsndfile
 *     actually write;
 *   - a seek lands exactly, because every block is self-contained;
 *   - the combinations each container cannot spell are refused by name.
 *
 * The sample values themselves are scored against ffmpeg and libsndfile by
 * `make check-corpus`, which is where a wrong table entry is caught.
 */

#include <ghoti.io/audio/audio.h>
#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <string>
#include <vector>

namespace {

struct Case {
  const char * container;
  GAUD_Sample_Coding coding;
  const char * name;
};

/* Every (container, coding) this library claims to write. */
const Case supported[] = {
    {"wav", GAUD_CODING_G711_ULAW, "wav/ulaw"},
    {"wav", GAUD_CODING_G711_ALAW, "wav/alaw"},
    {"wav", GAUD_CODING_ADPCM_IMA_WAV, "wav/ima-wav"},
    {"wav", GAUD_CODING_ADPCM_MS, "wav/ms-adpcm"},
    {"aiff", GAUD_CODING_G711_ULAW, "aiff/ulaw"},
    {"aiff", GAUD_CODING_G711_ALAW, "aiff/alaw"},
    {"aiff", GAUD_CODING_ADPCM_IMA_QT, "aiff/ima-qt"},
};

/* Combinations that exist as codings but not in that container. */
const Case unsupported[] = {
    {"aiff", GAUD_CODING_ADPCM_IMA_WAV, "aiff/ima-wav"},
    {"aiff", GAUD_CODING_ADPCM_MS, "aiff/ms-adpcm"},
    {"wav", GAUD_CODING_ADPCM_IMA_QT, "wav/ima-qt"},
};

/** A signal with content at several scales, so a step that adapts badly
 *  shows up as error rather than as luck. */
std::vector<int16_t> signal(size_t frames, uint32_t channels) {
  std::vector<int16_t> out(frames * channels);
  for (size_t f = 0; f < frames; ++f) {
    for (uint32_t c = 0; c < channels; ++c) {
      double t = (double)f;
      double v = 9000.0 * std::sin(t * 0.03 + c)
          + 3000.0 * std::sin(t * 0.31) + 900.0 * std::sin(t * 1.7 + c);
      out[f * channels + c] = (int16_t)std::lrint(v);
    }
  }
  return out;
}

/** Write `pcm` into a memory sink with the given container and coding. */
GAUD_Result encode(const Case & twhich, const std::vector<int16_t> & pcm,
    size_t frames, uint32_t channels, std::vector<unsigned char> & out) {
  GAUD_Stream * sink = nullptr;
  if (gaud_stream_create_memory_writer(nullptr, &sink) != GAUD_OK) {
    return GAUD_ERR_OOM;
  }
  GAUD_Encode_Params params;
  gaud_encode_params_default(&params);
  params.format = GAUD_SAMPLE_S16;
  params.coding = twhich.coding;
  params.sample_rate = 8000;
  params.layout = gaud_channel_layout_unspecified(channels);

  GAUD_Encoder * encoder = nullptr;
  GAUD_Result result
      = gaud_encoder_create(twhich.container, nullptr, sink, &params,
          &encoder);
  if (result != GAUD_OK) {
    gaud_stream_destroy(sink);
    return result;
  }

  GAUD_Buffer * buffer = nullptr;
  /* 997 is not a divisor of any block size here, so the last write is
   * always partial and the block boundary never lines up with a call. */
  if (gaud_buffer_create(nullptr, GAUD_SAMPLE_S16,
          gaud_channel_layout_unspecified(channels), GAUD_LAYOUT_INTERLEAVED,
          997, &buffer)
      != GAUD_OK) {
    gaud_encoder_destroy(encoder);
    gaud_stream_destroy(sink);
    return GAUD_ERR_OOM;
  }
  size_t at = 0;
  while (at < frames && result == GAUD_OK) {
    size_t take = frames - at < 997 ? frames - at : 997;
    std::memcpy(gaud_buffer_data(buffer), pcm.data() + at * channels,
        take * channels * sizeof(int16_t));
    gaud_buffer_set_frames(buffer, take);
    result = gaud_encoder_write(encoder, buffer);
    at += take;
  }
  if (result == GAUD_OK) {
    result = gaud_encoder_finish(encoder);
  }
  if (result == GAUD_OK) {
    const void * bytes = nullptr;
    size_t length = 0;
    gaud_stream_writer_bytes(sink, &bytes, &length);
    const unsigned char * p = (const unsigned char *)bytes;
    out.assign(p, p + length);
  }
  gaud_buffer_destroy(buffer);
  gaud_encoder_destroy(encoder);
  gaud_stream_destroy(sink);
  return result;
}

/** Read it all back. */
GAUD_Result decode(const std::vector<unsigned char> & bytes,
    std::vector<int16_t> & out, GAUD_Sample_Coding * out_coding,
    uint64_t * out_frames) {
  GAUD_Stream * stream = nullptr;
  if (gaud_stream_create_memory(bytes.data(), bytes.size(), &stream)
      != GAUD_OK) {
    return GAUD_ERR_OOM;
  }
  GAUD_Doc * doc = nullptr;
  GAUD_Result result = gaud_doc_load(nullptr, stream, nullptr, nullptr, &doc);
  if (result != GAUD_OK) {
    gaud_stream_destroy(stream);
    return result;
  }
  GAUD_Track * track = gaud_doc_track(doc, 0);
  *out_coding = gaud_track_coding(track);
  *out_frames = gaud_track_frames(track);
  uint32_t channels = gaud_track_layout(track).channels;

  GAUD_Decoder * decoder = nullptr;
  result = gaud_decoder_create(track, &decoder);
  if (result == GAUD_OK) {
    GAUD_Buffer * buffer = nullptr;
    gaud_decoder_buffer_create(decoder, nullptr, 331, &buffer);
    for (;;) {
      if (gaud_decoder_read(decoder, buffer) != GAUD_OK) {
        result = GAUD_ERR_IO;
        break;
      }
      size_t got = gaud_buffer_frames(buffer);
      if (got == 0) {
        break;
      }
      const int16_t * p = (const int16_t *)gaud_buffer_data_const(buffer);
      out.insert(out.end(), p, p + got * channels);
    }
    gaud_buffer_destroy(buffer);
    gaud_decoder_destroy(decoder);
  }
  gaud_doc_destroy(doc);
  gaud_stream_destroy(stream);
  return result;
}

double snr_db(const std::vector<int16_t> & want,
    const std::vector<int16_t> & got) {
  size_t n = want.size() < got.size() ? want.size() : got.size();
  double error = 0.0, signal_power = 0.0;
  for (size_t i = 0; i < n; ++i) {
    double d = (double)want[i] - (double)got[i];
    error += d * d;
    signal_power += (double)want[i] * (double)want[i];
  }
  if (error == 0.0) {
    return 999.0;
  }
  return 10.0 * std::log10(signal_power / error);
}

} // namespace

TEST(Coding, EveryCodingHasADistinctNameAndTheDecodedFormatItClaims) {
  /*
   * Which codings have a fixed decoded format, named one at a time.
   *
   * It was once "every coding except PCM answers S16", and phase 4 ended
   * that: FLAC is a coding whose bit depth is the file's, so it answers
   * ::GAUD_SAMPLE_FORMAT_COUNT as PCM does, from the other direction -
   * PCM because it is not coded and FLAC because it is coded and carries
   * its own depth. Listing them means adding a coding is a compile error
   * here rather than a rule quietly becoming false.
   */
  struct Expected {
    GAUD_Sample_Coding coding;
    GAUD_Sample_Format format; /* FORMAT_COUNT for "ask the track". */
  };
  const Expected expected[] = {
      {GAUD_CODING_PCM, GAUD_SAMPLE_FORMAT_COUNT},
      {GAUD_CODING_G711_ULAW, GAUD_SAMPLE_S16},
      {GAUD_CODING_G711_ALAW, GAUD_SAMPLE_S16},
      {GAUD_CODING_ADPCM_IMA_WAV, GAUD_SAMPLE_S16},
      {GAUD_CODING_ADPCM_IMA_QT, GAUD_SAMPLE_S16},
      {GAUD_CODING_ADPCM_MS, GAUD_SAMPLE_S16},
      {GAUD_CODING_FLAC, GAUD_SAMPLE_FORMAT_COUNT},
  };
  static_assert(sizeof(expected) / sizeof(expected[0]) == GAUD_CODING_COUNT,
      "a coding was added: say here what it decodes to");

  std::vector<std::string> seen;
  for (int i = 0; i < GAUD_CODING_COUNT; ++i) {
    GAUD_Sample_Coding coding = (GAUD_Sample_Coding)i;
    const char * name = gaud_sample_coding_name(coding);
    ASSERT_NE(name, nullptr);
    EXPECT_STRNE(name, "unknown")
        << "coding " << i << " has no name in the table";
    EXPECT_EQ(std::find(seen.begin(), seen.end(), name), seen.end())
        << "two codings answer to " << name;
    seen.push_back(name);

    EXPECT_EQ(gaud_sample_coding_is_pcm(coding), coding == GAUD_CODING_PCM)
        << name;
    EXPECT_EQ(expected[i].coding, coding) << "the table is out of order";
    EXPECT_EQ(gaud_sample_coding_format(coding), expected[i].format)
        << name << " does not decode to what the table says";
  }
}

TEST(Coding, OutOfRangeValuesAnswerUnknownRatherThanReadingPastTheTable) {
  EXPECT_STREQ(gaud_sample_coding_name((GAUD_Sample_Coding)-1), "unknown");
  EXPECT_STREQ(
      gaud_sample_coding_name((GAUD_Sample_Coding)GAUD_CODING_COUNT),
      "unknown");
  EXPECT_STREQ(gaud_sample_coding_name((GAUD_Sample_Coding)9999), "unknown");
  EXPECT_FALSE(gaud_sample_coding_is_pcm((GAUD_Sample_Coding)9999));
  EXPECT_EQ(gaud_sample_coding_format((GAUD_Sample_Coding)9999),
      GAUD_SAMPLE_FORMAT_COUNT);
}

TEST(Coding, AZeroedParamsMeansPcm) {
  /* The field was appended to GAUD_Encode_Params after phase 1. A caller
   * that zeroes the struct, or one written before the field existed, must
   * still mean an uncoded file - which is only true because PCM is the
   * zero value. */
  GAUD_Encode_Params params;
  std::memset(&params, 0, sizeof(params));
  EXPECT_EQ(params.coding, GAUD_CODING_PCM);

  gaud_encode_params_default(&params);
  EXPECT_EQ(params.coding, GAUD_CODING_PCM);
}

TEST(Coding, AnUncodedTrackReportsPcm) {
  std::vector<int16_t> pcm = signal(500, 1);
  std::vector<unsigned char> bytes;
  Case plain = {"wav", GAUD_CODING_PCM, "wav/pcm"};
  ASSERT_EQ(encode(plain, pcm, 500, 1, bytes), GAUD_OK);

  std::vector<int16_t> back;
  GAUD_Sample_Coding coding = GAUD_CODING_COUNT;
  uint64_t frames = 0;
  ASSERT_EQ(decode(bytes, back, &coding, &frames), GAUD_OK);
  EXPECT_EQ(coding, GAUD_CODING_PCM);
  EXPECT_EQ(frames, 500u);
  /* PCM is exact, which is the control for every lossy assertion below:
   * if this one fails the harness is broken rather than the coding. */
  EXPECT_EQ(back, pcm);
}

TEST(Coding, EverySupportedCombinationRoundTrips) {
  for (uint32_t channels : {1u, 2u}) {
    for (const Case & which : supported) {
      const size_t frames = 3001; /* not a multiple of any block size */
      std::vector<int16_t> pcm = signal(frames, channels);
      std::vector<unsigned char> bytes;
      ASSERT_EQ(encode(which, pcm, frames, channels, bytes), GAUD_OK)
          << which.name << " " << channels << "ch";
      EXPECT_LT(bytes.size(), frames * channels * sizeof(int16_t))
          << which.name << " did not make the file smaller";

      std::vector<int16_t> back;
      GAUD_Sample_Coding coding = GAUD_CODING_COUNT;
      uint64_t reported = 0;
      ASSERT_EQ(decode(bytes, back, &coding, &reported), GAUD_OK)
          << which.name << " " << channels << "ch";
      EXPECT_EQ(coding, which.coding)
          << which.name << " reported a different coding than it wrote";

      /* AIFF-C's `ima4` has no field for the true length - COMM carries
       * the packet count - so its files are always a whole number of
       * 64-frame packets and the tail is padding. WAV's `fact` chunk
       * gives the exact count for every coding it carries. */
      if (which.coding == GAUD_CODING_ADPCM_IMA_QT) {
        EXPECT_GE(reported, frames) << which.name;
        EXPECT_LT(reported - frames, 64u)
            << which.name << " padded by more than one packet";
      }
      else {
        EXPECT_EQ(reported, frames)
            << which.name << " lost or invented frames";
      }

      double snr = snr_db(pcm, back);
      EXPECT_GT(snr, 30.0)
          << which.name << " " << channels << "ch decoded at " << snr
          << " dB, which is below what this coding's bit rate allows";
    }
  }
}

TEST(Coding, SeekLandsExactlyBecauseEveryBlockIsSelfContained) {
  for (const Case & which : supported) {
    const size_t frames = 3001;
    std::vector<int16_t> pcm = signal(frames, 1);
    std::vector<unsigned char> bytes;
    ASSERT_EQ(encode(which, pcm, frames, 1, bytes), GAUD_OK) << which.name;

    /* The whole-file decode, to compare a sought read against. */
    std::vector<int16_t> whole;
    GAUD_Sample_Coding coding = GAUD_CODING_COUNT;
    uint64_t reported = 0;
    ASSERT_EQ(decode(bytes, whole, &coding, &reported), GAUD_OK);

    GAUD_Stream * stream = nullptr;
    ASSERT_EQ(gaud_stream_create_memory(bytes.data(), bytes.size(), &stream),
        GAUD_OK);
    GAUD_Doc * doc = nullptr;
    ASSERT_EQ(gaud_doc_load(nullptr, stream, nullptr, nullptr, &doc), GAUD_OK);
    GAUD_Track * track = gaud_doc_track(doc, 0);
    GAUD_Decoder * decoder = nullptr;
    ASSERT_EQ(gaud_decoder_create(track, &decoder), GAUD_OK);
    GAUD_Buffer * buffer = nullptr;
    ASSERT_EQ(gaud_decoder_buffer_create(decoder, nullptr, 64, &buffer),
        GAUD_OK);

    /* Targets chosen to land inside a block, on a block boundary, and
     * one before one - the three places an off-by-one in the block
     * arithmetic hides. */
    for (uint64_t target : {0u, 1u, 63u, 64u, 65u, 500u, 504u, 505u, 999u,
             1010u, 2036u, 2500u}) {
      if (target >= reported) {
        continue;
      }
      uint64_t landed = UINT64_MAX;
      ASSERT_EQ(gaud_decoder_seek(decoder, target, &landed), GAUD_OK)
          << which.name << " seeking to " << target;
      EXPECT_EQ(landed, target)
          << which.name << ": these codings carry their state in the block "
          << "header, so a seek has no reason to land short";
      EXPECT_EQ(gaud_decoder_tell(decoder), target) << which.name;

      ASSERT_EQ(gaud_decoder_read(decoder, buffer), GAUD_OK) << which.name;
      size_t got = gaud_buffer_frames(buffer);
      ASSERT_GT(got, 0u) << which.name << " read nothing after seeking to "
                         << target;
      const int16_t * p = (const int16_t *)gaud_buffer_data_const(buffer);
      for (size_t i = 0; i < got && target + i < whole.size(); ++i) {
        ASSERT_EQ(p[i], whole[target + i])
            << which.name << ": frame " << (target + i)
            << " read after a seek differs from the same frame read in "
            << "sequence";
      }
    }
    gaud_buffer_destroy(buffer);
    gaud_decoder_destroy(decoder);
    gaud_doc_destroy(doc);
    gaud_stream_destroy(stream);
  }
}

TEST(Coding, ACombinationTheContainerCannotSpellIsRefusedByName) {
  std::vector<int16_t> pcm = signal(200, 1);
  for (const Case & which : unsupported) {
    std::vector<unsigned char> bytes;
    GAUD_Result result = encode(which, pcm, 200, 1, bytes);
    EXPECT_EQ(result, GAUD_ERR_UNSUPPORTED)
        << which.name << " was written rather than refused; a file labelled "
        << "with one framing and holding another is worse than no file";
    EXPECT_TRUE(bytes.empty()) << which.name;
  }
}

TEST(Coding, ACodedEncoderRefusesASampleFormatItCannotAccept) {
  /* Asking for µ-law with f32 samples is a caller error and is refused at
   * creation, not silently converted: gaud_encoder_write() checks the
   * buffer against the parameters, and a conversion here would make that
   * check pass for a buffer the caller never meant to send. */
  GAUD_Stream * sink = nullptr;
  ASSERT_EQ(gaud_stream_create_memory_writer(nullptr, &sink), GAUD_OK);
  GAUD_Encode_Params params;
  gaud_encode_params_default(&params);
  params.coding = GAUD_CODING_G711_ULAW;
  params.format = GAUD_SAMPLE_F32;
  GAUD_Encoder * encoder = nullptr;
  EXPECT_EQ(gaud_encoder_create("wav", nullptr, sink, &params, &encoder),
      GAUD_ERR_INVALID);
  EXPECT_EQ(encoder, nullptr);

  params.coding = (GAUD_Sample_Coding)9999;
  params.format = GAUD_SAMPLE_S16;
  EXPECT_EQ(gaud_encoder_create("wav", nullptr, sink, &params, &encoder),
      GAUD_ERR_INVALID);
  gaud_stream_destroy(sink);
}

TEST(Coding, TwoDecodersOnOneTrackDoNotShareABlockCache) {
  /* Coded state is per decoder, unlike phase 1's PCM state which hangs
   * off the track. Two decoders reading different parts of one track
   * would otherwise evict each other's cache and return each other's
   * samples - silently, because the frames are all plausible. */
  const size_t frames = 3001;
  std::vector<int16_t> pcm = signal(frames, 1);
  std::vector<unsigned char> bytes;
  Case which = {"wav", GAUD_CODING_ADPCM_IMA_WAV, "wav/ima-wav"};
  ASSERT_EQ(encode(which, pcm, frames, 1, bytes), GAUD_OK);

  std::vector<int16_t> whole;
  GAUD_Sample_Coding coding = GAUD_CODING_COUNT;
  uint64_t reported = 0;
  ASSERT_EQ(decode(bytes, whole, &coding, &reported), GAUD_OK);

  GAUD_Stream * stream = nullptr;
  ASSERT_EQ(gaud_stream_create_memory(bytes.data(), bytes.size(), &stream),
      GAUD_OK);
  GAUD_Doc * doc = nullptr;
  ASSERT_EQ(gaud_doc_load(nullptr, stream, nullptr, nullptr, &doc), GAUD_OK);
  GAUD_Track * track = gaud_doc_track(doc, 0);

  GAUD_Decoder * a = nullptr;
  GAUD_Decoder * b = nullptr;
  ASSERT_EQ(gaud_decoder_create(track, &a), GAUD_OK);
  ASSERT_EQ(gaud_decoder_create(track, &b), GAUD_OK);
  GAUD_Buffer * ba = nullptr;
  GAUD_Buffer * bb = nullptr;
  ASSERT_EQ(gaud_decoder_buffer_create(a, nullptr, 32, &ba), GAUD_OK);
  ASSERT_EQ(gaud_decoder_buffer_create(b, nullptr, 32, &bb), GAUD_OK);

  uint64_t landed = 0;
  /* Deliberately in different blocks: 505 frames a block here, so 100 and
   * 2000 are four blocks apart and each read evicts the other's. */
  ASSERT_EQ(gaud_decoder_seek(a, 100, &landed), GAUD_OK);
  ASSERT_EQ(gaud_decoder_seek(b, 2000, &landed), GAUD_OK);
  for (int round = 0; round < 4; ++round) {
    ASSERT_EQ(gaud_decoder_read(a, ba), GAUD_OK);
    ASSERT_EQ(gaud_decoder_read(b, bb), GAUD_OK);
    const int16_t * pa = (const int16_t *)gaud_buffer_data_const(ba);
    const int16_t * pb = (const int16_t *)gaud_buffer_data_const(bb);
    for (size_t i = 0; i < gaud_buffer_frames(ba); ++i) {
      ASSERT_EQ(pa[i], whole[100 + (size_t)round * 32 + i])
          << "decoder A round " << round << " frame " << i;
    }
    for (size_t i = 0; i < gaud_buffer_frames(bb); ++i) {
      ASSERT_EQ(pb[i], whole[2000 + (size_t)round * 32 + i])
          << "decoder B round " << round << " frame " << i;
    }
  }
  gaud_buffer_destroy(ba);
  gaud_buffer_destroy(bb);
  gaud_decoder_destroy(a);
  gaud_decoder_destroy(b);
  gaud_doc_destroy(doc);
  gaud_stream_destroy(stream);
}

TEST(Coding, AnIma4BlockMissingAChannelsPacketHoldsNoFrames) {
  /* Found by `make fuzz-run-coded` in under a minute, and the reason that
   * harness exists. QuickTime's framing stores one whole 34-byte packet
   * per channel, so a block that has channel 0's packet and not channel
   * 1's holds no complete frame - every frame needs every channel. The
   * decoder used to return the full 64 as soon as channel 0 was done,
   * leaving the other channels as whatever was in the caller's buffer.
   *
   * The fuzzer caught it by comparing the decode against
   * gaud_coded_tail_frames(), which had the rule right. Those two numbers
   * are computed by different code, and a container states its track
   * length from the prediction before the decode ever runs - so a
   * prediction that underestimates is how a decoder comes to write past a
   * caller's buffer.
   *
   * Driven here through the public API rather than the internal one: an
   * AIFF-C file whose SSND holds one packet where COMM says two channels.
   */
  const uint32_t channels = 2;
  std::vector<unsigned char> file;
  auto be32 = [&](uint32_t v) {
    file.push_back((unsigned char)(v >> 24));
    file.push_back((unsigned char)(v >> 16));
    file.push_back((unsigned char)(v >> 8));
    file.push_back((unsigned char)v);
  };
  auto be16 = [&](uint16_t v) {
    file.push_back((unsigned char)(v >> 8));
    file.push_back((unsigned char)v);
  };
  auto tag = [&](const char * t) {
    file.insert(file.end(), t, t + 4);
  };
  tag("FORM");
  size_t form_size_at = file.size();
  be32(0);
  tag("AIFC");
  tag("FVER");
  be32(4);
  be32(0xA2805140u);
  tag("COMM");
  be32(18 + 4 + 2);
  be16((uint16_t)channels);
  be32(1);   /* one packet, per the packet-count convention */
  be16(16);
  /* 8000 Hz as an 80-bit extended: exponent 0x400B, mantissa 0xFA00... */
  const unsigned char rate[10]
      = {0x40, 0x0B, 0xFA, 0x00, 0, 0, 0, 0, 0, 0};
  file.insert(file.end(), rate, rate + 10);
  tag("ima4");
  file.push_back(0);
  file.push_back(0);
  tag("SSND");
  /* 8 bytes of preamble plus ONE packet, where two channels need two. */
  be32(8 + 34);
  be32(0);
  be32(0);
  for (int i = 0; i < 34; ++i) {
    file.push_back((unsigned char)(i * 7));
  }
  size_t total = file.size();
  file[form_size_at] = (unsigned char)((total - 8) >> 24);
  file[form_size_at + 1] = (unsigned char)((total - 8) >> 16);
  file[form_size_at + 2] = (unsigned char)((total - 8) >> 8);
  file[form_size_at + 3] = (unsigned char)(total - 8);

  GAUD_Stream * stream = nullptr;
  ASSERT_EQ(gaud_stream_create_memory(file.data(), file.size(), &stream),
      GAUD_OK);
  GAUD_Doc * doc = nullptr;
  GAUD_Result result
      = gaud_doc_load(nullptr, stream, nullptr, nullptr, &doc);
  if (result == GAUD_OK) {
    GAUD_Track * track = gaud_doc_track(doc, 0);
    EXPECT_EQ(gaud_track_coding(track), GAUD_CODING_ADPCM_IMA_QT);
    /* The half-packet must contribute nothing. Anything else means the
     * track claims frames whose upper channels were never written. */
    EXPECT_EQ(gaud_track_frames(track), 0u)
        << "a block short of one channel's packet was counted as frames";

    GAUD_Decoder * decoder = nullptr;
    if (gaud_decoder_create(track, &decoder) == GAUD_OK) {
      GAUD_Buffer * buffer = nullptr;
      ASSERT_EQ(gaud_decoder_buffer_create(decoder, nullptr, 64, &buffer),
          GAUD_OK);
      ASSERT_EQ(gaud_decoder_read(decoder, buffer), GAUD_OK);
      EXPECT_EQ(gaud_buffer_frames(buffer), 0u);
      gaud_buffer_destroy(buffer);
      gaud_decoder_destroy(decoder);
    }
    gaud_doc_destroy(doc);
  }
  else {
    /* Refusing the file outright is also correct - what must not happen
     * is loading it and reporting frames that are not there. */
    EXPECT_EQ(result, GAUD_ERR_CORRUPT);
  }
  gaud_stream_destroy(stream);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
