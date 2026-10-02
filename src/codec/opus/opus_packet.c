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
 * Splitting an Opus packet into the frames it carries: RFC 6716 section 3.2.
 *
 * ::gaud_opus_parse_toc already answers "how long is this packet", which
 * is all a reader of the container needs. Decoding needs the stronger
 * answer: where each frame starts and how many bytes are its own.
 *
 * The specification states its framing rules as numbered requirements
 * [R1] to [R7]. They are worth taking literally: several of them exclude
 * packets that parse perfectly well and whose frames would decode,
 * because the point of the rules is that a gateway repacking a stream
 * can rely on them.
 *
 * **Two of them cannot be seen to fail.** [R4] and [R7] bound a frame
 * length against the bytes remaining, and in both cases the neighbouring
 * arithmetic refuses the same inputs a step later - so deleting either
 * changes no verdict, which is how a mutation pass found them. They stay
 * because what they prevent is an unsigned subtraction wrapping, not
 * because a test can tell. Said here rather than left for the next
 * reader to re-derive.
 *
 * **They are not all enforced here**, and the first draft of this file
 * enforced three of them twice. [R1], [R3] and [R5] - a table of
 * contents byte, a code 1 payload that divides in two, and a frame count
 * that is neither zero nor more than 120 ms - are checked by
 * ::gaud_opus_parse_toc, which runs first and *has* to check them,
 * because the frame count it reports is what they constrain. Copying
 * them down here produced three guards no input could reach, which a
 * mutation pass found by removing each in turn and watching the tests
 * carry on passing. What is left below is the four rules that need the
 * frame boundaries: [R2], [R4], [R6] and [R7].
 *
 * **Self-delimiting framing** is the variant the multistream mapping
 * uses, where every stream but the last states its own total length. It
 * is accepted here so that the mapping has one parser rather than two;
 * see RFC 6716 Appendix B.
 */

#include "opus_internal.h"
#include <string.h>

/**
 * One frame length, section 3.2.1.
 *
 * @param data Where the length starts.
 * @param size How many bytes are left.
 * @param out_length Receives the length.
 * @return Bytes consumed, or 0 if there were not enough.
 */
static size_t parse_length(
    const unsigned char * data, size_t size, uint32_t * out_length) {
  if (size < 1u) {
    return 0;
  }
  if (data[0] < 252u) {
    *out_length = data[0];
    return 1u;
  }
  if (size < 2u) {
    return 0;
  }
  /* 252 to 255 means a second byte follows, and the pair reaches 1,275 -
   * which section 3.2.1 notes is about 510 kbit/s at 20 ms, past the
   * point where a lossless codec would be the better answer. */
  *out_length = 4u * (uint32_t)data[1] + (uint32_t)data[0];
  return 2u;
}

GAUD_Result gaud_opus_parse_packet(const unsigned char * data, size_t size,
    bool self_delimited, OPUS_Packet * out) {
  memset(out, 0, sizeof(*out));
  /* [R1], a table of contents byte, comes from the call below. */
  GAUD_Result result = gaud_opus_parse_toc(data, size, &out->toc);
  if (result != GAUD_OK) {
    return result;
  }

  const unsigned char * at = data + 1;
  size_t left = size - 1u;
  uint32_t count = 1;
  /* The length of the frame whose length is never written down: the
   * last one, which takes whatever is left. */
  uint32_t last = (uint32_t)left;
  bool cbr = false;

  switch (out->toc.code) {
  case 0:
    count = 1;
    break;

  case 1:
    count = 2;
    cbr = true;
    if (!self_delimited) {
      /* [R3], the payload dividing in two, is enforced by
       * ::gaud_opus_parse_toc above - it has to be, because the frame
       * count it reports depends on it. Repeating it here produced a
       * guard no input could reach. */
      last = (uint32_t)(left / 2u);
      out->length[0] = last;
    }
    break;

  case 2: {
    count = 2;
    uint32_t first = 0;
    size_t used = parse_length(at, left, &first);
    if (used == 0) {
      return GAUD_ERR_CORRUPT;
    }
    at += used;
    left -= used;
    /*
     * [R4] The stated length must fit in what is left after reading it.
     * This is what makes a two-byte code 2 packet invalid unless both
     * its frames are empty.
     *
     * **Not observable through this function's verdict.** A stated
     * length is at most 1,275, so `left - first` below would wrap to
     * something above 1,275 and [R2] would refuse it anyway - removing
     * this check changes no answer, which a mutation pass confirmed.
     * It is kept so the subtraction never wraps in the first place.
     */
    if (first > left) {
      return GAUD_ERR_CORRUPT;
    }
    out->length[0] = first;
    last = (uint32_t)(left - first);
    break;
  }

  default: {
    /* [R6, R7] Code 3 needs a frame count byte at the very least. */
    if (left < 1u) {
      return GAUD_ERR_CORRUPT;
    }
    unsigned char control = *at++;
    --left;
    /* [R5] - a non-zero count, and no more than 120 ms of audio - is
     * likewise already enforced by ::gaud_opus_parse_toc, for the same
     * reason: it is the count. So this reads the field and trusts it,
     * and the loops below are bounded by OPUS_MAX_FRAMES because the
     * field is six bits wide. */
    count = (uint32_t)(control & 0x3Fu);
    if ((control & 0x40u) != 0) {
      /*
       * Padding, section 3.2.5. A byte of 255 means 254 bytes of
       * padding and another length byte after it, so the loop below
       * subtracts both the length byte and what it describes - which
       * is what makes `P` of the specification's [R6] the total number
       * of bytes added rather than only the padding itself.
       */
      unsigned value;
      do {
        if (left < 1u) {
          return GAUD_ERR_CORRUPT;
        }
        value = *at++;
        --left;
        size_t amount = value == 255u ? 254u : value;
        if (amount > left) {
          return GAUD_ERR_CORRUPT;
        }
        left -= amount;
      } while (value == 255u);
    }
    cbr = (control & 0x80u) == 0;
    if (!cbr) {
      /* Variable rate: every frame but the last states its length. */
      last = (uint32_t)left;
      for (uint32_t i = 0; i + 1u < count; ++i) {
        uint32_t length = 0;
        size_t used = parse_length(at, left, &length);
        if (used == 0) {
          return GAUD_ERR_CORRUPT;
        }
        at += used;
        left -= used;
        /* [R7] Each length, and the bytes spent stating it, must fit.
         * Redundant in the same way [R4] is: the running total below
         * refuses whatever this would, so removing it changes no
         * answer. Kept so `last` is never decremented past zero. */
        if (length > left) {
          return GAUD_ERR_CORRUPT;
        }
        out->length[i] = length;
        if ((uint32_t)(used + length) > last) {
          return GAUD_ERR_CORRUPT;
        }
        last -= (uint32_t)(used + length);
      }
    } else if (!self_delimited) {
      /* [R6] Constant rate: what is left must divide by the count.
       * The `count` test guards the division rather than the format -
       * the same byte was refused above if it was zero - and is kept
       * because a division by zero is worse than a dead branch. */
      if (count == 0 || left % count != 0) {
        return GAUD_ERR_CORRUPT;
      }
      last = (uint32_t)(left / count);
      for (uint32_t i = 0; i + 1u < count; ++i) {
        out->length[i] = last;
      }
    }
    break;
  }
  }

  if (self_delimited) {
    /*
     * Appendix B: the last frame's length is stated too, and whatever
     * follows the packet belongs to the next stream rather than to this
     * frame. For a code 1 or constant-rate code 3 packet that one
     * length is every frame's length.
     */
    uint32_t length = 0;
    size_t used = parse_length(at, left, &length);
    if (used == 0) {
      return GAUD_ERR_CORRUPT;
    }
    at += used;
    left -= used;
    if (length > left) {
      return GAUD_ERR_CORRUPT;
    }
    last = length;
    if (cbr) {
      for (uint32_t i = 0; i + 1u < count; ++i) {
        out->length[i] = last;
      }
      if ((uint64_t)last * count > left) {
        return GAUD_ERR_CORRUPT;
      }
    }
  }

  /* [R2] No frame may exceed 1,275 bytes, so that a gateway repacking
   * the stream can rely on a bound. Only the inferred length needs the
   * check; a stated one cannot encode a larger number. */
  if (last > OPUS_MAX_FRAME_BYTES) {
    return GAUD_ERR_CORRUPT;
  }
  out->length[count - 1u] = last;

  out->count = count;
  for (uint32_t i = 0; i < count; ++i) {
    out->frame[i] = at;
    at += out->length[i];
  }
  /* The frames must all lie inside the packet. Every branch above has
   * checked its own arithmetic, so reaching this with a frame past the
   * end would be a defect here rather than a malformed packet - but a
   * packet parser is the wrong place to find that out from a crash. */
  if ((size_t)(at - data) > size) {
    return GAUD_ERR_CORRUPT;
  }
  out->payload_offset = (size_t)(out->frame[0] - data);
  return GAUD_OK;
}
