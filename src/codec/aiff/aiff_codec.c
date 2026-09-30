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
 * Decoding and encoding AIFF.
 *
 * The decode is WAV's with the byte order the other way up, which is the
 * point of having both: the swap is exercised on a little-endian host by
 * AIFF and on a big-endian one by WAV, so neither path is the one that only
 * runs somewhere nobody tests.
 */

#include "../../meta/scheme.h"
#include "../shared/bytes.h"
#include "../shared/coded.h"
#include "aiff_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

static GAUD_Result aiff_read(GAUD_Decoder * decoder, GAUD_Buffer * buffer) {
  GAUD_Track * track = gaud_decoder_track(decoder);
  AIFF_Track_State * state = gaud_track_private(track);
  GAUD_Stream * stream = gaud_doc_stream(gaud_track_doc(track));

  uint64_t position = gaud_decoder_tell(decoder);
  uint64_t total = gaud_track_frames(track);
  if (position >= total) {
    return GAUD_OK;
  }
  size_t want = gaud_buffer_capacity(buffer);
  uint64_t remaining = total - position;
  if ((uint64_t)want > remaining) {
    want = (size_t)remaining;
  }

  uint64_t at = state->data_offset + position * state->frame_size;
  if (gaud_stream_seek(stream, (int64_t)at, GAUD_SEEK_SET) != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  size_t got = gaud_stream_read(
      stream, gaud_buffer_data(buffer), want * state->frame_size);
  size_t frames = got / state->frame_size;

  if (state->needs_swap) {
    size_t width = gaud_sample_format_bits(gaud_buffer_format(buffer)) / 8u;
    gaud_swap_samples(gaud_buffer_data(buffer),
        frames * gaud_buffer_channels(buffer), width);
  }

  GAUD_Result result = gaud_buffer_set_frames(buffer, frames);
  if (result != GAUD_OK) {
    return result;
  }
  gaud_decoder_set_position(decoder, position + frames);
  return GAUD_OK;
}

static GAUD_Result aiff_seek(
    GAUD_Decoder * decoder, uint64_t frame, uint64_t * out_landed) {
  GAUD_Track * track = gaud_decoder_track(decoder);
  if (frame > gaud_track_frames(track)) {
    return GAUD_ERR_INVALID;
  }
  if (!gaud_stream_seekable(gaud_doc_stream(gaud_track_doc(track)))) {
    return GAUD_ERR_UNSUPPORTED;
  }
  gaud_decoder_set_position(decoder, frame);
  *out_landed = frame;
  return GAUD_OK;
}

static const GAUD_Decoder_Vtable aiff_decoder_vtable = {
    .read = aiff_read,
    .seek = aiff_seek,
    .close = NULL,
};

/** @brief Open a decoder over an AIFF track's sound chunk. */
GAUD_Result gaud_aiff_decoder_open(const GAUD_Codec * codec,
    GAUD_Track * track, GAUD_Decoder ** out_decoder) {
  (void)codec;
  AIFF_Track_State * state = gaud_track_private(track);
  if (!state) {
    return GAUD_ERR_INTERNAL;
  }
  if (state->coding != GAUD_CODING_PCM) {
    return gaud_coded_decoder_open(track, &state->geometry,
        state->data_offset, state->data_length, out_decoder);
  }
  return gaud_decoder_create_internal(
      track, &aiff_decoder_vtable, state, out_decoder);
}

/* ------------------------------------------------------------------ write */

/** @brief What the AIFF encoder must remember between calls. */
typedef struct {
  uint64_t form_size_offset; ///< Where FORM's length field is.
  uint64_t comm_frames_offset; ///< Where COMM's frame count is.
  uint64_t ssnd_size_offset; ///< Where SSND's length field is.
  uint64_t data_bytes;       ///< Samples written.
  size_t frame_size;         ///< Bytes per frame. Zero when coded.
  bool needs_swap;           ///< Whether the host's order is not the file's.
  GAUD_Sample_Coding coding; ///< ::GAUD_CODING_PCM for an uncoded file.
  GAUD_Coded_Writer coded;   ///< Block buffer; used only when coded.
  const GAUD_Meta * meta;    ///< Borrowed from the params; may be NULL.
  GAUD_Meta_Policy policy;   ///< What to do with it.
  bool padded;               ///< Whether SSND's pad byte is already out.
} AIFF_Encoder_State;

static GAUD_Result aiff_write(
    GAUD_Encoder * encoder, const GAUD_Buffer * buffer) {
  AIFF_Encoder_State * state = gaud_encoder_private(encoder);
  GAUD_Stream * stream = gaud_encoder_stream(encoder);
  size_t frames = gaud_buffer_frames(buffer);
  if (frames == 0) {
    return GAUD_OK;
  }
  if (state->coding != GAUD_CODING_PCM) {
    GAUD_Result coded = gaud_coded_writer_push(&state->coded, stream,
        (const int16_t *)gaud_buffer_data_const(buffer), frames);
    if (coded == GAUD_OK) {
      gaud_encoder_add_frames(encoder, frames);
    }
    return coded;
  }
  size_t bytes = frames * state->frame_size;
  const unsigned char * data = gaud_buffer_data_const(buffer);

  GAUD_Result result;
  if (!state->needs_swap) {
    result = gaud_stream_write(stream, data, bytes);
  }
  else {
    unsigned char scratch[4096];
    size_t width = gaud_sample_format_bits(gaud_buffer_format(buffer)) / 8u;
    size_t done = 0;
    result = GAUD_OK;
    while (done < bytes && result == GAUD_OK) {
      size_t chunk = bytes - done;
      if (chunk > sizeof(scratch)) {
        chunk = sizeof(scratch);
        chunk -= chunk % width; /* never split a sample */
      }
      memcpy(scratch, data + done, chunk);
      gaud_swap_samples(scratch, chunk / width, width);
      result = gaud_stream_write(stream, scratch, chunk);
      done += chunk;
    }
  }
  if (result == GAUD_OK) {
    state->data_bytes += bytes;
    gaud_encoder_add_frames(encoder, frames);
  }
  return result;
}

static GAUD_Result patch_u32be(
    GAUD_Encoder * encoder, uint64_t offset, uint32_t value) {
  GAUD_Stream * stream = gaud_encoder_stream(encoder);
  uint64_t here = gaud_stream_tell(stream);
  if (gaud_stream_seek(stream, (int64_t)offset, GAUD_SEEK_SET) != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  unsigned char bytes[4];
  gaud_wr_u32be(bytes, value);
  GAUD_Result result = gaud_stream_write(stream, bytes, sizeof(bytes));
  if (result != GAUD_OK) {
    return result;
  }
  return gaud_stream_seek(stream, (int64_t)here, GAUD_SEEK_SET) == GAUD_OK
      ? GAUD_OK
      : GAUD_ERR_IO;
}

/**
 * The metadata chunks that follow SSND.
 *
 * AIFF's own text chunks carry four of the twenty tags, so an `ID3 `
 * chunk goes out as well and carries the rest. Both: a reader that knows
 * only AIFF's four still finds a title, and one that reads ID3 finds
 * everything. Writing only the second would make this library's files
 * look untagged to anything that predates the convention.
 */
static GAUD_Result write_trailing_metadata(
    GAUD_Encoder * encoder, AIFF_Encoder_State * state) {
  if (!state->meta || state->policy == GAUD_META_DROP_ALL) {
    return GAUD_OK;
  }
  GAUD_Stream * stream = gaud_encoder_stream(encoder);
  const GAUD_Allocator * allocator = gaud_encoder_allocator(encoder);

  if (state->data_bytes & 1u) {
    const unsigned char pad = 0;
    GAUD_Result result = gaud_stream_write(stream, &pad, 1);
    if (result != GAUD_OK) {
      return result;
    }
    state->padded = true;
  }

  if (state->policy != GAUD_META_KEEP_RAW_ONLY) {
    static const struct {
      const char id[5];
      GAUD_Tag tag;
    } text_chunks[] = {
        {"NAME", GAUD_TAG_TITLE},
        {"AUTH", GAUD_TAG_ARTIST},
        {"(c) ", GAUD_TAG_COPYRIGHT},
        {"ANNO", GAUD_TAG_COMMENT},
    };
    for (size_t i = 0; i < sizeof(text_chunks) / sizeof(text_chunks[0]);
        ++i) {
      const char * value = gaud_meta_get(state->meta, text_chunks[i].tag, 0);
      if (!value || value[0] == '\0') {
        continue;
      }
      /* The length is the string's, with no terminator: AIFF text
       * chunks are counted, not terminated, and a writer that adds a
       * NUL puts it inside every reader's string. */
      size_t length = strlen(value);
      if (length > 0xFFFFFFFFu) {
        return GAUD_ERR_UNSUPPORTED;
      }
      unsigned char header[8];
      memcpy(header, text_chunks[i].id, 4);
      gaud_wr_u32be(header + 4, (uint32_t)length);
      GAUD_Result result = gaud_stream_write(stream, header, sizeof(header));
      if (result == GAUD_OK) {
        result = gaud_stream_write(stream, value, length);
      }
      if (result == GAUD_OK && (length & 1u)) {
        const unsigned char pad = 0;
        result = gaud_stream_write(stream, &pad, 1);
      }
      if (result != GAUD_OK) {
        return result;
      }
    }
  }

  unsigned char * id3 = NULL;
  size_t id3_size = 0;
  GAUD_Result result = gaud_id3v2_build(
      state->meta, state->policy, allocator, &id3, &id3_size);
  if (result != GAUD_OK) {
    return result;
  }
  if (id3_size > 0) {
    unsigned char header[8];
    /* Upper case here and lower case in RIFF, which is what each
     * format's convention is and what each format's readers look for. */
    memcpy(header, "ID3 ", 4);
    gaud_wr_u32be(header + 4, (uint32_t)id3_size);
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

static GAUD_Result aiff_finish(GAUD_Encoder * encoder) {
  AIFF_Encoder_State * state = gaud_encoder_private(encoder);
  GAUD_Stream * stream = gaud_encoder_stream(encoder);

  uint64_t coded_frames = 0;
  if (state->coding != GAUD_CODING_PCM) {
    GAUD_Result result = gaud_coded_writer_flush(&state->coded, stream);
    if (result != GAUD_OK) {
      return result;
    }
    state->data_bytes = state->coded.bytes;
    coded_frames = state->coded.frames;
  }

  GAUD_Result meta_written = write_trailing_metadata(encoder, state);
  if (meta_written != GAUD_OK) {
    return meta_written;
  }

  if ((state->data_bytes & 1u) && !state->padded) {
    const unsigned char pad = 0;
    GAUD_Result result = gaud_stream_write(stream, &pad, 1);
    if (result != GAUD_OK) {
      return result;
    }
  }
  uint64_t total = gaud_stream_tell(stream);
  if (total > 0xFFFFFFFFu || state->data_bytes > 0xFFFFFFF0u) {
    return GAUD_ERR_UNSUPPORTED;
  }

  uint64_t frames;
  if (state->coding == GAUD_CODING_ADPCM_IMA_QT) {
    /*
     * COMM gets the PACKET count, not the frame count.
     *
     * AIFF-C's specification says this field is the uncompressed
     * sample-frame count, which for `ima4` would be 64 times this. Both
     * writers in the oracle image write the packet count, neither
     * decoder reads the field at all, and ffmpeg's *reported duration*
     * is COMM x 64 - so writing the spec-literal value makes `ffprobe`
     * say the file is 64 times longer than it is. Matching two
     * independent implementations, and not misreporting the duration to
     * the most widely deployed reader, beats matching the prose.
     */
    frames = state->coded.bytes
        / (34u * (uint64_t)state->coded.geometry.channels);
  }
  else if (state->coding != GAUD_CODING_PCM) {
    frames = coded_frames;
  }
  else {
    frames = state->frame_size ? state->data_bytes / state->frame_size : 0;
  }
  GAUD_Result result = patch_u32be(
      encoder, state->comm_frames_offset, (uint32_t)frames);
  if (result != GAUD_OK) {
    return result;
  }
  /* SSND's size covers its 8-byte preamble as well as the samples. */
  result = patch_u32be(
      encoder, state->ssnd_size_offset, (uint32_t)(state->data_bytes + 8));
  if (result != GAUD_OK) {
    return result;
  }
  return patch_u32be(encoder, state->form_size_offset, (uint32_t)(total - 8));
}

static void aiff_close(GAUD_Encoder * encoder) {
  const GAUD_Allocator * allocator = gaud_encoder_allocator(encoder);
  AIFF_Encoder_State * state = gaud_encoder_private(encoder);
  if (state) {
    gaud_coded_writer_free(&state->coded, allocator);
  }
  gcu_allocator_free(allocator, state);
}

static const GAUD_Encoder_Vtable aiff_encoder_vtable = {
    .write = aiff_write,
    .finish = aiff_finish,
    .close = aiff_close,
};

/** @brief Write an AIFF or AIFF-C header and open an encoder. */
GAUD_Result gaud_aiff_encoder_open(const GAUD_Codec * codec,
    GAUD_Stream * stream, const GAUD_Encode_Params * params,
    GAUD_Encoder ** out_encoder) {
  (void)codec;
  if (!gaud_stream_seekable(stream)) {
    return GAUD_ERR_UNSUPPORTED;
  }

  /* Float samples need AIFF-C, because plain AIFF has no way to say a
   * sample is not an integer, and so does every coding. Integers are
   * written as plain AIFF, which more readers accept. */
  bool is_float = params->coding == GAUD_CODING_PCM
      && gaud_sample_format_is_float(params->format);
  const char * compression = NULL;
  switch (params->coding) {
  case GAUD_CODING_PCM:
    switch (params->format) {
    case GAUD_SAMPLE_S8:
    case GAUD_SAMPLE_S16:
    case GAUD_SAMPLE_S24:
    case GAUD_SAMPLE_S32:
    case GAUD_SAMPLE_F32:
    case GAUD_SAMPLE_F64: break;
    default:
      /* u8 has no AIFF spelling: this format's 8-bit is signed. Refusing
       * rather than shifting every sample by 128 unannounced. */
      return GAUD_ERR_UNSUPPORTED;
    }
    break;
  /* Apple's lower-case spellings. SGI's `ULAW` and `ALAW` are accepted on
   * read and not written: one spelling out is one thing to be wrong
   * about, and this is the one both references emit. */
  case GAUD_CODING_G711_ULAW: compression = "ulaw"; break;
  case GAUD_CODING_G711_ALAW: compression = "alaw"; break;
  case GAUD_CODING_ADPCM_IMA_QT: compression = "ima4"; break;
  case GAUD_CODING_ADPCM_IMA_WAV:
    /* WAV's block framing has no AIFF-C compression type. Writing it as
     * `ima4` would label QuickTime's packets on bytes that are not in
     * them. GAUD_CODING_ADPCM_IMA_QT is the spelling for this container. */
    return GAUD_ERR_UNSUPPORTED;
  case GAUD_CODING_ADPCM_MS:
    /* Microsoft ADPCM has no AIFF-C compression type at all. ffmpeg
     * refuses the same combination, producing a zero-byte file. */
    return GAUD_ERR_UNSUPPORTED;
  default: return GAUD_ERR_INVALID;
  }
  bool coded = params->coding != GAUD_CODING_PCM;
  bool aifc = is_float || coded;
  if (coded
      && !gaud_coded_writable(params->coding, params->layout.channels)) {
    /* See gaud_coded_writable(). */
    return GAUD_ERR_UNSUPPORTED;
  }

  if (params->layout.channels == 0 || params->layout.channels > 0x7FFFu) {
    return GAUD_ERR_INVALID;
  }
  GAUD_Coded_Geometry geometry;
  memset(&geometry, 0, sizeof(geometry));
  size_t frame_size = 0;
  uint16_t bits;
  if (coded) {
    GAUD_Result result = gaud_coded_geometry(
        params->coding, params->layout.channels, 0, 0, &geometry);
    if (result != GAUD_OK) {
      return result;
    }
    /*
     * COMM's sampleSize. The references disagree: ffmpeg writes 4 for
     * `ima4` (the stored nibble width) and libsndfile writes 16 (the
     * decoded width). Neither reader appears to use it. AIFF-C's
     * specification describes COMM as the uncompressed data's
     * parameters, which makes 16 the spec reading, so that is what goes
     * out; both values are accepted on read. G.711 is 8 in every
     * implementation and in the spec, because a companded byte IS the
     * stored sample.
     */
    bits = params->coding == GAUD_CODING_ADPCM_IMA_QT ? 16u : 8u;
  }
  else {
    frame_size
        = gaud_frame_size(params->format, params->layout.channels);
    if (frame_size == 0) {
      return GAUD_ERR_INVALID;
    }
    bits = (uint16_t)gaud_sample_format_bits(params->format);
  }

  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);
  AIFF_Encoder_State * state
      = gcu_allocator_malloc(allocator, sizeof(AIFF_Encoder_State));
  if (!state) {
    return GAUD_ERR_OOM;
  }

  /* The largest header this writes, counted rather than guessed:
   *
   *   FORM id 4 + size 4 + form type 4                        = 12
   *   FVER id 4 + size 4 + timestamp 4   (AIFF-C only)        = 12
   *   COMM id 4 + size 4 + channels 2 + frames 4 + bits 2
   *        + rate 10                                          = 26
   *        + compression 4 + Pascal string 1 + pad 1 (AIFF-C) =  6
   *   SSND id 4 + size 4 + offset 4 + block size 4            = 16
   *                                                      max  = 72
   *
   * It was 64, which is enough for plain AIFF and eight bytes short for
   * AIFF-C - so every float file overran this buffer. Every test passed:
   * the overrun landed in adjacent stack and the bytes written after it
   * were correct. ASan and UBSan caught it, which is what they are for.
   * The assertion keeps the number honest if another chunk is added. */
  unsigned char header[96];
  _Static_assert(sizeof(header) >= 72, "the largest AIFF-C header is 72");
  size_t n = 0;
  memcpy(header + n, "FORM", 4);
  n += 4;
  uint64_t form_size_offset = n;
  gaud_wr_u32be(header + n, 0);
  n += 4;
  memcpy(header + n, aifc ? "AIFC" : "AIFF", 4);
  n += 4;

  if (aifc) {
    /* AIFC requires a format-version chunk, and there is exactly one
     * legal value for it. */
    memcpy(header + n, "FVER", 4);
    n += 4;
    gaud_wr_u32be(header + n, 4);
    n += 4;
    gaud_wr_u32be(header + n, 0xA2805140u);
    n += 4;
  }

  memcpy(header + n, "COMM", 4);
  n += 4;
  /* AIFF-C's COMM carries the compression type and a Pascal string naming
   * it; the empty string is one zero byte, padded to even. */
  uint32_t comm_size = aifc ? 18u + 4u + 2u : 18u;
  gaud_wr_u32be(header + n, comm_size);
  n += 4;
  gaud_wr_u16be(header + n, (uint16_t)params->layout.channels);
  n += 2;
  uint64_t comm_frames_offset = n;
  gaud_wr_u32be(header + n, 0); /* patched by finish */
  n += 4;
  gaud_wr_u16be(header + n, bits);
  n += 2;
  gaud_aiff_write_extended(header + n, (double)params->sample_rate);
  n += 10;
  if (aifc) {
    const char * type = compression;
    if (!type) {
      type = bits == 32 ? "fl32" : "fl64";
    }
    memcpy(header + n, type, 4);
    n += 4;
    header[n++] = 0; /* empty Pascal string */
    header[n++] = 0; /* pad to even */
  }

  memcpy(header + n, "SSND", 4);
  n += 4;
  uint64_t ssnd_size_offset = n;
  gaud_wr_u32be(header + n, 0); /* patched by finish */
  n += 4;
  gaud_wr_u32be(header + n, 0); /* offset */
  n += 4;
  gaud_wr_u32be(header + n, 0); /* block size */
  n += 4;

  GAUD_Result result = gaud_stream_write(stream, header, n);
  if (result != GAUD_OK) {
    gcu_allocator_free(allocator, state);
    return result;
  }

  *state = (AIFF_Encoder_State){
      .form_size_offset = form_size_offset,
      .comm_frames_offset = comm_frames_offset,
      .ssnd_size_offset = ssnd_size_offset,
      .data_bytes = 0,
      .frame_size = frame_size,
      /* AIFF is big-endian, so a little-endian host swaps - but a coded
       * file never does. `ima4`'s packet header is big-endian and the
       * block layer writes it that way itself; G.711's samples are one
       * byte. */
      .needs_swap = !coded && gaud_host_is_little_endian() && bits > 8,
      .coding = params->coding,
      .meta = params->meta,
      .policy = params->meta_policy,
  };
  if (coded) {
    result = gaud_coded_writer_init(&state->coded, &geometry, allocator);
    if (result != GAUD_OK) {
      gcu_allocator_free(allocator, state);
      return result;
    }
  }
  result = gaud_encoder_create_internal(
      stream, params, allocator, &aiff_encoder_vtable, state, out_encoder);
  if (result != GAUD_OK) {
    gaud_coded_writer_free(&state->coded, allocator);
    gcu_allocator_free(allocator, state);
  }
  return result;
}
