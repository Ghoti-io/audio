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
 * Registering every built-in codec at once.
 */

#include <ghoti.io/audio/codecs.h>

/** @brief Register every codec this library ships with. */
void gaud_register_builtin_codecs(void) {
  gaud_wav_register();
  gaud_aiff_register();
  gaud_flac_register();
  gaud_ogg_flac_register();
  gaud_mp3_register();
  gaud_vorbis_register();
}
