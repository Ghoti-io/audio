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
 * Reading a RIFF/WAVE file: the chunk walk, `fmt `, and `data`.
 *
 * Covers plain RIFF and **RF64/BW64**, which is not an exotic variant - any
 * recorder writing more than 4 GiB emits it, and a reader that knows only
 * plain RIFF reports a 32-bit size that is simply wrong rather than failing.
 * The `ds64` chunk is consulted before any size is believed, which is the
 * only arrangement that does not require auditing every size again later.
 */

#include "../../core/meta_internal.h"
#include "../../meta/scheme.h"
#include "../shared/bytes.h"
#include "wav_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/** A chunk header: a four-character id and a 32-bit length. */
#define CHUNK_HEADER 8u

static bool id_is(const unsigned char * p, const char * four) {
  return memcmp(p, four, 4) == 0;
}

/*
 * The GUID that WAVE_FORMAT_EXTENSIBLE uses to say "this is really PCM":
 * the 16-bit tag, then the fixed suffix 00000000-1000-8000-00AA00389B71.
 * Compared as bytes rather than parsed, because only the first two matter
 * and the rest is a constant every writer emits identically.
 */
static const unsigned char ext_guid_suffix[14] = {0x00, 0x00, 0x00, 0x00,
    0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

/*
 * Map a WAVE tag onto a sample coding, or ::GAUD_CODING_PCM when the tag
 * names an uncoded format. Separate from format_for() because the two
 * answers are independent: every coded tag decodes to S16, and the bit
 * depth a coded header states describes the *stored* width (8 for G.711,
 * 4 for either ADPCM) and says nothing about the samples that come out.
 */
static GAUD_Sample_Coding coding_for(uint16_t tag) {
  switch (tag) {
  case WAV_FORMAT_MULAW: return GAUD_CODING_G711_ULAW;
  case WAV_FORMAT_ALAW: return GAUD_CODING_G711_ALAW;
  case WAV_FORMAT_IMA_ADPCM: return GAUD_CODING_ADPCM_IMA_WAV;
  case WAV_FORMAT_ADPCM: return GAUD_CODING_ADPCM_MS;
  default: return GAUD_CODING_PCM;
  }
}

/* Map a WAVE tag and bit depth onto a sample format. */
static GAUD_Result format_for(uint16_t tag, uint16_t bits,
    GAUD_Sample_Format * out_format) {
  if (tag == WAV_FORMAT_PCM) {
    switch (bits) {
    /* 8-bit WAV is UNSIGNED and 16-bit and up are signed. This is the
     * asymmetry GAUD_Sample_Format exists to carry rather than a `bits`
     * field plus a boolean somebody forgets to set. */
    case 8: *out_format = GAUD_SAMPLE_U8; return GAUD_OK;
    case 16: *out_format = GAUD_SAMPLE_S16; return GAUD_OK;
    case 24: *out_format = GAUD_SAMPLE_S24; return GAUD_OK;
    case 32: *out_format = GAUD_SAMPLE_S32; return GAUD_OK;
    default: return GAUD_ERR_UNSUPPORTED;
    }
  }
  if (tag == WAV_FORMAT_IEEE_FLOAT) {
    switch (bits) {
    case 32: *out_format = GAUD_SAMPLE_F32; return GAUD_OK;
    case 64: *out_format = GAUD_SAMPLE_F64; return GAUD_OK;
    default: return GAUD_ERR_UNSUPPORTED;
    }
  }
  return GAUD_ERR_UNSUPPORTED;
}

static void note(GAUD_Diagnostics * diagnostics, uint64_t offset,
    GAUD_Diag_Severity severity, const char * action) {
  if (!diagnostics) {
    return;
  }
  GAUD_Diagnostic entry = {
      .codec_name = "wav",
      .offset = offset,
      .element_id = 0,
      .severity = severity,
      .recommended_action = action,
  };
  gaud_diagnostics_append(diagnostics, &entry);
}

/** @brief Walk a RIFF/WAVE file's chunks and describe its one track. */
GAUD_Result gaud_wav_open(const GAUD_Codec * codec, GAUD_Stream * stream,
    const GAUD_Limits * limits, GAUD_Diagnostics * diagnostics,
    GAUD_Doc ** out_doc) {
  unsigned char header[12];
  if (gaud_stream_read(stream, header, sizeof(header)) != sizeof(header)) {
    return GAUD_ERR_CORRUPT;
  }
  bool rf64 = id_is(header, "RF64");
  if (!rf64 && !id_is(header, "RIFF")) {
    return GAUD_ERR_FORMAT;
  }
  /* BW64 is ITU-R BS.2088's name for the same thing RF64 is; both spell the
   * form type WAVE and both carry ds64. */
  if (!id_is(header + 8, "WAVE")) {
    return GAUD_ERR_FORMAT;
  }

  /* Metadata is parsed during the chunk walk, before it is known whether
   * the file has a track at all - so it is built here and moved into the
   * document at the end. Every early return frees it; `goto Fail` is what
   * makes that one line instead of thirty. */
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

  /* Filled from ds64 when present. A plain RIFF file leaves them at
   * UINT64_MAX, which means "the 32-bit field is the answer". */
  uint64_t ds64_data_size = UINT64_MAX;

  GAUD_Sample_Format format = GAUD_SAMPLE_S16;
  GAUD_Sample_Coding coding = GAUD_CODING_PCM;
  GAUD_Coded_Geometry geometry;
  memset(&geometry, 0, sizeof(geometry));
  uint32_t sample_rate = 0;
  GAUD_Channel_Layout layout = {0, 0};
  uint16_t bits = 0;
  uint16_t block_align = 0;
  uint16_t stated_block_frames = 0;
  uint16_t coef_count = 0;
  int16_t coef[2u * GAUD_MS_MAX_COEF];
  /* `fact` states the true frame count, which for a coded track is the
   * only place it is recorded: the last block is padded to its full
   * length and the block arithmetic cannot tell padding from signal.
   * UINT64_MAX means the chunk was absent. */
  uint64_t fact_frames = UINT64_MAX;
  bool have_fmt = false;
  uint64_t data_offset = 0;
  uint64_t data_length = 0;
  bool have_data = false;

  uint64_t stream_size = 0;
  bool know_size
      = gaud_stream_size(stream, &stream_size) == GAUD_OK;

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
      break; /* Clean end of file. */
    }
    if (got != CHUNK_HEADER) {
      note(diagnostics, chunk_at, GAUD_DIAG_WARNING,
          "trailing bytes shorter than a chunk header; ignored");
      break;
    }
    uint64_t size = gaud_rd_u32le(chunk + 4);

    if (id_is(chunk, "ds64")) {
      /* ds64 restates the RIFF and data sizes as 64-bit. It must come first
       * in an RF64 file, which is why it is consulted before `data`'s own
       * 32-bit size is believed. */
      unsigned char ds64[28];
      if (size < sizeof(ds64)
          || gaud_stream_read(stream, ds64, sizeof(ds64)) != sizeof(ds64)) {
        {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
      }
      ds64_data_size = gaud_rd_u64le(ds64 + 8);
      /* Skip whatever else the chunk carried: a table of sizes for other
       * chunks, which nothing here needs. */
      uint64_t consumed = sizeof(ds64);
      if (size > consumed
          && gaud_stream_seek(stream, (int64_t)(size - consumed),
                 GAUD_SEEK_CUR)
              != GAUD_OK) {
        {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
      }
    }
    else if (id_is(chunk, "fmt ")) {
      if (size < 16 || size > limits->max_element_size) {
        failure = size < 16 ? GAUD_ERR_CORRUPT : GAUD_ERR_LIMIT;
        goto Fail;
      }
      /* 40 covers WAVE_FORMAT_EXTENSIBLE. MS ADPCM's extension is longer:
       * 22 bytes to the end of wNumCoef plus four per coefficient pair,
       * so 150 at the 32 pairs GAUD_MS_MAX_COEF allows. Sized for the
       * largest rather than the common one, because a fmt chunk read
       * short does not fail - it silently loses the tail, and the tail
       * here is the coefficient table. That is exactly how this first
       * went wrong: a 50-byte fmt read into 40 bytes made every MS ADPCM
       * file this library wrote unreadable by this library. */
      unsigned char fmt[22u + 4u * GAUD_MS_MAX_COEF];
      _Static_assert(sizeof(fmt) >= 40u,
          "the fmt buffer must still cover WAVE_FORMAT_EXTENSIBLE");
      size_t want = size < sizeof(fmt) ? (size_t)size : sizeof(fmt);
      if (gaud_stream_read(stream, fmt, want) != want) {
        {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
      }
      uint16_t tag = gaud_rd_u16le(fmt);
      uint16_t channels = gaud_rd_u16le(fmt + 2);
      sample_rate = gaud_rd_u32le(fmt + 4);
      block_align = gaud_rd_u16le(fmt + 12);
      bits = gaud_rd_u16le(fmt + 14);

      if (channels == 0 || sample_rate == 0) {
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
      if (sample_rate > limits->max_sample_rate) {
        {
      failure = GAUD_ERR_LIMIT;
      goto Fail;
    }
      }

      layout = gaud_channel_layout_unspecified(channels);

      if (tag == WAV_FORMAT_EXTENSIBLE) {
        if (want < 40) {
          {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
        }
        /* The extension carries the channel mask - the one place any of
         * these containers states which speaker each channel is for - and
         * a GUID whose first two bytes are the real format tag. */
        uint32_t mask = gaud_rd_u32le(fmt + 20);
        uint16_t real_tag = gaud_rd_u16le(fmt + 24);
        if (memcmp(fmt + 26, ext_guid_suffix, sizeof(ext_guid_suffix)) != 0) {
          {
      failure = GAUD_ERR_UNSUPPORTED;
      goto Fail;
    }
        }
        tag = real_tag;
        if (mask != 0) {
          GAUD_Channel_Layout stated = {.mask = mask, .channels = channels};
          if (gaud_channel_layout_valid(stated)) {
            layout = stated;
          }
          else {
            /* A mask whose bit count disagrees with nChannels. Keeping the
             * count and dropping the mask is the recoverable reading: the
             * sample data's shape is not in doubt, only the labelling. */
            note(diagnostics, chunk_at, GAUD_DIAG_WARNING,
                "channel mask disagrees with the channel count; treated as "
                "unstated");
          }
        }
        /* A valid mask of 0 means "unstated" in the format too, so nothing
         * more is needed for that case. */
      }

      coding = coding_for(tag);
      if (coding != GAUD_CODING_PCM) {
        /* A coded track's samples are whatever the coding decodes to, and
         * `bits` is the stored width rather than the sample width, so
         * format_for() is not asked. */
        format = gaud_sample_coding_format(coding);
        /* Both ADPCM tags carry wSamplesPerBlock in the extension, and
         * MS ADPCM carries its coefficient table after it. cbSize is at
         * offset 16 and the extension follows. */
        if (coding == GAUD_CODING_ADPCM_IMA_WAV
            || coding == GAUD_CODING_ADPCM_MS) {
          if (want < 20) {
            {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
          }
          stated_block_frames = gaud_rd_u16le(fmt + 18);
        }
        if (coding == GAUD_CODING_ADPCM_MS) {
          if (want < 22) {
            {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
          }
          coef_count = gaud_rd_u16le(fmt + 20);
          if (coef_count == 0 || coef_count > GAUD_MS_MAX_COEF) {
            {
      failure = GAUD_ERR_UNSUPPORTED;
      goto Fail;
    }
          }
          if (want < 22u + 4u * (size_t)coef_count) {
            /* The header promises a table it did not carry. Falling back
             * to the standard seven would decode most files and produce
             * noise for the ones that meant it, so it is refused. */
            {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
          }
          for (uint16_t i = 0; i < coef_count; ++i) {
            coef[2u * i] = (int16_t)gaud_rd_u16le(fmt + 22u + 4u * i);
            coef[2u * i + 1u]
                = (int16_t)gaud_rd_u16le(fmt + 24u + 4u * i);
          }
        }
      }
      else {
        GAUD_Result result = format_for(tag, bits, &format);
        if (result != GAUD_OK) {
          {
      failure = result;
      goto Fail;
    }
        }
      }
      have_fmt = true;

      if (size > want
          && gaud_stream_seek(stream, (int64_t)(size - want), GAUD_SEEK_CUR)
              != GAUD_OK) {
        {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
      }
    }
    else if (id_is(chunk, "LIST") || id_is(chunk, "id3 ")
        || id_is(chunk, "ID3 ") || id_is(chunk, "bext")) {
      /* Metadata. Read into a scratch buffer and handed to its scheme,
       * because every one of them needs random access to its own bytes
       * and a stream cannot be rewound cheaply on a pipe.
       *
       * `id3 ` is the identifier the specification gives and `ID3 ` is
       * what several taggers write. Both are accepted; the first is
       * what goes out. */
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
        if (id_is(chunk, "LIST")) {
          /* A LIST may be any type; only INFO is metadata. An `adtl`
           * list holds cue labels and is phase 3's out-of-scope
           * neighbour, so it is kept raw rather than misread. */
          if (size >= 4 && id_is(block, "INFO")) {
            gaud_riff_info_parse(block + 4, (size_t)size - 4u, limits,
                pending_meta, diagnostics);
          }
          else if (size >= 4) {
            char type[5] = {0};
            memcpy(type, block, 4);
            gaud_meta_raw_attach(
                pending_meta, "riff", type, block, (size_t)size);
          }
        }
        else if (id_is(chunk, "bext")) {
          gaud_bext_parse(block, (size_t)size, pending_meta, diagnostics);
          /* Kept raw as well as read: the origination time, the
           * timecode and the loudness fields have no home in the common
           * vocabulary, and a round trip that lost them would make this
           * library unusable for the broadcast files bext exists for. */
          gaud_meta_raw_attach(
              pending_meta, "riff", "bext", block, (size_t)size);
        }
        else {
          gaud_id3v2_parse(
              block, (size_t)size, limits, pending_meta, diagnostics);
        }
        gcu_allocator_free(gaud_stream_allocator(stream), block);
      }
      if (size & 1u) {
        if (gaud_stream_seek(stream, 1, GAUD_SEEK_CUR) != GAUD_OK) {
          break;
        }
      }
    }
    else if (id_is(chunk, "fact")) {
      /* Four bytes: the sample-frame count. Only meaningful for a
       * non-PCM tag, where the data chunk's length cannot give it. */
      unsigned char fact[4];
      if (size >= sizeof(fact)
          && gaud_stream_read(stream, fact, sizeof(fact)) == sizeof(fact)) {
        fact_frames = gaud_rd_u32le(fact);
        uint64_t rest = size - sizeof(fact);
        if (rest > 0
            && gaud_stream_seek(stream, (int64_t)(rest + (size & 1u)),
                   GAUD_SEEK_CUR)
                != GAUD_OK) {
          {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
        }
        if (rest == 0 && (size & 1u)
            && gaud_stream_seek(stream, 1, GAUD_SEEK_CUR) != GAUD_OK) {
          {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
        }
      }
      else {
        {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
      }
    }
    else if (id_is(chunk, "data")) {
      data_offset = gaud_stream_tell(stream);
      /* RF64's 0xFFFFFFFF is the sentinel meaning "look in ds64". A file
       * that says that and carries no ds64 is malformed rather than
       * enormous. */
      if (rf64 && size == 0xFFFFFFFFu) {
        if (ds64_data_size == UINT64_MAX) {
          {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
        }
        data_length = ds64_data_size;
      }
      else {
        data_length = size;
      }
      /* Believe the file only as far as the file actually goes. A truncated
       * download states the length it was going to have, and a decoder
       * reading that far would return whatever followed in memory. */
      if (know_size && data_offset + data_length > stream_size) {
        note(diagnostics, chunk_at, GAUD_DIAG_WARNING,
            "data chunk runs past the end of the file; truncated to what is "
            "present");
        data_length = stream_size > data_offset ? stream_size - data_offset : 0;
      }
      have_data = true;
      /* Chunks are padded to even length; the pad byte is not part of the
       * data. Seeking past it rather than stopping, because a WAV may carry
       * metadata after the samples and phase 3 will want it. */
      uint64_t advance = data_length + (data_length & 1u);
      if (gaud_stream_seekable(stream)) {
        if (gaud_stream_seek(stream, (int64_t)advance, GAUD_SEEK_CUR)
            != GAUD_OK) {
          break;
        }
      }
      else {
        break; /* Cannot skip forward without seeking; data is last anyway. */
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

  if (!have_fmt || !have_data) {
    {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
  }

  size_t frame_size = 0;
  uint64_t frames = 0;
  if (coding == GAUD_CODING_PCM) {
    frame_size = gaud_frame_size(format, layout.channels);
    if (frame_size == 0) {
      {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
    }
    frames = data_length / frame_size;
  }
  else {
    GAUD_Result result = gaud_coded_geometry(
        coding, layout.channels, block_align, stated_block_frames, &geometry);
    if (result != GAUD_OK) {
      {
      failure = result;
      goto Fail;
    }
    }
    if (coef_count > 0) {
      memcpy(geometry.coef, coef, 2u * (size_t)coef_count * sizeof(int16_t));
      geometry.coef_count = coef_count;
    }
    /* Whole blocks, plus whatever a short final block yields. The final
     * block is padded by the writer, so this is an upper bound on the
     * real length and `fact` is what narrows it. */
    uint64_t whole = data_length / geometry.block_bytes;
    uint64_t tail_bytes = data_length % geometry.block_bytes;
    frames = whole * geometry.block_frames;
    if (tail_bytes > 0) {
      frames += gaud_coded_tail_frames(&geometry, (size_t)tail_bytes);
    }
    /* `fact` is believed only when it is not longer than the bytes can
     * supply. A file whose `fact` overstates is truncated, and trusting
     * it would make the decoder report frames it cannot produce - which
     * the short read would then contradict. Understating is the normal
     * case and is exactly what the chunk is for.
     *
     * Note that no reference in the oracle image honours this chunk:
     * ffmpeg, sox and libsndfile all decode the padded length. That
     * disagreement is measured in notes/audio/phase2-calibration.md and
     * scored as a stated exclusion rather than resolved by matching
     * them, because the file says what it says. */
    if (fact_frames != UINT64_MAX && fact_frames <= frames) {
      if (frames - fact_frames >= geometry.block_frames) {
        /* More than a whole block of disagreement is not padding. */
        note(diagnostics, 0, GAUD_DIAG_WARNING,
            "the fact chunk is more than one block shorter than the data "
            "chunk; the data chunk is used");
      }
      else {
        frames = fact_frames;
      }
    }
    else if (fact_frames != UINT64_MAX) {
      note(diagnostics, 0, GAUD_DIAG_WARNING,
          "the fact chunk claims more frames than the data chunk holds; the "
          "data chunk is used");
    }
  }
  if (frames > limits->max_frames) {
    {
      failure = GAUD_ERR_LIMIT;
      goto Fail;
    }
  }

  GAUD_Doc * doc = NULL;
  GAUD_Result result = gaud_doc_create_internal(
      codec, stream, gaud_stream_allocator(stream), &doc);
  if (result != GAUD_OK) {
    {
      failure = result;
      goto Fail;
    }
  }

  WAV_Track_State * state = gcu_allocator_malloc(
      gaud_stream_allocator(stream), sizeof(WAV_Track_State));
  if (!state) {
    gaud_doc_destroy(doc);
    {
      failure = GAUD_ERR_OOM;
      goto Fail;
    }
  }
  /* For a coded track the bytes are the container's blocks, not
   * frames * frame_size, and truncating to the frame count would cut the
   * padded tail of the last block off mid-block. */
  uint64_t stored_bytes
      = coding == GAUD_CODING_PCM ? frames * frame_size : data_length;

  *state = (WAV_Track_State){
      .data_offset = data_offset,
      .data_length = stored_bytes,
      .frame_size = frame_size,
      /* WAV is little-endian, so a big-endian host has to swap. Anything
       * one byte wide never does - and a coded track never does either,
       * because what the block layer produces is already host-order
       * int16_t rather than bytes copied out of the file. */
      .needs_swap = coding == GAUD_CODING_PCM
          && !gaud_host_is_little_endian()
          && gaud_sample_format_bits(format) > 8,
      .coding = coding,
      .geometry = geometry,
  };

  GAUD_Track_Desc desc = {
      .format = format,
      .coding = coding,
      .sample_rate = sample_rate,
      .layout = layout,
      .sample_layout = GAUD_LAYOUT_INTERLEAVED,
      .frames = frames,
      .trim = {0, 0, false}, /* WAV states neither. */
      .data_offset = data_offset,
      .data_length = stored_bytes,
      .codec_private = state,
  };
  result = gaud_doc_add_track(doc, &desc, NULL);
  if (result != GAUD_OK) {
    gcu_allocator_free(gaud_stream_allocator(stream), state);
    gaud_doc_destroy(doc);
    {
      failure = result;
      goto Fail;
    }
  }

  /* The document owns the metadata from here, so the failure path below
   * must not also free it. */
  gaud_doc_take_meta(doc, pending_meta);
  gaud_meta_verify_pictures(gaud_doc_meta(doc));
  *out_doc = doc;
  return GAUD_OK;

Fail:
  gaud_meta_destroy(pending_meta);
  return failure;
}

/** @brief Release the per-track state gaud_wav_open() attached. */
void gaud_wav_close(const GAUD_Codec * codec, GAUD_Doc * doc) {
  (void)codec;
  GAUD_Track * track = gaud_doc_track(doc, 0);
  if (track) {
    gcu_allocator_free(
        gaud_stream_allocator(gaud_doc_stream(doc)), gaud_track_private(track));
  }
}
