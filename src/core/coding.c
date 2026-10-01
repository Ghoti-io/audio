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
 * The three questions a caller can ask about a sample coding.
 *
 * Table-driven rather than switch-driven so that the name and the decoded
 * format for one coding sit on one line: adding a coding and forgetting to
 * give it a name is then a missing row rather than a missing case, and the
 * _Static_assert below makes it a compile error either way.
 */

#include <ghoti.io/audio/coding.h>

/** @brief One coding's row: what it is called and what it decodes to. */
typedef struct {
  const char * name;         ///< Short lower-case name.
  GAUD_Sample_Format format; ///< What a decoder produces for it.
} Coding_Row;

static const Coding_Row table[GAUD_CODING_COUNT] = {
    [GAUD_CODING_PCM] = {"pcm", GAUD_SAMPLE_FORMAT_COUNT},
    [GAUD_CODING_G711_ULAW] = {"ulaw", GAUD_SAMPLE_S16},
    [GAUD_CODING_G711_ALAW] = {"alaw", GAUD_SAMPLE_S16},
    [GAUD_CODING_ADPCM_IMA_WAV] = {"ima-wav", GAUD_SAMPLE_S16},
    [GAUD_CODING_ADPCM_IMA_QT] = {"ima-qt", GAUD_SAMPLE_S16},
    [GAUD_CODING_ADPCM_MS] = {"ms-adpcm", GAUD_SAMPLE_S16},
    /* FLAC's decoded format depends on the file's bit depth, so its row
     * says "ask the track" the same way PCM's does. */
    [GAUD_CODING_FLAC] = {"flac", GAUD_SAMPLE_FORMAT_COUNT},
    /* The MPEG layers carry no bit depth at all, so unlike FLAC's row
     * there is nothing to ask the track: 16 is this library's choice and
     * is the same for every file. */
    [GAUD_CODING_MPEG_LAYER1] = {"mp1", GAUD_SAMPLE_S16},
    [GAUD_CODING_MPEG_LAYER2] = {"mp2", GAUD_SAMPLE_S16},
    [GAUD_CODING_MPEG_LAYER3] = {"mp3", GAUD_SAMPLE_S16},
};

/* A designated initialiser leaves an unmentioned row zeroed, and a zeroed
 * row's name is NULL rather than absent - so the count alone does not
 * prove the table is full. This is the assertion that does. */
_Static_assert(GAUD_CODING_COUNT == 10,
    "a coding was added: give it a row in table[] and a case in the "
    "container that spells it");

/** @brief A short lower-case name for @p coding. */
const char * gaud_sample_coding_name(GAUD_Sample_Coding coding) {
  if (coding < 0 || coding >= GAUD_CODING_COUNT || !table[coding].name) {
    return "unknown";
  }
  return table[coding].name;
}

/** @brief Whether @p coding stores samples uncoded. */
bool gaud_sample_coding_is_pcm(GAUD_Sample_Coding coding) {
  return coding == GAUD_CODING_PCM;
}

/** @brief What @p coding decodes to. */
GAUD_Sample_Format gaud_sample_coding_format(GAUD_Sample_Coding coding) {
  if (coding <= GAUD_CODING_PCM || coding >= GAUD_CODING_COUNT) {
    return GAUD_SAMPLE_FORMAT_COUNT;
  }
  return table[coding].format;
}
