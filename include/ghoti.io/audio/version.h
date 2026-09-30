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
 * @file version.h
 *
 * What this build of the library calls itself.
 */

#ifndef GHOTI_IO_GAUD_VERSION_H
#define GHOTI_IO_GAUD_VERSION_H

#include <ghoti.io/audio/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief This build's version, as a string.
 *
 * "0.0.0" for an ordinary build, "0.0.0-dev" for one made with BRANCH=-dev,
 * and "-debug" appended for BUILD=debug. The numbers come from the Makefile,
 * so this and the soname, the .pc Version: field and the install directory
 * cannot disagree.
 *
 * @return A static string, never NULL and never freed.
 */
GAUD_API const char * gaud_version_string(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GAUD_VERSION_H
