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
 * Opening a bare MPEG audio stream: the tags on the ends, the first frame,
 * and how long the thing is.
 *
 * **Every other loader in this library reads a header. This one makes three
 * judgements**, and they are worth naming because each is a place where two
 * readers of the same file legitimately disagree:
 *
 * 1. **Where the audio starts.** A file may open with an ID3v2 tag of any
 *    size, and may then open with bytes that are not a frame at all. A sync
 *    word is eleven bits, which occurs by chance every few hundred bytes in
 *    anything compressed, so a candidate is only believed when the frames
 *    its own stated length points at are there as well.
 * 2. **Where it stops.** An ID3v1 trailer is 128 bytes that are not audio,
 *    and an APE trailer is as many as it says. Counting either as audio
 *    inflates the length and leaves the decoder trying to resynchronise in
 *    a tag.
 * 3. **How long it is.** This is the one with no good answer. Nothing in
 *    the format states a length; a Xing, Info or VBRI frame may, and may be
 *    wrong; counting frames is exact and costs a pass over the file. So the
 *    answer carries its provenance (::MP3_Length_Source) and the estimate
 *    says in a diagnostic what it assumed. planning/audio.md section 2 asks
 *    for exactly this: anything reporting a duration for an MP3 is making a
 *    claim it should be able to qualify.
 *
 * **No samples are decoded here**, as in every loader in this library:
 * `gaud_doc_load()` is what a tagger or a library scanner calls.
 */

#include "../../core/meta_internal.h"
#include "../../meta/scheme.h"
#include "mp3_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/** How many following frames must agree before a sync word is believed. */
#define MP3_CHAIN_WANTED 2u

/** How many frames to walk before giving up on counting them exactly. */
#define MP3_WALK_LIMIT 64u

/** The window the sync search scans at a time. */
#define MP3_SCAN_WINDOW 4096u

/** Bytes in an ID3v1 trailer. Fixed; the tag states no length. */
#define MP3_ID3V1_SIZE 128u

/** Bytes in an ID3v2 header, which is also the size of its footer. */
#define MP3_ID3V2_HEADER 10u

/** Bytes in an APE footer, which is where its own length is stated. */
#define MP3_APE_FOOTER 32u

static void note(GAUD_Diagnostics * diagnostics, uint64_t offset,
    GAUD_Diag_Severity severity, const char * message) {
  if (!diagnostics) {
    return;
  }
  GAUD_Diagnostic entry = {
      .codec_name = "mp3",
      .offset = offset,
      .element_id = 0,
      .severity = severity,
      .recommended_action = message,
  };
  (void)gaud_diagnostics_append(diagnostics, &entry);
}

void gaud_mp3_file_free(MP3_File * file) {
  if (!file) {
    return;
  }
  gcu_allocator_free(file->allocator, file);
}

/** Read a 32-bit little-endian field, as APE's footer stores its sizes. */
static uint32_t rd_u32le(const unsigned char * at) {
  return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16)
      | ((uint32_t)at[3] << 24);
}

/** Read @p want bytes at @p offset, answering how many arrived. */
static size_t read_at(
    GAUD_Stream * stream, uint64_t offset, unsigned char * into, size_t want) {
  if (gaud_stream_seek(stream, (int64_t)offset, GAUD_SEEK_SET) != GAUD_OK) {
    return 0;
  }
  return gaud_stream_read(stream, into, want);
}

uint64_t gaud_mp3_id3v2_span(GAUD_Stream * stream) {
  uint64_t origin = gaud_stream_tell(stream);
  unsigned char header[MP3_ID3V2_HEADER];
  size_t got = gaud_stream_read(stream, header, sizeof(header));
  (void)gaud_stream_seek(stream, (int64_t)origin, GAUD_SEEK_SET);
  if (got != sizeof(header) || memcmp(header, "ID3", 3) != 0) {
    return 0;
  }
  /* A syncsafe size: seven bits per byte, so that no byte of it can look
   * like a frame sync. The eighth bit being set means something other than
   * an ID3v2 tag wrote this, and believing the number anyway would skip a
   * wrong distance. */
  for (unsigned i = 6; i < 10; ++i) {
    if (header[i] & 0x80u) {
      return 0;
    }
  }
  uint64_t body = ((uint64_t)header[6] << 21) | ((uint64_t)header[7] << 14)
      | ((uint64_t)header[8] << 7) | (uint64_t)header[9];
  /* A footer, which 2.4 allows, is ten more bytes after the body. */
  uint64_t span = MP3_ID3V2_HEADER + body;
  if (header[5] & 0x10u) {
    span += MP3_ID3V2_HEADER;
  }
  return span;
}

/**
 * Follow a candidate frame's own length and see whether frames follow.
 *
 * @param allow_lone Whether a candidate with nothing after it may stand.
 *   True only for a candidate at the very start of the search, which is
 *   where a file of one frame - or one truncated frame - has its frame. It
 *   is a narrow exception and it has to be: the first draft allowed any
 *   candidate whose frame reached the end of the stream, and 64 KiB of
 *   pseudorandom bytes then probed as MPEG audio. Roughly one byte pair in
 *   every few thousand of anything passes the header's own validity
 *   checks, so in any large block of noise there are a dozen candidates,
 *   and an exception that lets *one* of them through unconfirmed is an
 *   exception that claims the file.
 * @param out_chain How many frames agreed; at most @p wanted.
 */
static bool confirm_chain(GAUD_Stream * stream, uint64_t offset,
    const MP3_Header * header, unsigned wanted, bool allow_lone,
    uint64_t stream_size, bool have_size, unsigned * out_chain) {
  MP3_Header current = *header;
  uint64_t at = offset;
  unsigned found = 0;

  while (found < wanted) {
    uint64_t next = at + current.frame_size;
    if (have_size && next + MP3_HEADER_SIZE > stream_size) {
      /* The stream has no room for another header, so there is nothing
       * left that could confirm this. */
      break;
    }
    unsigned char bytes[MP3_HEADER_SIZE];
    if (read_at(stream, next, bytes, sizeof(bytes)) != sizeof(bytes)) {
      break;
    }
    MP3_Header following;
    if (!gaud_mp3_header_parse(bytes, &following)
        || !gaud_mp3_headers_compatible(header, &following)
        || following.frame_size == 0u) {
      *out_chain = found;
      return false;
    }
    ++found;
    at = next;
    current = following;
  }
  *out_chain = found;
  return found >= wanted || allow_lone;
}

GAUD_Result gaud_mp3_find_frame(GAUD_Stream * stream, uint64_t start,
    uint64_t search, uint64_t * out_offset, MP3_Header * out_header,
    unsigned * out_chain) {
  if (!stream || !out_offset || !out_header) {
    return GAUD_ERR_INVALID;
  }
  uint64_t stream_size = 0;
  bool have_size = gaud_stream_size(stream, &stream_size) == GAUD_OK;
  unsigned char window[MP3_SCAN_WINDOW];
  uint64_t at = start;
  uint64_t end = start + search;

  while (at < end) {
    size_t want = MP3_SCAN_WINDOW;
    if (end - at < (uint64_t)want) {
      want = (size_t)(end - at);
    }
    size_t got = read_at(stream, at, window, want);
    if (got < MP3_HEADER_SIZE) {
      break;
    }
    for (size_t i = 0; i + MP3_HEADER_SIZE <= got; ++i) {
      if (window[i] != 0xFFu) {
        continue;
      }
      MP3_Header header;
      if (!gaud_mp3_header_parse(window + i, &header)) {
        continue;
      }
      if (header.frame_size == 0u) {
        /* Free format. The length is the distance to the next sync word,
         * so there is no arithmetic that could confirm this candidate -
         * which is why it is believed only at the very start of the data,
         * where a stream that really is in the free format has its first
         * frame. **Anywhere else it is discarded**, and that is not
         * caution for its own sake: 64 KiB of pseudorandom bytes contains
         * about twenty byte pairs that pass this header's validity
         * checks, two of them with bitrate index 0, and none of them with
         * a second frame where its length says - so an unconfirmable
         * candidate accepted from the middle of a stream makes this codec
         * claim noise. The refusal by name happens in gaud_mp3_open(), so
         * that the message is about the file rather than about an offset
         * in a scan. */
        if (at + i != start) {
          continue;
        }
        *out_offset = at + i;
        *out_header = header;
        if (out_chain) {
          *out_chain = 0;
        }
        return GAUD_OK;
      }
      unsigned chain = 0;
      if (confirm_chain(stream, at + i, &header, MP3_CHAIN_WANTED,
              at + i == start, stream_size, have_size, &chain)) {
        *out_offset = at + i;
        *out_header = header;
        if (out_chain) {
          *out_chain = chain;
        }
        return GAUD_OK;
      }
    }
    /* Three bytes of overlap, because a header straddling the boundary
     * must be seen whole by one pass or the other. */
    at += got - (MP3_HEADER_SIZE - 1u);
  }
  return GAUD_ERR_FORMAT;
}

/**
 * Walk frame headers from @p from, counting them.
 *
 * @param out_reached_end Set when the walk ran out of stream rather than
 *   out of patience, which makes @p out_count the exact frame count.
 * @param out_constant_rate Set when every frame seen carried the same
 *   bitrate index.
 */
static void walk_frames(GAUD_Stream * stream, uint64_t from, uint64_t until,
    const MP3_Header * first, uint32_t * out_count, bool * out_reached_end,
    bool * out_constant_rate) {
  uint32_t count = 0;
  bool constant = true;
  uint64_t at = from;
  MP3_Header header = *first;

  while (count < MP3_WALK_LIMIT) {
    if (at + MP3_HEADER_SIZE > until) {
      *out_reached_end = true;
      break;
    }
    unsigned char bytes[MP3_HEADER_SIZE];
    if (read_at(stream, at, bytes, sizeof(bytes)) != sizeof(bytes)) {
      *out_reached_end = true;
      break;
    }
    if (!gaud_mp3_header_parse(bytes, &header)
        || !gaud_mp3_headers_compatible(first, &header)
        || header.frame_size == 0u) {
      /* Something that is not a frame of this stream. Stopping rather than
       * resynchronising: this is a count, and a count that silently
       * skipped a gap would report a length the decoder does not produce. */
      break;
    }
    if (at + header.frame_size > until) {
      /* The frame's header is there and its body is not. **Not counted**:
       * the count is multiplied by the samples a frame decodes to, so
       * counting a frame whose data is missing states a duration the file
       * does not contain - which is the same lie as an inflated Xing tag,
       * arrived at from the other direction. A partial download ends this
       * way, and so does any file cut on a byte boundary. */
      *out_reached_end = true;
      break;
    }
    if (header.bitrate_index != first->bitrate_index) {
      constant = false;
    }
    ++count;
    at += header.frame_size;
  }
  *out_count = count;
  *out_constant_rate = constant;
}

/** Read the ID3v1 and APE trailers, and say where the audio really ends. */
static uint64_t audio_end(GAUD_Stream * stream, uint64_t stream_size,
    uint64_t floor_offset, GAUD_Meta * meta, GAUD_Diagnostics * diagnostics) {
  uint64_t end = stream_size;

  if (end >= floor_offset + MP3_ID3V1_SIZE) {
    unsigned char trailer[MP3_ID3V1_SIZE];
    if (read_at(stream, end - MP3_ID3V1_SIZE, trailer, sizeof(trailer))
            == sizeof(trailer)
        && memcmp(trailer, "TAG", 3) == 0) {
      /* Read into the metadata as well as excluded: gaud_id3v1_parse()
       * fills only fields nothing richer supplied, so a file carrying both
       * gets the ID3v2 reading and the v1 one only where v2 was silent. */
      (void)gaud_id3v1_parse(trailer, meta);
      end -= MP3_ID3V1_SIZE;
    }
  }

  if (end >= floor_offset + MP3_APE_FOOTER) {
    unsigned char footer[MP3_APE_FOOTER];
    if (read_at(stream, end - MP3_APE_FOOTER, footer, sizeof(footer))
            == sizeof(footer)
        && memcmp(footer, "APETAGEX", 8) == 0) {
      /* The footer's size covers the tag and the footer but not the
       * header, which is present only when a flag says so. */
      uint64_t span = rd_u32le(footer + 12);
      if (rd_u32le(footer + 20) & 0x80000000u) {
        span += MP3_APE_FOOTER;
      }
      if (span <= end - floor_offset) {
        end -= span;
        note(diagnostics, end, GAUD_DIAG_WARNING,
            "an APE tag was found at the end of the stream; it is excluded "
            "from the audio data and not interpreted, as this library has "
            "no APE reader");
      }
    }
  }
  return end;
}

/** Read the ID3v2 tag at the front, if any, into @p meta. */
static GAUD_Result read_id3v2(GAUD_Stream * stream, uint64_t origin,
    uint64_t span, const GAUD_Limits * limits, GAUD_Meta * meta,
    GAUD_Diagnostics * diagnostics) {
  if (span == 0) {
    return GAUD_OK;
  }
  if (span > limits->max_metadata_bytes) {
    return GAUD_ERR_LIMIT;
  }
  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);
  unsigned char * block = gcu_allocator_malloc(allocator, (size_t)span);
  if (!block) {
    return GAUD_ERR_OOM;
  }
  GAUD_Result result = GAUD_OK;
  if (read_at(stream, origin, block, (size_t)span) != (size_t)span) {
    /* The tag's own header says it is longer than the file. The frames may
     * still be fine, so this is a diagnostic rather than a refusal. */
    note(diagnostics, 0, GAUD_DIAG_WARNING,
        "the ID3v2 tag at the start of the stream is longer than the "
        "stream; the tag is ignored and the frames are read");
  }
  else {
    result = gaud_id3v2_parse(block, (size_t)span, limits, meta, diagnostics);
    if (result == GAUD_ERR_FORMAT || result == GAUD_ERR_UNSUPPORTED) {
      /* A version this library does not read. The audio is unaffected. */
      result = GAUD_OK;
    }
  }
  gcu_allocator_free(allocator, block);
  return result;
}

/** Which coding this layer is, for ::GAUD_Track_Desc::coding. */
static GAUD_Sample_Coding coding_for_layer(unsigned layer) {
  switch (layer) {
  case 1u: return GAUD_CODING_MPEG_LAYER1;
  case 2u: return GAUD_CODING_MPEG_LAYER2;
  default: return GAUD_CODING_MPEG_LAYER3;
  }
}

GAUD_Result gaud_mp3_open(const GAUD_Codec * codec, GAUD_Stream * stream,
    const GAUD_Limits * limits, GAUD_Diagnostics * diagnostics,
    GAUD_Doc ** out_doc) {
  if (!stream || !limits || !out_doc) {
    return GAUD_ERR_INVALID;
  }
  if (!gaud_stream_seekable(stream)) {
    /* Every other loader here can read its container forwards. This one
     * cannot, and the reason is the format rather than the code: an MPEG
     * stream states no length, so the length comes from the file's size
     * and from trailers at the *end*, and the frame search has to be able
     * to confirm a candidate by reading ahead and then carry on from where
     * it was. A pipe can be decoded frame by frame, but it cannot be
     * opened into a document that answers gaud_track_frames(), and a
     * document that answered "unknown" to every question about itself
     * would be worse than this refusal. */
    note(diagnostics, 0, GAUD_DIAG_ERROR,
        "an MPEG audio stream can only be opened from a seekable stream, "
        "because its length is not stated anywhere in it");
    return GAUD_ERR_UNSUPPORTED;
  }
  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);
  uint64_t origin = gaud_stream_tell(stream);
  GAUD_Result failure = GAUD_ERR_INTERNAL;
  GAUD_Meta * meta = NULL;
  MP3_File * file = NULL;
  GAUD_Doc * doc = NULL;
  unsigned char * frame = NULL;

  GAUD_Result result = gaud_meta_create(allocator, &meta);
  if (result != GAUD_OK) {
    return result;
  }
  file = gcu_allocator_malloc(allocator, sizeof(*file));
  if (!file) {
    gaud_meta_destroy(meta);
    return GAUD_ERR_OOM;
  }
  memset(file, 0, sizeof(*file));
  file->allocator = allocator;
  file->frames = UINT64_MAX;

  uint64_t id3v2 = gaud_mp3_id3v2_span(stream);
  result = read_id3v2(stream, origin, id3v2, limits, meta, diagnostics);
  if (result != GAUD_OK) {
    failure = result;
    goto Fail;
  }

  unsigned chain = 0;
  result = gaud_mp3_find_frame(stream, origin + id3v2, MP3_SYNC_SEARCH,
      &file->first_frame_offset, &file->first, &chain);
  if (result != GAUD_OK) {
    failure = GAUD_ERR_FORMAT;
    goto Fail;
  }
  if (file->first.frame_size == 0u) {
    note(diagnostics, file->first_frame_offset, GAUD_DIAG_ERROR,
        "this stream is in the free format, whose frames state no bitrate "
        "and whose length is the distance to the next sync word; this "
        "library does not read it");
    failure = GAUD_ERR_UNSUPPORTED;
    goto Fail;
  }
  if (file->first_frame_offset > origin + id3v2) {
    note(diagnostics, origin + id3v2, GAUD_DIAG_WARNING,
        "bytes before the first frame were not a frame and not a tag; they "
        "are skipped");
  }
  if (file->first.sample_rate > limits->max_sample_rate) {
    failure = GAUD_ERR_LIMIT;
    goto Fail;
  }
  if (file->first.channels > limits->max_channels) {
    failure = GAUD_ERR_LIMIT;
    goto Fail;
  }

  uint64_t stream_size = 0;
  bool have_size = gaud_stream_size(stream, &stream_size) == GAUD_OK;
  uint64_t end = have_size ? audio_end(stream, stream_size,
                                 file->first_frame_offset, meta, diagnostics)
                           : UINT64_MAX;

  /* The first frame, whole, so that a Xing or VBRI tag in it can be found.
   * A frame is at most MP3_MAX_FRAME_SIZE, so this is bounded. */
  frame = gcu_allocator_malloc(allocator, file->first.frame_size);
  if (!frame) {
    failure = GAUD_ERR_OOM;
    goto Fail;
  }
  size_t frame_got = read_at(
      stream, file->first_frame_offset, frame, file->first.frame_size);
  bool tagged = gaud_mp3_tag_parse(frame, frame_got, &file->first, &file->tag);

  file->audio_offset = file->first_frame_offset;
  if (tagged) {
    if (file->first_frame_offset + file->first.frame_size > end) {
      /* The tag is in a frame the stream ends inside, so there is nothing
       * after it. Refusing rather than reporting a track that starts past
       * the end of its own file: the alternative is a document whose data
       * offset is outside the stream, which every caller would then have
       * to defend against. The fuzz harness found this one, and what it
       * found was not a crash - it was a document whose extents were a
       * fiction. */
      note(diagnostics, file->first_frame_offset, GAUD_DIAG_ERROR,
          "the stream ends inside its first frame, which is the frame a "
          "Xing, Info or VBRI tag occupies; there is no audio after it");
      failure = GAUD_ERR_CORRUPT;
      goto Fail;
    }
    /* The tag occupies the frame's data, so the frame is not audio. */
    file->audio_offset += file->first.frame_size;
  }
  file->audio_length = end > file->audio_offset ? end - file->audio_offset : 0u;

  uint32_t walked = 0;
  bool reached_end = false;
  bool constant_rate = true;
  walk_frames(stream, file->audio_offset, end, &file->first, &walked,
      &reached_end, &constant_rate);

  /* The most frames the bytes could hold, whatever the tag says. A Xing
   * frame count is a number an encoder wrote and nothing has ever checked,
   * and a file that has been cut or rewritten carries the original's
   * count - so a tag claiming more frames than the shortest possible
   * frame length allows is not a length, it is a fabrication, and a
   * caller shown it would seek and scrub against it. */
  uint32_t shortest = gaud_mp3_min_frame_size(&file->first);
  uint64_t possible = shortest ? file->audio_length / shortest : 0u;

  bool tag_states_length = file->tag.has_frames && file->tag.frames > 0u;
  bool tag_is_possible
      = tag_states_length && (uint64_t)file->tag.frames <= possible;
  if (tag_states_length && !tag_is_possible) {
    note(diagnostics, file->audio_offset, GAUD_DIAG_WARNING,
        "the length tag claims more frames than this many bytes could "
        "hold even at the lowest bitrate the format defines; it is "
        "ignored and the length is established without it");
  }

  if (tag_is_possible) {
    file->frames = (uint64_t)file->tag.frames * file->first.samples;
    file->length_source = MP3_LENGTH_STATED;
  }
  else if (reached_end) {
    file->frames = (uint64_t)walked * file->first.samples;
    file->length_source = MP3_LENGTH_COUNTED;
  }
  else if (constant_rate && file->first.bitrate > 0u) {
    /* Bytes to seconds to sample frames, in one expression so that the
     * intermediate seconds are never rounded: an MP3 of three minutes at
     * 128 kbit/s is 2.9 MB, and a second of rounding there is 44,100
     * frames of claim. */
    file->frames = file->audio_length * 8u * file->first.sample_rate
        / file->first.bitrate;
    file->length_source = MP3_LENGTH_ESTIMATED;
    note(diagnostics, file->audio_offset, GAUD_DIAG_WARNING,
        "no Xing, Info or VBRI frame states this stream's length; the "
        "frame count is derived from the data size and the bitrate of the "
        "first frames, which is exact only if the bitrate never changes");
  }
  else {
    note(diagnostics, file->audio_offset, GAUD_DIAG_WARNING,
        "the bitrate varies and no Xing, Info or VBRI frame states the "
        "length, so the frame count is not known without decoding; the "
        "track reports an unknown length rather than an estimate that "
        "would be wrong");
  }
  if (file->frames != UINT64_MAX && file->frames > limits->max_frames) {
    failure = GAUD_ERR_LIMIT;
    goto Fail;
  }

  GAUD_Trim trim = gaud_mp3_tag_trim(&file->tag);
  if (trim.stated && file->frames != UINT64_MAX
      && trim.encoder_delay + trim.padding >= file->frames) {
    /* A delay and padding that between them account for the whole file.
     * The arithmetic is twelve bits each, so a writer that put something
     * else in those three bytes produces a number in range and absurd,
     * and the result of believing it is a track of zero length that looks
     * like a decoder bug. Refusing the pair rather than clamping it: a
     * clamp would report a shorter recording as though it were measured. */
    note(diagnostics, file->first_frame_offset, GAUD_DIAG_WARNING,
        "the encoder delay and padding in the LAME tag account for the "
        "whole file; they are ignored and the track is reported untrimmed");
    memset(&trim, 0, sizeof(trim));
  }

  result = gaud_doc_create_internal(codec, stream, allocator, &doc);
  if (result != GAUD_OK) {
    failure = result;
    goto Fail;
  }
  /* Before the track is added, so that a failure from here on is cleaned
   * up by gaud_doc_destroy() calling this codec's close. */
  gaud_doc_set_private(doc, file);

  GAUD_Track_Desc desc;
  memset(&desc, 0, sizeof(desc));
  desc.format = GAUD_SAMPLE_S16;
  desc.coding = coding_for_layer(file->first.layer);
  desc.sample_rate = file->first.sample_rate;
  desc.layout = gaud_channel_layout_default(file->first.channels);
  desc.sample_layout = GAUD_LAYOUT_INTERLEAVED;
  desc.frames = file->frames;
  desc.trim = trim;
  desc.data_offset = file->audio_offset;
  desc.data_length = file->audio_length;
  desc.codec_private = file;
  result = gaud_doc_add_track(doc, &desc, NULL);
  if (result != GAUD_OK) {
    failure = result;
    goto Fail;
  }

  gcu_allocator_free(allocator, frame);
  gaud_meta_verify_pictures(meta);
  gaud_doc_take_meta(doc, meta);
  *out_doc = doc;
  return GAUD_OK;

Fail:
  gcu_allocator_free(allocator, frame);
  if (doc) {
    gaud_doc_destroy(doc);
  }
  else {
    gaud_mp3_file_free(file);
  }
  gaud_meta_destroy(meta);
  return failure;
}

void gaud_mp3_close(const GAUD_Codec * codec, GAUD_Doc * doc) {
  (void)codec;
  gaud_mp3_file_free(gaud_doc_private(doc));
}
