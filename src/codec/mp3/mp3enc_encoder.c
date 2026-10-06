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

/** How many analysed granules are held: two to make a frame, one more
 *  waiting for its successor's verdict on whether it starts a transient,
 *  and one arriving. */
#define RING 4u

/** One granule's transforms and verdict. */
typedef struct {
  int32_t spectrum[4][2][MP3E_LINES]; ///< By block type, channel.
  MP3E_Psy_Result psy[2];
  bool attack;
  int type; ///< Block type, or -1 until its successor has been seen.
} Analysed;

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
  unsigned reservoir_max;
  MP3E_Rate rate;
  unsigned tag_index;     ///< The tag frame's bit rate index.
  uint32_t tag_bytes;     ///< And its length.
  bool vbr_tag;           ///< Variable rate: a Xing tag with a table.
  uint32_t quality;       ///< 1 to 100, for the tag.
  uint32_t * sizes;       ///< Every audio frame's length, for the table.
  size_t sizes_count;
  size_t sizes_capacity;

  MP3E_Layout layout[2]; ///< Long, short.
  MP3E_Filter filter[2];
  MP3E_Psy_Rate psy_rate;
  MP3E_Psy_Channel psy[2];
  int16_t pcm[2][MP3E_LINES];
  unsigned fill;
  Analysed * ring;        ///< Granules analysed and not yet coded.
  int32_t mid_side[2][2][MP3E_LINES]; ///< A frame's spectra as mid and side.
  uint64_t analysed;      ///< Granules analysed so far.
  uint64_t decided;       ///< Of those, how many have a block type.
  uint64_t coded_granules;
  int previous_type;      ///< Block type of the granule before the next.
  uint64_t window_energy[2][2][14]; ///< Last two short windows' band energies.
  int snr_offset_q8;
  MP3E_Granule coded[2][2];

  uint64_t cursor;
  uint64_t slot_end;
  Queued * queue;
  unsigned queue_head;
  unsigned queue_count;

  uint64_t samples_in;
  uint64_t audio_frames;
  uint64_t bytes_out;
  uint64_t tag_position;
  uint16_t music_crc;
  GAUD_Result sticky;
} MP3_Encoder;

/* ---------------------------------------------------------- the tag frame */

/** What the tag frame says, from what has been written so far. */
static void fill_tag(const MP3_Encoder * enc, MP3E_Tag * tag, unsigned char * toc) {
  memset(tag, 0, sizeof(*tag));
  tag->version = enc->version;
  tag->rate_index = enc->rate_index;
  tag->channels = enc->channels;
  tag->bitrate_index = enc->tag_index;
  tag->frame_bytes = enc->tag_bytes;
  tag->vbr = enc->vbr_tag;
  tag->frames = (uint32_t)enc->audio_frames;
  tag->bytes = (uint32_t)(enc->bytes_out + enc->tag_bytes);
  uint64_t decoded = enc->audio_frames * enc->granules * MP3E_LINES;
  tag->delay = TOTAL_DELAY - MP3_DECODER_DELAY;
  uint64_t padding_total = decoded >= TOTAL_DELAY + enc->samples_in
      ? decoded - TOTAL_DELAY - enc->samples_in
      : 0u;
  tag->padding = (uint32_t)(padding_total + MP3_DECODER_DELAY);
  tag->music_crc = enc->music_crc;
  tag->music_length = tag->bytes;
  tag->quality = 100u - enc->quality;
  switch (enc->rate.mode) {
  case GAUD_RATE_ABR:
    tag->vbr_method = 2;
    tag->bitrate_byte = enc->rate.target_bps / 1000u;
    break;
  case GAUD_RATE_VBR:
    tag->vbr_method = 4;
    tag->bitrate_byte = gaud_mp3e_bitrate_kbps(enc->version, enc->rate.min_index);
    break;
  default:
    tag->vbr_method = 1;
    tag->bitrate_byte = gaud_mp3e_bitrate_kbps(enc->version, enc->rate.fixed_index);
    break;
  }
  if (enc->vbr_tag && enc->audio_frames > 0 && enc->sizes_count > 0) {
    /* Entry i is where, as a 256th of the file, the frame at i per cent of
     * the duration begins. */
    uint64_t total = tag->bytes;
    uint64_t offset = enc->tag_bytes;
    size_t frame = 0;
    for (unsigned i = 0; i < 100u; ++i) {
      size_t want = (size_t)(i * enc->sizes_count / 100u);
      while (frame < want) {
        offset += enc->sizes[frame++];
      }
      uint64_t value = offset * 256u / total;
      toc[i] = (unsigned char)(value > 255u ? 255u : value);
    }
    tag->toc = toc;
  }
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

static Analysed * slot_of(MP3_Encoder * enc, uint64_t granule) {
  return &enc->ring[granule % RING];
}

/** The model's thresholds for one channel, in the quantiser's run order. */
static void allowed_for(const Analysed * slot, unsigned ch, unsigned type,
    unsigned sfb_count, uint64_t * out) {
  if (type == 2u) {
    for (unsigned b = 0; b < sfb_count; ++b) {
      for (unsigned w = 0; w < 3u; ++w) {
        out[3u * b + w] = slot->psy[ch].allowed_short[w][b];
      }
    }
  }
  else {
    for (unsigned b = 0; b < sfb_count; ++b) {
      out[b] = slot->psy[ch].allowed_long[b];
    }
  }
}

/** Middle and side of two spectra, in place of left and right. */
static void to_middle_side(const int32_t * left, const int32_t * right,
    int32_t * middle, int32_t * side) {
  for (unsigned i = 0; i < MP3E_LINES; ++i) {
    int64_t sum = (int64_t)left[i] + right[i];
    int64_t difference = (int64_t)left[i] - right[i];
    middle[i] = (int32_t)((sum * gaud_mp3_inv_sqrt2 + (1 << (MP3_Q - 1))) >> MP3_Q);
    side[i] = (int32_t)((difference * gaud_mp3_inv_sqrt2 + (1 << (MP3_Q - 1))) >> MP3_Q);
  }
}

static GAUD_Result encode_frame(MP3_Encoder * enc) {
  MP3E_Frame_Header header = {
      .version = enc->version,
      .rate_index = enc->rate_index,
      .mode = enc->channels == 1u ? MP3_MODE_SINGLE_CHANNEL : MP3_MODE_JOINT_STEREO,
      .channels = enc->channels,
      .sample_rate = enc->sample_rate,
  };
  uint32_t side_bytes = gaud_mp3e_side_bytes(enc->version, enc->channels);
  uint32_t head = 4u + side_bytes;
  if (enc->queue_count >= QUEUE_FRAMES) {
    return GAUD_ERR_INTERNAL;
  }
  uint64_t reservoir = enc->slot_end - enc->cursor;
  if (reservoir > enc->reservoir_max) {
    return GAUD_ERR_INTERNAL;
  }

  unsigned channels = enc->channels;
  unsigned granules = enc->granules;
  uint64_t first = enc->coded_granules;
  /* The spectra and thresholds this frame is coded from, per granule and
   * channel, in left/right or - for a frame that chooses it - mid/side. */
  int32_t (*mid_side)[2][MP3E_LINES] = enc->mid_side;
  uint64_t allowed[2][2][MP3E_MAX_BANDS];
  const int32_t * spectrum[2][2];
  unsigned type[2];

  unsigned layout_index[2];
  for (unsigned gr = 0; gr < granules; ++gr) {
    Analysed * slot = slot_of(enc, first + gr);
    type[gr] = (unsigned)slot->type;
    layout_index[gr] = type[gr] == 2u ? 1u : 0u;
    for (unsigned ch = 0; ch < channels; ++ch) {
      spectrum[gr][ch] = slot->spectrum[type[gr]][ch];
      allowed_for(slot, ch, type[gr], enc->layout[layout_index[gr]].sfb_count, allowed[gr][ch]);
    }
  }

  bool middle_side = false;
  if (channels == 2u) {
    uint64_t lr = 0;
    uint64_t ms = 0;
    uint64_t ms_allowed[2][MP3E_MAX_BANDS];
    for (unsigned gr = 0; gr < granules; ++gr) {
      const MP3E_Layout * layout = &enc->layout[layout_index[gr]];
      to_middle_side(spectrum[gr][0], spectrum[gr][1], mid_side[gr][0], mid_side[gr][1]);
      for (unsigned r = 0; r < layout->count; ++r) {
        uint64_t m = allowed[gr][0][r] < allowed[gr][1][r] ? allowed[gr][0][r] : allowed[gr][1][r];
        ms_allowed[0][r] = m;
        ms_allowed[1][r] = m;
      }
      lr += gaud_mp3e_estimate_bits(layout, spectrum[gr][0], allowed[gr][0]);
      lr += gaud_mp3e_estimate_bits(layout, spectrum[gr][1], allowed[gr][1]);
      ms += gaud_mp3e_estimate_bits(layout, mid_side[gr][0], ms_allowed[0]);
      ms += gaud_mp3e_estimate_bits(layout, mid_side[gr][1], ms_allowed[1]);
    }
    middle_side = ms < lr;
    if (middle_side) {
      for (unsigned gr = 0; gr < granules; ++gr) {
        const MP3E_Layout * layout = &enc->layout[layout_index[gr]];
        for (unsigned r = 0; r < layout->count; ++r) {
          uint64_t m = allowed[gr][0][r] < allowed[gr][1][r] ? allowed[gr][0][r] : allowed[gr][1][r];
          allowed[gr][0][r] = m;
          allowed[gr][1][r] = m;
        }
        spectrum[gr][0] = mid_side[gr][0];
        spectrum[gr][1] = mid_side[gr][1];
      }
      header.mode_extension = 2u;
    }
  }

  /* What each granule-channel needs is found by asking the quantiser, not
   * by estimating: the first pass codes every one as coarsely as masking
   * allows, up to the most a granule can hold, and what it used is what it
   * needed. If the frame can afford all of them, that is the frame. If it
   * cannot, the bits there are - the frame's own and most of the
   * reservoir's - are shared in proportion to need and everything is
   * coded again within its share. If there are more than the frame can
   * hold in the reservoir at all, the surplus would be stuffed away, so it
   * is spent instead on finer steps. */
  unsigned units = granules * channels;
  uint32_t need[4] = {0, 0, 0, 0};
  uint64_t needed = 0;
  for (unsigned gr = 0, u = 0; gr < granules; ++gr) {
    for (unsigned ch = 0; ch < channels; ++ch, ++u) {
      MP3E_Quant_Input input;
      memset(&input, 0, sizeof(input));
      input.spectrum = spectrum[gr][ch];
      input.block_type = (uint8_t)type[gr];
      memcpy(input.allowed, allowed[gr][ch], sizeof(input.allowed));
      input.target_bits = MP3E_MAX_PART23;
      gaud_mp3e_quantize_granule(&input, &enc->layout[layout_index[gr]], enc->row,
          enc->version, &enc->coded[gr][ch]);
      need[u] = enc->coded[gr][ch].side.part2_3_length;
      needed += need[u];
    }
  }
  /* The frame's size, now that what it needs is known. */
  unsigned bitrate_index = gaud_mp3e_rate_choose(&enc->rate, needed, reservoir, head);
  bool padding = false;
  uint32_t total = gaud_mp3e_rate_size(&enc->rate, bitrate_index, &padding);
  if (total < head || total > MP3_MAX_FRAME_SIZE) {
    return GAUD_ERR_INTERNAL;
  }
  uint32_t area = total - head;
  header.bitrate_index = bitrate_index;
  header.bitrate = gaud_mp3e_bitrate_kbps(enc->version, bitrate_index) * 1000u;
  header.padding = padding;
  uint64_t base = 8u * (uint64_t)area;
  uint64_t available = base + 8u * reservoir * 9u / 10u;
  uint64_t grant = 0;
  bool recode = false;
  if (needed > available) {
    grant = available;
    recode = true;
  }
  else {
    uint64_t used_bytes = (needed + 7u) / 8u;
    uint64_t left = area + reservoir - used_bytes;
    /* Keep enough in the reservoir for a granule that needs more than its
     * share - five eighths of what it can hold - and spend the rest. */
    uint64_t keep = enc->reservoir_max * 5u / 8u;
    if (left > keep) {
      grant = needed + 8u * (left - keep);
      recode = true;
    }
  }
  if (recode) {
    for (unsigned gr = 0, u = 0; gr < granules; ++gr) {
      for (unsigned ch = 0; ch < channels; ++ch, ++u) {
        MP3E_Quant_Input input;
        memset(&input, 0, sizeof(input));
        input.spectrum = spectrum[gr][ch];
        input.block_type = (uint8_t)type[gr];
        memcpy(input.allowed, allowed[gr][ch], sizeof(input.allowed));
        uint64_t share = needed ? grant * need[u] / needed : grant / units;
        if (share > MP3E_MAX_PART23) {
          share = MP3E_MAX_PART23;
        }
        input.target_bits = (uint32_t)share;
        input.spend = true;
        gaud_mp3e_quantize_granule(&input, &enc->layout[layout_index[gr]], enc->row,
            enc->version, &enc->coded[gr][ch]);
      }
    }
  }

  /* Scalefactor reuse between the two granules of an MPEG-1 frame. */
  uint8_t scfsi[2] = {0, 0};
  if (enc->version == MP3_MPEG1) {
    for (unsigned ch = 0; ch < channels; ++ch) {
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
  for (unsigned gr = 0; gr < granules; ++gr) {
    for (unsigned ch = 0; ch < channels; ++ch) {
      MP3E_Granule * g = &enc->coded[gr][ch];
      size_t before = bits.bits;
      if (g->side.part2_3_length > MP3E_MAX_PART23) {
        return GAUD_ERR_INTERNAL; /* Its field is twelve bits wide. */
      }
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
  gaud_mp3e_rate_commit(&enc->rate, bitrate_index);
  if (enc->sizes_count == enc->sizes_capacity) {
    size_t capacity = enc->sizes_capacity ? enc->sizes_capacity * 2u : 1024u;
    uint32_t * grown = gcu_allocator_realloc(enc->allocator, enc->sizes, capacity * sizeof(*grown));
    if (!grown) {
      return GAUD_ERR_OOM;
    }
    enc->sizes = grown;
    enc->sizes_capacity = capacity;
  }
  enc->sizes[enc->sizes_count++] = total;
  ++enc->audio_frames;
  enc->coded_granules += granules;
  return emit_ready(enc, false);
}

/**
 * Decide the block type of granule @p which, now that it is known whether
 * the granule after it starts a transient.
 *
 * The legal sequences are the format's: a long block may be followed by a
 * start block, which must be followed by a short one; short blocks run
 * until a stop block, which is followed by a long block or - because the
 * standard allows it - another start block. Everything below is those
 * three rules and nothing else.
 */
static void decide_type(MP3_Encoder * enc, uint64_t which, bool next_attack) {
  Analysed * slot = slot_of(enc, which);
  int previous = enc->previous_type;
  int type;
  if (previous == 1) {
    type = 2;
  }
  else if (slot->attack) {
    type = 2;
  }
  else if (next_attack) {
    type = previous == 2 ? 2 : 1;
  }
  else {
    type = previous == 2 ? 3 : 0;
  }
  slot->type = type;
  enc->previous_type = type;
  ++enc->decided;
}

/** Code every frame whose granules all have a block type. */
static GAUD_Result drain(MP3_Encoder * enc) {
  while (enc->decided >= enc->coded_granules + enc->granules) {
    GAUD_Result result = encode_frame(enc);
    if (result != GAUD_OK) {
      return result;
    }
  }
  return GAUD_OK;
}

/**
 * Whether a short window of channel @p ch starts a transient.
 *
 * A transient is a window much louder, in some band that matters, than the
 * two before it. "Matters" is the model's own: a band counts only if its
 * energy is well above the noise the long transform would be allowed to
 * leave there, because noise below the signal is masked and noise above it
 * is exactly the pre-echo a short block exists to avoid. "Much louder" is
 * measured against the larger of the two preceding windows plus a floor of
 * a quarter of that same noise, so that a window following silence is
 * judged against what the noise would be and not against nothing; and the
 * larger of two, not the last, so that a voiced sound's pulse train - loud
 * every second window - is not mistaken for a series of onsets.
 */
static bool detect_attack(MP3_Encoder * enc, Analysed * slot, unsigned ch) {
  const MP3E_Layout * layout = &enc->layout[1];
  bool attack = false;
  const uint16_t * bounds = gaud_mp3_sfb_short[enc->row];
  uint64_t window[3][14];
  uint64_t noise_of[14];
  for (unsigned b = 0; b < layout->sfb_count; ++b) {
    uint64_t noise = 0;
    for (unsigned k = 3u * bounds[b]; k < 3u * bounds[b + 1u]; ++k) {
      noise += slot->psy[ch].allowed_line[k];
    }
    noise_of[b] = noise;
  }
  for (unsigned w = 0; w < 3u; ++w) {
    for (unsigned b = 0; b < layout->sfb_count; ++b) {
      const MP3E_Band * band = &layout->band[3u * b + w];
      uint64_t e = 0;
      for (unsigned i = band->start; i < (unsigned)band->start + band->width; ++i) {
        int64_t scaled = (int64_t)slot->spectrum[2][ch][layout->order[i]]
            / (1 << MP3E_ENERGY_SHIFT);
        e += (uint64_t)(scaled * scaled);
      }
      window[w][b] = e;
      uint64_t before = enc->window_energy[ch][0][b] > enc->window_energy[ch][1][b]
          ? enc->window_energy[ch][0][b]
          : enc->window_energy[ch][1][b];
      if (e > 4u * noise_of[b] && e / 10u > before + noise_of[b] / 4u) {
        attack = true;
      }
    }
    memcpy(enc->window_energy[ch][1], enc->window_energy[ch][0], sizeof(window[w]));
    memcpy(enc->window_energy[ch][0], window[w], sizeof(window[w]));
  }
  /* The same thing from the other side. A granule that is loud in one of its
   * windows and nearly silent in another has its quantisation noise, which a
   * long window spreads evenly over all of them, well over the signal in
   * the quiet one - after an attack, as a decay; before one, as a rise. The
   * attack test above sees only the rise, and only against what came
   * before the granule. */
  for (unsigned b = 0; b < layout->sfb_count && !attack; ++b) {
    uint64_t loud = 0;
    uint64_t quiet = UINT64_MAX;
    for (unsigned w = 0; w < 3u; ++w) {
      loud = window[w][b] > loud ? window[w][b] : loud;
      quiet = window[w][b] < quiet ? window[w][b] : quiet;
    }
    if (loud > 16u * noise_of[b] && quiet * 4u < noise_of[b]) {
      attack = true;
    }
  }
  return attack;
}

static GAUD_Result process_granule(MP3_Encoder * enc) {
  uint64_t g = enc->analysed++;
  Analysed * slot = slot_of(enc, g);
  slot->attack = false;
  slot->type = -1;
  for (unsigned ch = 0; ch < enc->channels; ++ch) {
    gaud_mp3e_filter_analyze(&enc->filter[ch], enc->pcm[ch], 1u);
    for (unsigned t = 0; t < 4u; ++t) {
      gaud_mp3e_filter_transform(&enc->filter[ch], t, slot->spectrum[t][ch]);
    }
    gaud_mp3e_psy_analyze(&enc->psy_rate, &enc->psy[ch], enc->pcm[ch],
        enc->snr_offset_q8, &slot->psy[ch]);
    if (detect_attack(enc, slot, ch)) {
      slot->attack = true;
    }
  }
  if (g >= 1u) {
    decide_type(enc, g - 1u, slot->attack);
  }
  return drain(enc);
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
  /* Enough zeros to flush the filters, and then to end on a whole frame,
   * and one granule more: each granule's block type waits on its
   * successor, so the last one coded needs a successor to ask. */
  uint64_t need = enc->samples_in + TOTAL_DELAY;
  uint64_t frame_samples = (uint64_t)enc->granules * MP3E_LINES;
  uint64_t target_granules = (need + frame_samples - 1u) / frame_samples * enc->granules;
  uint64_t have = enc->analysed;
  if (enc->fill != 0) {
    while (enc->fill < MP3E_LINES) {
      for (unsigned ch = 0; ch < enc->channels; ++ch) {
        enc->pcm[ch][enc->fill] = 0;
      }
      ++enc->fill;
    }
    enc->fill = 0;
    GAUD_Result result = process_granule(enc);
    if (result != GAUD_OK) {
      return result;
    }
    have = enc->analysed;
  }
  while (have < target_granules + 1u) {
    for (unsigned ch = 0; ch < enc->channels; ++ch) {
      memset(enc->pcm[ch], 0, sizeof(enc->pcm[ch]));
    }
    GAUD_Result result = process_granule(enc);
    if (result != GAUD_OK) {
      return result;
    }
    have = enc->analysed;
  }
  GAUD_Result result = emit_ready(enc, true);
  if (result != GAUD_OK) {
    return result;
  }
  /* The tag frame is rewritten now that the counts are known. */
  unsigned char tag[MP3_MAX_FRAME_SIZE];
  unsigned char toc[100];
  MP3E_Tag description;
  fill_tag(enc, &description, toc);
  gaud_mp3e_tag_build(&description, tag);
  uint64_t end = gaud_stream_tell(enc->stream);
  if (gaud_stream_seek(enc->stream, (int64_t)enc->tag_position, GAUD_SEEK_SET) != GAUD_OK) {
    return GAUD_ERR_IO;
  }
  GAUD_Result written = gaud_stream_write(enc->stream, tag, enc->tag_bytes);
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
  gcu_allocator_free(enc->allocator, enc->ring);
  gcu_allocator_free(enc->allocator, enc->sizes);
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
  GAUD_Rate_Control mode = params->rate_control == GAUD_RATE_DEFAULT
      ? GAUD_RATE_CBR
      : params->rate_control;
  if (mode > GAUD_RATE_VBR || params->bitrate % 1000u != 0u
      || params->min_bitrate % 1000u != 0u) {
    return GAUD_ERR_UNSUPPORTED;
  }
  unsigned lowest = 1;
  unsigned highest = 14;
  unsigned fixed_index = 0;
  uint32_t target_bps = 0;
  unsigned kbps = params->bitrate / 1000u;
  if (mode == GAUD_RATE_CBR) {
    if (kbps == 0) {
      kbps = version == MP3_MPEG1 ? (channels == 1u ? 64u : 128u)
                                  : (channels == 1u ? 32u : 64u);
    }
    int index = gaud_mp3e_bitrate_index(version, kbps);
    if (index < 0) {
      return GAUD_ERR_UNSUPPORTED;
    }
    fixed_index = (unsigned)index;
    lowest = highest = fixed_index;
  }
  else {
    if (params->min_bitrate != 0) {
      int index = gaud_mp3e_bitrate_index(version, params->min_bitrate / 1000u);
      if (index < 0) {
        return GAUD_ERR_UNSUPPORTED;
      }
      lowest = (unsigned)index;
    }
    if (mode == GAUD_RATE_VBR && kbps != 0) {
      int index = gaud_mp3e_bitrate_index(version, kbps);
      if (index < 0) {
        return GAUD_ERR_UNSUPPORTED;
      }
      highest = (unsigned)index;
    }
    if (lowest > highest) {
      return GAUD_ERR_INVALID;
    }
    if (mode == GAUD_RATE_ABR) {
      if (kbps == 0
          || kbps < gaud_mp3e_bitrate_kbps(version, lowest)
          || kbps > gaud_mp3e_bitrate_kbps(version, highest)) {
        return GAUD_ERR_UNSUPPORTED;
      }
      target_bps = kbps * 1000u;
    }
  }
  uint32_t quality = params->quality ? params->quality : 50u;
  if (quality > 100u) {
    return GAUD_ERR_INVALID;
  }

  const GAUD_Allocator * allocator = gaud_stream_allocator(stream);
  MP3_Encoder * enc = gcu_allocator_malloc(allocator, sizeof(*enc));
  if (!enc) {
    return GAUD_ERR_OOM;
  }
  memset(enc, 0, sizeof(*enc));
  enc->queue = gcu_allocator_malloc(allocator, sizeof(Queued) * QUEUE_FRAMES);
  enc->ring = gcu_allocator_malloc(allocator, sizeof(Analysed) * RING);
  if (!enc->queue || !enc->ring) {
    gcu_allocator_free(allocator, enc->queue);
    gcu_allocator_free(allocator, enc->ring);
    gcu_allocator_free(allocator, enc);
    return GAUD_ERR_OOM;
  }
  memset(enc->ring, 0, sizeof(Analysed) * RING);
  enc->allocator = allocator;
  enc->stream = stream;
  enc->meta = params->meta;
  enc->policy = params->meta_policy;
  enc->version = version;
  enc->rate_index = rate_index;
  enc->channels = channels;
  enc->granules = version == MP3_MPEG1 ? 2u : 1u;
  enc->sample_rate = params->sample_rate;
  enc->quality = quality;
  /* Four tenths of a decibel of signal-to-noise ratio per point of quality,
   * and the middle of the scale four decibels under the model's own level:
   * measured against LAME on the same input, that puts the default where
   * LAME's -V4 is, and the whole range is 40 decibels of how much
   * quantisation noise the encoder leaves under what the ear masks. */
  enc->snr_offset_q8 = mode == GAUD_RATE_VBR
      ? ((int)quality - 50) * 4 * 256 / 10 - 4 * 256
      : 0;
  enc->vbr_tag = mode != GAUD_RATE_CBR;
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
    gaud_mp3e_psy_channel_reset(&enc->psy[ch]);
  }
  gaud_mp3e_psy_rate_init(&enc->psy_rate, 3u * (unsigned)version + rate_index, enc->row);
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
  gaud_mp3e_rate_init(&enc->rate, mode, version, params->sample_rate, lowest,
      highest, fixed_index, target_bps);
  if (failure == GAUD_OK) {
    /* The tag frame has to be long enough to hold the tag: with a table of
     * contents and the encoder's extension that is 156 bytes after the
     * side information; without the table, 52. A constant-rate file keeps
     * its own rate if that is enough. */
    uint32_t head = 4u + gaud_mp3e_side_bytes(version, channels);
    uint32_t needed_bytes = head + (enc->vbr_tag ? 156u : 52u);
    unsigned index = mode == GAUD_RATE_CBR ? fixed_index : lowest;
    for (;; ++index) {
      if (gaud_mp3e_rate_size(&enc->rate, index, NULL) >= needed_bytes || index >= 14u) {
        break;
      }
    }
    enc->tag_index = index;
    enc->tag_bytes = gaud_mp3e_rate_size(&enc->rate, index, NULL);
    if (enc->tag_bytes < needed_bytes) {
      failure = GAUD_ERR_INTERNAL;
    }
    gaud_mp3e_rate_commit(&enc->rate, index);
    enc->rate.actual_bytes = 0;
    enc->rate.target_accumulator = 0;
  }
  if (failure == GAUD_OK) {
    enc->tag_position = gaud_stream_tell(stream);
    unsigned char tag[MP3_MAX_FRAME_SIZE];
    unsigned char toc[100];
    MP3E_Tag description;
    fill_tag(enc, &description, toc);
    gaud_mp3e_tag_build(&description, tag);
    failure = gaud_stream_write(stream, tag, enc->tag_bytes);
  }
  if (failure == GAUD_OK) {
    failure = gaud_encoder_create_internal(
        stream, params, allocator, &mp3_encoder_vtable, enc, out_encoder);
  }
  if (failure != GAUD_OK) {
    gcu_allocator_free(allocator, enc->queue);
    gcu_allocator_free(allocator, enc->ring);
    gcu_allocator_free(allocator, enc->sizes);
    gcu_allocator_free(allocator, enc);
  }
  return failure;
}
