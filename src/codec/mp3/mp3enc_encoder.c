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
 * The MPEG audio Layer III encoder: the object a caller holds, and the
 * frame assembly around the quantiser.
 *
 * **A frame is not written when it is encoded.** Layer III's bit
 * reservoir lets a frame's main data begin up to 511 bytes *before* its
 * own, in the unused tails of earlier frames - so when frame N is encoded,
 * some of its bytes belong in frames already built, and frame N-1 cannot be
 * written until it is known that nothing more will land in it. The encoder
 * therefore keeps a short queue of built frames and a **cursor**: the
 * position, counted in main-data bytes from the first audio frame (the
 * headers and side information are not counted, because the decoder does
 * not count them either), where the next granule's data begins. A frame is
 * written once the cursor has passed the end of its main-data area.
 *
 * The reservoir at the start of a frame is then simply the distance from
 * the cursor to the start of that frame's own area, and the frame states it
 * as `main_data_begin`. The only discipline needed is that it never
 * exceeds what nine bits (eight, for MPEG-2) can say; when the encoder has
 * saved more than that it **stuffs** - writes padding into the previous
 * frame's data - rather than lose the frame the field cannot reach.
 */

#include "../../meta/scheme.h"
#include "../shared/bytes.h"
#include "mp3enc_internal.h"
#include <ghoti.io/audio/decoder.h>
#include <ghoti.io/cutil/allocator.h>

/** Frames kept while they may still receive reservoir data. */
#define QUEUE_FRAMES 40u

/**
 * Samples between a sample entering the encoder and appearing in the
 * decoder's output, counting both ends' filterbanks: the encoder's
 * polyphase and hybrid transforms and the decoder's own 529. Measured, not
 * derived; the unit test that measures it is `MeasuredDelay`.
 */
#define TOTAL_DELAY 1057u

typedef struct {
  unsigned char bytes[MP3_MAX_FRAME_SIZE];
  uint32_t total;      ///< Bytes in the frame.
  uint32_t head;       ///< Bytes before its main-data area.
  uint64_t slot_start; ///< Cursor coordinate of that area's first byte.
  uint64_t slot_end;   ///< And one past its last.
} Queued;

typedef struct {
  const GAUD_Allocator * allocator;
  GAUD_Stream * stream;
  const GAUD_Meta * meta;
  GAUD_Meta_Policy policy;

  MP3_Version version;
  unsigned rate_index;
  unsigned channels;
  unsigned row;
  unsigned granules; ///< Per frame: 2 for MPEG-1, otherwise 1.
  uint32_t sample_rate;
  unsigned bitrate_index;
  uint32_t bitrate;
  unsigned reservoir_max;

  MP3E_Layout layout[2]; ///< Long, short.
  MP3E_Filter filter[2];
  int16_t pcm[2][MP3E_LINES];
  unsigned fill;
  int32_t spectrum[2][2][MP3E_LINES];
  unsigned pending;
  MP3E_Granule coded[2][2];

  uint64_t cursor;
  uint64_t slot_end;
  uint64_t pad_accumulator;
  Queued * queue;
  unsigned queue_head;
  unsigned queue_count;

  uint64_t samples_in;
  uint64_t audio_frames;
  uint64_t bytes_out;
  uint64_t tag_position;
  uint16_t music_crc;
  uint32_t first_frame_bytes;
  GAUD_Result sticky;
} MP3_Encoder;

/* ---------------------------------------------------------- the tag frame */

/** Write the Info/Xing frame, which stands where the first frame would. */
static void build_tag_frame(
    const MP3_Encoder * enc, unsigned char * frame, uint32_t total) {
  MP3E_Frame_Header h = {
      .version = enc->version,
      .bitrate_index = enc->bitrate_index,
      .rate_index = enc->rate_index,
      .mode = enc->channels == 1u ? MP3_MODE_SINGLE_CHANNEL : MP3_MODE_STEREO,
      .channels = enc->channels,
      .bitrate = enc->bitrate,
      .sample_rate = enc->sample_rate,
  };
  memset(frame, 0, total);
  gaud_mp3e_header_write(frame, &h);
  unsigned char * at = frame + 4u + gaud_mp3e_side_bytes(enc->version, enc->channels);
  memcpy(at, "Info", 4);
  /* Flags: frames, bytes. A seek table is for a variable-rate file. */
  at[7] = 0x03;
  uint32_t frames = (uint32_t)enc->audio_frames;
  uint32_t bytes = (uint32_t)(enc->bytes_out + total);
  at[8] = (unsigned char)(frames >> 24);
  at[9] = (unsigned char)(frames >> 16);
  at[10] = (unsigned char)(frames >> 8);
  at[11] = (unsigned char)frames;
  at[12] = (unsigned char)(bytes >> 24);
  at[13] = (unsigned char)(bytes >> 16);
  at[14] = (unsigned char)(bytes >> 8);
  at[15] = (unsigned char)bytes;
  unsigned char * lame = at + 16u;
  memcpy(lame, "Ghoti.io 1", 9);
  uint64_t decoded = enc->audio_frames * enc->granules * MP3E_LINES;
  uint32_t delay = TOTAL_DELAY - MP3_DECODER_DELAY;
  uint64_t padding_total = decoded - TOTAL_DELAY - enc->samples_in;
  uint32_t padding = (uint32_t)(padding_total + MP3_DECODER_DELAY);
  lame[21] = (unsigned char)(delay >> 4);
  lame[22] = (unsigned char)(((delay & 0xFu) << 4) | ((padding >> 8) & 0xFu));
  lame[23] = (unsigned char)padding;
  lame[28] = (unsigned char)(bytes >> 24);
  lame[29] = (unsigned char)(bytes >> 16);
  lame[30] = (unsigned char)(bytes >> 8);
  lame[31] = (unsigned char)bytes;
  lame[32] = (unsigned char)(enc->music_crc >> 8);
  lame[33] = (unsigned char)enc->music_crc;
  uint16_t tag_crc = gaud_mp3e_crc16(frame, 190u, 0);
  lame[34] = (unsigned char)(tag_crc >> 8);
  lame[35] = (unsigned char)tag_crc;
}

/* ------------------------------------------------------------ the queue */

static Queued * queue_at(MP3_Encoder * enc, unsigned i) {
  return &enc->queue[(enc->queue_head + i) % QUEUE_FRAMES];
}

static GAUD_Result write_bytes(
    MP3_Encoder * enc, const unsigned char * data, size_t size) {
  GAUD_Result result = gaud_stream_write(enc->stream, data, size);
  if (result == GAUD_OK) {
    enc->bytes_out += size;
  }
  return result;
}

/** Write every frame that can no longer receive data. */
static GAUD_Result emit_ready(MP3_Encoder * enc, bool everything) {
  while (enc->queue_count > 0) {
    Queued * frame = queue_at(enc, 0);
    if (!everything && frame->slot_end > enc->cursor) {
      break;
    }
    GAUD_Result result = write_bytes(enc, frame->bytes, frame->total);
    if (result != GAUD_OK) {
      return result;
    }
    enc->music_crc = gaud_mp3e_crc16(frame->bytes, frame->total, enc->music_crc);
    enc->queue_head = (enc->queue_head + 1u) % QUEUE_FRAMES;
    --enc->queue_count;
  }
  return GAUD_OK;
}

/** Put @p size bytes into the main-data areas from the cursor on. */
static void place_data(MP3_Encoder * enc, const unsigned char * data, size_t size) {
  size_t done = 0;
  for (unsigned i = 0; i < enc->queue_count && done < size; ++i) {
    Queued * frame = queue_at(enc, i);
    uint64_t at = enc->cursor + done;
    if (at >= frame->slot_end) {
      continue;
    }
    size_t offset = (size_t)(at - frame->slot_start);
    size_t room = (size_t)(frame->slot_end - at);
    size_t take = size - done < room ? size - done : room;
    memcpy(frame->bytes + frame->head + offset, data + done, take);
    done += take;
  }
  enc->cursor += size;
}

/* -------------------------------------------------------------- the frame */

/**
 * Noise each band may carry, for now: a fixed signal-to-noise ratio below
 * the band's own energy. The psychoacoustic model replaces it; what stays
 * is the shape of the answer, one number per band in bitstream order.
 */
static void provisional_allowed(const MP3E_Layout * layout,
    const int32_t * spectrum, uint64_t * allowed) {
  for (unsigned r = 0; r < layout->count; ++r) {
    const MP3E_Band * band = &layout->band[r];
    uint64_t e = 0;
    for (unsigned i = band->start; i < (unsigned)band->start + band->width; ++i) {
      int64_t scaled = (int64_t)spectrum[layout->order[i]] / (1 << MP3E_ENERGY_SHIFT);
      e += (uint64_t)(scaled * scaled);
    }
    allowed[r] = e / 128u + 64u;
  }
}

static GAUD_Result encode_frame(MP3_Encoder * enc) {
  MP3E_Frame_Header header = {
      .version = enc->version,
      .bitrate_index = enc->bitrate_index,
      .rate_index = enc->rate_index,
      .mode = enc->channels == 1u ? MP3_MODE_SINGLE_CHANNEL : MP3_MODE_STEREO,
      .channels = enc->channels,
      .bitrate = enc->bitrate,
      .sample_rate = enc->sample_rate,
  };
  /* The padding byte, from the fractional part of the frame length. */
  uint64_t per_frame = (enc->version == MP3_MPEG1 ? 144u : 72u) * (uint64_t)enc->bitrate;
  uint64_t whole = (enc->pad_accumulator + per_frame) / enc->sample_rate;
  enc->pad_accumulator = (enc->pad_accumulator + per_frame) % enc->sample_rate;
  header.padding = whole > per_frame / enc->sample_rate;
  uint32_t total = (uint32_t)whole;
  uint32_t side_bytes = gaud_mp3e_side_bytes(enc->version, enc->channels);
  uint32_t head = 4u + side_bytes;
  if (total < head || total > MP3_MAX_FRAME_SIZE) {
    return GAUD_ERR_INTERNAL;
  }
  uint32_t area = total - head;

  if (enc->queue_count >= QUEUE_FRAMES) {
    return GAUD_ERR_INTERNAL;
  }
  uint64_t reservoir = enc->slot_end - enc->cursor;
  if (reservoir > enc->reservoir_max) {
    return GAUD_ERR_INTERNAL;
  }

  /* Budget: the frame's own bytes, and half of what has been saved. */
  uint32_t frame_bits = 8u * area + 4u * (uint32_t)reservoir;
  uint32_t hard_bits = 8u * (area + (uint32_t)reservoir);
  unsigned units = enc->granules * enc->channels;

  uint32_t spent = 0;
  unsigned done = 0;
  for (unsigned gr = 0; gr < enc->granules; ++gr) {
    for (unsigned ch = 0; ch < enc->channels; ++ch, ++done) {
      MP3E_Quant_Input input;
      memset(&input, 0, sizeof(input));
      input.spectrum = enc->spectrum[gr][ch];
      input.block_type = 0;
      provisional_allowed(&enc->layout[0], input.spectrum, input.allowed);
      uint32_t share = (frame_bits - (spent < frame_bits ? spent : frame_bits)) / (units - done);
      if (share > MP3E_MAX_PART23) {
        share = MP3E_MAX_PART23;
      }
      input.target_bits = share;
      input.hard_bits = hard_bits - spent;
      gaud_mp3e_quantize_granule(
          &input, &enc->layout[0], enc->row, enc->version, &enc->coded[gr][ch]);
      spent += enc->coded[gr][ch].side.part2_3_length;
    }
  }

  /* Scalefactor reuse between the two granules of an MPEG-1 frame. */
  uint8_t scfsi[2] = {0, 0};
  if (enc->version == MP3_MPEG1) {
    for (unsigned ch = 0; ch < enc->channels; ++ch) {
      MP3E_Granule * g0 = &enc->coded[0][ch];
      MP3E_Granule * g1 = &enc->coded[1][ch];
      if (g0->side.block_type == 2u || g1->side.block_type == 2u) {
        continue;
      }
      static const unsigned start[5] = {0, 6, 11, 16, 21};
      bool any = false;
      for (unsigned group = 0; group < 4u; ++group) {
        bool same = true;
        for (unsigned sfb = start[group]; sfb < start[group + 1u]; ++sfb) {
          same = same && g0->sf_long[sfb] == g1->sf_long[sfb];
        }
        g1->scfsi_reused[group] = same;
        if (same) {
          scfsi[ch] |= (uint8_t)(8u >> group);
          any = true;
        }
      }
      if (any) {
        gaud_mp3e_scalefactor_plan(g1, enc->version);
        g1->side.part2_3_length = g1->part2_bits + g1->part3_bits;
      }
    }
  }

  unsigned char data[MP3E_MAX_FRAME_DATA];
  memset(data, 0, sizeof(data));
  MP3E_Bits bits;
  gaud_mp3e_bits_init(&bits, data, sizeof(data));
  for (unsigned gr = 0; gr < enc->granules; ++gr) {
    for (unsigned ch = 0; ch < enc->channels; ++ch) {
      MP3E_Granule * g = &enc->coded[gr][ch];
      size_t before = bits.bits;
      gaud_mp3e_scalefactor_write(&bits, g, enc->version);
      gaud_mp3e_huffman_write(&bits, g, enc->row);
      if (bits.overflow || bits.bits - before != g->side.part2_3_length) {
        return GAUD_ERR_INTERNAL;
      }
    }
  }
  uint32_t data_bytes = (uint32_t)((bits.bits + 7u) / 8u);
  if (data_bytes > area + reservoir) {
    return GAUD_ERR_INTERNAL;
  }
  uint64_t leftover = area + reservoir - data_bytes;
  if (leftover > enc->reservoir_max) {
    /* Stuff: more than the back-pointer can reach is no use to anyone. */
    data_bytes += (uint32_t)(leftover - enc->reservoir_max);
  }

  Queued * frame = &enc->queue[(enc->queue_head + enc->queue_count) % QUEUE_FRAMES];
  ++enc->queue_count;
  memset(frame->bytes, 0, total);
  frame->total = total;
  frame->head = head;
  frame->slot_start = enc->slot_end;
  frame->slot_end = enc->slot_end + area;
  gaud_mp3e_header_write(frame->bytes, &header);
  gaud_mp3e_side_write(frame->bytes + 4u, &header, (uint32_t)reservoir, scfsi,
      (const MP3E_Granule (*)[2])enc->coded);
  enc->slot_end += area;
  place_data(enc, data, data_bytes > sizeof(data) ? sizeof(data) : data_bytes);
  if (data_bytes > sizeof(data)) {
    enc->cursor += data_bytes - sizeof(data);
  }
  ++enc->audio_frames;
  return emit_ready(enc, false);
}

static GAUD_Result process_granule(MP3_Encoder * enc) {
  for (unsigned ch = 0; ch < enc->channels; ++ch) {
    gaud_mp3e_filter_analyze(&enc->filter[ch], enc->pcm[ch], 1u);
    gaud_mp3e_filter_transform(&enc->filter[ch], 0, enc->spectrum[enc->pending][ch]);
  }
  if (++enc->pending == enc->granules) {
    enc->pending = 0;
    return encode_frame(enc);
  }
  return GAUD_OK;
}

/* -------------------------------------------------------------- vtable */

static GAUD_Result encoder_write(
    GAUD_Encoder * encoder, const GAUD_Buffer * buffer) {
  MP3_Encoder * enc = gaud_encoder_private(encoder);
  if (enc->sticky != GAUD_OK) {
    return enc->sticky;
  }
  size_t frames = gaud_buffer_frames(buffer);
  const unsigned char * data = gaud_buffer_data_const(buffer);
  size_t frame_size = gaud_buffer_frame_size(buffer);
  bool little = gaud_host_is_little_endian();
  for (size_t f = 0; f < frames; ++f) {
    for (unsigned ch = 0; ch < enc->channels; ++ch) {
      const unsigned char * at = data + f * frame_size + (size_t)ch * 2u;
      uint16_t raw = little ? (uint16_t)(at[0] | (at[1] << 8))
                            : (uint16_t)((at[0] << 8) | at[1]);
      enc->pcm[ch][enc->fill] = (int16_t)raw;
    }
    ++enc->samples_in;
    if (++enc->fill == MP3E_LINES) {
      enc->fill = 0;
      enc->sticky = process_granule(enc);
      if (enc->sticky != GAUD_OK) {
        return enc->sticky;
      }
    }
  }
  gaud_encoder_add_frames(encoder, frames);
  return GAUD_OK;
}

static GAUD_Result encoder_finish(GAUD_Encoder * encoder) {
  MP3_Encoder * enc = gaud_encoder_private(encoder);
  if (enc->sticky != GAUD_OK) {
    return enc->sticky;
  }
  /* Enough zeros to flush the filters, and then to end on a whole frame. */
  uint64_t need = enc->samples_in + TOTAL_DELAY;
  uint64_t have = (enc->audio_frames * enc->granules + enc->pending) * MP3E_LINES
      + enc->fill;
  while (have < need || enc->pending != 0 || enc->fill != 0) {
    while (enc->fill < MP3E_LINES) {
      for (unsigned ch = 0; ch < enc->channels; ++ch) {
        enc->pcm[ch][enc->fill] = 0;
      }
      ++enc->fill;
      ++have;
    }
    enc->fill = 0;
    GAUD_Result result = process_granule(enc);
    if (result != GAUD_OK) {
      return result;
    }
  }
  GAUD_Result result = emit_ready(enc, true);
  if (result != GAUD_OK) {
    return result;
  }
  /* The tag frame is rewritten now that the counts are known. */
  unsigned char tag[MP3_MAX_FRAME_SIZE];
  build_tag_frame(enc, tag, enc->first_frame_bytes);
  uint64_t end = gaud_stream_tell(enc->stream);
  if (gaud_stream_seek(enc->stream, (int64_t)enc->tag_position, GAUD_SEEK_SET) != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  GAUD_Result written = gaud_stream_write(enc->stream, tag, enc->first_frame_bytes);
  if (written != GAUD_OK) {
    return written;
  }
  if (gaud_stream_seek(enc->stream, (int64_t)end, GAUD_SEEK_SET) != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  return GAUD_OK;
}

static void encoder_close(GAUD_Encoder * encoder) {
  MP3_Encoder * enc = gaud_encoder_private(encoder);
  if (!enc) {
    return;
  }
  gcu_allocator_free(enc->allocator, enc->queue);
  gcu_allocator_free(enc->allocator, enc);
}

static const GAUD_Encoder_Vtable mp3_encoder_vtable = {
    .write = encoder_write,
    .finish = encoder_finish,
    .close = encoder_close,
};

GAUD_Result gaud_mp3_encoder_open(const GAUD_Codec * codec,
    GAUD_Stream * stream, const GAUD_Encode_Params * params,
    GAUD_Encoder ** out_encoder) {
  (void)codec;
  if (params->coding != GAUD_CODING_MPEG_LAYER3
      || params->format != GAUD_SAMPLE_S16) {
    return GAUD_ERR_UNSUPPORTED;
  }
  unsigned channels = params->layout.channels;
  if (channels != 1u && channels != 2u) {
    return GAUD_ERR_UNSUPPORTED;
  }
  MP3_Version version;
  unsigned rate_index;
  if (!gaud_mp3e_rate_lookup(params->sample_rate, &version, &rate_index)) {
    return GAUD_ERR_UNSUPPORTED;
  }
  if (params->rate_control > GAUD_RATE_CBR) {
    return GAUD_ERR_UNSUPPORTED;
  }
  unsigned kbps = params->bitrate / 1000u;
  if (params->bitrate == 0) {
    kbps = version == MP3_MPEG1 ? (channels == 1u ? 64u : 128u)
                                : (channels == 1u ? 32u : 64u);
  }
  int index = gaud_mp3e_bitrate_index(version, kbps);
  if (index < 0 || params->bitrate % 1000u != 0u) {
    return GAUD_ERR_UNSUPPORTED;
  }

  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);
  MP3_Encoder * enc = gcu_allocator_malloc(allocator, sizeof(*enc));
  if (!enc) {
    return GAUD_ERR_OOM;
  }
  memset(enc, 0, sizeof(*enc));
  enc->queue = gcu_allocator_malloc(allocator, sizeof(Queued) * QUEUE_FRAMES);
  if (!enc->queue) {
    gcu_allocator_free(allocator, enc);
    return GAUD_ERR_OOM;
  }
  enc->allocator = allocator;
  enc->stream = stream;
  enc->meta = params->meta;
  enc->policy = params->meta_policy;
  enc->version = version;
  enc->rate_index = rate_index;
  enc->channels = channels;
  enc->granules = version == MP3_MPEG1 ? 2u : 1u;
  enc->sample_rate = params->sample_rate;
  enc->bitrate_index = (unsigned)index;
  enc->bitrate = kbps * 1000u;
  enc->reservoir_max = version == MP3_MPEG1 ? MP3E_RESERVOIR_MAX_V1 : MP3E_RESERVOIR_MAX_V2;
  MP3_Header probe;
  memset(&probe, 0, sizeof(probe));
  probe.version = version;
  probe.rate_index = rate_index;
  gaud_mp3_band_row(&probe, &enc->row);
  gaud_mp3e_layout(&enc->layout[0], enc->row, false);
  gaud_mp3e_layout(&enc->layout[1], enc->row, true);
  for (unsigned ch = 0; ch < 2u; ++ch) {
    gaud_mp3e_filter_reset(&enc->filter[ch]);
  }
  enc->sticky = GAUD_OK;

  GAUD_Result failure = GAUD_OK;
  /* ID3v2 in front, then the tag frame as a placeholder. */
  unsigned char * id3 = NULL;
  size_t id3_size = 0;
  if (enc->meta && enc->policy != GAUD_META_DROP_ALL) {
    failure = gaud_id3v2_build(enc->meta, enc->policy, allocator, &id3, &id3_size);
    if (failure == GAUD_OK && id3_size > 0) {
      failure = gaud_stream_write(stream, id3, id3_size);
    }
    gcu_allocator_free(allocator, id3);
  }
  if (failure == GAUD_OK) {
    enc->tag_position = gaud_stream_tell(stream);
    uint64_t per_frame = (version == MP3_MPEG1 ? 144u : 72u) * (uint64_t)enc->bitrate;
    uint64_t whole = (enc->pad_accumulator + per_frame) / enc->sample_rate;
    enc->pad_accumulator = (enc->pad_accumulator + per_frame) % enc->sample_rate;
    enc->first_frame_bytes = (uint32_t)whole;
    unsigned char tag[MP3_MAX_FRAME_SIZE];
    build_tag_frame(enc, tag, enc->first_frame_bytes);
    failure = gaud_stream_write(stream, tag, enc->first_frame_bytes);
  }
  if (failure == GAUD_OK) {
    failure = gaud_encoder_create_internal(
        stream, params, allocator, &mp3_encoder_vtable, enc, out_encoder);
  }
  if (failure != GAUD_OK) {
    gcu_allocator_free(allocator, enc->queue);
    gcu_allocator_free(allocator, enc);
  }
  return failure;
}
