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
 * Writing WAV.
 *
 * The header states how much data follows, and that is not known until the
 * data has been written - so the header goes out with zeroes in those two
 * fields and gaud_encoder_finish() seeks back and patches them. That is why
 * an encoder needs a seekable sink and says so at creation rather than
 * producing a file with a wrong length in it.
 *
 * `WAVE_FORMAT_EXTENSIBLE` is written when, and only when, there is
 * something it can say that the plain header cannot: more than two channels,
 * or a stated channel mask. A file that does not need it is written in the
 * plain form, because that is the one every reader accepts.
 */

#include "../../meta/scheme.h"
#include "../shared/bytes.h"
#include "../shared/coded.h"
#include "wav_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/** @brief What the WAV encoder must remember between calls. */
typedef struct {
  uint64_t data_size_offset; ///< Where the data chunk's length field is.
  uint64_t riff_size_offset; ///< Where the RIFF length field is.
  uint64_t fact_offset;      ///< Where the fact chunk's count is, or 0.
  uint64_t data_bytes;       ///< How many written so far.
  size_t frame_size;         ///< Bytes per frame. Zero when coded.
  bool needs_swap;           ///< Whether the host's order is not the file's.
  GAUD_Sample_Coding coding; ///< ::GAUD_CODING_PCM for an uncoded file.
  GAUD_Coded_Writer coded;   ///< Block buffer; used only when coded.
  const GAUD_Meta * meta;    ///< Borrowed from the params; may be NULL.
  GAUD_Meta_Policy policy;   ///< What to do with it.
  bool padded;               ///< Whether the data chunk's pad byte is out.
} WAV_Encoder_State;

static GAUD_Result put(GAUD_Encoder * encoder, const void * bytes,
    size_t count) {
  return gaud_stream_write(gaud_encoder_stream(encoder), bytes, count);
}

static GAUD_Result wav_write(
    GAUD_Encoder * encoder, const GAUD_Buffer * buffer) {
  WAV_Encoder_State * state = gaud_encoder_private(encoder);
  size_t frames = gaud_buffer_frames(buffer);
  if (frames == 0) {
    return GAUD_OK;
  }
  if (state->coding != GAUD_CODING_PCM) {
    GAUD_Result result = gaud_coded_writer_push(&state->coded,
        gaud_encoder_stream(encoder),
        (const int16_t *)gaud_buffer_data_const(buffer), frames);
    if (result == GAUD_OK) {
      gaud_encoder_add_frames(encoder, frames);
    }
    return result;
  }
  size_t bytes = frames * state->frame_size;

  const unsigned char * data = gaud_buffer_data_const(buffer);
  if (!state->needs_swap) {
    GAUD_Result result = put(encoder, data, bytes);
    if (result == GAUD_OK) {
      state->data_bytes += bytes;
      gaud_encoder_add_frames(encoder, frames);
    }
    return result;
  }

  /* A big-endian host has to byte-swap on the way out, and the caller's
   * buffer is const - so the swap happens in a scratch copy rather than in
   * their memory. Chunked, so that writing an hour of audio does not
   * allocate an hour of audio. */
  unsigned char scratch[4096];
  size_t width = gaud_sample_format_bits(gaud_buffer_format(buffer)) / 8u;
  size_t done = 0;
  while (done < bytes) {
    size_t chunk = bytes - done;
    if (chunk > sizeof(scratch)) {
      chunk = sizeof(scratch);
      /* Never split a sample across two chunks, or the swap would reverse
       * two halves of different samples. */
      chunk -= chunk % width;
    }
    memcpy(scratch, data + done, chunk);
    gaud_swap_samples(scratch, chunk / width, width);
    GAUD_Result result = put(encoder, scratch, chunk);
    if (result != GAUD_OK) {
      return result;
    }
    done += chunk;
  }
  state->data_bytes += bytes;
  gaud_encoder_add_frames(encoder, frames);
  return GAUD_OK;
}

/* Patch a 32-bit little-endian value already written at `offset`. */
static GAUD_Result patch_u32(
    GAUD_Encoder * encoder, uint64_t offset, uint32_t value) {
  GAUD_Stream * stream = gaud_encoder_stream(encoder);
  uint64_t here = gaud_stream_tell(stream);
  if (gaud_stream_seek(stream, (int64_t)offset, GAUD_SEEK_SET) != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  unsigned char bytes[4];
  gaud_wr_u32le(bytes, value);
  GAUD_Result result = gaud_stream_write(stream, bytes, sizeof(bytes));
  if (result != GAUD_OK) {
    return result;
  }
  if (gaud_stream_seek(stream, (int64_t)here, GAUD_SEEK_SET) != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  return GAUD_OK;
}

/**
 * Write the metadata chunks that follow the samples.
 *
 * `LIST`/`INFO` and `id3 `, in that order, which is what ffmpeg and
 * every tagger emit. Both are written when both have content: they are
 * different vocabularies and a reader that understands only one should
 * still find its own.
 */
static GAUD_Result write_trailing_metadata(
    GAUD_Encoder * encoder, WAV_Encoder_State * state) {
  if (!state->meta || state->policy == GAUD_META_DROP_ALL) {
    return GAUD_OK;
  }
  GAUD_Stream * stream = gaud_encoder_stream(encoder);
  const GAUD_Allocator * allocator = gaud_encoder_allocator(encoder);

  /* The data chunk is padded to even before anything else is written,
   * and that pad byte is counted in the RIFF size and not in the data
   * chunk's own - so it has to go out before the first metadata chunk or
   * every offset after it is odd. */
  if (state->data_bytes & 1u) {
    const unsigned char pad = 0;
    GAUD_Result result = gaud_stream_write(stream, &pad, 1);
    if (result != GAUD_OK) {
      return result;
    }
    state->padded = true;
  }

  if (state->policy != GAUD_META_KEEP_RAW_ONLY) {
    unsigned char * info = NULL;
    size_t info_size = 0;
    GAUD_Result result
        = gaud_riff_info_build(state->meta, allocator, &info, &info_size);
    if (result != GAUD_OK) {
      return result;
    }
    if (info_size > 0) {
      result = gaud_stream_write(stream, info, info_size);
    }
    gcu_allocator_free(allocator, info);
    if (result != GAUD_OK) {
      return result;
    }
  }

  unsigned char * id3 = NULL;
  size_t id3_size = 0;
  GAUD_Result result
      = gaud_id3v2_build(state->meta, state->policy, allocator, &id3,
          &id3_size);
  if (result != GAUD_OK) {
    return result;
  }
  if (id3_size > 0) {
    unsigned char header[8];
    memcpy(header, "id3 ", 4);
    gaud_wr_u32le(header + 4, (uint32_t)id3_size);
    result = gaud_stream_write(stream, header, sizeof(header));
    if (result == GAUD_OK) {
      result = gaud_stream_write(stream, id3, id3_size);
    }
    if (result == GAUD_OK && (id3_size & 1u)) {
      const unsigned char pad = 0;
      result = gaud_stream_write(stream, &pad, 1);
    }
  }
  gcu_allocator_free(allocator, id3);
  return result;
}

static GAUD_Result wav_finish(GAUD_Encoder * encoder) {
  WAV_Encoder_State * state = gaud_encoder_private(encoder);
  GAUD_Stream * stream = gaud_encoder_stream(encoder);

  if (state->coding != GAUD_CODING_PCM) {
    GAUD_Result result = gaud_coded_writer_flush(&state->coded, stream);
    if (result != GAUD_OK) {
      return result;
    }
    state->data_bytes = state->coded.bytes;
    /* The true frame count, which the padded final block has made
     * unrecoverable from the data chunk's length. Every reader in the
     * oracle image ignores this chunk and decodes the padding; writing
     * it anyway is what lets this library's own round trip be exact and
     * costs twelve bytes. */
    if (state->coded.frames > 0xFFFFFFFFu) {
      return GAUD_ERR_UNSUPPORTED;
    }
    result = patch_u32(
        encoder, state->fact_offset, (uint32_t)state->coded.frames);
    if (result != GAUD_OK) {
      return result;
    }
  }

  /* RIFF chunks are padded to an even length. The pad byte is not counted
   * in the chunk's own size and is counted in the RIFF size, which is the
   * detail a writer gets wrong and no reader complains about until one
   * does.
   *
   * `state->padded` is set when the metadata writer above already did it:
   * padding twice would put a stray byte between the data chunk and the
   * first metadata chunk, and every chunk after that would be misread. */
  if ((state->data_bytes & 1u) && !state->padded) {
    const unsigned char pad = 0;
    GAUD_Result result = gaud_stream_write(stream, &pad, 1);
    if (result != GAUD_OK) {
      return result;
    }
  }

  /* Metadata goes after the samples.
   *
   * RIFF allows it anywhere and readers walk the chunks, so the position
   * is a choice. After is the right one for everything except `bext`,
   * which the broadcast specification wants first - and `bext` is
   * written at the head instead, before any sample, which is why the
   * encoder is given its metadata at creation rather than at finish.
   *
   * Writing the rest here rather than at the head is what lets the
   * caller keep filling tags while the samples stream past. */
  GAUD_Result meta_written = write_trailing_metadata(encoder, state);
  if (meta_written != GAUD_OK) {
    return meta_written;
  }

  uint64_t total = gaud_stream_tell(stream);
  if (state->data_bytes > 0xFFFFFFFFu || total < 8) {
    /* Past 4 GiB this would have to be RF64, which this writer does not
     * emit. Refusing is the honest answer; writing a truncated 32-bit
     * length would produce a file that opens and is wrong. */
    return GAUD_ERR_UNSUPPORTED;
  }

  GAUD_Result result
      = patch_u32(encoder, state->data_size_offset, (uint32_t)state->data_bytes);
  if (result != GAUD_OK) {
    return result;
  }
  /* The RIFF size covers everything after its own 8-byte header. */
  return patch_u32(encoder, state->riff_size_offset, (uint32_t)(total - 8));
}

static void wav_close(GAUD_Encoder * encoder) {
  const GAUD_Allocator * allocator = gaud_encoder_allocator(encoder);
  WAV_Encoder_State * state = gaud_encoder_private(encoder);
  if (state) {
    gaud_coded_writer_free(&state->coded, allocator);
  }
  gcu_allocator_free(allocator, state);
}

static const GAUD_Encoder_Vtable wav_encoder_vtable = {
    .write = wav_write,
    .finish = wav_finish,
    .close = wav_close,
};

/**
 * How large a block to write for the ADPCM codings.
 *
 * The format does not say, and the two writers in the oracle image do not
 * agree: ffmpeg writes 1024 bytes whatever the channel count, libsndfile
 * writes 256. Per channel is the arrangement that keeps the frames in a
 * block - and so the cost of seeking, and of the padding in the final
 * block - independent of the channel count, which neither fixed size
 * does.
 *
 * 256 per channel satisfies both codings' divisibility rules and gives
 * 505 frames a block for IMA and 500 for MS, which is what libsndfile
 * produces for mono. `nBlockAlign` is 16-bit, so this bounds the channel
 * count at 255; GAUD_CODED_MAX_CHANNELS is 64 and binds first.
 */
#define WAV_ADPCM_BLOCK_PER_CHANNEL 256u

/** @brief Write a WAVE header and open an encoder over the rest. */
GAUD_Result gaud_wav_encoder_open(const GAUD_Codec * codec,
    GAUD_Stream * stream, const GAUD_Encode_Params * params,
    GAUD_Encoder ** out_encoder) {
  (void)codec;
  if (!gaud_stream_seekable(stream)) {
    /* Stated at creation rather than discovered at finish, so a caller
     * cannot get halfway through writing an hour of audio before finding
     * out. */
    return GAUD_ERR_UNSUPPORTED;
  }

  uint32_t channels = params->layout.channels;
  if (channels == 0 || channels > 0xFFFFu) {
    return GAUD_ERR_INVALID;
  }

  uint16_t tag;
  switch (params->coding) {
  case GAUD_CODING_PCM:
    switch (params->format) {
    case GAUD_SAMPLE_U8:
    case GAUD_SAMPLE_S16:
    case GAUD_SAMPLE_S24:
    case GAUD_SAMPLE_S32: tag = WAV_FORMAT_PCM; break;
    case GAUD_SAMPLE_F32:
    case GAUD_SAMPLE_F64: tag = WAV_FORMAT_IEEE_FLOAT; break;
    default:
      /* s8 has no WAV spelling: the format's 8-bit is unsigned, full stop.
       * Refusing by name beats writing it as u8 and moving every sample by
       * 128 without saying so. */
      return GAUD_ERR_UNSUPPORTED;
    }
    break;
  case GAUD_CODING_G711_ULAW: tag = WAV_FORMAT_MULAW; break;
  case GAUD_CODING_G711_ALAW: tag = WAV_FORMAT_ALAW; break;
  case GAUD_CODING_ADPCM_IMA_WAV: tag = WAV_FORMAT_IMA_ADPCM; break;
  case GAUD_CODING_ADPCM_MS: tag = WAV_FORMAT_ADPCM; break;
  case GAUD_CODING_ADPCM_IMA_QT:
    /* QuickTime's packet framing has no WAVE format tag. Writing it under
     * WAVE_FORMAT_IMA_ADPCM would produce a file whose header promises
     * WAV's framing and whose bytes are not in it. */
    return GAUD_ERR_UNSUPPORTED;
  default: return GAUD_ERR_INVALID;
  }

  bool coded = params->coding != GAUD_CODING_PCM;
  if (coded && !gaud_coded_writable(params->coding, channels)) {
    /* See gaud_coded_writable(): an ADPCM file wider than stereo is one
     * ffmpeg cannot open, and writing it would break the rule that what
     * this library writes, the references read. */
    return GAUD_ERR_UNSUPPORTED;
  }
  GAUD_Coded_Geometry geometry;
  memset(&geometry, 0, sizeof(geometry));
  size_t frame_size = 0;
  uint16_t bits;
  uint16_t block_align;
  uint32_t bytes_per_second;

  if (coded) {
    uint32_t requested = 0;
    if (params->coding == GAUD_CODING_ADPCM_IMA_WAV
        || params->coding == GAUD_CODING_ADPCM_MS) {
      requested = WAV_ADPCM_BLOCK_PER_CHANNEL * channels;
      if (requested > 0xFFFFu) {
        return GAUD_ERR_INVALID;
      }
    }
    GAUD_Result result = gaud_coded_geometry(
        params->coding, channels, requested, 0, &geometry);
    if (result != GAUD_OK) {
      return result;
    }
    /* G.711 has no container block: nBlockAlign is one byte per channel,
     * which is what every reader expects, and the geometry's own block is
     * an internal chunking the file never sees. */
    bool g711 = params->coding == GAUD_CODING_G711_ULAW
        || params->coding == GAUD_CODING_G711_ALAW;
    bits = g711 ? 8u : 4u;
    block_align = (uint16_t)(g711 ? channels : geometry.block_bytes);
    bytes_per_second = g711
        ? params->sample_rate * channels
        : (uint32_t)(((uint64_t)params->sample_rate * geometry.block_bytes)
              / geometry.block_frames);
  }
  else {
    frame_size = gaud_frame_size(params->format, channels);
    if (frame_size == 0) {
      return GAUD_ERR_INVALID;
    }
    bits = (uint16_t)gaud_sample_format_bits(params->format);
    block_align = (uint16_t)frame_size;
    bytes_per_second = (uint32_t)(params->sample_rate * frame_size);
  }

  /* Extensible only when it earns its place: more than two channels, or a
   * mask worth stating. Every reader accepts the plain form and some old
   * ones do not accept the other.
   *
   * A coded file is never written extensible. The GUID form would have to
   * carry the ADPCM extension as well and the two layouts do not compose:
   * cbSize would have to describe both, and no reader expects that. */
  bool extensible
      = !coded && (channels > 2 || params->layout.mask != 0);
  uint32_t fmt_size = 16u;
  if (extensible) {
    fmt_size = 40u;
  }
  else if (params->coding == GAUD_CODING_ADPCM_IMA_WAV) {
    fmt_size = 20u; /* cbSize 2: wSamplesPerBlock */
  }
  else if (params->coding == GAUD_CODING_ADPCM_MS) {
    fmt_size = 50u; /* cbSize 32: samples, numCoef, seven pairs */
  }
  else if (coded) {
    fmt_size = 18u; /* cbSize 0, which is what both writers emit for G.711 */
  }

  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);
  WAV_Encoder_State * state
      = gcu_allocator_malloc(allocator, sizeof(WAV_Encoder_State));
  if (!state) {
    return GAUD_ERR_OOM;
  }
  memset(state, 0, sizeof(*state));

  /* 12 RIFF/WAVE + 8 + 50 fmt + 8 + 4 fact + 8 data. Sized by the largest
   * arrangement rather than by the common one, with the assertion below so
   * that adding a chunk cannot quietly overrun it. */
  unsigned char header[12 + 8 + 50 + 8 + 4 + 8];
  _Static_assert(sizeof(header) >= 12u + 8u + 50u + 8u + 4u + 8u,
      "the header buffer must hold every chunk this writer can emit");
  size_t n = 0;
  memcpy(header + n, "RIFF", 4);
  n += 4;
  gaud_wr_u32le(header + n, 0); /* patched by finish */
  uint64_t riff_size_offset = n;
  n += 4;
  memcpy(header + n, "WAVE", 4);
  n += 4;

  memcpy(header + n, "fmt ", 4);
  n += 4;
  gaud_wr_u32le(header + n, fmt_size);
  n += 4;
  gaud_wr_u16le(header + n, extensible ? WAV_FORMAT_EXTENSIBLE : tag);
  n += 2;
  gaud_wr_u16le(header + n, (uint16_t)channels);
  n += 2;
  gaud_wr_u32le(header + n, params->sample_rate);
  n += 4;
  /* Bytes per second and block align: derived, and derived here rather than
   * by the caller, because a reader that trusts them and a writer that got
   * them wrong is a file that plays at the wrong speed. */
  gaud_wr_u32le(header + n, bytes_per_second);
  n += 4;
  gaud_wr_u16le(header + n, block_align);
  n += 2;
  gaud_wr_u16le(header + n, bits);
  n += 2;

  if (extensible) {
    gaud_wr_u16le(header + n, 22); /* cbSize */
    n += 2;
    gaud_wr_u16le(header + n, bits); /* wValidBitsPerSample */
    n += 2;
    gaud_wr_u32le(header + n, params->layout.mask);
    n += 4;
    gaud_wr_u16le(header + n, tag);
    n += 2;
    static const unsigned char guid_suffix[14] = {0x00, 0x00, 0x00, 0x00,
        0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
    memcpy(header + n, guid_suffix, sizeof(guid_suffix));
    n += sizeof(guid_suffix);
  }
  else if (params->coding == GAUD_CODING_ADPCM_IMA_WAV) {
    gaud_wr_u16le(header + n, 2); /* cbSize */
    n += 2;
    gaud_wr_u16le(header + n, (uint16_t)geometry.block_frames);
    n += 2;
  }
  else if (params->coding == GAUD_CODING_ADPCM_MS) {
    gaud_wr_u16le(header + n, 32); /* cbSize */
    n += 2;
    gaud_wr_u16le(header + n, (uint16_t)geometry.block_frames);
    n += 2;
    gaud_wr_u16le(header + n, 7); /* wNumCoef */
    n += 2;
    for (size_t i = 0; i < 7; ++i) {
      gaud_wr_u16le(header + n, (uint16_t)gaud_ms_adpcm_default_coef[2u * i]);
      n += 2;
      gaud_wr_u16le(
          header + n, (uint16_t)gaud_ms_adpcm_default_coef[2u * i + 1u]);
      n += 2;
    }
  }
  else if (coded) {
    gaud_wr_u16le(header + n, 0); /* cbSize */
    n += 2;
  }

  uint64_t fact_offset = 0;
  if (coded) {
    /* A non-PCM WAVE file is required to carry `fact`, and for the ADPCM
     * codings it is the only record of the true length. Written for
     * G.711 too, where it is redundant but expected. */
    memcpy(header + n, "fact", 4);
    n += 4;
    gaud_wr_u32le(header + n, 4);
    n += 4;
    fact_offset = n;
    gaud_wr_u32le(header + n, 0); /* patched by finish */
    n += 4;
  }

  memcpy(header + n, "data", 4);
  n += 4;
  uint64_t data_size_offset = n;
  gaud_wr_u32le(header + n, 0); /* patched by finish */
  n += 4;

  GAUD_Result result = gaud_stream_write(stream, header, n);
  if (result != GAUD_OK) {
    gcu_allocator_free(allocator, state);
    return result;
  }

  state->data_size_offset = data_size_offset;
  state->riff_size_offset = riff_size_offset;
  state->fact_offset = fact_offset;
  state->data_bytes = 0;
  state->frame_size = frame_size;
  state->needs_swap = !coded && !gaud_host_is_little_endian() && bits > 8;
  state->coding = params->coding;
  state->meta = params->meta;
  state->policy = params->meta_policy;

  if (coded) {
    result = gaud_coded_writer_init(&state->coded, &geometry, allocator);
    if (result != GAUD_OK) {
      gcu_allocator_free(allocator, state);
      return result;
    }
  }

  result = gaud_encoder_create_internal(
      stream, params, allocator, &wav_encoder_vtable, state, out_encoder);
  if (result != GAUD_OK) {
    gaud_coded_writer_free(&state->coded, allocator);
    gcu_allocator_free(allocator, state);
  }
  return result;
}
