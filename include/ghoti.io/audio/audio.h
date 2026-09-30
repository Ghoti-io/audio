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
 * @file audio.h
 *
 * Umbrella header for the Ghoti.io Audio library.
 */

#ifndef GHOTI_IO_GAUD_AUDIO_H
#define GHOTI_IO_GAUD_AUDIO_H

#include <ghoti.io/audio/allocator.h>
#include <ghoti.io/audio/buffer.h>
#include <ghoti.io/audio/codec.h>
#include <ghoti.io/audio/codecs.h>
#include <ghoti.io/audio/coding.h>
#include <ghoti.io/audio/core.h>
#include <ghoti.io/audio/decoder.h>
#include <ghoti.io/audio/doc.h>
#include <ghoti.io/audio/macros.h>
#include <ghoti.io/audio/meta.h>
#include <ghoti.io/audio/ops.h>
#include <ghoti.io/audio/stream.h>
#include <ghoti.io/audio/version.h>

/*
 * codec_sdk.h is deliberately absent. It is installed, and a codec includes
 * it explicitly; nothing a caller reading audio needs is in it, and pulling
 * it in here would put the document-building entry points in front of every
 * consumer of this library.
 */

#endif // GHOTI_IO_GAUD_AUDIO_H
