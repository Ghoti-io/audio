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
 * @file namespace.h
 *
 * Maps every public name of this library into its version namespace.
 *
 * Generated from the built library's dynamic symbol table and kept in one
 * file rather than beside each declaration: a type rename has to be in
 * effect before any struct tag that uses the name, and an internal header may
 * define such a tag without including the public header that declares the
 * typedef.
 *
 * `make check-symbols` fails if an exported symbol is missing from this list.
 *
 * See CONVENTIONS.md section 4.
 */

#ifndef GHOTI_IO_GAUD_NAMESPACE_H
#define GHOTI_IO_GAUD_NAMESPACE_H

#include <ghoti.io/audio/libver.h>

/// @cond HIDDEN_SYMBOLS

// Public types. Renamed as well as the functions, so that two versions whose
// structs differ in layout cannot be confused for one another - which is the
// whole point of the scheme, and which renaming only the functions leaves
// undone. GCU_* names are deliberately absent: they are cutil's, and cutil
// has already renamed them.
#define GAUD_Allocator GHOTIIO_AUDIO(GAUD_Allocator)
#define GAUD_Capabilities GHOTIIO_AUDIO(GAUD_Capabilities)
#define GAUD_Codec GHOTIIO_AUDIO(GAUD_Codec)
#define GAUD_Codec_Magic GHOTIIO_AUDIO(GAUD_Codec_Magic)
#define GAUD_Codec_Probe_Fn GHOTIIO_AUDIO(GAUD_Codec_Probe_Fn)
#define GAUD_Confidence GHOTIIO_AUDIO(GAUD_Confidence)
#define GAUD_Diag_Severity GHOTIIO_AUDIO(GAUD_Diag_Severity)
#define GAUD_Diagnostic GHOTIIO_AUDIO(GAUD_Diagnostic)
#define GAUD_Diagnostics GHOTIIO_AUDIO(GAUD_Diagnostics)
#define GAUD_Encoder_Tier GHOTIIO_AUDIO(GAUD_Encoder_Tier)
#define GAUD_Limits GHOTIIO_AUDIO(GAUD_Limits)
#define GAUD_Probe_Result GHOTIIO_AUDIO(GAUD_Probe_Result)
#define GAUD_Registry GHOTIIO_AUDIO(GAUD_Registry)
#define GAUD_Result GHOTIIO_AUDIO(GAUD_Result)
#define GAUD_Seek_Origin GHOTIIO_AUDIO(GAUD_Seek_Origin)
#define GAUD_Stream GHOTIIO_AUDIO(GAUD_Stream)
#define GAUD_Strictness GHOTIIO_AUDIO(GAUD_Strictness)

// Allocator.
#define gaud_allocator_default GHOTIIO_AUDIO(gaud_allocator_default)

// Core: results, diagnostics, limits, build capabilities.
#define gaud_result_string GHOTIIO_AUDIO(gaud_result_string)
#define gaud_diagnostics_init GHOTIIO_AUDIO(gaud_diagnostics_init)
#define gaud_diagnostics_append GHOTIIO_AUDIO(gaud_diagnostics_append)
#define gaud_diagnostics_clear GHOTIIO_AUDIO(gaud_diagnostics_clear)
#define gaud_diagnostics_destroy GHOTIIO_AUDIO(gaud_diagnostics_destroy)
#define gaud_limits_default GHOTIIO_AUDIO(gaud_limits_default)
#define gaud_have_image_validation GHOTIIO_AUDIO(gaud_have_image_validation)

// Streams.
#define gaud_stream_create_memory GHOTIIO_AUDIO(gaud_stream_create_memory)
#define gaud_stream_create_memory_with_allocator                              \
  GHOTIIO_AUDIO(gaud_stream_create_memory_with_allocator)
#define gaud_stream_read GHOTIIO_AUDIO(gaud_stream_read)
#define gaud_stream_seek GHOTIIO_AUDIO(gaud_stream_seek)
#define gaud_stream_tell GHOTIIO_AUDIO(gaud_stream_tell)
#define gaud_stream_size GHOTIIO_AUDIO(gaud_stream_size)
#define gaud_stream_eof GHOTIIO_AUDIO(gaud_stream_eof)
#define gaud_stream_seekable GHOTIIO_AUDIO(gaud_stream_seekable)
#define gaud_stream_destroy GHOTIIO_AUDIO(gaud_stream_destroy)

// The codec SDK and its registry.
#define gaud_registry_default GHOTIIO_AUDIO(gaud_registry_default)
#define gaud_registry_create GHOTIIO_AUDIO(gaud_registry_create)
#define gaud_registry_destroy GHOTIIO_AUDIO(gaud_registry_destroy)
#define gaud_registry_register GHOTIIO_AUDIO(gaud_registry_register)
#define gaud_registry_count GHOTIIO_AUDIO(gaud_registry_count)
#define gaud_registry_by_index GHOTIIO_AUDIO(gaud_registry_by_index)
#define gaud_registry_find GHOTIIO_AUDIO(gaud_registry_find)
#define gaud_probe GHOTIIO_AUDIO(gaud_probe)

// Version.
#define gaud_version_string GHOTIIO_AUDIO(gaud_version_string)

/// @endcond

#endif // GHOTI_IO_GAUD_NAMESPACE_H
