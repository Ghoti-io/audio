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
 * Reading AIFF and AIFF-C.
 *
 * IFF rather than RIFF, so **big-endian**, which is the whole reason this
 * codec is worth having beside WAV in phase 1: the two formats disagree
 * about byte order and about the sign of an 8-bit sample, so a bug in
 * either one's handling of those shows up as a difference between them on
 * any host.
 *
 * AIFF-C adds a compression type to the common chunk. The two that matter
 * here are `NONE`, which is ordinary big-endian PCM, and `sowt`, which is
 * the same samples little-endian - Apple's spelling of "this came from an
 * Intel machine". `fl32` and `fl64` are big-endian floats.
 */

#include "../../core/meta_internal.h"
#include "../../meta/id3_internal.h"
#include "../../meta/scheme.h"
#include "../shared/bytes.h"
#include "../shared/coded.h"
#include "aiff_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/** A chunk header: a four-character id and a 32-bit length. */
#define CHUNK_HEADER 8u

static bool id_is(const unsigned char * p, const char * four) {
  return memcmp(p, four, 4) == 0;
}

static void note(GAUD_Diagnostics * diagnostics, uint64_t offset,
    const char * action) {
  if (!diagnostics) {
    return;
  }
  GAUD_Diagnostic entry = {
      .codec_name = "aiff",
      .offset = offset,
      .element_id = 0,
      .severity = GAUD_DIAG_WARNING,
      .recommended_action = action,
  };
  gaud_diagnostics_append(diagnostics, &entry);
}

/** @brief Walk an AIFF or AIFF-C file's chunks and describe its track. */
GAUD_Result gaud_aiff_open(const GAUD_Codec * codec, GAUD_Stream * stream,
    const GAUD_Limits * limits, GAUD_Diagnostics * diagnostics,
    GAUD_Doc ** out_doc) {
  unsigned char header[12];
  if (gaud_stream_read(stream, header, sizeof(header)) != sizeof(header)) {
    return GAUD_ERR_CORRUPT;
  }
  if (!id_is(header, "FORM")) {
    return GAUD_ERR_FORMAT;
  }
  bool aifc = id_is(header + 8, "AIFC");
  if (!aifc && !id_is(header + 8, "AIFF")) {
    return GAUD_ERR_FORMAT;
  }

  /* As in the WAV loader: metadata is parsed while walking chunks, before
   * the document exists, so it is built standalone and moved in. */
  GAUD_Result failure = GAUD_ERR_INTERNAL;
  GAUD_Meta * pending_meta = NULL;
  GAUD_Result meta_result
      = gaud_meta_create(gaud_stream_allocator(stream), &pending_meta);
  if (meta_result != GAUD_OK) {
    {
      failure = meta_result;
      goto Fail;
    }
  }

  uint32_t channels = 0;
  uint64_t frames_stated = 0;
  uint16_t bits = 0;
  double rate = 0.0;
  bool have_comm = false;
  bool little_endian = false; /* `sowt` and friends. */
  bool is_float = false;
  GAUD_Sample_Coding coding = GAUD_CODING_PCM;
  GAUD_Coded_Geometry geometry;
  memset(&geometry, 0, sizeof(geometry));
  uint64_t data_offset = 0;
  uint64_t data_length = 0;
  bool have_ssnd = false;

  uint64_t stream_size = 0;
  bool know_size = gaud_stream_size(stream, &stream_size) == GAUD_OK;

  uint32_t depth = 0;
  for (;;) {
    if (++depth > limits->max_nesting_depth) {
      {
      failure = GAUD_ERR_LIMIT;
      goto Fail;
    }
    }
    unsigned char chunk[CHUNK_HEADER];
    uint64_t chunk_at = gaud_stream_tell(stream);
    size_t got = gaud_stream_read(stream, chunk, CHUNK_HEADER);
    if (got == 0) {
      break;
    }
    if (got != CHUNK_HEADER) {
      note(diagnostics, chunk_at,
          "trailing bytes shorter than a chunk header; ignored");
      break;
    }
    uint64_t size = gaud_rd_u32be(chunk + 4);

    if (id_is(chunk, "COMM")) {
      /* 18 bytes for AIFF; AIFF-C appends a four-character compression type
       * and a Pascal string naming it. */
      if (size < 18 || size > limits->max_element_size) {
        failure = size < 18 ? GAUD_ERR_CORRUPT : GAUD_ERR_LIMIT;
        goto Fail;
      }
      unsigned char comm[22];
      size_t want = size < sizeof(comm) ? (size_t)size : sizeof(comm);
      if (gaud_stream_read(stream, comm, want) != want) {
        {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
      }
      channels = gaud_rd_u16be(comm);
      frames_stated = gaud_rd_u32be(comm + 2);
      bits = gaud_rd_u16be(comm + 6);
      rate = gaud_aiff_read_extended(comm + 8);

      if (aifc && want >= 22) {
        const unsigned char * compression = comm + 18;
        if (id_is(compression, "NONE")) {
          /* Big-endian PCM, which is AIFF's own arrangement. */
        }
        else if (id_is(compression, "sowt")) {
          /* "twos" backwards: little-endian PCM. */
          little_endian = true;
        }
        else if (id_is(compression, "twos")) {
          /* Explicitly big-endian two's complement; same as NONE. */
        }
        else if (id_is(compression, "fl32") || id_is(compression, "FL32")) {
          is_float = true;
        }
        else if (id_is(compression, "fl64") || id_is(compression, "FL64")) {
          is_float = true;
        }
        /* G.711 has two spellings apiece: Apple's lower case and SGI's
         * upper. They name the same coding and both appear in the wild,
         * so both are accepted; the writer emits Apple's, which is what
         * both writers in the oracle image emit. */
        else if (id_is(compression, "ulaw") || id_is(compression, "ULAW")) {
          coding = GAUD_CODING_G711_ULAW;
        }
        else if (id_is(compression, "alaw") || id_is(compression, "ALAW")) {
          coding = GAUD_CODING_G711_ALAW;
        }
        else if (id_is(compression, "ima4")) {
          coding = GAUD_CODING_ADPCM_IMA_QT;
        }
        else {
          /* Refused by name rather than decoded as PCM. An AIFF-C in a
           * compression this does not implement, read as though it were
           * PCM, produces loud noise - a worse answer than saying no. */
          {
      failure = GAUD_ERR_UNSUPPORTED;
      goto Fail;
    }
        }
      }
      have_comm = true;

      if (size > want
          && gaud_stream_seek(stream, (int64_t)(size - want), GAUD_SEEK_CUR)
              != GAUD_OK) {
        {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
      }
    }
    else if (id_is(chunk, "SSND")) {
      /* An 8-byte preamble - offset and block size - precedes the samples.
       * The offset is almost always zero and is almost always ignored by
       * readers that then get it wrong for the file where it is not. */
      unsigned char preamble[8];
      if (size < sizeof(preamble)
          || gaud_stream_read(stream, preamble, sizeof(preamble))
              != sizeof(preamble)) {
        {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
      }
      uint32_t offset = gaud_rd_u32be(preamble);
      data_offset = gaud_stream_tell(stream) + offset;
      uint64_t body = size - sizeof(preamble);
      data_length = body > offset ? body - offset : 0;
      if (know_size && data_offset + data_length > stream_size) {
        note(diagnostics, chunk_at,
            "sound chunk runs past the end of the file; truncated to what is "
            "present");
        data_length = stream_size > data_offset ? stream_size - data_offset : 0;
      }
      have_ssnd = true;
      uint64_t advance = size - sizeof(preamble);
      advance += (size & 1u); /* IFF pads to even, like RIFF. */
      if (!gaud_stream_seekable(stream)
          || gaud_stream_seek(stream, (int64_t)advance, GAUD_SEEK_CUR)
              != GAUD_OK) {
        break;
      }
    }
    else if (id_is(chunk, "ID3 ") || id_is(chunk, "id3 ")
        || gaud_aiff_text_tag((const char *)chunk, &(GAUD_Tag){0})) {
      if (size > limits->max_element_size
          || size > limits->max_metadata_bytes) {
        {
      failure = GAUD_ERR_LIMIT;
      goto Fail;
    }
      }
      if (size > 0) {
        unsigned char * block
            = gcu_allocator_malloc(gaud_stream_allocator(stream), size);
        if (!block) {
          {
      failure = GAUD_ERR_OOM;
      goto Fail;
    }
        }
        if (gaud_stream_read(stream, block, (size_t)size) != (size_t)size) {
          gcu_allocator_free(gaud_stream_allocator(stream), block);
          {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
        }
        GAUD_Tag tag;
        if (gaud_aiff_text_tag((const char *)chunk, &tag)) {
          /* AIFF's text chunks are NOT Pascal strings and are NOT
           * NUL-terminated: the chunk's length is the string's length,
           * full stop. Trimming a trailing zero that a writer added
           * anyway, because several do and a caller should not see it. */
          size_t length = (size_t)size;
          while (length > 0 && block[length - 1] == 0) {
            --length;
          }
          char * text = gaud_id3_bytes_to_utf8(
              gaud_stream_allocator(stream), block, length);
          if (text) {
            if (text[0] != '\0') {
              gaud_meta_add_unique(pending_meta, tag, text);
            }
            gcu_allocator_free(gaud_stream_allocator(stream), text);
          }
        }
        else {
          gaud_id3v2_parse(
              block, (size_t)size, limits, pending_meta, diagnostics);
        }
        gcu_allocator_free(gaud_stream_allocator(stream), block);
      }
      if ((size & 1u)
          && gaud_stream_seek(stream, 1, GAUD_SEEK_CUR) != GAUD_OK) {
        break;
      }
    }
    else {
      if (size > limits->max_element_size) {
        {
      failure = GAUD_ERR_LIMIT;
      goto Fail;
    }
      }
      uint64_t advance = size + (size & 1u);
      if (!gaud_stream_seekable(stream)
          || gaud_stream_seek(stream, (int64_t)advance, GAUD_SEEK_CUR)
              != GAUD_OK) {
        break;
      }
    }
  }

  if (!have_comm || !have_ssnd) {
    {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
  }
  if (channels == 0 || !(rate > 0.0)) {
    {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
  }
  if (channels > limits->max_channels) {
    {
      failure = GAUD_ERR_LIMIT;
      goto Fail;
    }
  }
  if (rate > (double)limits->max_sample_rate) {
    {
      failure = GAUD_ERR_LIMIT;
      goto Fail;
    }
  }

  GAUD_Sample_Format format;
  if (coding != GAUD_CODING_PCM) {
    format = gaud_sample_coding_format(coding);
  }
  else if (is_float) {
    if (bits == 32) {
      format = GAUD_SAMPLE_F32;
    }
    else if (bits == 64) {
      format = GAUD_SAMPLE_F64;
    }
    else {
      {
      failure = GAUD_ERR_UNSUPPORTED;
      goto Fail;
    }
    }
  }
  else {
    switch (bits) {
    /* AIFF's 8-bit is SIGNED, where WAV's is unsigned. The two formats
     * genuinely disagree, and this is the line that carries it. */
    case 8: format = GAUD_SAMPLE_S8; break;
    case 16: format = GAUD_SAMPLE_S16; break;
    case 24: format = GAUD_SAMPLE_S24; break;
    case 32: format = GAUD_SAMPLE_S32; break;
    default: {
      failure = GAUD_ERR_UNSUPPORTED;
      goto Fail;
    }
    }
  }

  size_t frame_size = 0;
  uint64_t frames = 0;
  if (coding == GAUD_CODING_PCM) {
    frame_size = gaud_frame_size(format, channels);
    if (frame_size == 0) {
      {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
    }
    uint64_t frames_present = data_length / frame_size;
    /* COMM states the frame count and SSND carries the bytes. Where they
     * disagree the bytes win, because they are what can actually be read;
     * the disagreement is worth a diagnostic because it means something
     * truncated the file. */
    frames = frames_stated;
    if (frames_stated != frames_present) {
      note(diagnostics, 0,
          "COMM's frame count disagrees with the sound chunk's length; the "
          "chunk's length is used");
      frames = frames_present;
    }
  }
  else {
    GAUD_Result result
        = gaud_coded_geometry(coding, channels, 0, 0, &geometry);
    if (result != GAUD_OK) {
      {
      failure = result;
      goto Fail;
    }
    }
    uint64_t whole = data_length / geometry.block_bytes;
    uint64_t tail = data_length % geometry.block_bytes;
    frames = whole * geometry.block_frames;
    if (tail > 0) {
      frames += gaud_coded_tail_frames(&geometry, (size_t)tail);
    }
    /*
     * COMM is advisory here, and deliberately so.
     *
     * For `ima4` the field does not mean what the AIFF-C specification
     * says it means. The spec describes COMM as stating the uncompressed
     * data's sample-frame count; ffmpeg 7.1.5 and libsndfile 1.2.2 both
     * *write* the packet count instead - 32 for a 2048-frame file - and
     * both *decoders* ignore the field entirely, deriving the length from
     * SSND. Patching it to 32, 2048, 7 or 100000 changes neither
     * reference's output. So the bytes are the authority, two independent
     * implementations agree that they are, and COMM only gets a
     * diagnostic when it is inconsistent with them under either reading.
     *
     * notes/audio/phase2-calibration.md records the measurements.
     */
    if (frames_stated != 0 && frames_stated != frames
        && frames_stated * 64u != frames) {
      note(diagnostics, 0,
          "COMM's frame count matches neither the sound chunk's frames nor "
          "its packet count; the chunk's length is used");
    }
  }
  if (frames > limits->max_frames) {
    {
      failure = GAUD_ERR_LIMIT;
      goto Fail;
    }
  }

  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);
  GAUD_Doc * doc = NULL;
  GAUD_Result result
      = gaud_doc_create_internal(codec, stream, allocator, &doc);
  if (result != GAUD_OK) {
    {
      failure = result;
      goto Fail;
    }
  }

  AIFF_Track_State * state
      = gcu_allocator_malloc(allocator, sizeof(AIFF_Track_State));
  if (!state) {
    gaud_doc_destroy(doc);
    {
      failure = GAUD_ERR_OOM;
      goto Fail;
    }
  }
  /* The file is big-endian unless a `sowt`-style compression type said
   * otherwise, so whether a swap is needed depends on both. */
  bool file_is_little = little_endian;
  bool host_is_little = gaud_host_is_little_endian();
  uint64_t stored_bytes
      = coding == GAUD_CODING_PCM ? frames * frame_size : data_length;
  *state = (AIFF_Track_State){
      .data_offset = data_offset,
      .data_length = stored_bytes,
      .frame_size = frame_size,
      /* A coded track never swaps: the block layer produces host-order
       * int16_t rather than bytes copied out of the file. */
      .needs_swap = coding == GAUD_CODING_PCM
          && (file_is_little != host_is_little)
          && gaud_sample_format_bits(format) > 8,
      .coding = coding,
      .geometry = geometry,
  };

  GAUD_Track_Desc desc = {
      .format = format,
      .coding = coding,
      /* The rate is stored as a float and used as an integer. Rounding
       * rather than truncating: 44100 written by a writer that lost a bit
       * would otherwise read back as 44099. */
      .sample_rate = (uint32_t)(rate + 0.5),
      .layout = gaud_channel_layout_unspecified(channels),
      .sample_layout = GAUD_LAYOUT_INTERLEAVED,
      .frames = frames,
      .trim = {0, 0, false},
      .data_offset = data_offset,
      .data_length = stored_bytes,
      .codec_private = state,
  };
  result = gaud_doc_add_track(doc, &desc, NULL);
  if (result != GAUD_OK) {
    gcu_allocator_free(allocator, state);
    gaud_doc_destroy(doc);
    {
      failure = result;
      goto Fail;
    }
  }
  gaud_doc_take_meta(doc, pending_meta);
  gaud_meta_verify_pictures(gaud_doc_meta(doc));
  *out_doc = doc;
  return GAUD_OK;

Fail:
  gaud_meta_destroy(pending_meta);
  return failure;
}

/** @brief Release the per-track state gaud_aiff_open() attached. */
void gaud_aiff_close(const GAUD_Codec * codec, GAUD_Doc * doc) {
  (void)codec;
  GAUD_Track * track = gaud_doc_track(doc, 0);
  if (track) {
    gcu_allocator_free(
        gaud_stream_allocator(gaud_doc_stream(doc)), gaud_track_private(track));
  }
}
