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
 * Reading ID3v2.2, 2.3 and 2.4, and the ID3v1 trailer.
 *
 * ## Sizes are syncsafe, and which ones depends on the version
 *
 * A syncsafe integer keeps the top bit of each byte clear so that no
 * length can be mistaken for an MPEG frame sync. The **header** size is
 * syncsafe in every version. The **frame** size is syncsafe in 2.4 and a
 * plain big-endian integer in 2.3, which is the single most common ID3
 * bug: reading 2.3's frame sizes as syncsafe silently drops every frame
 * after the first one longer than 127 bytes, and reading 2.4's as plain
 * overruns.
 *
 * ## Unsynchronisation
 *
 * `0xFF 0x00` stands for `0xFF` so that no byte pair in the tag looks like
 * a sync word. In 2.3 it is a whole-tag flag; in 2.4 it is per frame as
 * well. Undone into a scratch copy before anything is parsed, because
 * doing it as the parser walks means every offset in the frame is wrong.
 */

#include "id3_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief The ten bytes every ID3v2 tag begins with. */
#define ID3_HEADER 10u

/** A syncsafe 28-bit integer: seven bits per byte. */
static uint32_t syncsafe(const unsigned char * p) {
  return ((uint32_t)(p[0] & 0x7Fu) << 21) | ((uint32_t)(p[1] & 0x7Fu) << 14)
      | ((uint32_t)(p[2] & 0x7Fu) << 7) | (uint32_t)(p[3] & 0x7Fu);
}

/** A plain big-endian 32-bit integer, which 2.3 frame sizes are. */
static uint32_t be32(const unsigned char * p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
      | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/**
 * How many entries @p meta now holds, counting values and not frames.
 *
 * `GAUD_Limits::max_metadata_entries` is a promise about what comes back
 * to a caller, and counting frames does not keep it: one ID3v2.4 text
 * frame may carry any number of NUL-separated values, and `TRCK` alone
 * yields two tags. A cap of four frames let a single frame produce a
 * hundred tags, which the fuzzer found by varying the cap - a limit
 * that is never moved is a refusal path no corpus reaches.
 */
static size_t entry_count(const GAUD_Meta * meta) {
  size_t total = gaud_meta_custom_count(meta) + gaud_meta_picture_count(meta)
      + gaud_meta_raw_count(meta);
  for (int t = 0; t < GAUD_TAG_COUNT; ++t) {
    total += gaud_meta_count(meta, (GAUD_Tag)t);
  }
  return total;
}

static void note(GAUD_Diagnostics * diagnostics, uint64_t offset,
    const char * action) {
  if (!diagnostics) {
    return;
  }
  GAUD_Diagnostic entry = {
      .codec_name = "id3",
      .offset = offset,
      .element_id = 0,
      .severity = GAUD_DIAG_WARNING,
      .recommended_action = action,
  };
  gaud_diagnostics_append(diagnostics, &entry);
}

/**
 * Whether @p id is a frame identifier at all.
 *
 * The specification says A-Z and 0-9, and nothing else. This matters for
 * two reasons beyond tidiness.
 *
 * The identifier is handed to a caller - as a custom key for an
 * unmapped text frame, and as the `id` of a raw block - and **every
 * string this API returns is UTF-8**. A frame id read out of a corrupt
 * file can be any byte at all, and passing `TL\xd5` through produces a
 * key no caller's validator accepts. The fuzzer found exactly that.
 *
 * And a frame whose identifier is not an identifier means the walk has
 * lost its place: the sizes that follow are being read at an offset
 * that is not a frame header. Stopping is the honest response -
 * continuing invents frames out of sample data.
 */
static bool frame_id_is_valid(const char * id, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)id[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
      return false;
    }
  }
  return true;
}

/**
 * The flags of a kept-raw frame, in 2.4's spelling.
 *
 * **The bit meanings differ between 2.3 and 2.4**, and this library
 * always writes 2.4 - so storing the flags as they were read and
 * emitting them unchanged relabels the frame. 2.3's compression bit is
 * 0x0080 and 2.4's is 0x0008; 2.3's grouping is 0x0020 and 2.4's is
 * 0x0040. Copying one into the other turns a compressed frame into a
 * grouped one, or into nothing.
 *
 * Only the flags that describe *content this library did not decode*
 * survive. Unsynchronisation does not: the body is stored with it
 * already undone, so re-emitting the flag would say the bytes are
 * stuffed when they are not, and the next reader would un-stuff them
 * again. The fuzzer found both of these as an instability - build,
 * re-parse, re-build produced different bytes - rather than as anything
 * a single pass could see.
 *
 * One mapping is not a rename. 2.3 puts a four-byte uncompressed size
 * at the head of a compressed frame's body; 2.4 spells that same
 * arrangement as compression *plus* the data-length indicator. So a 2.3
 * compressed frame becomes 2.4 `0x0008 | 0x0001`, and the body needs no
 * change.
 */
static uint16_t normalise_frame_flags(unsigned major, unsigned flags) {
  uint16_t out = 0;
  if (major == 3) {
    if (flags & 0x0080u) {
      out |= 0x0008u | 0x0001u; /* compressed, with its length prefix */
    }
    if (flags & 0x0040u) {
      out |= 0x0004u; /* encrypted */
    }
    if (flags & 0x0020u) {
      out |= 0x0040u; /* grouped */
    }
  }
  else if (major == 4) {
    /* Everything but unsynchronisation, which the stored body has
     * already had undone. */
    out = (uint16_t)(flags & (0x0040u | 0x0008u | 0x0004u | 0x0001u));
  }
  /* 2.2 frames have no flag byte at all. */
  return out;
}

/** Undo `0xFF 0x00` in place. Returns the new length. */
static size_t unsynchronise(unsigned char * data, size_t size) {
  size_t out = 0;
  for (size_t i = 0; i < size; ++i) {
    data[out++] = data[i];
    if (data[i] == 0xFFu && i + 1 < size && data[i + 1] == 0x00u) {
      ++i; /* Drop the stuffed zero. */
    }
  }
  return out;
}

/**
 * Split "3/12" into a number and a total.
 *
 * ID3 stores both in one string and Vorbis comment uses two keys, so one
 * of them has to be converted and this is the place. Doing it in each
 * container instead is how the two come to disagree.
 */
static void split_pair(GAUD_Meta * meta, const char * text, GAUD_Tag number,
    GAUD_Tag total) {
  const char * slash = strchr(text, '/');
  if (!slash) {
    gaud_meta_add_unique(meta, number, text);
    return;
  }
  size_t head = (size_t)(slash - text);
  /*
   * Allocated, not a fixed buffer.
   *
   * The first version used `char buffer[32]` and skipped the number
   * when it did not fit, which is silent loss: a track number longer
   * than 31 characters is nonsense, but this library's job is to report
   * what the file says and not to decide that part of it is too long to
   * mention. The fuzzer found it as an instability - the number
   * vanished on the second read while the total survived, so a rewrite
   * lost half the field - rather than as anything a valid file shows.
   */
  const GAUD_Allocator * allocator = gaud_meta_allocator(meta);
  char * head_text = gcu_allocator_malloc(allocator, head + 1u);
  if (head_text) {
    if (head > 0) {
      memcpy(head_text, text, head);
    }
    head_text[head] = '\0';
    if (head_text[0] != '\0') {
      gaud_meta_add_unique(meta, number, head_text);
    }
    gcu_allocator_free(allocator, head_text);
  }
  if (slash[1] != '\0') {
    gaud_meta_add_unique(meta, total, slash + 1);
  }
}

/**
 * A `TCON` genre, which may be a bare number, "(17)" or "(17)Rock".
 *
 * ID3v2.3 inherited ID3v1's numeric genres and wrapped them in
 * parentheses; 2.4 allows the bare number. A reader that passes the
 * digits through gives a caller "17" where every other tool says "Rock".
 *
 * **The normalisation has to reach a fixed point**, or a round trip
 * changes the genre a little at a time. Two ways it failed to:
 *
 *   `(0)2`        the refinement text wins, giving `2` - and `2` on the
 *                 next read is a bare number meaning Country.
 *   `(0)(666)x`   the specification allows the prefix to repeat, and
 *                 stripping one level leaves `(666)x`, which the next
 *                 read strips again.
 *
 * So the prefix is stripped in a loop and the bare-number check runs on
 * what is left. Each turn removes at least three characters, so it
 * terminates; what comes out is either a name from the table or a
 * string that is neither a number nor parenthesised, and reading it
 * again returns it unchanged.
 *
 * The fuzzer found both as build/re-parse/re-build instability. Neither
 * is reachable from a file any tagger writes, and no single pass could
 * show either.
 */
static void add_genre(GAUD_Meta * meta, const char * text) {
  const char * at = text;
  const char * last_name = NULL;

  /* Strip every "(N)" prefix. The text after the last one, where there
   * is any, is the writer spelling the genre out and wins over the
   * numbers; where there is none, the last number is the answer. */
  for (;;) {
    if (at[0] != '(') {
      break;
    }
    const char * close = strchr(at, ')');
    if (!close) {
      break;
    }
    size_t length = (size_t)(close - at - 1);
    char digits[8];
    if (length == 0 || length >= sizeof(digits)) {
      break;
    }
    memcpy(digits, at + 1, length);
    digits[length] = '\0';
    char * end = NULL;
    unsigned long value = strtoul(digits, &end, 10);
    if (!end || *end != '\0') {
      break; /* "(RX)" and "(CR)" are not numbers; they are the text. */
    }
    const char * name = gaud_id3v1_genre((unsigned)value);
    last_name = name ? name : NULL;
    at = close + 1;
    if (at[0] == '\0') {
      /* Nothing after the last prefix: the number is the genre. */
      gaud_meta_add_unique(
          meta, GAUD_TAG_GENRE, last_name ? last_name : digits);
      return;
    }
  }

  /* A bare number, which 2.4 allows and which stripping a prefix may
   * also have left behind. */
  char * end = NULL;
  unsigned long value = strtoul(at, &end, 10);
  if (end && end != at && *end == '\0') {
    const char * name = gaud_id3v1_genre((unsigned)value);
    gaud_meta_add_unique(meta, GAUD_TAG_GENRE, name ? name : at);
    return;
  }
  gaud_meta_add_unique(meta, GAUD_TAG_GENRE, at);
}

/** One `APIC` (2.3/2.4) or `PIC` (2.2) frame. */
static void read_picture(GAUD_Meta * meta, const GAUD_Allocator * allocator,
    const unsigned char * body, size_t size, bool v22,
    const GAUD_Limits * limits, GAUD_Diagnostics * diagnostics) {
  if (size < 4) {
    return;
  }
  unsigned encoding = body[0];
  size_t at = 1;

  char * mime = NULL;
  if (v22) {
    /* 2.2 carries a three-character image format - "PNG", "JPG" - where
     * 2.3 carries a MIME type. Expanded here so a caller sees one thing. */
    if (size < at + 3u) {
      return;
    }
    char kind[4] = {0};
    memcpy(kind, body + at, 3);
    at += 3;
    const char * expanded = "application/octet-stream";
    if (memcmp(kind, "PNG", 3) == 0) {
      expanded = "image/png";
    }
    else if (memcmp(kind, "JPG", 3) == 0) {
      expanded = "image/jpeg";
    }
    mime = gaud_meta_dup(allocator, expanded);
  }
  else {
    size_t end = gaud_id3_find_terminator(body + at, size - at, 0);
    /*
     * ID3 stores the MIME type as Latin-1, never in the frame's own
     * encoding - but this library hands a caller UTF-8 and writes back
     * what it was handed, so reading it as Latin-1 unconditionally
     * re-encodes the library's own output on every pass. The fuzzer
     * found it as a frame that grew four bytes per generation.
     *
     * A MIME type is US-ASCII by RFC 2045, so anything else in the
     * field is already corrupt and the only question is which reading
     * is stable. The same UTF-8-or-Latin-1 rule the encoding-less
     * schemes use is: our own ASCII survives untouched, and a corrupt
     * field settles after one pass instead of doubling forever.
     */
    mime = gaud_id3_bytes_to_utf8(allocator, body + at, end);
    at += end + 1u;
  }
  if (!mime || at >= size) {
    gcu_allocator_free(allocator, mime);
    return;
  }

  unsigned kind = body[at++];
  if (at > size) {
    gcu_allocator_free(allocator, mime);
    return;
  }
  size_t description_end
      = gaud_id3_find_terminator(body + at, size - at, encoding);
  char * description
      = gaud_id3_decode_text(allocator, encoding, body + at, description_end);
  at += description_end + gaud_id3_terminator_width(encoding);
  if (at > size) {
    gcu_allocator_free(allocator, mime);
    gcu_allocator_free(allocator, description);
    return;
  }

  size_t payload = size - at;
  if (payload > limits->max_picture_bytes) {
    note(diagnostics, 0,
        "an embedded picture exceeds the picture size limit; it is skipped "
        "and the rest of the tag is kept");
    gcu_allocator_free(allocator, mime);
    gcu_allocator_free(allocator, description);
    return;
  }
  /* The mapping is in id3_frames.c because FLAC's PICTURE block uses this
   * same registry, and two copies of it would drift. */
  GAUD_Picture_Kind mapped = gaud_picture_kind_from_apic(kind);
  /* APIC states no dimensions, so every picture from ID3 is
   * NOT_STATED - which planning/audio.md 11.4 keeps distinct from
   * "this build could not check". */
  gaud_meta_picture_add(
      meta, mapped, mime, description ? description : "", body + at, payload);
  gcu_allocator_free(allocator, mime);
  gcu_allocator_free(allocator, description);
}

GAUD_Result gaud_id3v2_parse(const unsigned char * data, size_t size,
    const GAUD_Limits * limits, GAUD_Meta * meta,
    GAUD_Diagnostics * diagnostics) {
  if (!data || !meta || !limits || size < ID3_HEADER) {
    return GAUD_ERR_INVALID;
  }
  if (memcmp(data, "ID3", 3) != 0) {
    return GAUD_ERR_FORMAT;
  }
  unsigned major = data[3];
  if (major < 2 || major > 4) {
    /* A version this does not know. Refusing rather than guessing: the
     * frame header's shape changes between them, and parsing 2.5 as 2.4
     * would produce frames at wrong offsets rather than no frames. */
    note(diagnostics, 0,
        "an ID3v2 version this library does not implement; the tag is kept "
        "raw and not interpreted");
    return GAUD_ERR_UNSUPPORTED;
  }
  unsigned flags = data[5];
  uint32_t body_size = syncsafe(data + 6);
  if ((size_t)body_size + ID3_HEADER > size) {
    return GAUD_ERR_CORRUPT;
  }
  if (body_size > limits->max_metadata_bytes) {
    return GAUD_ERR_LIMIT;
  }

  const GAUD_Allocator * allocator = gaud_meta_allocator(meta);
  unsigned char * body = gcu_allocator_malloc(allocator, body_size + 1u);
  if (!body) {
    return GAUD_ERR_OOM;
  }
  memcpy(body, data + ID3_HEADER, body_size);
  size_t length = body_size;

  /* Whole-tag unsynchronisation, 2.3's spelling. 2.4 keeps the header
   * flag but also has a per-frame one; both are handled, and a tag that
   * sets neither is the common case. */
  if (flags & 0x80u) {
    length = unsynchronise(body, length);
  }
  size_t at = 0;
  if (flags & 0x40u) {
    /* An extended header. Its own size field is syncsafe in 2.4 and a
     * plain integer in 2.3, and in 2.3 the size EXCLUDES itself while in
     * 2.4 it includes itself - so the skip differs by four bytes between
     * versions and getting it wrong puts the first frame at the wrong
     * offset. */
    if (length < 4) {
      gcu_allocator_free(allocator, body);
      return GAUD_ERR_CORRUPT;
    }
    uint32_t extended = major >= 4 ? syncsafe(body) : be32(body) + 4u;
    if (extended > length) {
      gcu_allocator_free(allocator, body);
      return GAUD_ERR_CORRUPT;
    }
    at = extended;
  }

  size_t id_length = major == 2 ? 3u : 4u;
  size_t frame_header = major == 2 ? 6u : 10u;

  while (at + frame_header <= length) {
    /*
     * Checked at the top, where no path can bypass it. It was at the
     * bottom, and the three `continue`s that keep a frame raw stepped
     * straight over it - so a tag made of nothing but compressed frames
     * ignored the cap entirely. The fuzzer found it by varying the
     * limit; a gate that only ever runs the defaults cannot.
     */
    if (entry_count(meta) > limits->max_metadata_entries) {
      gcu_allocator_free(allocator, body);
      return GAUD_ERR_LIMIT;
    }
    if (body[at] == 0) {
      break; /* Padding, which every writer leaves and none states. */
    }
    char id[5] = {0};
    memcpy(id, body + at, id_length);
    if (!frame_id_is_valid(id, id_length)) {
      note(diagnostics, at,
          "an ID3v2 frame identifier is not A-Z0-9, so the frame table has "
          "lost its place; the rest of the tag is ignored");
      break;
    }

    uint32_t frame_size;
    unsigned frame_flags = 0;
    if (major == 2) {
      frame_size = ((uint32_t)body[at + 3] << 16)
          | ((uint32_t)body[at + 4] << 8) | (uint32_t)body[at + 5];
    }
    else if (major == 3) {
      /* Plain, not syncsafe. This is the line that is wrong in most
       * hand-written ID3 readers. */
      frame_size = be32(body + at + 4);
      frame_flags = ((unsigned)body[at + 8] << 8) | body[at + 9];
    }
    else {
      frame_size = syncsafe(body + at + 4);
      frame_flags = ((unsigned)body[at + 8] << 8) | body[at + 9];
    }
    if (frame_size == 0 || at + frame_header + frame_size > length) {
      note(diagnostics, at,
          "an ID3v2 frame claims more bytes than the tag holds; the rest of "
          "the tag is ignored");
      break;
    }

    unsigned char * frame = body + at + frame_header;
    size_t frame_length = frame_size;

    /* 2.4's per-frame flags. Compression and encryption are declined by
     * name and kept raw - a frame this library cannot read is still a
     * frame it must not lose. */
    if (major == 4) {
      if (frame_flags & 0x0002u) {
        frame_length = unsynchronise(frame, frame_length);
      }
      if (frame_flags & 0x0008u || frame_flags & 0x0004u) {
        gaud_meta_raw_attach_flagged(meta, "id3v2", id, frame, frame_length,
            normalise_frame_flags(major, frame_flags));
        at += frame_header + frame_size;
        continue;
      }
    }
    else if (major == 3 && (frame_flags & 0x0080u || frame_flags & 0x0040u)) {
      /* Compressed or encrypted. Kept with its flags: they are why it is
       * raw, and a copy written back without them is relabelled as an
       * ordinary frame that the next reader then fails to parse. */
      gaud_meta_raw_attach_flagged(meta, "id3v2", id, frame, frame_length,
          normalise_frame_flags(major, frame_flags));
      at += frame_header + frame_size;
      continue;
    }

    bool is_text = id[0] == 'T';
    bool is_txxx = memcmp(id, major == 2 ? "TXX" : "TXXX", id_length) == 0;
    bool is_comment = memcmp(id, major == 2 ? "COM" : "COMM", id_length) == 0;
    bool is_lyrics = memcmp(id, major == 2 ? "ULT" : "USLT", id_length) == 0;
    bool is_picture = memcmp(id, major == 2 ? "PIC" : "APIC", id_length) == 0;

    /*
     * A frame whose type is known but whose body is too short to hold
     * what that type requires is **kept raw, not dropped**. The comment
     * on the default branch below says a frame this library cannot read
     * is still one it must not lose, and that has to apply to a
     * malformed COMM as much as to an unrecognised one.
     *
     * The fuzzer found this as an instability rather than as a loss: a
     * one-byte COMM was kept raw for its flags, written back, and then
     * silently discarded on the second read - so build, re-parse,
     * re-build lost a frame per generation, and nothing that looked at
     * a single pass would have noticed.
     */
    size_t shortest = 0;
    if (is_picture) {
      shortest = 4;
    }
    else if (is_comment || is_lyrics) {
      shortest = 5; /* encoding, three-byte language, a terminator */
    }
    else if (is_txxx) {
      shortest = 2; /* encoding and a terminator */
    }
    else if (is_text) {
      shortest = 1; /* the encoding byte */
    }
    if (shortest > 0 && frame_length < shortest) {
      note(diagnostics, at,
          "an ID3v2 frame is too short for its own type; it is kept "
          "verbatim rather than discarded");
      gaud_meta_raw_attach_flagged(meta, "id3v2", id, frame, frame_length,
          normalise_frame_flags(major, frame_flags));
      at += frame_header + frame_size;
      continue;
    }

    if (is_picture) {
      read_picture(meta, allocator, frame, frame_length, major == 2, limits,
          diagnostics);
    }
    else if (is_comment || is_lyrics) {
      /* Both are: encoding, a three-byte language, a terminated short
       * description, then the text. The description is not the value,
       * and a reader that takes the first string gets "" for every
       * comment iTunes ever wrote. */
      if (frame_length >= 5) {
        unsigned encoding = frame[0];
        size_t body_at = 4;
        size_t description_end = gaud_id3_find_terminator(
            frame + body_at, frame_length - body_at, encoding);
        body_at += description_end + gaud_id3_terminator_width(encoding);
        if (body_at <= frame_length) {
          char * text = gaud_id3_decode_text(
              allocator, encoding, frame + body_at, frame_length - body_at);
          if (text) {
            gaud_meta_add_unique(
                meta, is_lyrics ? GAUD_TAG_LYRICS : GAUD_TAG_COMMENT, text);
            gcu_allocator_free(allocator, text);
          }
        }
      }
    }
    else if (is_txxx) {
      /* A user-defined key: encoding, terminated description, value. Its
       * description IS the key, which is what makes TXXX the place
       * ReplayGain and MusicBrainz identifiers live. */
      if (frame_length >= 2) {
        unsigned encoding = frame[0];
        size_t key_end
            = gaud_id3_find_terminator(frame + 1, frame_length - 1, encoding);
        char * key = gaud_id3_decode_text(allocator, encoding, frame + 1,
            key_end);
        size_t value_at = 1 + key_end + gaud_id3_terminator_width(encoding);
        if (key && value_at <= frame_length) {
          char * value = gaud_id3_decode_text(allocator, encoding,
              frame + value_at, frame_length - value_at);
          if (value) {
            gaud_meta_custom_add_unique(meta, key, value);
            gcu_allocator_free(allocator, value);
          }
        }
        gcu_allocator_free(allocator, key);
      }
    }
    else if (is_text && frame_length >= 1) {
      unsigned encoding = frame[0];
      /* 2.4 allows several values in one frame, separated by the
       * encoding's terminator. 2.3 does not, and a 2.3 frame with an
       * embedded zero is one value that happens to contain it - so the
       * split is version-gated rather than applied to everything. */
      size_t value_at = 1;
      GAUD_Tag tag;
      bool mapped = gaud_id3_frame_tag(id, id_length, &tag);
      while (value_at < frame_length) {
        size_t end = major >= 4
            ? gaud_id3_find_terminator(
                  frame + value_at, frame_length - value_at, encoding)
            : frame_length - value_at;
        char * text
            = gaud_id3_decode_text(allocator, encoding, frame + value_at, end);
        if (!text) {
          break;
        }
        if (text[0] != '\0') {
          if (!mapped) {
            gaud_meta_custom_add_unique(meta, id, text);
          }
          else if (tag == GAUD_TAG_TRACK_NUMBER) {
            split_pair(meta, text, GAUD_TAG_TRACK_NUMBER,
                GAUD_TAG_TRACK_TOTAL);
          }
          else if (tag == GAUD_TAG_DISC_NUMBER) {
            split_pair(meta, text, GAUD_TAG_DISC_NUMBER, GAUD_TAG_DISC_TOTAL);
          }
          else if (tag == GAUD_TAG_GENRE) {
            add_genre(meta, text);
          }
          else {
            gaud_meta_add_unique(meta, tag, text);
          }
        }
        gcu_allocator_free(allocator, text);
        value_at += end + gaud_id3_terminator_width(encoding);
        if (major < 4) {
          break;
        }
      }
    }
    else {
      /* Everything else, kept byte for byte so a round trip puts it
       * back. This is where replay gain, ownership and the eighty frames
       * nobody maps end up - with their flags, for the same reason the
       * branches above keep theirs. */
      gaud_meta_raw_attach_flagged(meta, "id3v2", id, frame, frame_length,
          normalise_frame_flags(major, frame_flags));
    }

    at += frame_header + frame_size;
  }

  /* And once more, because the last frame's yield is not seen by the
   * check at the top of an iteration that never happens. */
  if (entry_count(meta) > limits->max_metadata_entries) {
    gcu_allocator_free(allocator, body);
    return GAUD_ERR_LIMIT;
  }
  gcu_allocator_free(allocator, body);
  return GAUD_OK;
}

/* ---------------------------------------------------------------- ID3v1 */

/*
 * The genre list. Numbers 0-79 are ID3v1's own; the rest are Winamp's
 * successive extensions, which every tool implements and no
 * specification contains.
 *
 * **Derived, not transcribed.** The first draft of this table was typed
 * out and had 148 entries where mutagen has 192 - the tail is a decade
 * of Winamp additions that no document collects. Typing it and reading
 * it back over would not have found that, because the error was an
 * absence. So the table is generated from mutagen's `TCON.GENRES`, which
 * is the list every tagger actually implements, and `make check-tags`
 * compares the two entry by entry on every run: a divergence is one
 * failing row naming the index, rather than a wrong genre nobody notices
 * for years.
 */
static const char * const genres[] = {
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk",
    "Grunge", "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies", "Other",
    "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial",
    "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack",
    "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk", "Fusion",
    "Trance", "Classical", "Instrumental", "Acid", "House", "Game",
    "Sound Clip", "Gospel", "Noise", "Alt. Rock", "Bass", "Soul", "Punk",
    "Space", "Meditative", "Instrumental Pop", "Instrumental Rock",
    "Ethnic", "Gothic", "Darkwave", "Techno-Industrial", "Electronic",
    "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult",
    "Gangsta Rap", "Top 40", "Christian Rap", "Pop/Funk", "Jungle",
    "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave",
    "Showtunes", "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz",
    "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock", "Folk",
    "Folk-Rock", "National Folk", "Swing", "Fast-Fusion", "Bebop",
    "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde",
    "Gothic Rock", "Progressive Rock", "Psychedelic Rock",
    "Symphonic Rock", "Slow Rock", "Big Band", "Chorus", "Easy Listening",
    "Acoustic", "Humour", "Speech", "Chanson", "Opera", "Chamber Music",
    "Sonata", "Symphony", "Booty Bass", "Primus", "Porn Groove", "Satire",
    "Slow Jam", "Club", "Tango", "Samba", "Folklore", "Ballad",
    "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet", "Punk Rock",
    "Drum Solo", "A Cappella", "Euro-House", "Dance Hall", "Goa",
    "Drum & Bass", "Club-House", "Hardcore", "Terror", "Indie", "BritPop",
    "Afro-Punk", "Polsk Punk", "Beat", "Christian Gangsta Rap",
    "Heavy Metal", "Black Metal", "Crossover", "Contemporary Christian",
    "Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime",
    "JPop", "Synthpop", "Abstract", "Art Rock", "Baroque", "Bhangra",
    "Big Beat", "Breakbeat", "Chillout", "Downtempo", "Dub", "EBM",
    "Eclectic", "Electro", "Electroclash", "Emo", "Experimental",
    "Garage", "Global", "IDM", "Illbient", "Industro-Goth", "Jam Band",
    "Krautrock", "Leftfield", "Lounge", "Math Rock", "New Romantic",
    "Nu-Breakz", "Post-Punk", "Post-Rock", "Psytrance", "Shoegaze",
    "Space Rock", "Trop Rock", "World Music", "Neoclassical", "Audiobook",
    "Audio Theatre", "Neue Deutsche Welle", "Podcast", "Indie Rock",
    "G-Funk", "Dubstep", "Garage Rock", "Psybient"};

const char * gaud_id3v1_genre(unsigned index) {
  if (index >= sizeof(genres) / sizeof(genres[0])) {
    return NULL;
  }
  return genres[index];
}

unsigned gaud_id3v1_genre_count(void) {
  return (unsigned)(sizeof(genres) / sizeof(genres[0]));
}

/**
 * Copy a fixed-width ID3v1 field, trimming the padding.
 *
 * The fields are 30 bytes and are padded with spaces by some writers and
 * with zeros by others - and a few with zeros followed by whatever was in
 * the buffer, which is why the scan stops at the first zero rather than
 * trimming from the end.
 */
static void add_fixed(GAUD_Meta * meta, const GAUD_Allocator * allocator,
    GAUD_Tag tag, const unsigned char * field, size_t width) {
  size_t length = 0;
  while (length < width && field[length] != 0) {
    ++length;
  }
  while (length > 0 && field[length - 1] == ' ') {
    --length;
  }
  if (length == 0) {
    return;
  }
  /* ID3v1 is Latin-1 by specification. In practice it is whatever the
   * tagger's code page was, and there is no field saying which - so
   * Latin-1 it is, which at least never produces invalid UTF-8. */
  char * text = gaud_id3_latin1_to_utf8(allocator, field, length);
  if (text) {
    gaud_meta_add_unique(meta, tag, text);
    gcu_allocator_free(allocator, text);
  }
}

bool gaud_id3v1_parse(const unsigned char data[128], GAUD_Meta * meta) {
  if (!data || !meta || memcmp(data, "TAG", 3) != 0) {
    return false;
  }
  const GAUD_Allocator * allocator = gaud_meta_allocator(meta);

  /* Only fields nothing else supplied. An ID3v2 block is read first and
   * is richer; a file carrying both usually carries the same strings
   * truncated to 30 characters here, and letting those overwrite would
   * silently shorten every tag. */
  if (gaud_meta_count(meta, GAUD_TAG_TITLE) == 0) {
    add_fixed(meta, allocator, GAUD_TAG_TITLE, data + 3, 30);
  }
  if (gaud_meta_count(meta, GAUD_TAG_ARTIST) == 0) {
    add_fixed(meta, allocator, GAUD_TAG_ARTIST, data + 33, 30);
  }
  if (gaud_meta_count(meta, GAUD_TAG_ALBUM) == 0) {
    add_fixed(meta, allocator, GAUD_TAG_ALBUM, data + 63, 30);
  }
  if (gaud_meta_count(meta, GAUD_TAG_DATE) == 0) {
    add_fixed(meta, allocator, GAUD_TAG_DATE, data + 93, 4);
  }

  /* ID3v1.1 stole the last two bytes of the comment for a track number:
   * a zero at offset 125 and the number at 126. A v1.0 comment uses all
   * 30, so reading the track unconditionally invents one from whatever
   * character happened to be last. */
  bool has_track = data[125] == 0 && data[126] != 0;
  if (gaud_meta_count(meta, GAUD_TAG_COMMENT) == 0) {
    add_fixed(meta, allocator, GAUD_TAG_COMMENT, data + 97,
        has_track ? 28u : 30u);
  }
  if (has_track && gaud_meta_count(meta, GAUD_TAG_TRACK_NUMBER) == 0) {
    char number[8];
    snprintf(number, sizeof(number), "%u", (unsigned)data[126]);
    gaud_meta_add_unique(meta, GAUD_TAG_TRACK_NUMBER, number);
  }
  if (gaud_meta_count(meta, GAUD_TAG_GENRE) == 0) {
    const char * name = gaud_id3v1_genre(data[127]);
    if (name) {
      gaud_meta_add_unique(meta, GAUD_TAG_GENRE, name);
    }
  }
  return true;
}
