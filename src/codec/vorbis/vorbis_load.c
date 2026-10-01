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
 * Opening a Vorbis stream: the three headers, the tags, and the length.
 *
 * **The length is the hard part and it is not in the file.** Vorbis states
 * no sample count anywhere - not in the identification header, not in a
 * trailer, nowhere - and a packet's contribution depends on the block size
 * of the packet before it, so counting packets does not answer it either.
 * What answers it is the last page's granule position, which Ogg carries
 * and attaches no meaning to. So this loader reads the head of the file for
 * what the stream *is* and the tail of it for how long it is, and a stream
 * that is not seekable cannot be opened into a document at all - the same
 * refusal mp3_load.c makes, for the same reason.
 *
 * **The setup header is parsed at open and kept on the document**, not on
 * the decoder. None of it is a property a caller can ask about - the
 * codebooks, floors, residues and modes are the definition of how the
 * audio packets are coded and nothing else - but every decoder on the
 * track needs the same parsed copy, and parsing it per decoder would make
 * opening two decoders on one track twice the work for identical results.
 * It is also where a stream that is malformed in a way no later packet
 * could recover from is refused: a mapping names a floor by number, and
 * checking that number once here is what lets the audio path index an
 * array without a bound check in its inner loop.
 */

#include "../../container/ogg/ogg.h"
#include "../../core/meta_internal.h"
#include "../../meta/scheme.h"
#include "../shared/bytes.h"
#include "vorbis_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

static void note(GAUD_Diagnostics * diagnostics, uint64_t offset,
    GAUD_Diag_Severity severity, const char * message) {
  if (!diagnostics) {
    return;
  }
  GAUD_Diagnostic entry = {
      .codec_name = "vorbis",
      .offset = offset,
      .element_id = 0,
      .severity = severity,
      .recommended_action = message,
  };
  (void)gaud_diagnostics_append(diagnostics, &entry);
}

bool gaud_vorbis_is_header(
    const unsigned char * data, size_t size, unsigned type) {
  return size >= VORBIS_HEAD_SIZE && data[0] == (unsigned char)type
      && memcmp(data + 1, VORBIS_SIGNATURE, 6) == 0;
}

GAUD_Result gaud_vorbis_parse_identification(
    const unsigned char * data, size_t size, VORBIS_Info * out) {
  if (!gaud_vorbis_is_header(data, size, VORBIS_PACKET_IDENTIFICATION)) {
    return GAUD_ERR_FORMAT;
  }
  if (size < VORBIS_IDENTIFICATION_SIZE) {
    return GAUD_ERR_CORRUPT;
  }
  const unsigned char * p = data + VORBIS_HEAD_SIZE;
  memset(out, 0, sizeof(*out));
  out->version = gaud_rd_u32le(p);
  out->channels = p[4];
  out->sample_rate = gaud_rd_u32le(p + 5);
  out->bitrate_maximum = (int32_t)gaud_rd_u32le(p + 9);
  out->bitrate_nominal = (int32_t)gaud_rd_u32le(p + 13);
  out->bitrate_minimum = (int32_t)gaud_rd_u32le(p + 17);

  /*
   * The two block sizes share one byte, and this is the first field in
   * this library that is packed least-significant-first: blocksize_0 is
   * the *low* nibble. Both are log2 values, so the byte 0xB8 is a short
   * block of 2^8 = 256 and a long one of 2^11 = 2,048 - which is what
   * every libvorbis encoder at a normal rate writes, and so is the value
   * a reader that swapped the nibbles would still see as plausible.
   */
  unsigned short_log = p[21] & 0x0Fu;
  unsigned long_log = (p[21] >> 4) & 0x0Fu;
  out->blocksize_short = 1u << short_log;
  out->blocksize_long = 1u << long_log;

  /* The framing bit. Its only job is to be set, and a cleared one means
   * the packet was truncated at exactly the right place to look whole. */
  if ((p[22] & 0x01u) == 0) {
    return GAUD_ERR_CORRUPT;
  }

  if (out->version != 0) {
    /* A version this does not know is a different bitstream wearing the
     * same signature, and the specification says a decoder must refuse
     * it rather than try. FORMAT and not CORRUPT: the file is not wrong,
     * it is not ours. */
    return GAUD_ERR_FORMAT;
  }
  if (out->channels == 0 || out->sample_rate == 0) {
    return GAUD_ERR_CORRUPT;
  }
  if (out->blocksize_short < VORBIS_MIN_BLOCKSIZE
      || out->blocksize_long > VORBIS_MAX_BLOCKSIZE
      || out->blocksize_short > out->blocksize_long) {
    return GAUD_ERR_CORRUPT;
  }
  return GAUD_OK;
}

/**
 * Read the three header packets, taking the tags out of the second.
 *
 * The specification requires them to be the first three packets and to be
 * in this order, and requires the first to be alone on its page. Nothing
 * here relies on the pages: the reader assembles packets, so a stream that
 * packed the comment and setup headers onto one page - which libvorbis
 * does - reads the same as one that did not.
 */
static GAUD_Result read_headers(OGG_Reader * reader, VORBIS_File * state,
    const GAUD_Limits * limits, GAUD_Meta * meta,
    GAUD_Diagnostics * diagnostics) {
  const unsigned char * packet = NULL;
  size_t size = 0;
  GAUD_Result result
      = gaud_ogg_reader_packet(reader, &packet, &size, NULL, NULL);
  if (result != GAUD_OK) {
    return result == GAUD_ERR_FORMAT ? GAUD_ERR_CORRUPT : result;
  }
  result = gaud_vorbis_parse_identification(packet, size, &state->info);
  if (result != GAUD_OK) {
    return result;
  }

  result = gaud_ogg_reader_packet(reader, &packet, &size, NULL, NULL);
  if (result != GAUD_OK) {
    return result == GAUD_ERR_FORMAT ? GAUD_ERR_CORRUPT : result;
  }
  if (!gaud_vorbis_is_header(packet, size, VORBIS_PACKET_COMMENT)) {
    return GAUD_ERR_CORRUPT;
  }
  if (size - VORBIS_HEAD_SIZE > limits->max_metadata_bytes) {
    return GAUD_ERR_LIMIT;
  }
  /*
   * The comment header is a Vorbis comment block with a framing bit after
   * it, and src/meta/vorbis_comment.c already reads one: it is the same
   * structure FLAC carries, which is where it came from. The framing bit
   * is a trailing byte the parser reads past, which is why the size is
   * handed over whole rather than trimmed - and the parser stopping where
   * the comment list ends rather than at the end of its buffer is what
   * makes that safe.
   */
  result = gaud_vorbis_comment_parse(packet + VORBIS_HEAD_SIZE,
      size - VORBIS_HEAD_SIZE, limits, meta, diagnostics);
  if (result != GAUD_OK) {
    return result;
  }

  result = gaud_ogg_reader_packet(reader, &packet, &size, NULL, NULL);
  if (result != GAUD_OK) {
    return result == GAUD_ERR_FORMAT ? GAUD_ERR_CORRUPT : result;
  }
  if (!gaud_vorbis_is_header(packet, size, VORBIS_PACKET_SETUP)) {
    return GAUD_ERR_CORRUPT;
  }
  state->setup.allocator = state->allocator;
  result = gaud_vorbis_parse_setup(
      packet, size, state->info.channels, limits, &state->setup);
  if (result != GAUD_OK) {
    note(diagnostics, 0, GAUD_DIAG_ERROR,
        "the setup header does not describe a stream this can decode");
    return result;
  }
  return GAUD_OK;
}

GAUD_Result gaud_vorbis_open(const GAUD_Codec * codec, GAUD_Stream * stream,
    const GAUD_Limits * limits, GAUD_Diagnostics * diagnostics,
    GAUD_Doc ** out_doc) {
  if (!stream || !limits || !out_doc) {
    return GAUD_ERR_INVALID;
  }
  if (!gaud_stream_seekable(stream)) {
    note(diagnostics, 0, GAUD_DIAG_ERROR,
        "a Vorbis stream can only be opened from a seekable stream, "
        "because its length is stated only by its last page");
    return GAUD_ERR_UNSUPPORTED;
  }

  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);
  GAUD_Meta * meta = NULL;
  GAUD_Result result = gaud_meta_create(allocator, &meta);
  if (result != GAUD_OK) {
    return result;
  }

  VORBIS_File * state = NULL;
  GAUD_Doc * doc = NULL;
  GAUD_Result failure = GAUD_OK;
  OGG_Reader reader;
  gaud_ogg_reader_init(&reader, stream, allocator);

  state = gcu_allocator_malloc(allocator, sizeof(*state));
  if (!state) {
    failure = GAUD_ERR_OOM;
    goto Fail;
  }
  memset(state, 0, sizeof(*state));
  state->allocator = allocator;

  result = read_headers(&reader, state, limits, meta, diagnostics);
  if (result != GAUD_OK) {
    failure = result;
    goto Fail;
  }

  /*
   * Where the audio starts, for the decoder to begin at.
   *
   * **The page the setup header finished on, and not the byte after it.**
   * libvorbis does not flush after the setup header, so that page may
   * also carry the first audio packet - and the stream's position after
   * reading a packet is in the middle of a page body, which is not
   * somewhere a reader can be restarted. A decoder resuming here reads
   * at most one page of headers again, and skips them by their first
   * byte: an audio packet's first bit is zero and every header type is
   * odd, so a packet with an odd first byte is a header and nothing
   * else.
   */
  state->serial = reader.serial;
  state->audio_offset = reader.page_offset;

  uint64_t granule = 0;
  if (gaud_ogg_last_granule(stream, allocator, state->serial, &granule)
      == GAUD_OK) {
    state->frames = granule;
    state->frames_known = true;
  }
  else {
    /* No page of this stream states a position, which means the file was
     * truncated before any audio page finished a packet. The stream is
     * still describable - channels, rate, tags - so it opens, and
     * gaud_track_frames() answers "unknown" rather than zero. Saying
     * zero would be a length, and a caller cannot tell a length of zero
     * from a length nobody knows. */
    note(diagnostics, 0, GAUD_DIAG_WARNING,
        "no page states a granule position, so the length is unknown");
    state->frames = 0;
    state->frames_known = false;
  }

  if (state->info.channels > limits->max_channels
      || state->info.sample_rate > limits->max_sample_rate
      || (state->frames_known && state->frames > limits->max_frames)) {
    failure = GAUD_ERR_LIMIT;
    goto Fail;
  }

  result = gaud_doc_create_internal(codec, stream, allocator, &doc);
  if (result != GAUD_OK) {
    failure = result;
    goto Fail;
  }
  /* Before the track, so a failure after this point is cleaned up by
   * gaud_doc_destroy() calling close; see flac_load.c. */
  gaud_doc_set_private(doc, state);

  GAUD_Track_Desc desc;
  memset(&desc, 0, sizeof(desc));
  desc.format = GAUD_SAMPLE_S16;
  desc.coding = GAUD_CODING_VORBIS;
  desc.sample_rate = state->info.sample_rate;
  desc.layout = gaud_channel_layout_default(state->info.channels);
  desc.sample_layout = GAUD_LAYOUT_INTERLEAVED;
  desc.frames = state->frames_known ? state->frames : UINT64_MAX;
  desc.data_offset = state->audio_offset;
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
    gaud_vorbis_setup_free(&state->setup);
    gcu_allocator_free(allocator, state);
  }
  gaud_meta_destroy(meta);
  return failure;
}

void gaud_vorbis_close(const GAUD_Codec * codec, GAUD_Doc * doc) {
  (void)codec;
  VORBIS_File * state = gaud_doc_private(doc);
  if (!state) {
    return;
  }
  gaud_vorbis_setup_free(&state->setup);
  gcu_allocator_free(state->allocator, state);
  gaud_doc_set_private(doc, NULL);
}
