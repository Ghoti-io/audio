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

  /* Filled from ds64 when present. A plain RIFF file leaves them at
   * UINT64_MAX, which means "the 32-bit field is the answer". */
  uint64_t ds64_data_size = UINT64_MAX;

  GAUD_Sample_Format format = GAUD_SAMPLE_S16;
  uint32_t sample_rate = 0;
  GAUD_Channel_Layout layout = {0, 0};
  uint16_t bits = 0;
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
      return GAUD_ERR_LIMIT;
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
        return GAUD_ERR_CORRUPT;
      }
      ds64_data_size = gaud_rd_u64le(ds64 + 8);
      /* Skip whatever else the chunk carried: a table of sizes for other
       * chunks, which nothing here needs. */
      uint64_t consumed = sizeof(ds64);
      if (size > consumed
          && gaud_stream_seek(stream, (int64_t)(size - consumed),
                 GAUD_SEEK_CUR)
              != GAUD_OK) {
        return GAUD_ERR_CORRUPT;
      }
    }
    else if (id_is(chunk, "fmt ")) {
      if (size < 16 || size > limits->max_element_size) {
        return size < 16 ? GAUD_ERR_CORRUPT : GAUD_ERR_LIMIT;
      }
      unsigned char fmt[40];
      size_t want = size < sizeof(fmt) ? (size_t)size : sizeof(fmt);
      if (gaud_stream_read(stream, fmt, want) != want) {
        return GAUD_ERR_CORRUPT;
      }
      uint16_t tag = gaud_rd_u16le(fmt);
      uint16_t channels = gaud_rd_u16le(fmt + 2);
      sample_rate = gaud_rd_u32le(fmt + 4);
      bits = gaud_rd_u16le(fmt + 14);

      if (channels == 0 || sample_rate == 0) {
        return GAUD_ERR_CORRUPT;
      }
      if (channels > limits->max_channels) {
        return GAUD_ERR_LIMIT;
      }
      if (sample_rate > limits->max_sample_rate) {
        return GAUD_ERR_LIMIT;
      }

      layout = gaud_channel_layout_unspecified(channels);

      if (tag == WAV_FORMAT_EXTENSIBLE) {
        if (want < 40) {
          return GAUD_ERR_CORRUPT;
        }
        /* The extension carries the channel mask - the one place any of
         * these containers states which speaker each channel is for - and
         * a GUID whose first two bytes are the real format tag. */
        uint32_t mask = gaud_rd_u32le(fmt + 20);
        uint16_t real_tag = gaud_rd_u16le(fmt + 24);
        if (memcmp(fmt + 26, ext_guid_suffix, sizeof(ext_guid_suffix)) != 0) {
          return GAUD_ERR_UNSUPPORTED;
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

      GAUD_Result result = format_for(tag, bits, &format);
      if (result != GAUD_OK) {
        return result;
      }
      have_fmt = true;

      if (size > want
          && gaud_stream_seek(stream, (int64_t)(size - want), GAUD_SEEK_CUR)
              != GAUD_OK) {
        return GAUD_ERR_CORRUPT;
      }
    }
    else if (id_is(chunk, "data")) {
      data_offset = gaud_stream_tell(stream);
      /* RF64's 0xFFFFFFFF is the sentinel meaning "look in ds64". A file
       * that says that and carries no ds64 is malformed rather than
       * enormous. */
      if (rf64 && size == 0xFFFFFFFFu) {
        if (ds64_data_size == UINT64_MAX) {
          return GAUD_ERR_CORRUPT;
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
        return GAUD_ERR_LIMIT;
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
    return GAUD_ERR_CORRUPT;
  }

  size_t frame_size = gaud_frame_size(format, layout.channels);
  if (frame_size == 0) {
    return GAUD_ERR_CORRUPT;
  }
  uint64_t frames = data_length / frame_size;
  if (frames > limits->max_frames) {
    return GAUD_ERR_LIMIT;
  }

  GAUD_Doc * doc = NULL;
  GAUD_Result result = gaud_doc_create_internal(
      codec, stream, gaud_stream_allocator(stream), &doc);
  if (result != GAUD_OK) {
    return result;
  }

  WAV_Track_State * state = gcu_allocator_malloc(
      gaud_stream_allocator(stream), sizeof(WAV_Track_State));
  if (!state) {
    gaud_doc_destroy(doc);
    return GAUD_ERR_OOM;
  }
  *state = (WAV_Track_State){
      .data_offset = data_offset,
      .data_length = frames * frame_size,
      .frame_size = frame_size,
      /* WAV is little-endian, so a big-endian host has to swap. Anything
       * one byte wide never does. */
      .needs_swap = !gaud_host_is_little_endian()
          && gaud_sample_format_bits(format) > 8,
  };

  GAUD_Track_Desc desc = {
      .format = format,
      .sample_rate = sample_rate,
      .layout = layout,
      .sample_layout = GAUD_LAYOUT_INTERLEAVED,
      .frames = frames,
      .trim = {0, 0, false}, /* WAV states neither. */
      .data_offset = data_offset,
      .data_length = frames * frame_size,
      .codec_private = state,
  };
  result = gaud_doc_add_track(doc, &desc, NULL);
  if (result != GAUD_OK) {
    gcu_allocator_free(gaud_stream_allocator(stream), state);
    gaud_doc_destroy(doc);
    return result;
  }

  *out_doc = doc;
  return GAUD_OK;
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
