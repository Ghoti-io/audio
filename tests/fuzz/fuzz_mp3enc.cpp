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
 * Fuzz the MP3 encoder: arbitrary samples, arbitrary settings.
 *
 * The first bytes of the input choose a sampling frequency, a channel
 * count, a rate policy, a bit rate, a quality and how the samples are cut
 * into writes; the rest is read as 16-bit samples, repeated to fill what the
 * settings ask for. Nothing about it is a plausible recording, which is the
 * point: the encoder's arithmetic is written for full-scale wide-band
 * noise, silence, DC and the highest frequency the format holds, and an
 * input a fuzzer makes is all of those and mixtures.
 *
 * What it asserts beyond not crashing, aborting or tripping a sanitizer:
 *
 * - **The encoder either refuses at creation or finishes.** A request it
 *   accepts is a request it completes; there is no input that is accepted
 *   and then fails halfway, which is what a granule that outgrew its
 *   twelve-bit length used to do.
 * - **What it writes this library reads.** The file decodes, to exactly as
 *   many frames as were given once the tag's delay and padding are taken
 *   off - so the tag, the reservoir and the frame count are right for
 *   whatever the samples were.
 * - **Cutting the samples differently does not change the file.** The
 *   second encode feeds the same samples in other sized writes, and the
 *   bytes must be identical. An encoder whose output depended on where the
 *   caller's buffers ended would give two answers to one question, and no
 *   gate that always writes in one size would see it.
 *
 * Build with: make fuzz-mp3enc
 * Run:        make fuzz-run-mp3enc FUZZ_TIME=300
 */

#include <ghoti.io/audio/audio.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size);

namespace {

struct Registered {
  Registered() { gaud_register_builtin_codecs(); }
};
const Registered registered;

const uint32_t kRates[9] = {44100, 48000, 32000, 22050, 24000, 16000, 11025, 12000, 8000};

/** Encode @p pcm in writes of @p chunk frames; false if it was refused. */
bool encode(const std::vector<int16_t> & pcm, GAUD_Encode_Params params,
    size_t chunk, std::vector<unsigned char> * out) {
  GAUD_Stream * stream = nullptr;
  if (gaud_stream_create_memory_writer(nullptr, &stream) != GAUD_OK) {
    abort();
  }
  GAUD_Encoder * encoder = nullptr;
  GAUD_Result created = gaud_encoder_create("mp3", nullptr, stream, &params, &encoder);
  if (created != GAUD_OK) {
    gaud_stream_destroy(stream);
    return false;
  }
  unsigned channels = params.layout.channels;
  GAUD_Buffer * buffer = nullptr;
  if (gaud_buffer_create(nullptr, GAUD_SAMPLE_S16, params.layout,
          GAUD_LAYOUT_INTERLEAVED, chunk, &buffer) != GAUD_OK) {
    abort();
  }
  size_t frames = pcm.size() / channels;
  for (size_t at = 0; at < frames; at += chunk) {
    size_t n = std::min(chunk, frames - at);
    gaud_buffer_set_frames(buffer, n);
    std::memcpy(gaud_buffer_data(buffer), pcm.data() + at * channels, n * channels * 2u);
    if (gaud_encoder_write(encoder, buffer) != GAUD_OK) {
      abort(); /* an accepted request must complete */
    }
  }
  if (gaud_encoder_finish(encoder) != GAUD_OK) {
    abort();
  }
  const void * bytes = nullptr;
  size_t length = 0;
  gaud_stream_writer_bytes(stream, &bytes, &length);
  out->assign((const unsigned char *)bytes, (const unsigned char *)bytes + length);
  gaud_buffer_destroy(buffer);
  gaud_encoder_destroy(encoder);
  gaud_stream_destroy(stream);
  return true;
}

/** Frames in the file after the tag's trim, by decoding it. */
size_t decoded_frames(const std::vector<unsigned char> & bytes, unsigned channels) {
  GAUD_Stream * stream = nullptr;
  if (gaud_stream_create_memory(bytes.data(), bytes.size(), &stream) != GAUD_OK) {
    abort();
  }
  GAUD_Doc * doc = nullptr;
  if (gaud_doc_load(nullptr, stream, nullptr, nullptr, &doc) != GAUD_OK) {
    abort(); /* a file this library wrote must open */
  }
  GAUD_Track * track = gaud_doc_track(doc, 0);
  GAUD_Decoder * decoder = nullptr;
  if (gaud_decoder_create(track, &decoder) != GAUD_OK) {
    abort();
  }
  GAUD_Buffer * buffer = nullptr;
  gaud_decoder_buffer_create(decoder, nullptr, 4096, &buffer);
  size_t total = 0;
  for (;;) {
    if (gaud_decoder_read(decoder, buffer) != GAUD_OK) {
      abort();
    }
    size_t n = gaud_buffer_frames(buffer);
    if (n == 0) {
      break;
    }
    total += n;
  }
  GAUD_Trim trim = gaud_track_trim(track);
  (void)channels;
  gaud_buffer_destroy(buffer);
  gaud_decoder_destroy(decoder);
  gaud_doc_destroy(doc);
  gaud_stream_destroy(stream);
  size_t cut = (size_t)(trim.encoder_delay + trim.padding);
  return total >= cut ? total - cut : 0;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (size < 10) {
    return 0;
  }
  GAUD_Encode_Params params;
  gaud_encode_params_default(&params);
  params.sample_rate = kRates[data[0] % 9u];
  unsigned channels = 1u + (data[1] & 1u);
  params.layout = gaud_channel_layout_default(channels);
  params.coding = GAUD_CODING_MPEG_LAYER3;
  static const GAUD_Rate_Control modes[3] = {GAUD_RATE_CBR, GAUD_RATE_ABR, GAUD_RATE_VBR};
  params.rate_control = modes[data[2] % 3u];
  static const uint32_t kbps[15]
      = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 320};
  params.bitrate = kbps[data[3] % 15u] * 1000u;
  params.quality = data[4] % 102u; /* 101 is refused */
  params.min_bitrate = (data[5] & 1u) ? kbps[data[5] % 15u] * 1000u : 0u;
  size_t chunk_a = 1u + data[6] * 7u;
  size_t chunk_b = 1u + data[7] * 13u;
  size_t frames = 1u + ((size_t)data[8] << 4) + data[9];
  const uint8_t * body = data + 10;
  size_t body_size = size - 10;

  std::vector<int16_t> pcm(frames * channels);
  for (size_t i = 0; i < pcm.size(); ++i) {
    size_t at = (i * 2u) % (body_size & ~(size_t)1u ? (body_size & ~(size_t)1u) : 2u);
    uint16_t lo = at < body_size ? body[at] : 0;
    uint16_t hi = at + 1 < body_size ? body[at + 1] : 0;
    pcm[i] = (int16_t)(lo | (hi << 8));
  }

  std::vector<unsigned char> a;
  if (!encode(pcm, params, chunk_a, &a)) {
    return 0;
  }
  std::vector<unsigned char> b;
  if (!encode(pcm, params, chunk_b, &b) || a != b) {
    abort(); /* the file must not depend on how the samples were cut */
  }
  if (decoded_frames(a, channels) != frames) {
    abort();
  }
  return 0;
}
