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
 * The decoder and encoder the library owns, around the vtables a codec fills.
 *
 * Everything here is the half that is the same for every format: checking
 * that a buffer matches the track, keeping the position, refusing a call
 * after the end. The codec's half is one function each.
 */

#include "doc_internal.h"
#include <ghoti.io/cutil/allocator.h>
#include <string.h>

/*
 * Whether a buffer is the shape this track's decoder produces.
 *
 * Read and write both convert nothing, which is this library's rule, and the
 * consequence is that a mismatched buffer has to be refused rather than
 * quietly adapted. Checked in one place so the two paths cannot drift.
 */
static bool buffer_matches(const GAUD_Buffer * buffer,
    GAUD_Sample_Format format, GAUD_Channel_Layout layout,
    GAUD_Sample_Layout sample_layout) {
  if (!buffer) {
    return false;
  }
  if (gaud_buffer_format(buffer) != format) {
    return false;
  }
  if (gaud_buffer_channels(buffer) != layout.channels) {
    return false;
  }
  if (gaud_buffer_sample_layout(buffer) != sample_layout) {
    return false;
  }
  /* The mask is deliberately NOT compared. It says which speaker each
   * channel is for, which is metadata about the same samples; a caller that
   * relabelled a 6-channel buffer has not changed its shape, and refusing
   * that would be refusing something harmless. */
  return true;
}

GAUD_Result gaud_decoder_create_internal(GAUD_Track * track,
    const GAUD_Decoder_Vtable * vtable, void * codec_private,
    GAUD_Decoder ** out_decoder) {
  if (!track || !vtable || !vtable->read || !out_decoder) {
    return GAUD_ERR_INVALID;
  }
  const GAUD_Allocator * allocator = track->doc->allocator;
  GAUD_Decoder * decoder
      = gcu_allocator_malloc(allocator, sizeof(GAUD_Decoder));
  if (!decoder) {
    return GAUD_ERR_OOM;
  }
  *decoder = (GAUD_Decoder){
      .track = track,
      .vtable = vtable,
      .codec_private = codec_private,
      .position = 0,
  };
  *out_decoder = decoder;
  return GAUD_OK;
}

GAUD_Result gaud_decoder_create(
    GAUD_Track * track, GAUD_Decoder ** out_decoder) {
  if (!track || !out_decoder) {
    return GAUD_ERR_INVALID;
  }
  const GAUD_Codec * codec = track->doc->codec;
  /* The size gate, doing the job it was designed for: a codec built against
   * the phase 0 header has no decoder_open field at all, and reading one
   * would be reading past the end of what its compiler allocated. */
  if (!codec || !gaud_codec_has(codec, offsetof(GAUD_Codec, decoder_open))
      || !codec->decoder_open) {
    return GAUD_ERR_UNSUPPORTED;
  }
  return codec->decoder_open(codec, track, out_decoder);
}

void gaud_decoder_destroy(GAUD_Decoder * decoder) {
  if (!decoder) {
    return;
  }
  if (decoder->vtable->close) {
    decoder->vtable->close(decoder);
  }
  gcu_allocator_free(decoder->track->doc->allocator, decoder);
}

GAUD_Result gaud_decoder_read(GAUD_Decoder * decoder, GAUD_Buffer * buffer) {
  if (!decoder || !buffer) {
    return GAUD_ERR_INVALID;
  }
  if (!buffer_matches(buffer, decoder->track->desc.format,
          decoder->track->desc.layout, decoder->track->desc.sample_layout)) {
    return GAUD_ERR_INVALID;
  }
  /* Cleared before the codec sees it, so that a codec which returns an error
   * without writing a count cannot leave a stale one from the last read
   * looking like fresh samples. */
  GAUD_Result result = gaud_buffer_set_frames(buffer, 0);
  if (result != GAUD_OK) {
    return result;
  }
  return decoder->vtable->read(decoder, buffer);
}

GAUD_Result gaud_decoder_buffer_create(const GAUD_Decoder * decoder,
    const GAUD_Allocator * allocator, size_t frames,
    GAUD_Buffer ** out_buffer) {
  if (!decoder) {
    return GAUD_ERR_INVALID;
  }
  return gaud_buffer_create(allocator, decoder->track->desc.format,
      decoder->track->desc.layout, decoder->track->desc.sample_layout, frames,
      out_buffer);
}

GAUD_Result gaud_decoder_seek(
    GAUD_Decoder * decoder, uint64_t frame, uint64_t * out_landed) {
  if (!decoder || !out_landed) {
    return GAUD_ERR_INVALID;
  }
  if (!decoder->vtable->seek) {
    return GAUD_ERR_UNSUPPORTED;
  }
  return decoder->vtable->seek(decoder, frame, out_landed);
}

uint64_t gaud_decoder_tell(const GAUD_Decoder * decoder) {
  return decoder ? decoder->position : 0;
}

GAUD_Track * gaud_decoder_track(const GAUD_Decoder * decoder) {
  return decoder ? decoder->track : NULL;
}

void * gaud_decoder_private(const GAUD_Decoder * decoder) {
  return decoder ? decoder->codec_private : NULL;
}

void gaud_decoder_set_position(GAUD_Decoder * decoder, uint64_t frame) {
  if (decoder) {
    decoder->position = frame;
  }
}

const GAUD_Allocator * gaud_decoder_allocator(const GAUD_Decoder * decoder) {
  return decoder ? decoder->track->doc->allocator : NULL;
}

void gaud_encode_params_default(GAUD_Encode_Params * params) {
  if (!params) {
    return;
  }
  *params = (GAUD_Encode_Params){
      .format = GAUD_SAMPLE_S16,
      .sample_rate = 44100u,
      .layout = gaud_channel_layout_default(2),
      .limits = NULL,
  };
}

GAUD_Result gaud_encoder_create_internal(GAUD_Stream * stream,
    const GAUD_Encode_Params * params, const GAUD_Allocator * allocator,
    const GAUD_Encoder_Vtable * vtable, void * codec_private,
    GAUD_Encoder ** out_encoder) {
  if (!stream || !params || !vtable || !vtable->finish || !out_encoder) {
    return GAUD_ERR_INVALID;
  }
  GAUD_Encoder * encoder
      = gcu_allocator_malloc(allocator, sizeof(GAUD_Encoder));
  if (!encoder) {
    return GAUD_ERR_OOM;
  }
  *encoder = (GAUD_Encoder){
      .allocator = allocator,
      .stream = stream,
      .params = *params,
      .vtable = vtable,
      .codec_private = codec_private,
  };
  /* The limits are copied into the encoder and params.limits repointed at
   * that copy, so a caller whose GAUD_Limits went out of scope after this
   * call does not leave the encoder reading freed memory. */
  if (params->limits) {
    encoder->limits = *params->limits;
  }
  else {
    gaud_limits_default(&encoder->limits);
  }
  encoder->params.limits = &encoder->limits;

  *out_encoder = encoder;
  return GAUD_OK;
}

GAUD_Result gaud_encoder_create(const char * codec_name,
    GAUD_Registry * registry, GAUD_Stream * stream,
    const GAUD_Encode_Params * params, GAUD_Encoder ** out_encoder) {
  if (!codec_name || !stream || !params || !out_encoder) {
    return GAUD_ERR_INVALID;
  }
  if (params->sample_rate == 0 || !gaud_channel_layout_valid(params->layout)) {
    return GAUD_ERR_INVALID;
  }
  if (!gaud_stream_writable(stream)) {
    return GAUD_ERR_INVALID;
  }
  const GAUD_Codec * codec = gaud_registry_find(registry, codec_name);
  if (!codec) {
    return GAUD_ERR_FORMAT;
  }
  if (!(codec->capabilities & GAUD_CAP_ENCODE)) {
    return GAUD_ERR_UNSUPPORTED;
  }
  if (!gaud_codec_has(codec, offsetof(GAUD_Codec, encoder_open))
      || !codec->encoder_open) {
    return GAUD_ERR_UNSUPPORTED;
  }
  return codec->encoder_open(codec, stream, params, out_encoder);
}

GAUD_Result gaud_encoder_write(
    GAUD_Encoder * encoder, const GAUD_Buffer * buffer) {
  if (!encoder || !buffer) {
    return GAUD_ERR_INVALID;
  }
  if (encoder->finished) {
    /* Writing after finish would append past a header that already states
     * the length, which is a file nothing reads correctly. */
    return GAUD_ERR_INVALID;
  }
  if (!buffer_matches(buffer, encoder->params.format, encoder->params.layout,
          GAUD_LAYOUT_INTERLEAVED)) {
    return GAUD_ERR_INVALID;
  }
  if (!encoder->vtable->write) {
    return GAUD_ERR_UNSUPPORTED;
  }
  return encoder->vtable->write(encoder, buffer);
}

GAUD_Result gaud_encoder_finish(GAUD_Encoder * encoder) {
  if (!encoder) {
    return GAUD_ERR_INVALID;
  }
  if (encoder->finished) {
    return GAUD_OK; /* Idempotent: finishing twice is not an error. */
  }
  GAUD_Result result = encoder->vtable->finish(encoder);
  if (result == GAUD_OK) {
    encoder->finished = true;
  }
  return result;
}

void gaud_encoder_destroy(GAUD_Encoder * encoder) {
  if (!encoder) {
    return;
  }
  if (encoder->vtable->close) {
    encoder->vtable->close(encoder);
  }
  gcu_allocator_free(encoder->allocator, encoder);
}

uint64_t gaud_encoder_frames_written(const GAUD_Encoder * encoder) {
  return encoder ? encoder->frames_written : 0;
}

void * gaud_encoder_private(const GAUD_Encoder * encoder) {
  return encoder ? encoder->codec_private : NULL;
}

GAUD_Stream * gaud_encoder_stream(const GAUD_Encoder * encoder) {
  return encoder ? encoder->stream : NULL;
}

const GAUD_Encode_Params * gaud_encoder_params(const GAUD_Encoder * encoder) {
  return encoder ? &encoder->params : NULL;
}

void gaud_encoder_add_frames(GAUD_Encoder * encoder, uint64_t frames) {
  if (encoder) {
    encoder->frames_written += frames;
  }
}

const GAUD_Allocator * gaud_encoder_allocator(const GAUD_Encoder * encoder) {
  return encoder ? encoder->allocator : NULL;
}
