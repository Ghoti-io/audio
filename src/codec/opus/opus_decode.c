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
 * The track decoder: Ogg packets in, interleaved 16-bit samples out.
 * RFC 7845 around RFC 6716. Never installed.
 *
 * Everything the codec itself does is in opus_decoder.c; this is the
 * part the container asks for. Four things are its own.
 *
 * **The first samples are not the recording's.** `OpusHead` states a
 * pre-skip, the number of samples at 48 kHz the encoder's filters needed
 * and the recording does not contain, so that many are decoded and
 * dropped before the first one is handed out. It is dropped *after* the
 * decoder has run on them, never by starting later, because the state
 * those samples build is what the rest depends on.
 *
 * **The last samples may not be the recording's either.** A packet is
 * 2.5 to 120 ms and the recording is whatever length it is, so the last
 * page's granule position says where the audio really ends and
 * everything decoded beyond it is padding. That is what the length the
 * track reports already assumes, and this is where it is enforced.
 *
 * **One Ogg packet may hold several streams.** Channel mapping families
 * 1 and 255 multiplex up to 255 Opus streams, and all but the last are
 * written with Appendix B's self-delimiting framing so that a reader
 * can find where each one ends. Some streams are stereo pairs and some
 * are single channels, and the channel table says which decoded channel
 * feeds which output; a channel mapped to 255 is silent.
 *
 * **A seek bisects, and its samples are not a straight read's.** An Opus
 * decoder's state converges rather than resets, so a decoder started
 * part-way through produces samples that differ from a straight read's
 * in their least significant bits until it has converged. RFC 7845
 * section 4.6 says to decode 80 ms of pre-roll before the target and
 * discard it, and that is what happens: the page bisection finds the
 * last page at or before 80 ms ahead of the target, every decoder is
 * reset there, and the packets from it are decoded and dropped until the
 * target. What is given up is bit-exactness with a straight read, and
 * only after a seek that landed part-way; a seek that lands in the first
 * 80 ms, or forwards by less than 160 ms, or onto a page that begins with
 * the tail of a packet, still decodes on from where it is or from the
 * start and is exact.
 */

#include "opus_decoder.h"
#include "../../container/ogg/ogg.h"
#include <string.h>

/** RFC 7845's pre-roll: 80 ms at 48 kHz, decoded and dropped after a jump. */
#define OPUS_PREROLL 3840u

/** The most earlier pages a landing on a continued packet is moved back by. */
#define OPUS_LAND_TRIES 4u

/** The unit of the output: interleaved signed 16-bit. */
typedef struct {
  const GAUD_Allocator * allocator; ///< What everything here came from.
  OPUS_File * file;                 ///< The document's state, borrowed.
  OGG_Reader reader;                ///< Where the packets come from.
  OPUS_Decoder ** streams;          ///< One per stream in the file.
  uint32_t stream_count;            ///< How many.
  uint32_t channels;                ///< Output channels.
  int16_t * scratch;                ///< One stream's decode, for the mix.
  int16_t * pending;                ///< The packet decoded, mixed to output.
  uint32_t pending_frames;          ///< Its length.
  uint32_t pending_used;            ///< How much of it was handed out.
  uint64_t position;                ///< Frames handed out so far.
  uint64_t skip;                    ///< Pre-skip frames still to drop.
  bool ended;                       ///< The stream has no more packets.
  /** For output channel `slot`: which stream and which of its channels. */
  struct {
    int16_t stream;                 ///< Stream index, or -1 for silence.
    uint8_t channel;                ///< Channel within that stream, 0 or 1.
  } route[OPUS_MAX_CHANNELS];
} OPUS_Player;

static void player_close(GAUD_Decoder * decoder) {
  OPUS_Player * player = gaud_decoder_private(decoder);
  if (!player) {
    return;
  }
  const GAUD_Allocator * allocator = player->allocator;
  gaud_ogg_reader_free(&player->reader);
  if (player->streams) {
    for (uint32_t i = 0; i < player->stream_count; ++i) {
      gaud_opus_decoder_destroy(allocator, player->streams[i]);
    }
  }
  gcu_allocator_free(allocator, player->streams);
  gcu_allocator_free(allocator, player->scratch);
  gcu_allocator_free(allocator, player->pending);
  gcu_allocator_free(allocator, player);
}

/** Put every decoder back to where a stream starts. */
static GAUD_Result rewind_player(OPUS_Player * player) {
  for (uint32_t i = 0; i < player->stream_count; ++i) {
    gaud_opus_decoder_reset(player->streams[i]);
  }
  player->pending_frames = 0;
  player->pending_used = 0;
  player->position = 0;
  player->skip = player->file->head.pre_skip;
  player->ended = false;
  return gaud_ogg_reader_seek(&player->reader, player->file->audio_offset);
}

/**
 * Decode the next packet into ::OPUS_Player::pending, mixed to the
 * output's channels and with the pre-skip dropped.
 */
static GAUD_Result decode_next(OPUS_Player * player) {
  const OPUS_Head * head = &player->file->head;
  for (;;) {
    const unsigned char * data = NULL;
    size_t size = 0;
    GAUD_Result result = gaud_ogg_reader_packet(
        &player->reader, &data, &size, NULL, NULL);
    if (result != GAUD_OK) {
      player->ended = true;
      return result;
    }
    if (size == 0) {
      continue;
    }
    // The reader starts on the page the headers ended on, which may be
    // the comment header's: skip a header rather than decode it. Neither
    // magic can begin an audio packet, because the bytes after the first
    // would state a code-3 packet of 48 frames, which is 960 ms and more
    // than a packet may hold.
    if (size >= OPUS_MAGIC_SIZE
        && (memcmp(data, OPUS_TAGS_MAGIC, OPUS_MAGIC_SIZE) == 0
            || memcmp(data, OPUS_HEAD_MAGIC, OPUS_MAGIC_SIZE) == 0)) {
      continue;
    }
    uint32_t frames = 0;
    size_t at = 0;
    for (uint32_t s = 0; s < player->stream_count; ++s) {
      bool last = s + 1u == player->stream_count;
      uint32_t got = 0;
      size_t used = 0;
      if (at >= size) {
        return GAUD_ERR_CORRUPT;
      }
      result = gaud_opus_decode_stream_packet(player->streams[s], data + at,
          size - at, !last, player->scratch, OPUS_MAX_PACKET_SAMPLES, &got,
          &used);
      if (result != GAUD_OK) {
        return result == GAUD_ERR_INVALID ? GAUD_ERR_CORRUPT : result;
      }
      if (s == 0) {
        frames = got;
      }
      else if (got != frames) {
        // Every stream of a packet must be as long as the others.
        return GAUD_ERR_CORRUPT;
      }
      at += used;
      uint32_t stream_channels = player->streams[s]->channels;
      for (uint32_t slot = 0; slot < player->channels; ++slot) {
        if (player->route[slot].stream != (int16_t)s) {
          continue;
        }
        uint32_t from = player->route[slot].channel;
        for (uint32_t i = 0; i < got; ++i) {
          player->pending[(size_t)i * player->channels + slot]
              = player->scratch[(size_t)i * stream_channels + from];
        }
      }
    }
    for (uint32_t slot = 0; slot < player->channels; ++slot) {
      if (player->route[slot].stream < 0) {
        for (uint32_t i = 0; i < frames; ++i) {
          player->pending[(size_t)i * player->channels + slot] = 0;
        }
      }
    }
    gaud_opus_apply_gain(
        player->pending, (size_t)frames * player->channels, head->output_gain);
    player->pending_frames = frames;
    player->pending_used = 0;
    if (player->skip > 0) {
      uint32_t drop = player->skip < frames ? (uint32_t)player->skip : frames;
      player->pending_used = drop;
      player->skip -= drop;
    }
    return GAUD_OK;
  }
}

static GAUD_Result player_read(GAUD_Decoder * decoder, GAUD_Buffer * buffer) {
  OPUS_Player * player = gaud_decoder_private(decoder);
  size_t capacity = gaud_buffer_capacity(buffer);
  uint32_t channels = player->channels;
  int16_t * out = (int16_t *)(void *)gaud_buffer_data(buffer);
  size_t filled = 0;

  while (filled < capacity) {
    if (player->pending_used >= player->pending_frames) {
      if (player->ended) {
        break;
      }
      GAUD_Result result = decode_next(player);
      if (result == GAUD_ERR_FORMAT) {
        break; // The end of the audio.
      }
      if (result != GAUD_OK) {
        return result;
      }
      continue;
    }
    size_t available = player->pending_frames - player->pending_used;
    size_t take = capacity - filled < available ? capacity - filled : available;
    memcpy(out + filled * channels,
        player->pending + (size_t)player->pending_used * channels,
        take * channels * sizeof *out);
    player->pending_used += (uint32_t)take;
    filled += take;
    player->position += take;
  }

  // The tail trim: the last page's granule position is the length, and
  // anything past it is the padding the encoder added to fill a packet.
  uint64_t total = gaud_track_frames(gaud_decoder_track(decoder));
  if (total != UINT64_MAX && player->position > total) {
    uint64_t excess = player->position - total;
    filled = excess >= filled ? 0 : filled - (size_t)excess;
    player->position = total;
  }
  gaud_decoder_set_position(decoder, player->position);
  return gaud_buffer_set_frames(buffer, filled);
}

/**
 * Put the decoders where a seek to @p frame can begin: the last page at
 * least ::OPUS_PREROLL before it, every decoder reset there, or the
 * start of the stream where that is no better. Does nothing when the
 * place found is behind where the decoders already are and the target is
 * ahead of them, because decoding on is then both faster and exact.
 */
static GAUD_Result land_near(OPUS_Player * player, uint64_t frame) {
  uint64_t pre_skip = player->file->head.pre_skip;
  uint64_t want = frame + pre_skip;
  uint64_t target = want > OPUS_PREROLL ? want - OPUS_PREROLL : 0u;
  uint64_t offset = 0;
  uint64_t granule = 0;
  // A page whose first packet began on the page before cannot be landed
  // on, because that packet is dropped and its samples would be missing
  // from the position; step back to an earlier page instead.
  for (unsigned tries = 0; tries < OPUS_LAND_TRIES && target > pre_skip;
      ++tries) {
    if (gaud_ogg_bisect(&player->reader, target, player->file->audio_offset,
            &offset, &granule)
        != GAUD_OK) {
      break;
    }
    if (granule <= pre_skip) {
      break;
    }
    OGG_Page_Info page;
    if (gaud_ogg_find_page(player->reader.stream, player->allocator, offset,
            offset + 1u, &page)
            != GAUD_OK
        || (page.flags & OGG_FLAG_CONTINUED)) {
      target = granule - 1u;
      continue;
    }
    uint64_t landed = granule - pre_skip;
    if (frame >= player->position && landed <= player->position) {
      return GAUD_OK; // Decoding on is already as near and is exact.
    }
    for (uint32_t i = 0; i < player->stream_count; ++i) {
      gaud_opus_decoder_reset(player->streams[i]);
    }
    player->pending_frames = 0;
    player->pending_used = 0;
    player->position = landed;
    player->skip = 0;
    player->ended = false;
    return gaud_ogg_reader_seek(&player->reader, offset);
  }
  if (frame < player->position) {
    return rewind_player(player);
  }
  return GAUD_OK;
}

static GAUD_Result player_seek(
    GAUD_Decoder * decoder, uint64_t frame, uint64_t * out_landed) {
  OPUS_Player * player = gaud_decoder_private(decoder);
  uint64_t total = gaud_track_frames(gaud_decoder_track(decoder));
  if (total != UINT64_MAX && frame > total) {
    frame = total;
  }
  // Where the state is now is a position on the decoder's own timeline;
  // a jump is only worth its pre-roll when it moves the state further
  // forward than decoding on would.
  if (frame < player->position || frame - player->position > 2u * OPUS_PREROLL) {
    GAUD_Result result = land_near(player, frame);
    if (result != GAUD_OK) {
      return result;
    }
  }
  while (player->position < frame) {
    if (player->pending_used >= player->pending_frames) {
      if (player->ended) {
        break;
      }
      GAUD_Result result = decode_next(player);
      if (result == GAUD_ERR_FORMAT) {
        break; // Past the end: land where the audio stopped.
      }
      if (result != GAUD_OK) {
        return result;
      }
      continue;
    }
    uint64_t available = player->pending_frames - player->pending_used;
    uint64_t take = frame - player->position < available
        ? frame - player->position
        : available;
    player->pending_used += (uint32_t)take;
    player->position += take;
  }
  if (total != UINT64_MAX && player->position > total) {
    player->position = total;
  }
  gaud_decoder_set_position(decoder, player->position);
  *out_landed = player->position;
  return GAUD_OK;
}

static const GAUD_Decoder_Vtable opus_decoder_vtable = {
    .read = player_read,
    .seek = player_seek,
    .close = player_close,
};

GAUD_Result gaud_opus_decoder_open(
    const GAUD_Codec * codec, GAUD_Track * track, GAUD_Decoder ** out) {
  (void)codec;
  OPUS_File * file = gaud_track_private(track);
  if (!file) {
    return GAUD_ERR_INTERNAL;
  }
  const OPUS_Head * head = &file->head;
  const GAUD_Allocator * allocator = file->allocator;
  OPUS_Player * player = gcu_allocator_malloc(allocator, sizeof *player);
  if (!player) {
    return GAUD_ERR_OOM;
  }
  memset(player, 0, sizeof *player);
  player->allocator = allocator;
  player->file = file;
  player->channels = head->channels;
  player->stream_count = head->streams;
  player->streams = gcu_allocator_malloc(
      allocator, (size_t)head->streams * sizeof *player->streams);
  player->scratch = gcu_allocator_malloc(allocator,
      (size_t)OPUS_MAX_PACKET_SAMPLES * 2u * sizeof *player->scratch);
  player->pending = gcu_allocator_malloc(allocator,
      (size_t)OPUS_MAX_PACKET_SAMPLES * head->channels
          * sizeof *player->pending);
  GAUD_Result failure = GAUD_ERR_OOM;
  if (!player->streams || !player->scratch || !player->pending) {
    goto Fail;
  }
  memset(player->streams, 0, (size_t)head->streams * sizeof *player->streams);

  for (uint32_t s = 0; s < head->streams; ++s) {
    uint32_t cc = s < head->coupled ? 2u : 1u;
    GAUD_Result result =
        gaud_opus_decoder_create(allocator, cc, &player->streams[s]);
    if (result != GAUD_OK) {
      failure = result;
      goto Fail;
    }
  }

  // The routing table. A decoded channel is numbered as the stream
  // order has it: the coupled streams contribute two each, left then
  // right, and the single ones follow with one each.
  for (uint32_t slot = 0; slot < head->channels; ++slot) {
    // The file's channels are in the stream's own order, which for
    // family 1 is Vorbis's; the track's are in WAV's.
    unsigned file_channel = head->mapping_family == 1u
        ? gaud_vorbis_channel_slot(head->channels, slot)
        : slot;
    unsigned decoded = head->mapping[file_channel];
    player->route[slot].stream = -1;
    player->route[slot].channel = 0;
    if (decoded == 255u) {
      continue;
    }
    if (decoded < 2u * head->coupled) {
      player->route[slot].stream = (int16_t)(decoded / 2u);
      player->route[slot].channel = (uint8_t)(decoded % 2u);
    }
    else if (decoded - head->coupled < head->streams) {
      player->route[slot].stream = (int16_t)(decoded - head->coupled);
    }
  }

  gaud_ogg_reader_init(
      &player->reader, gaud_doc_stream(gaud_track_doc(track)), allocator);
  player->reader.serial = file->serial;
  player->reader.have_serial = true;
  failure = rewind_player(player);
  if (failure != GAUD_OK) {
    goto Fail;
  }
  failure = gaud_decoder_create_internal(
      track, &opus_decoder_vtable, player, out);
  if (failure != GAUD_OK) {
    goto Fail;
  }
  return GAUD_OK;

Fail:
  gaud_ogg_reader_free(&player->reader);
  if (player->streams) {
    for (uint32_t i = 0; i < head->streams; ++i) {
      gaud_opus_decoder_destroy(allocator, player->streams[i]);
    }
  }
  gcu_allocator_free(allocator, player->streams);
  gcu_allocator_free(allocator, player->scratch);
  gcu_allocator_free(allocator, player->pending);
  gcu_allocator_free(allocator, player);
  return failure;
}
