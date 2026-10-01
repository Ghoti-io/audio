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
 * @file codecs.h
 *
 * Registering the codecs this library ships with, by hand.
 *
 * Each of them registers itself from a constructor when the library is
 * loaded, which is what happens with a shared library and is the ordinary
 * case. **A static archive is different**: a constructor living in an object
 * file that nothing references by name is discarded by the linker unless the
 * consumer passes `--whole-archive`, so the codecs quietly are not there.
 *
 * These are the explicit path for that. Calling one twice is harmless - the
 * second registration is refused as a duplicate name and nothing breaks - so
 * a program that does not know how it was linked can simply call them.
 *
 * An out-of-tree codec is expected to ship the same pair, for the same
 * reason; documentation/writing-a-codec.md says so.
 */

#ifndef GHOTI_IO_GAUD_CODECS_H
#define GHOTI_IO_GAUD_CODECS_H

#include <ghoti.io/audio/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Register the WAV codec (RIFF/WAVE, RF64 and BW64). */
GAUD_API void gaud_wav_register(void);

/** @brief Register the AIFF codec (AIFF and AIFF-C). */
GAUD_API void gaud_aiff_register(void);

/** @brief Register the native FLAC codec (RFC 9639). */
GAUD_API void gaud_flac_register(void);

/** @brief Register FLAC carried in Ogg pages. */
GAUD_API void gaud_ogg_flac_register(void);

/** @brief Register every codec this library ships with. */
/**
 * @brief Register the bare MPEG audio codec: Layer I, II and III.
 *
 * Named `mp3` because that is what the files are called, and it reads all
 * three layers because they share a frame header and a synthesis
 * filterbank; see ::GAUD_CODING_MPEG_LAYER1 for why the *coding* keeps them
 * apart even so.
 */
GAUD_API void gaud_mp3_register(void);

GAUD_API void gaud_register_builtin_codecs(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_CODECS_H
