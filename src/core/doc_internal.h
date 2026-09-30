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
 * Documents, tracks, decoders and encoders. Never installed.
 */

#ifndef GHOTI_IO_GAUD_SRC_CORE_DOC_INTERNAL_H
#define GHOTI_IO_GAUD_SRC_CORE_DOC_INTERNAL_H

#include <ghoti.io/audio/codec_sdk.h>
#include <ghoti.io/audio/macros.h>
#include <stddef.h>
#include <stdint.h>

/** @brief One track of a document. */
struct GAUD_Track {
  GAUD_Doc * doc;             ///< The document that owns it.
  size_t index;               ///< Its position in that document.
  GAUD_Track_Desc desc;       ///< What the container said.
};

/** @brief A parsed file. */
struct GAUD_Doc {
  const GAUD_Allocator * allocator; ///< Everything here comes from this.
  const GAUD_Codec * codec;         ///< What parsed it.
  GAUD_Stream * stream;             ///< Borrowed; outlives the document.
  GAUD_Track ** tracks;             ///< Owned.
  size_t track_count;               ///< How many.
  size_t track_capacity;            ///< Room in @p tracks.
  void * codec_private;             ///< The codec's, freed by its close.
};

/** @brief Reads samples from one track. */
struct GAUD_Decoder {
  GAUD_Track * track;                  ///< What is being read.
  const GAUD_Decoder_Vtable * vtable;  ///< The codec's half.
  void * codec_private;                ///< The codec's state.
  uint64_t position;                   ///< Frame the next read returns.
};

/** @brief Writes samples into one stream. */
struct GAUD_Encoder {
  const GAUD_Allocator * allocator;    ///< Everything here comes from this.
  GAUD_Stream * stream;                ///< Borrowed; outlives the encoder.
  GAUD_Encode_Params params;           ///< Copied at creation.
  GAUD_Limits limits;                  ///< Resolved copy; params.limits aims here.
  const GAUD_Encoder_Vtable * vtable;  ///< The codec's half.
  void * codec_private;                ///< The codec's state.
  uint64_t frames_written;             ///< Running total.
  bool finished;                       ///< Whether finish() has succeeded.
};

#endif // GHOTI_IO_GAUD_SRC_CORE_DOC_INTERNAL_H
