#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Audio.
#
# Ghoti.io Audio is free software: you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
"""Generate tests/data/ with the pinned references.

    make corpus

**The fixtures are written by ffmpeg and sox, not by this library.** A corpus
grown from our own writer measures our own writer: it would agree with our
reader by construction, and every disagreement the differential exists to find
would already have been excluded. The generator therefore only ever asks a
reference to produce a file, and the committed result is an input.

What is varied, and why each axis is here rather than being one more file:

  format      Every PCM width both containers can carry. The two disagree
              about the sign of an 8-bit sample - WAV's is unsigned, AIFF's
              signed - which is the single most likely thing for a reader to
              get wrong, and it is invisible unless both are present.
  endianness  WAV is little-endian and AIFF big, so the corpus covers both
              orders on any host. AIFF-C `sowt` adds a little-endian AIFF,
              which is the case where the container's order and the format's
              default disagree.
  channels    1, 2 and 6. Six is where a channel mask stops being decorative:
              a 5.1 file whose centre and LFE are swapped sounds wrong and
              passes any test that compares total energy.
  length      Deliberately odd frame counts, so that the data chunk's length
              is odd and the pad byte both formats require is exercised. A
              corpus of round numbers never writes one.
  rate        44100 and 48000, plus 22050 - and for AIFF that matters more
              than it looks, because AIFF stores the rate as an 80-bit float
              and a rate that is not a power-of-two multiple exercises the
              significand rather than the exponent.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import oracle_env as oracle  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(ROOT, "tests", "data")

# name -> (container, ffmpeg codec, channels, rate, frames)
#
# The frame counts are prime-ish on purpose: an even byte count never
# exercises the pad byte, and a count divisible by the probe's block size
# never exercises the short read at the end.
CASES = [
    ("wav_u8_mono_22050",     "wav",  "pcm_u8",    1, 22050, 1021),
    ("wav_s16_stereo_44100",  "wav",  "pcm_s16le", 2, 44100, 4409),
    ("wav_s24_stereo_48000",  "wav",  "pcm_s24le", 2, 48000, 2399),
    ("wav_s32_stereo_44100",  "wav",  "pcm_s32le", 2, 44100, 1777),
    ("wav_f32_stereo_48000",  "wav",  "pcm_f32le", 2, 48000, 1301),
    ("wav_f64_mono_44100",    "wav",  "pcm_f64le", 1, 44100,  997),
    ("wav_s16_5dot1_48000",   "wav",  "pcm_s16le", 6, 48000, 1499),
    ("aiff_s8_mono_22050",    "aiff", "pcm_s8",    1, 22050, 1021),
    ("aiff_s16_stereo_44100", "aiff", "pcm_s16be", 2, 44100, 4409),
    ("aiff_s24_stereo_48000", "aiff", "pcm_s24be", 2, 48000, 2399),
    ("aiff_s32_stereo_44100", "aiff", "pcm_s32be", 2, 44100, 1777),
]

# AIFF-C, whose compression type says the samples are little-endian. ffmpeg
# spells the container `aiff` and picks AIFC when the codec demands it.
AIFC_CASES = [
    ("aifc_sowt_stereo_44100", "pcm_s16le", 2, 44100, 2003),
    ("aifc_fl32_stereo_48000", "pcm_f32be", 2, 48000, 1499),
]


def run(argv, **kwargs):
    finished = subprocess.run(argv, capture_output=True, text=True, **kwargs)
    if finished.returncode != 0:
        raise SystemExit("failed: %s\n%s" % (" ".join(argv[-8:]),
                                             finished.stderr[-2000:]))
    return finished


def ffmpeg_make(path, codec, channels, rate, frames, container):
    """One fixture, from ffmpeg's own signal generator.

    The source is a sum of two sines at different frequencies per channel, so
    that the channels differ from each other - a corpus whose channels are
    identical cannot see a decoder that swaps or duplicates one, which is
    exactly the defect a channel mask exists to prevent.
    """
    duration = frames / float(rate)
    # `aevalsrc` rather than `sine`, because it takes one expression per
    # channel and so can make them differ.
    exprs = "|".join(
        "0.6*sin(2*PI*%d*t)+0.25*sin(2*PI*%d*t)" % (220 * (i + 1), 1330 + 97 * i)
        for i in range(channels))
    argv = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi",
        "-i", "aevalsrc=%s:s=%d:d=%.9f" % (exprs, rate, duration),
        "-c:a", codec,
        # Exactly the frame count asked for: -t rounds, and a fixture whose
        # length drifted with ffmpeg's rounding would make every length
        # assertion below a moving target.
        "-af", "atrim=end_sample=%d" % frames,
        # No encoder string and no creation time, or the fixture's bytes
        # change whenever ffmpeg is upgraded and the hash gate fires for a
        # reason that is not a finding.
        "-fflags", "+bitexact", "-flags:a", "+bitexact",
        "-map_metadata", "-1",
        "-f", container, path,
    ]
    run(oracle.command("ffmpeg", argv, scratch=DATA))


def main():
    os.makedirs(DATA, exist_ok=True)
    print(oracle.provenance(["ffmpeg", "sox", "libsndfile", "pywave"]))

    made = []
    for name, container, codec, channels, rate, frames in CASES:
        ext = "wav" if container == "wav" else "aiff"
        path = os.path.join(DATA, "%s.%s" % (name, ext))
        ffmpeg_make(path, codec, channels, rate, frames, container)
        made.append(path)
        print("  %-26s %s" % (name, os.path.getsize(path)))

    for name, codec, channels, rate, frames in AIFC_CASES:
        path = os.path.join(DATA, "%s.aifc" % name)
        ffmpeg_make(path, codec, channels, rate, frames, "aiff")
        made.append(path)
        print("  %-26s %s" % (name, os.path.getsize(path)))

    # A second writer for the same question. sox's RIFF differs from
    # ffmpeg's in what optional chunks it emits, so a reader that only ever
    # met ffmpeg's layout has met one writer's habits rather than the format.
    path = os.path.join(DATA, "sox_s16_stereo_44100.wav")
    run(oracle.command("sox", [
        "sox", "-R", "-n", "-b", "16", "-r", "44100", "-c", "2",
        "-e", "signed-integer", path, "synth", "0.05", "sine", "440",
        "sine", "660",
    ], scratch=DATA))
    made.append(path)
    print("  %-26s %s" % ("sox_s16_stereo_44100", os.path.getsize(path)))

    print("%d fixtures in tests/data/" % len(made))


if __name__ == "__main__":
    main()
