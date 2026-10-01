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
 * FLAC inside Ogg: the mapping, and nothing else.
 *
 * **Every line of the frame decoder is shared with the native codec**, and
 * that is the point of the file existing separately. The FLAC bitstream is
 * identical in both; what differs is how the bytes are found. Natively a
 * frame follows the one before it and its length is discovered by decoding
 * it; in Ogg each frame is exactly one packet and the page framing states
 * the length. So this file is a different way of feeding
 * gaud_flac_frame_decode(), and if it ever grows a second copy of a
 * subframe reader something has gone wrong.
 *
 * The mapping, from the Xiph specification:
 *
 *   - The first page carries one packet: `0x7F`, `FLAC`, a major and minor
 *     mapping version, a **big-endian** count of header packets, then
 *     `fLaC` and the STREAMINFO block complete with its own four-byte
 *     metadata header. The count may be zero, meaning "unknown", and this
 *     reader does not rely on it.
 *   - Each remaining metadata block is one packet of its own.
 *   - Each audio frame is one packet.
 *   - A page's granule position is the sample number of the last sample
 *     finished on it.
 *
 * Two consequences worth stating, because both are places where carrying
 * native FLAC's habits across would be wrong:
 *
 * **A SEEKTABLE is meaningless here and is not written.** Its offsets are
 * byte positions into a native FLAC stream's frames, and in Ogg the frames
 * are scattered through page bodies at positions no such table describes.
 * Seeking uses the granule positions, which is what Ogg provides them for.
 *
 * **The STREAMINFO MD5 still applies**, because it is over the samples
 * rather than over the file, so `flac -t` verifies an Ogg FLAC file the
 * same way it verifies a native one.
 */

#include "../../container/ogg/ogg.h"
#include "../../core/meta_internal.h"
#include "../../meta/scheme.h"
#include "../shared/bytes.h"
#include "flac_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/**
 * The first packet's fixed head: `0x7F`, `FLAC`, two version bytes, a
 * big-endian header-packet count, and the `fLaC` signature. The
 * STREAMINFO block's own header follows at this offset.
 */
#define OGG_FLAC_HEAD_SIZE 13u

/** What one Ogg FLAC document keeps. */
typedef struct {
  FLAC_File file;    ///< Shared with the native codec's decoder.
  /**
   * The logical stream the FLAC is in.
   *
   * Kept because a decoder opened later has to follow the same one. An
   * Ogg file may multiplex - that is what a video file is - and a decoder
   * that let the reader choose a serial afresh could pick a different
   * stream from the one the document describes.
   */
  uint32_t serial;
} OGG_FLAC_File;

static void note(GAUD_Diagnostics * diagnostics, uint64_t offset,
    GAUD_Diag_Severity severity, const char * message) {
  if (!diagnostics) {
    return;
  }
  GAUD_Diagnostic entry = {
      .codec_name = "ogg-flac",
      .offset = offset,
      .element_id = 0,
      .severity = severity,
      .recommended_action = message,
  };
  (void)gaud_diagnostics_append(diagnostics, &entry);
}

/** Whether @p data is the Ogg FLAC mapping's first packet. */
static bool is_mapping_head(const unsigned char * data, size_t size) {
  return size >= OGG_FLAC_HEAD_SIZE + FLAC_BLOCK_HEADER + FLAC_STREAMINFO_SIZE
      && data[0] == 0x7Fu && memcmp(data + 1, "FLAC", 4) == 0
      /* Mapping version 1.x. A major version this does not know means a
       * framing this cannot read, and guessing would be worse than
       * declining. */
      && data[5] == 1u && memcmp(data + 9, FLAC_MAGIC, 4) == 0;
}

/* ------------------------------------------------------------- decoding */

/** What one Ogg FLAC decoder holds between calls. */
typedef struct {
  OGG_FLAC_File * doc_state; ///< Borrowed from the document.
  OGG_Reader reader;         ///< Positioned in the audio pages.
  FLAC_Frame frame;          ///< The frame being drained, if any.
  uint32_t frame_used;       ///< How many of its sample frames are gone.
  bool frame_live;           ///< Whether @p frame holds anything.
  uint64_t position;         ///< The next sample the caller will receive.
  const GAUD_Allocator * allocator; ///< Everything here comes from it.
} OGG_FLAC_Decoder;

static void decoder_close(GAUD_Decoder * decoder) {
  OGG_FLAC_Decoder * state = gaud_decoder_private(decoder);
  if (!state) {
    return;
  }
  gaud_flac_frame_free(&state->frame);
  gaud_ogg_reader_free(&state->reader);
  gcu_allocator_free(state->allocator, state);
}

/**
 * Position at the start of the stream, before any packet has been read.
 *
 * **Byte zero, not the offset the open left off at**, and the difference
 * is a fragility worth not having. The audio packets could be found by
 * remembering where the header pages ended, and that is only right while
 * no page carries both a header packet and an audio one - which the
 * mapping's writers all honour and which nothing in the format requires.
 * Starting from the beginning and skipping metadata packets is correct
 * for any packing, and costs re-reading the header pages, which are a
 * few kilobytes.
 *
 * What makes the skip unambiguous is that a metadata block header can
 * never begin with 0xFF: the block type would have to be 127, which the
 * format forbids precisely so that it cannot be confused with a frame
 * sync.
 */
static GAUD_Result rewind_to_audio(OGG_FLAC_Decoder * state) {
  state->position = 0;
  state->frame_live = false;
  state->frame_used = 0;
  return gaud_ogg_reader_seek(&state->reader, 0);
}

/** Pull the next audio packet and decode it as a frame. */
static GAUD_Result decode_next(OGG_FLAC_Decoder * state) {
  for (;;) {
    const unsigned char * data = NULL;
    size_t size = 0;
    GAUD_Result result = gaud_ogg_reader_packet(
        &state->reader, &data, &size, NULL, NULL);
    if (result != GAUD_OK) {
      return result;
    }
    if (size == 0) {
      continue;
    }
    /* A metadata packet can only appear before the audio, and a stream
     * that put one later is one this does not try to resynchronise. The
     * test is the frame sync itself, which no metadata block header can
     * imitate: a block type of 127 is forbidden for exactly that reason. */
    if (data[0] != 0xFFu) {
      continue;
    }
    size_t used = 0;
    result = gaud_flac_frame_decode(
        data, size, &state->doc_state->file.info, &state->frame, &used);
    if (result != GAUD_OK) {
      return result;
    }
    state->frame_used = 0;
    state->frame_live = true;
    return GAUD_OK;
  }
}

static GAUD_Result decoder_read(GAUD_Decoder * decoder, GAUD_Buffer * buffer) {
  OGG_FLAC_Decoder * state = gaud_decoder_private(decoder);
  size_t capacity = gaud_buffer_capacity(buffer);
  size_t filled = 0;

  while (filled < capacity) {
    if (!state->frame_live || state->frame_used >= state->frame.block_size) {
      GAUD_Result result = decode_next(state);
      if (result == GAUD_ERR_FORMAT) {
        break;
      }
      if (result != GAUD_OK) {
        return result;
      }
    }
    uint32_t available = state->frame.block_size - state->frame_used;
    size_t room = capacity - filled;
    uint32_t take = available < room ? available : (uint32_t)room;
    gaud_flac_emit(&state->frame, state->frame_used, buffer, filled, take);
    state->frame_used += take;
    filled += take;
    state->position += take;
  }

  uint64_t total = state->doc_state->file.info.total_samples;
  if (total && state->position > total) {
    uint64_t excess = state->position - total;
    filled = excess >= filled ? 0 : filled - (size_t)excess;
    state->position = total;
    state->frame_live = false;
    state->frame_used = 0;
  }

  gaud_decoder_set_position(decoder, state->position);
  return gaud_buffer_set_frames(buffer, filled);
}

/**
 * Position at a page boundary, with @p granule samples already behind it.
 *
 * ::gaud_ogg_reader_seek() drops a packet continued from before the
 * landing page, so the first packet this yields is whole - which is the
 * one thing a mid-stream landing has to get right and the one thing that
 * cannot be noticed by looking at the decoded audio, because the tail of
 * a FLAC frame decodes to *something*.
 */
static GAUD_Result seek_to_page(
    OGG_FLAC_Decoder * state, uint64_t offset, uint64_t granule) {
  state->position = granule;
  state->frame_live = false;
  state->frame_used = 0;
  return gaud_ogg_reader_seek(&state->reader, offset);
}

/**
 * Seek by bisecting the file to a page, then decoding forward from it.
 *
 * **Phase 4 wrote this linearly and said why**, which is worth repeating
 * because the reasons have been answered rather than withdrawn: a
 * bisection can be subtly wrong near the ends of a file, and a scan for
 * `OggS` can find a page that is not there. Both now have an owner.
 * src/container/ogg/ogg_seek.c identifies a page by its checksum and not
 * by its magic, so a false `OggS` costs a read and nothing else; and its
 * bisection returns the first audio page when it finds nothing better, so
 * the end that is easy to get wrong has one answer rather than a special
 * case. The remaining risk is in the forward decode from the landing page,
 * which is the same code that ran for every seek before this change.
 *
 * What is gained is not speed here - these fixtures are small - but that
 * the search exists once, for three codecs. Vorbis and Opus state their
 * length nowhere but in a granule position, so they need the same code to
 * open a file at all, not only to seek in one.
 */
static GAUD_Result decoder_seek(
    GAUD_Decoder * decoder, uint64_t frame, uint64_t * out_landed) {
  OGG_FLAC_Decoder * state = gaud_decoder_private(decoder);
  uint64_t total = state->doc_state->file.info.total_samples;
  if (total && frame > total) {
    frame = total;
  }
  if (state->frame_live && state->position - state->frame_used <= frame
      && frame
          < state->position - state->frame_used + state->frame.block_size) {
    state->frame_used
        = (uint32_t)(frame - (state->position - state->frame_used));
    state->position = frame;
    gaud_decoder_set_position(decoder, frame);
    *out_landed = frame;
    return GAUD_OK;
  }

  /*
   * The floor is byte zero rather than the first audio page's offset, for
   * the reason rewind_to_audio() gives: where the audio starts is only
   * knowable by assuming no page mixes a header packet with a frame, and
   * decode_next() skips metadata packets anyway. A bisection that lands on
   * byte zero is then exactly a rewind.
   */
  uint64_t offset = 0;
  uint64_t granule = 0;
  GAUD_Result result = GAUD_OK;
  if (gaud_ogg_bisect(&state->reader, frame, 0, &offset, &granule) != GAUD_OK
      || granule > frame) {
    result = rewind_to_audio(state);
  }
  else {
    result = seek_to_page(state, offset, granule);
  }
  if (result != GAUD_OK) {
    return result;
  }
  while (state->position < frame) {
    result = decode_next(state);
    if (result == GAUD_ERR_FORMAT) {
      break;
    }
    if (result != GAUD_OK) {
      return result;
    }
    uint64_t block = state->frame.block_size;
    if (state->position + block > frame) {
      state->frame_used = (uint32_t)(frame - state->position);
      state->position = frame;
      break;
    }
    state->position += block;
    state->frame_live = false;
  }
  gaud_decoder_set_position(decoder, state->position);
  *out_landed = state->position;
  return GAUD_OK;
}

static const GAUD_Decoder_Vtable ogg_flac_decoder_vtable = {
    .read = decoder_read,
    .seek = decoder_seek,
    .close = decoder_close,
};

static GAUD_Result ogg_flac_decoder_open(
    const GAUD_Codec * codec, GAUD_Track * track, GAUD_Decoder ** out) {
  (void)codec;
  OGG_FLAC_File * doc_state = gaud_track_private(track);
  if (!doc_state) {
    return GAUD_ERR_INTERNAL;
  }
  GAUD_Stream * stream = gaud_doc_stream(gaud_track_doc(track));
  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);

  OGG_FLAC_Decoder * state = gcu_allocator_malloc(allocator, sizeof(*state));
  if (!state) {
    return GAUD_ERR_OOM;
  }
  memset(state, 0, sizeof(*state));
  state->doc_state = doc_state;
  state->allocator = allocator;
  state->frame.allocator = allocator;
  gaud_ogg_reader_init(&state->reader, stream, allocator);
  /* Told which logical stream to follow rather than letting it choose
   * from the first page it meets: a multiplexed file's other streams are
   * then skipped rather than spliced in. */
  state->reader.serial = doc_state->serial;
  state->reader.have_serial = true;
  GAUD_Result result = rewind_to_audio(state);
  if (result != GAUD_OK) {
    gaud_ogg_reader_free(&state->reader);
    gcu_allocator_free(allocator, state);
    return result;
  }

  result = gaud_decoder_create_internal(
      track, &ogg_flac_decoder_vtable, state, out);
  if (result != GAUD_OK) {
    gaud_ogg_reader_free(&state->reader);
    gcu_allocator_free(allocator, state);
  }
  return result;
}

/* -------------------------------------------------------------- opening */

static void ogg_flac_close(const GAUD_Codec * codec, GAUD_Doc * doc) {
  (void)codec;
  OGG_FLAC_File * state = gaud_doc_private(doc);
  if (!state) {
    return;
  }
  const GAUD_Allocator * allocator = state->file.allocator;
  gcu_allocator_free(allocator, state->file.seek_points);
  gcu_allocator_free(allocator, state);
}

static GAUD_Result ogg_flac_open(const GAUD_Codec * codec,
    GAUD_Stream * stream, const GAUD_Limits * limits,
    GAUD_Diagnostics * diagnostics, GAUD_Doc ** out_doc) {
  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);
  GAUD_Result failure = GAUD_ERR_INTERNAL;
  GAUD_Meta * meta = NULL;
  OGG_FLAC_File * state = NULL;
  GAUD_Doc * doc = NULL;
  OGG_Reader reader;
  gaud_ogg_reader_init(&reader, stream, allocator);

  GAUD_Result result = gaud_meta_create(allocator, &meta);
  if (result != GAUD_OK) {
    return result;
  }
  state = gcu_allocator_malloc(allocator, sizeof(*state));
  if (!state) {
    failure = GAUD_ERR_OOM;
    goto Fail;
  }
  memset(state, 0, sizeof(*state));
  state->file.allocator = allocator;
  state->file.frames_length = UINT64_MAX;

  const unsigned char * packet = NULL;
  size_t size = 0;
  result = gaud_ogg_reader_packet(&reader, &packet, &size, NULL, NULL);
  if (result != GAUD_OK) {
    failure = result == GAUD_ERR_FORMAT ? GAUD_ERR_CORRUPT : result;
    goto Fail;
  }
  if (!is_mapping_head(packet, size)) {
    /* An Ogg file that is not an Ogg FLAC one - Vorbis, Opus, a video.
     * GAUD_ERR_FORMAT rather than CORRUPT, so the registry goes on to
     * ask another codec. */
    failure = GAUD_ERR_FORMAT;
    goto Fail;
  }
  result = gaud_flac_parse_streaminfo(
      packet + OGG_FLAC_HEAD_SIZE + FLAC_BLOCK_HEADER,
      size - OGG_FLAC_HEAD_SIZE - FLAC_BLOCK_HEADER, &state->file.info);
  if (result != GAUD_OK) {
    failure = result;
    goto Fail;
  }

  /* Every packet after the first is a metadata block until one turns out
   * to be a frame. The last-block flag says which is the last, and it is
   * believed - but a packet beginning with a frame sync ends the header
   * run even if the flag never arrived, because a truncated flag would
   * otherwise make the whole stream unreadable. */
  bool last = (packet[OGG_FLAC_HEAD_SIZE] & 0x80u) != 0;
  uint64_t metadata_bytes = FLAC_STREAMINFO_SIZE;
  uint32_t blocks = 1u;
  while (!last) {
    uint64_t at = gaud_stream_tell(stream);
    result = gaud_ogg_reader_packet(&reader, &packet, &size, NULL, NULL);
    if (result != GAUD_OK) {
      failure = result == GAUD_ERR_FORMAT ? GAUD_ERR_CORRUPT : result;
      goto Fail;
    }
    if (size < FLAC_BLOCK_HEADER) {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
    if (packet[0] == 0xFFu) {
      note(diagnostics, at, GAUD_DIAG_WARNING,
          "header packets ended without a last-block flag");
      break;
    }
    if (++blocks > limits->max_metadata_entries) {
      failure = GAUD_ERR_LIMIT;
      goto Fail;
    }
    last = (packet[0] & 0x80u) != 0;
    unsigned type = packet[0] & 0x7Fu;
    uint32_t body_size = ((uint32_t)packet[1] << 16)
        | ((uint32_t)packet[2] << 8) | (uint32_t)packet[3];
    if (body_size > size - FLAC_BLOCK_HEADER) {
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
    const unsigned char * body = packet + FLAC_BLOCK_HEADER;
    metadata_bytes += body_size;
    if (metadata_bytes > limits->max_metadata_bytes) {
      failure = GAUD_ERR_LIMIT;
      goto Fail;
    }

    if (type == FLAC_BLOCK_VORBIS_COMMENT) {
      result = gaud_vorbis_comment_parse(
          body, body_size, limits, meta, diagnostics);
      if (result != GAUD_OK) {
        failure = result;
        goto Fail;
      }
    }
    else if (type == FLAC_BLOCK_PICTURE) {
      result = gaud_flac_parse_picture(
          body, body_size, limits, meta, diagnostics);
      if (result != GAUD_OK) {
        failure = result;
        goto Fail;
      }
    }
    else if (type == FLAC_BLOCK_CUESHEET) {
      if (gaud_flac_check_cuesheet(body, body_size, limits, NULL, diagnostics)
          == GAUD_OK) {
        result = gaud_meta_raw_attach(meta, "flac", "CUESHEET", body,
            body_size);
        if (result != GAUD_OK) {
          failure = result;
          goto Fail;
        }
      }
    }
    else if (type == FLAC_BLOCK_APPLICATION) {
      result = gaud_meta_raw_attach(
          meta, "flac", "APPLICATION", body, body_size);
      if (result != GAUD_OK) {
        failure = result;
        goto Fail;
      }
    }
    /* SEEKTABLE and PADDING are dropped here rather than carried, for the
     * reasons flac_load.c gives: a seek table's offsets describe a native
     * stream's bytes and mean nothing in Ogg, and padding is room for a
     * tagger that this writer makes afresh. */
  }

  /* Where the audio starts, for the decoder to rewind to. The reader is
   * positioned after the last header page, which is the first audio page
   * for every stream the mapping permits: a header packet and an audio
   * packet never share a page. */
  state->file.first_frame_offset = gaud_stream_tell(stream);
  state->serial = reader.serial;

  if (state->file.info.channels > limits->max_channels
      || state->file.info.sample_rate > limits->max_sample_rate
      || state->file.info.total_samples > limits->max_frames) {
    failure = GAUD_ERR_LIMIT;
    goto Fail;
  }
  GAUD_Sample_Format format = GAUD_SAMPLE_S16;
  if (!gaud_flac_format_for_depth(state->file.info.bits_per_sample,
          &format)) {
    note(diagnostics, 0, GAUD_DIAG_ERROR,
        "bit depth has no lossless sample format in this library");
    failure = GAUD_ERR_UNSUPPORTED;
    goto Fail;
  }

  result = gaud_doc_create_internal(codec, stream, allocator, &doc);
  if (result != GAUD_OK) {
    failure = result;
    goto Fail;
  }
  /* Before the track, so that a failure after this point is cleaned up
   * by gaud_doc_destroy() calling close; see flac_load.c. */
  gaud_doc_set_private(doc, state);

  GAUD_Track_Desc desc;
  memset(&desc, 0, sizeof(desc));
  desc.format = format;
  desc.coding = GAUD_CODING_FLAC;
  desc.sample_rate = state->file.info.sample_rate;
  desc.layout = gaud_channel_layout_default(state->file.info.channels);
  desc.sample_layout = GAUD_LAYOUT_INTERLEAVED;
  desc.frames = state->file.info.total_samples
      ? state->file.info.total_samples
      : UINT64_MAX;
  desc.data_offset = state->file.first_frame_offset;
  desc.data_length = UINT64_MAX;
  desc.codec_private = state;
  result = gaud_doc_add_track(doc, &desc, NULL);
  if (result != GAUD_OK) {
    failure = result;
    goto Fail;
  }

  gaud_meta_verify_pictures(meta);
  gaud_doc_take_meta(doc, meta);
  gaud_ogg_reader_free(&reader);
  *out_doc = doc;
  return GAUD_OK;

Fail:
  gaud_ogg_reader_free(&reader);
  if (doc) {
    gaud_doc_destroy(doc);
  }
  else if (state) {
    gcu_allocator_free(allocator, state->file.seek_points);
    gcu_allocator_free(allocator, state);
  }
  gaud_meta_destroy(meta);
  return failure;
}

/* ---------------------------------------------------------- registration */

static const unsigned char ogg_magic[] = {'O', 'g', 'g', 'S'};

static const GAUD_Codec_Magic ogg_magics[] = {
    {0, ogg_magic, 4},
};

/**
 * `OggS` says only that this is an Ogg file, and Vorbis, Opus, Theora and
 * Speex all begin the same way - so the probe has to look inside the first
 * page's packet, which is where the codec identifies itself. This is the
 * case planning/audio.md's phase 1 note was about: a probe must be able to
 * veto its own magic, and here the magic is shared four ways.
 */
static GAUD_Result ogg_flac_probe(const GAUD_Codec * codec,
    GAUD_Stream * stream, unsigned int * out_confidence) {
  (void)codec;
  *out_confidence = GAUD_CONFIDENCE_NONE;
  unsigned char head[OGG_HEADER_FIXED + 1u + OGG_FLAC_HEAD_SIZE];
  uint64_t start = gaud_stream_tell(stream);
  size_t got = gaud_stream_read(stream, head, sizeof(head));
  gaud_stream_seek(stream, (int64_t)start, GAUD_SEEK_SET);
  if (got != sizeof(head) || memcmp(head, OGG_MAGIC, 4) != 0) {
    return GAUD_OK;
  }
  /* One segment in the first page's table, then the packet. A mapping
   * head is 51 bytes, so it is always a single segment. */
  if (head[26] != 1u) {
    return GAUD_OK;
  }
  const unsigned char * packet = head + OGG_HEADER_FIXED + 1u;
  if (packet[0] != 0x7Fu || memcmp(packet + 1, "FLAC", 4) != 0) {
    return GAUD_OK;
  }
  *out_confidence = GAUD_CONFIDENCE_CERTAIN;
  return GAUD_OK;
}

static const GAUD_Codec ogg_flac_codec = {
    .abi_version = GAUD_CODEC_ABI_VERSION,
    .size = sizeof(GAUD_Codec),
    .name = "ogg-flac",
    .ctx = NULL,
    .capabilities = GAUD_CAP_DECODE | GAUD_CAP_ENCODE,
    .encoder_tier = GAUD_ENCODER_EXACT,
    .magics = ogg_magics,
    .magic_count = sizeof(ogg_magics) / sizeof(ogg_magics[0]),
    .probe = ogg_flac_probe,
    .open = ogg_flac_open,
    .close = ogg_flac_close,
    .decoder_open = ogg_flac_decoder_open,
    .encoder_open = gaud_flac_ogg_encoder_open,
};

/** @brief Register Ogg FLAC when the shared library is loaded. */
GAUD_INIT_FUNCTION(gaud_ogg_flac_register_ctor) {
  gaud_registry_register(NULL, &ogg_flac_codec);
}

/** @brief Register the Ogg FLAC codec explicitly; see gaud_wav_register(). */
GAUD_API void gaud_ogg_flac_register(void) {
  gaud_registry_register(NULL, &ogg_flac_codec);
}
