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
 * The one place the optional `image` dependency is used.
 *
 * planning/audio.md 11.4: a container that states a picture's dimensions
 * can have got them wrong, and `image` is what can tell. The two builds
 * differ **additively** - what a file stated is reported either way, and
 * what the bytes actually are is reported only here - so this file is the
 * entire difference between them.
 *
 * Isolating it in one translation unit is deliberate. `make check-aliasing`
 * and the CI matrix both build the library twice, and a `#ifdef` spread
 * across the readers would mean the arm that is not compiled is the arm
 * that rots. Here there is one function, and its no-image body is three
 * lines.
 */

#include "meta_internal.h"

#ifdef GAUD_HAVE_IMAGE
#include <ghoti.io/image/image.h>

/**
 * Decode far enough to learn the real dimensions.
 *
 * @return true and fills the outputs, or false when the bytes are not an
 *   image this build can read - which is itself a finding, and is why the
 *   caller records GAUD_PICTURE_MISMATCH rather than giving up.
 */
static bool measure(const void * data, size_t size, uint32_t * out_width,
    uint32_t * out_height) {
  GIMG_Stream * stream = NULL;
  if (gimg_stream_create_memory(data, size, &stream) != GIMG_OK) {
    return false;
  }
  GIMG_Doc * doc = NULL;
  if (gimg_doc_load(stream, NULL, NULL, &doc) != GIMG_OK) {
    gimg_stream_destroy(stream);
    return false;
  }
  bool ok = false;
  GIMG_Item * item = gimg_doc_item(doc, 0);
  if (item && gimg_item_ensure_decoded(item, NULL) == GIMG_OK) {
    GIMG_Raster * raster = gimg_item_raster(item);
    if (raster) {
      *out_width = gimg_raster_width(raster);
      *out_height = gimg_raster_height(raster);
      ok = true;
    }
  }
  gimg_doc_destroy(doc);
  gimg_stream_destroy(stream);
  return ok;
}
#endif

void gaud_meta_verify_pictures(GAUD_Meta * meta) {
  if (!meta) {
    return;
  }
#ifndef GAUD_HAVE_IMAGE
  /* Every picture keeps the status gaud_meta_picture_add_stated() gave it:
   * UNVERIFIED where the container stated dimensions, NOT_STATED where it
   * did not. Those are different facts and this build reports both. */
  (void)meta;
#else
  for (size_t i = 0; i < meta->picture_count; ++i) {
    GAUD_Picture * picture = &meta->pictures[i].picture;
    if (picture->status != GAUD_PICTURE_UNVERIFIED) {
      /* NOT_STATED has nothing to check against. Checking it anyway and
       * reporting VERIFIED would invent an agreement with a claim the
       * container never made. */
      continue;
    }
    uint32_t width = 0, height = 0;
    if (!measure(picture->data, picture->size, &width, &height)) {
      /* The container stated dimensions for something that will not
       * parse as an image. That is a finding, not an absence. */
      picture->status = GAUD_PICTURE_MISMATCH;
      continue;
    }
    picture->verified_width = width;
    picture->verified_height = height;
    /* A dimension the container left at zero is one it did not state, so
     * it cannot disagree. Comparing against zero would report a mismatch
     * for every writer that filled in one field and not the other. */
    bool agrees = (picture->stated_width == 0
                      || picture->stated_width == width)
        && (picture->stated_height == 0 || picture->stated_height == height);
    picture->status = agrees ? GAUD_PICTURE_VERIFIED : GAUD_PICTURE_MISMATCH;
  }
#endif
}
