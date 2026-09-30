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
 * ID3v2's four text encodings, converted to UTF-8.
 *
 * This is the part of ID3 that is actually hard, and it is hard because
 * the format has four answers and two of them are UTF-16:
 *
 *   0  ISO-8859-1   one byte a character, and **not** ASCII: 0xA9 is (c)
 *   1  UTF-16       with a byte-order mark, either order
 *   2  UTF-16BE     without one, 2.4 only
 *   3  UTF-8        2.4 only
 *
 * planning/audio.md 8 settled that this needs no new dependency. `unicode`
 * has UTF-8 and codepoints but neither Latin-1 nor UTF-16, and depending on
 * `text` for two short loops would drag in `chron`, `unicode` and `regex`.
 * So the loops are here.
 *
 * Three things a Latin-1 converter gets wrong, all of them here:
 *
 * - **Latin-1 is not ASCII.** Bytes 0x80-0xFF are characters, and copying
 *   them through unchanged produces invalid UTF-8 rather than mojibake -
 *   which means a caller's UTF-8 validator rejects the whole string.
 * - **A surrogate pair is one character.** UTF-16 above the BMP is two
 *   units, and encoding each separately produces CESU-8, which is not
 *   UTF-8 and which some readers accept, so the mistake survives.
 * - **An unpaired surrogate has to go somewhere.** It cannot be encoded,
 *   and dropping the string loses a tag over one bad byte. U+FFFD is what
 *   goes out, which is what every other converter in this tree does.
 */

#include "id3_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/** Append one codepoint to @p out as UTF-8. Returns bytes written. */
static size_t put_utf8(unsigned char * out, uint32_t code) {
  if (code < 0x80u) {
    out[0] = (unsigned char)code;
    return 1;
  }
  if (code < 0x800u) {
    out[0] = (unsigned char)(0xC0u | (code >> 6));
    out[1] = (unsigned char)(0x80u | (code & 0x3Fu));
    return 2;
  }
  if (code < 0x10000u) {
    out[0] = (unsigned char)(0xE0u | (code >> 12));
    out[1] = (unsigned char)(0x80u | ((code >> 6) & 0x3Fu));
    out[2] = (unsigned char)(0x80u | (code & 0x3Fu));
    return 3;
  }
  out[0] = (unsigned char)(0xF0u | (code >> 18));
  out[1] = (unsigned char)(0x80u | ((code >> 12) & 0x3Fu));
  out[2] = (unsigned char)(0x80u | ((code >> 6) & 0x3Fu));
  out[3] = (unsigned char)(0x80u | (code & 0x3Fu));
  return 4;
}

/** How many bytes @p code occupies in UTF-8. */
static size_t utf8_length(uint32_t code) {
  if (code < 0x80u) {
    return 1;
  }
  if (code < 0x800u) {
    return 2;
  }
  if (code < 0x10000u) {
    return 3;
  }
  return 4;
}

char * gaud_id3_latin1_to_utf8(
    const GAUD_Allocator * allocator, const unsigned char * in, size_t size) {
  size_t needed = 1; /* the terminator */
  for (size_t i = 0; i < size; ++i) {
    /* Every Latin-1 byte IS its Unicode codepoint, which is the one
     * pleasant fact about the encoding, so the length is one or two. */
    needed += in[i] < 0x80u ? 1u : 2u;
  }
  char * out = gcu_allocator_malloc(allocator, needed);
  if (!out) {
    return NULL;
  }
  size_t at = 0;
  for (size_t i = 0; i < size; ++i) {
    at += put_utf8((unsigned char *)out + at, in[i]);
  }
  out[at] = '\0';
  return out;
}

char * gaud_id3_utf16_to_utf8(const GAUD_Allocator * allocator,
    const unsigned char * in, size_t size, bool big_endian,
    bool allow_bom) {
  size_t at = 0;
  if (allow_bom && size >= 2) {
    if (in[0] == 0xFFu && in[1] == 0xFEu) {
      big_endian = false;
      at = 2;
    }
    else if (in[0] == 0xFEu && in[1] == 0xFFu) {
      big_endian = true;
      at = 2;
    }
    /* No mark on an encoding that requires one. The specification says
     * the frame is invalid; every real reader guesses, and guessing
     * little-endian is what the overwhelming majority of such files
     * are - they come from Windows taggers. The caller's default is
     * kept rather than overridden, so a 2.4 UTF-16BE frame with no mark
     * still reads as big-endian. */
  }

  /* Two passes so the buffer is exact: sized wrong is either a waste or
   * an overflow, and a surrogate pair makes the ratio non-constant. */
  size_t needed = 1;
  for (size_t i = at; i + 1 < size; i += 2) {
    uint32_t unit = big_endian ? (uint32_t)((in[i] << 8) | in[i + 1])
                               : (uint32_t)((in[i + 1] << 8) | in[i]);
    if (unit >= 0xD800u && unit <= 0xDBFFu && i + 3 < size) {
      uint32_t low = big_endian
          ? (uint32_t)((in[i + 2] << 8) | in[i + 3])
          : (uint32_t)((in[i + 3] << 8) | in[i + 2]);
      if (low >= 0xDC00u && low <= 0xDFFFu) {
        needed += 4; /* One character, four bytes. */
        i += 2;
        continue;
      }
    }
    if (unit >= 0xD800u && unit <= 0xDFFFu) {
      needed += 3; /* U+FFFD, for a surrogate with no partner. */
      continue;
    }
    needed += utf8_length(unit);
  }

  char * out = gcu_allocator_malloc(allocator, needed);
  if (!out) {
    return NULL;
  }
  size_t written = 0;
  for (size_t i = at; i + 1 < size; i += 2) {
    uint32_t unit = big_endian ? (uint32_t)((in[i] << 8) | in[i + 1])
                               : (uint32_t)((in[i + 1] << 8) | in[i]);
    if (unit >= 0xD800u && unit <= 0xDBFFu && i + 3 < size) {
      uint32_t low = big_endian
          ? (uint32_t)((in[i + 2] << 8) | in[i + 3])
          : (uint32_t)((in[i + 3] << 8) | in[i + 2]);
      if (low >= 0xDC00u && low <= 0xDFFFu) {
        uint32_t code
            = 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u);
        written += put_utf8((unsigned char *)out + written, code);
        i += 2;
        continue;
      }
    }
    if (unit >= 0xD800u && unit <= 0xDFFFu) {
      written += put_utf8((unsigned char *)out + written, 0xFFFDu);
      continue;
    }
    written += put_utf8((unsigned char *)out + written, unit);
  }
  out[written] = '\0';
  return out;
}

char * gaud_id3_bytes_to_utf8(const GAUD_Allocator * allocator,
    const unsigned char * in, size_t size);

char * gaud_id3_decode_text(const GAUD_Allocator * allocator,
    unsigned encoding, const unsigned char * in, size_t size) {
  switch (encoding) {
  case 0: return gaud_id3_latin1_to_utf8(allocator, in, size);
  case 1: return gaud_id3_utf16_to_utf8(allocator, in, size, false, true);
  case 2: return gaud_id3_utf16_to_utf8(allocator, in, size, true, false);
  case 3:
    /*
     * The frame says UTF-8. It is checked anyway, and read as Latin-1
     * when it is not.
     *
     * The first draft copied these bytes through unvalidated, reasoning
     * that a tag this library did not author is the file's business.
     * That is wrong, and meta.h is where it is wrong: "everything
     * crossing this API is UTF-8" is a promise, and a promise that
     * holds only for well-formed input is not one. A caller's own
     * validator rejecting a string this library handed it is the
     * failure, and the fuzzer enforces the promise directly.
     *
     * Falling back to Latin-1 rather than dropping the tag, because a
     * tagger that wrote CP1252 bytes under encoding byte 3 meant the
     * text - and Latin-1 always produces valid UTF-8, so the result is
     * legible-ish rather than absent.
     */
    return gaud_id3_bytes_to_utf8(allocator, in, size);
  default: return NULL;
  }
}

size_t gaud_id3_terminator_width(unsigned encoding) {
  /* A UTF-16 string ends with two zero bytes, not one - and a reader that
   * searches for a single zero finds the high byte of the first ASCII
   * character instead, which truncates every such field to nothing. */
  return (encoding == 1u || encoding == 2u) ? 2u : 1u;
}

size_t gaud_id3_find_terminator(
    const unsigned char * in, size_t size, unsigned encoding) {
  size_t width = gaud_id3_terminator_width(encoding);
  if (width == 1u) {
    for (size_t i = 0; i < size; ++i) {
      if (in[i] == 0) {
        return i;
      }
    }
    return size;
  }
  /* Aligned to the unit: a zero byte at an odd offset is half of a
   * character, not a terminator. */
  for (size_t i = 0; i + 1 < size; i += 2) {
    if (in[i] == 0 && in[i + 1] == 0) {
      return i;
    }
  }
  return size;
}

/**
 * Whether @p in is well-formed UTF-8.
 *
 * Strict: over-long encodings, surrogates and values above U+10FFFF are
 * all rejected, because the point is to tell UTF-8 from Latin-1 and a
 * lax check says yes to text that is neither.
 */
static bool is_utf8(const unsigned char * in, size_t size) {
  size_t i = 0;
  bool any_multibyte = false;
  while (i < size) {
    unsigned char c = in[i];
    if (c < 0x80u) {
      ++i;
      continue;
    }
    size_t need;
    uint32_t code;
    if ((c & 0xE0u) == 0xC0u) {
      need = 1;
      code = c & 0x1Fu;
    }
    else if ((c & 0xF0u) == 0xE0u) {
      need = 2;
      code = c & 0x0Fu;
    }
    else if ((c & 0xF8u) == 0xF0u) {
      need = 3;
      code = c & 0x07u;
    }
    else {
      return false; /* A continuation byte or 0xFE/0xFF leading. */
    }
    if (i + need >= size + 0u && i + need > size - 1u) {
      return false;
    }
    for (size_t k = 1; k <= need; ++k) {
      if ((in[i + k] & 0xC0u) != 0x80u) {
        return false;
      }
      code = (code << 6) | (in[i + k] & 0x3Fu);
    }
    /* Over-long: the shortest form is the only legal one, and an
     * over-long sequence is the classic way to smuggle a "/" past a
     * filter. */
    if ((need == 1 && code < 0x80u) || (need == 2 && code < 0x800u)
        || (need == 3 && code < 0x10000u)) {
      return false;
    }
    if (code > 0x10FFFFu || (code >= 0xD800u && code <= 0xDFFFu)) {
      return false;
    }
    any_multibyte = true;
    i += need + 1u;
  }
  /* Pure ASCII is valid UTF-8 and valid Latin-1 and identical either
   * way, so the answer does not matter; saying true keeps the copy path
   * and avoids a pointless conversion. */
  (void)any_multibyte;
  return true;
}

char * gaud_id3_bytes_to_utf8(
    const GAUD_Allocator * allocator, const unsigned char * in, size_t size) {
  /*
   * RIFF `INFO` and AIFF's text chunks carry no encoding field at all.
   * The specifications predate Unicode and say the file's code page,
   * which in practice means Latin-1 or CP1252 for anything written
   * before about 2005 and UTF-8 for everything since: ffmpeg 7.1.5
   * writes UTF-8 and reads it back, and round-trips Japanese through an
   * INFO chunk, which Latin-1 cannot represent at all.
   *
   * So the bytes decide. Valid UTF-8 is taken as UTF-8 and anything
   * else as Latin-1 - which is safe in the direction that matters,
   * because Latin-1 text with any high byte is almost never valid
   * multi-byte UTF-8, while treating UTF-8 as Latin-1 produces the
   * doubled-up mojibake this library emitted before the rule was
   * measured.
   */
  if (is_utf8(in, size)) {
    char * out = gcu_allocator_malloc(allocator, size + 1u);
    if (!out) {
      return NULL;
    }
    if (size > 0) {
      memcpy(out, in, size);
    }
    out[size] = '\0';
    return out;
  }
  return gaud_id3_latin1_to_utf8(allocator, in, size);
}
