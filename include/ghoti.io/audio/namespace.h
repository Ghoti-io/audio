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
#define GAUD_Sample_Coding GHOTIIO_AUDIO(GAUD_Sample_Coding)
#define GAUD_Diagnostic GHOTIIO_AUDIO(GAUD_Diagnostic)
#define GAUD_Diagnostics GHOTIIO_AUDIO(GAUD_Diagnostics)
#define GAUD_Encoder_Tier GHOTIIO_AUDIO(GAUD_Encoder_Tier)
#define GAUD_Limits GHOTIIO_AUDIO(GAUD_Limits)
#define GAUD_Meta GHOTIIO_AUDIO(GAUD_Meta)
#define GAUD_Meta_Policy GHOTIIO_AUDIO(GAUD_Meta_Policy)
#define GAUD_Picture GHOTIIO_AUDIO(GAUD_Picture)
#define GAUD_Picture_Kind GHOTIIO_AUDIO(GAUD_Picture_Kind)
#define GAUD_Picture_Status GHOTIIO_AUDIO(GAUD_Picture_Status)
#define GAUD_Tag GHOTIIO_AUDIO(GAUD_Tag)
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
#define gaud_stream_allocator GHOTIIO_AUDIO(gaud_stream_allocator)
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

// Phase 1 types.
#define GAUD_Buffer GHOTIIO_AUDIO(GAUD_Buffer)
#define GAUD_Channel GHOTIIO_AUDIO(GAUD_Channel)
#define GAUD_Channel_Layout GHOTIIO_AUDIO(GAUD_Channel_Layout)
#define GAUD_Codec_Close_Fn GHOTIIO_AUDIO(GAUD_Codec_Close_Fn)
#define GAUD_Codec_Decoder_Open_Fn GHOTIIO_AUDIO(GAUD_Codec_Decoder_Open_Fn)
#define GAUD_Codec_Encoder_Open_Fn GHOTIIO_AUDIO(GAUD_Codec_Encoder_Open_Fn)
#define GAUD_Codec_Open_Fn GHOTIIO_AUDIO(GAUD_Codec_Open_Fn)
#define GAUD_Convert_Options GHOTIIO_AUDIO(GAUD_Convert_Options)
#define GAUD_Decoder GHOTIIO_AUDIO(GAUD_Decoder)
#define GAUD_Decoder_Vtable GHOTIIO_AUDIO(GAUD_Decoder_Vtable)
#define GAUD_Dither GHOTIIO_AUDIO(GAUD_Dither)
#define GAUD_Doc GHOTIIO_AUDIO(GAUD_Doc)
#define GAUD_Encode_Params GHOTIIO_AUDIO(GAUD_Encode_Params)
#define GAUD_Encoder GHOTIIO_AUDIO(GAUD_Encoder)
#define GAUD_Encoder_Vtable GHOTIIO_AUDIO(GAUD_Encoder_Vtable)
#define GAUD_Ops_Measure GHOTIIO_AUDIO(GAUD_Ops_Measure)
#define GAUD_Sample_Format GHOTIIO_AUDIO(GAUD_Sample_Format)
#define GAUD_Sample_Layout GHOTIIO_AUDIO(GAUD_Sample_Layout)
#define GAUD_Track GHOTIIO_AUDIO(GAUD_Track)
#define GAUD_Track_Desc GHOTIIO_AUDIO(GAUD_Track_Desc)
#define GAUD_Trim GHOTIIO_AUDIO(GAUD_Trim)

// Samples, channel layouts and the buffer.
#define gaud_buffer_capacity GHOTIIO_AUDIO(gaud_buffer_capacity)
#define gaud_buffer_channels GHOTIIO_AUDIO(gaud_buffer_channels)
#define gaud_buffer_create GHOTIIO_AUDIO(gaud_buffer_create)
#define gaud_buffer_data GHOTIIO_AUDIO(gaud_buffer_data)
#define gaud_buffer_data_const GHOTIIO_AUDIO(gaud_buffer_data_const)
#define gaud_buffer_destroy GHOTIIO_AUDIO(gaud_buffer_destroy)
#define gaud_buffer_bytes_used GHOTIIO_AUDIO(gaud_buffer_bytes_used)
#define gaud_buffer_format GHOTIIO_AUDIO(gaud_buffer_format)
#define gaud_frame_size GHOTIIO_AUDIO(gaud_frame_size)
#define gaud_buffer_frame_size GHOTIIO_AUDIO(gaud_buffer_frame_size)
#define gaud_buffer_frames GHOTIIO_AUDIO(gaud_buffer_frames)
#define gaud_buffer_layout GHOTIIO_AUDIO(gaud_buffer_layout)
#define gaud_buffer_offset GHOTIIO_AUDIO(gaud_buffer_offset)
#define gaud_buffer_sample_layout GHOTIIO_AUDIO(gaud_buffer_sample_layout)
#define gaud_buffer_set_frames GHOTIIO_AUDIO(gaud_buffer_set_frames)
#define gaud_buffer_silence GHOTIIO_AUDIO(gaud_buffer_silence)
#define gaud_buffer_size_bytes GHOTIIO_AUDIO(gaud_buffer_size_bytes)
#define gaud_channel_layout_at GHOTIIO_AUDIO(gaud_channel_layout_at)
#define gaud_channel_layout_default GHOTIIO_AUDIO(gaud_channel_layout_default)
#define gaud_channel_layout_unspecified \
  GHOTIIO_AUDIO(gaud_channel_layout_unspecified)
#define gaud_channel_layout_valid GHOTIIO_AUDIO(gaud_channel_layout_valid)
#define gaud_channel_string GHOTIIO_AUDIO(gaud_channel_string)

// Metadata.
#define gaud_doc_meta GHOTIIO_AUDIO(gaud_doc_meta)
#define gaud_meta_add GHOTIIO_AUDIO(gaud_meta_add)
#define gaud_meta_clear GHOTIIO_AUDIO(gaud_meta_clear)
#define gaud_meta_copy GHOTIIO_AUDIO(gaud_meta_copy)
#define gaud_meta_count GHOTIIO_AUDIO(gaud_meta_count)
#define gaud_meta_create GHOTIIO_AUDIO(gaud_meta_create)
#define gaud_meta_custom GHOTIIO_AUDIO(gaud_meta_custom)
#define gaud_meta_custom_add GHOTIIO_AUDIO(gaud_meta_custom_add)
#define gaud_meta_custom_count GHOTIIO_AUDIO(gaud_meta_custom_count)
#define gaud_meta_destroy GHOTIIO_AUDIO(gaud_meta_destroy)
#define gaud_meta_get GHOTIIO_AUDIO(gaud_meta_get)
#define gaud_meta_picture GHOTIIO_AUDIO(gaud_meta_picture)
#define gaud_meta_picture_add GHOTIIO_AUDIO(gaud_meta_picture_add)
#define gaud_meta_picture_count GHOTIIO_AUDIO(gaud_meta_picture_count)
#define gaud_meta_raw GHOTIIO_AUDIO(gaud_meta_raw)
#define gaud_meta_raw_attach GHOTIIO_AUDIO(gaud_meta_raw_attach)
#define gaud_meta_raw_count GHOTIIO_AUDIO(gaud_meta_raw_count)
#define gaud_tag_from_name GHOTIIO_AUDIO(gaud_tag_from_name)
#define gaud_tag_name GHOTIIO_AUDIO(gaud_tag_name)

#define gaud_sample_coding_format GHOTIIO_AUDIO(gaud_sample_coding_format)
#define gaud_sample_coding_is_pcm GHOTIIO_AUDIO(gaud_sample_coding_is_pcm)
#define gaud_sample_coding_name GHOTIIO_AUDIO(gaud_sample_coding_name)
#define gaud_sample_format_bits GHOTIIO_AUDIO(gaud_sample_format_bits)
#define gaud_sample_format_is_float GHOTIIO_AUDIO(gaud_sample_format_is_float)
#define gaud_sample_format_is_pcm GHOTIIO_AUDIO(gaud_sample_format_is_pcm)
#define gaud_sample_format_string GHOTIIO_AUDIO(gaud_sample_format_string)

// Documents and tracks.
#define gaud_doc_add_track GHOTIIO_AUDIO(gaud_doc_add_track)
#define gaud_doc_codec_name GHOTIIO_AUDIO(gaud_doc_codec_name)
#define gaud_doc_create_internal GHOTIIO_AUDIO(gaud_doc_create_internal)
#define gaud_doc_destroy GHOTIIO_AUDIO(gaud_doc_destroy)
#define gaud_doc_load GHOTIIO_AUDIO(gaud_doc_load)
#define gaud_doc_private GHOTIIO_AUDIO(gaud_doc_private)
#define gaud_doc_set_private GHOTIIO_AUDIO(gaud_doc_set_private)
#define gaud_doc_stream GHOTIIO_AUDIO(gaud_doc_stream)
#define gaud_doc_track GHOTIIO_AUDIO(gaud_doc_track)
#define gaud_doc_track_count GHOTIIO_AUDIO(gaud_doc_track_count)
#define gaud_track_channels GHOTIIO_AUDIO(gaud_track_channels)
#define gaud_track_data_length GHOTIIO_AUDIO(gaud_track_data_length)
#define gaud_track_data_offset GHOTIIO_AUDIO(gaud_track_data_offset)
#define gaud_track_doc GHOTIIO_AUDIO(gaud_track_doc)
#define gaud_track_duration GHOTIIO_AUDIO(gaud_track_duration)
#define gaud_track_format GHOTIIO_AUDIO(gaud_track_format)
#define gaud_track_coding GHOTIIO_AUDIO(gaud_track_coding)
#define gaud_track_frames GHOTIIO_AUDIO(gaud_track_frames)
#define gaud_track_index GHOTIIO_AUDIO(gaud_track_index)
#define gaud_track_layout GHOTIIO_AUDIO(gaud_track_layout)
#define gaud_track_private GHOTIIO_AUDIO(gaud_track_private)
#define gaud_track_sample_layout GHOTIIO_AUDIO(gaud_track_sample_layout)
#define gaud_track_sample_rate GHOTIIO_AUDIO(gaud_track_sample_rate)
#define gaud_track_trim GHOTIIO_AUDIO(gaud_track_trim)

// Decoding and encoding.
#define gaud_decoder_allocator GHOTIIO_AUDIO(gaud_decoder_allocator)
#define gaud_decoder_buffer_create GHOTIIO_AUDIO(gaud_decoder_buffer_create)
#define gaud_decoder_create GHOTIIO_AUDIO(gaud_decoder_create)
#define gaud_decoder_create_internal \
  GHOTIIO_AUDIO(gaud_decoder_create_internal)
#define gaud_decoder_destroy GHOTIIO_AUDIO(gaud_decoder_destroy)
#define gaud_decoder_private GHOTIIO_AUDIO(gaud_decoder_private)
#define gaud_decoder_read GHOTIIO_AUDIO(gaud_decoder_read)
#define gaud_decoder_seek GHOTIIO_AUDIO(gaud_decoder_seek)
#define gaud_decoder_set_position GHOTIIO_AUDIO(gaud_decoder_set_position)
#define gaud_decoder_tell GHOTIIO_AUDIO(gaud_decoder_tell)
#define gaud_decoder_track GHOTIIO_AUDIO(gaud_decoder_track)
#define gaud_encode_params_default GHOTIIO_AUDIO(gaud_encode_params_default)
#define gaud_encoder_add_frames GHOTIIO_AUDIO(gaud_encoder_add_frames)
#define gaud_encoder_allocator GHOTIIO_AUDIO(gaud_encoder_allocator)
#define gaud_encoder_create GHOTIIO_AUDIO(gaud_encoder_create)
#define gaud_encoder_create_internal \
  GHOTIIO_AUDIO(gaud_encoder_create_internal)
#define gaud_encoder_destroy GHOTIIO_AUDIO(gaud_encoder_destroy)
#define gaud_encoder_finish GHOTIIO_AUDIO(gaud_encoder_finish)
#define gaud_encoder_frames_written GHOTIIO_AUDIO(gaud_encoder_frames_written)
#define gaud_encoder_params GHOTIIO_AUDIO(gaud_encoder_params)
#define gaud_encoder_private GHOTIIO_AUDIO(gaud_encoder_private)
#define gaud_encoder_stream GHOTIIO_AUDIO(gaud_encoder_stream)
#define gaud_encoder_write GHOTIIO_AUDIO(gaud_encoder_write)

// Operations.
#define gaud_convert_options_default \
  GHOTIIO_AUDIO(gaud_convert_options_default)
#define gaud_ops_convert_format GHOTIIO_AUDIO(gaud_ops_convert_format)
#define gaud_ops_convert_sample_layout \
  GHOTIIO_AUDIO(gaud_ops_convert_sample_layout)
#define gaud_ops_dc_offset GHOTIIO_AUDIO(gaud_ops_dc_offset)
#define gaud_ops_peak GHOTIIO_AUDIO(gaud_ops_peak)
#define gaud_ops_rms GHOTIIO_AUDIO(gaud_ops_rms)

// Streams added in phase 1.
#define gaud_stream_create_file GHOTIIO_AUDIO(gaud_stream_create_file)
#define gaud_stream_create_file_with_allocator \
  GHOTIIO_AUDIO(gaud_stream_create_file_with_allocator)
#define gaud_stream_create_file_writer \
  GHOTIIO_AUDIO(gaud_stream_create_file_writer)
#define gaud_stream_create_memory_writer \
  GHOTIIO_AUDIO(gaud_stream_create_memory_writer)
#define gaud_stream_create_unseekable \
  GHOTIIO_AUDIO(gaud_stream_create_unseekable)
#define gaud_stream_writable GHOTIIO_AUDIO(gaud_stream_writable)
#define gaud_stream_write GHOTIIO_AUDIO(gaud_stream_write)
#define gaud_stream_writer_bytes GHOTIIO_AUDIO(gaud_stream_writer_bytes)

// The codec SDK and the built-in codecs.
#define gaud_aiff_register GHOTIIO_AUDIO(gaud_aiff_register)
#define gaud_codec_has GHOTIIO_AUDIO(gaud_codec_has)
#define gaud_register_builtin_codecs \
  GHOTIIO_AUDIO(gaud_register_builtin_codecs)
#define gaud_wav_register GHOTIIO_AUDIO(gaud_wav_register)

/// @endcond

#endif // GHOTI_IO_GAUD_NAMESPACE_H
