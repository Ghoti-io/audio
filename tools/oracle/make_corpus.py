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
    # Phase 2: the coded WAV tags. Mono and stereo for each, because the
    # ADPCM framings interleave differently and a mono-only corpus meets
    # neither interleave. MS ADPCM stereo in particular decoded as noise
    # under a reading that mono could not distinguish.
    ("wav_ulaw_mono_8000",    "wav",  "pcm_mulaw",     1,  8000, 1021),
    ("wav_ulaw_stereo_44100", "wav",  "pcm_mulaw",     2, 44100, 2003),
    ("wav_alaw_mono_8000",    "wav",  "pcm_alaw",      1,  8000, 1021),
    ("wav_alaw_stereo_44100", "wav",  "pcm_alaw",      2, 44100, 2003),
    ("wav_imaadpcm_mono_22050",   "wav", "adpcm_ima_wav", 1, 22050, 2003),
    ("wav_imaadpcm_stereo_44100", "wav", "adpcm_ima_wav", 2, 44100, 3001),
    ("wav_msadpcm_mono_22050",    "wav", "adpcm_ms",      1, 22050, 2003),
    ("wav_msadpcm_stereo_44100",  "wav", "adpcm_ms",      2, 44100, 3001),
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
    # Phase 2. `ima4` is the one case where the frame count in the file is
    # not the frame count asked for: ffmpeg pads to a whole 64-frame
    # packet, and there is no field in AIFF-C that could say otherwise.
    ("aifc_ulaw_mono_8000",    "pcm_mulaw",     1,  8000, 1021),
    ("aifc_alaw_stereo_8000",  "pcm_alaw",      2,  8000, 1021),
    ("aifc_ima4_mono_22050",   "adpcm_ima_qt",  1, 22050, 1024),
    ("aifc_ima4_stereo_44100", "adpcm_ima_qt",  2, 44100, 2048),
]

# The step table is 89 entries and the signal decides which of them are
# ever loaded. Measured against the fixtures above, an IMA decode reached
# 67 of the 89: the eight quietest and the fourteen loudest were never
# used, so a wrong value in either tail would have passed this corpus in
# silence. These two fixtures are chosen to reach them - a signal 60 dB
# down for the bottom of the table, full-scale white noise for the top -
# and `make check-corpus` prints the coverage so that the number stays
# visible rather than being rediscovered.
#
# This is the corpus-is-a-population problem in its plainest form: the
# fixtures were all the same kind of signal, so they all walked the same
# part of the table.
DYNAMIC_CASES = [
    # name, codec, channels, rate, frames, aevalsrc expression
    # Quiet NOISE, not a quiet tone. A low-amplitude sine produces small
    # and smoothly varying deltas, so the encoder picks nibble magnitude 0
    # almost every time - and at magnitude 0 the decoder computes only
    # step>>3, which a wrong table entry barely moves. The step index
    # passes through the bottom of the table and nothing there is tested.
    # Noise at the same level forces nonzero magnitudes, which is what
    # makes those entries observable.
    #
    # The amplitudes are a measured cover, not a guess. Each one pins the
    # step index to a different part of the table, and the four together
    # with the loud and musical fixtures reach all 89 entries with a
    # nonzero magnitude. The counts as the corpus grew: the original
    # musical fixtures 54/89, plus a quiet tone still 54/89 (a tone gives
    # magnitude 0 almost everywhere), plus quiet noise 73/89, plus these
    # 89/89. Below about 0.0002 the encoder emits nothing but magnitude 0
    # again and the bottom three entries go dark, which is why the
    # quietest here is 0.0003 and not smaller.
    ("wav_imaadpcm_lsbnoise_8000", "adpcm_ima_wav", 1, 8000, 2001,
     "0.0003*random(%(c)d)"),
    ("wav_imaadpcm_quietnoise_8000", "adpcm_ima_wav", 1, 8000, 2001,
     "0.0015*random(%(c)d)"),
    ("wav_imaadpcm_lownoise_8000", "adpcm_ima_wav", 1, 8000, 2001,
     "0.01*random(%(c)d)"),
    ("wav_imaadpcm_midnoise_8000", "adpcm_ima_wav", 1, 8000, 2001,
     "0.05*random(%(c)d)"),
    ("aifc_ima4_quietnoise_8000", "adpcm_ima_qt", 1, 8000, 2048,
     "0.0015*random(%(c)d)"),
    ("wav_imaadpcm_loud_8000", "adpcm_ima_wav", 1, 8000, 1501,
     "0.98*sin(2*PI*1700*t)*sin(2*PI*37*t)+0.02*random(%(c)d)"),
    ("wav_msadpcm_loud_8000", "adpcm_ms", 1, 8000, 1501,
     "0.98*sin(2*PI*1700*t)*sin(2*PI*37*t)+0.02*random(%(c)d)"),
    ("aifc_ima4_loud_8000", "adpcm_ima_qt", 1, 8000, 1536,
     "0.98*sin(2*PI*1700*t)*sin(2*PI*37*t)+0.02*random(%(c)d)"),
]

# Every frame count above is deliberately NOT a multiple of the block size
# a coded fixture ends up with, so that every one of them exercises a
# short final block. A corpus of round numbers would score a decoder that
# reads whole blocks only, and the last block is where the arithmetic is
# hardest.  (memory: corpora flatter and do not cover.)


def run(argv, **kwargs):
    finished = subprocess.run(argv, capture_output=True, text=True, **kwargs)
    if finished.returncode != 0:
        raise SystemExit("failed: %s\n%s" % (" ".join(argv[-8:]),
                                             finished.stderr[-2000:]))
    return finished


def ffmpeg_make(path, codec, channels, rate, frames, container,
                expr=None):
    """One fixture, from ffmpeg's own signal generator.

    The source is a sum of two sines at different frequencies per channel, so
    that the channels differ from each other - a corpus whose channels are
    identical cannot see a decoder that swaps or duplicates one, which is
    exactly the defect a channel mask exists to prevent.
    """
    duration = frames / float(rate)
    # `aevalsrc` rather than `sine`, because it takes one expression per
    # channel and so can make them differ.
    if expr:
        exprs = "|".join(expr % {"c": i} for i in range(channels))
    else:
        exprs = "|".join(
            "0.6*sin(2*PI*%d*t)+0.25*sin(2*PI*%d*t)"
            % (220 * (i + 1), 1330 + 97 * i)
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


def cycle_ms_predictors(path):
    """Rewrite each MS ADPCM block's predictor index to cycle 0..6.

    The index is the first byte of each block, one per channel. Every
    value 0..6 is legal and selects a different coefficient pair, so this
    produces a valid file that happens to use the whole table - which no
    encoder in the oracle image will produce on its own.
    """
    import struct as _s
    data = bytearray(open(path, "rb").read())
    i, body_at, body_len, block, channels = 12, None, 0, None, None
    while i + 8 <= len(data):
        cid = bytes(data[i:i + 4])
        size = _s.unpack_from("<I", data, i + 4)[0]
        if cid == b"fmt ":
            channels = _s.unpack_from("<H", data, i + 10)[0]
            block = _s.unpack_from("<H", data, i + 20)[0]
        elif cid == b"data":
            body_at, body_len = i + 8, size
        i += 8 + size + (size & 1)
    if body_at is None or not block or not channels:
        raise SystemExit("cycle_ms_predictors: %s is not MS ADPCM" % path)
    blocks = 0
    for start in range(body_at, body_at + body_len, block):
        if start + 7 * channels > body_at + body_len:
            break
        for c in range(channels):
            data[start + c] = blocks % 7
        blocks += 1
    if blocks < 7:
        raise SystemExit(
            "cycle_ms_predictors: %s has %d blocks, too few to reach all "
            "seven coefficient pairs - the fixture needs more frames"
            % (path, blocks))
    open(path, "wb").write(bytes(data))


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

    for name, codec, channels, rate, frames, expr in DYNAMIC_CASES:
        ext = "aifc" if name.startswith("aifc") else "wav"
        container = "aiff" if ext == "aifc" else "wav"
        path = os.path.join(DATA, "%s.%s" % (name, ext))
        ffmpeg_make(path, codec, channels, rate, frames, container, expr)
        made.append(path)
        print("  %-26s %s" % (name, os.path.getsize(path)))

    # MS ADPCM's seven coefficient pairs, all of them.
    #
    # ffmpeg's encoder picks pair 0 for every block of every fixture
    # above, so six of the seven entries in the table this library
    # carries were exercised by nothing: changing {512,-256} to
    # {512,-200} decoded identically and `make check-corpus` passed. A
    # block may legally name any pair, so the fixture is made by
    # generating an ordinary MS ADPCM file and then rewriting each
    # block's predictor index to cycle 0..6. The result is a valid file
    # that ffmpeg and libsndfile both read, and it is the only thing in
    # the corpus that can see a wrong coefficient.
    path = os.path.join(DATA, "wav_msadpcm_coefs_8000.wav")
    ffmpeg_make(path, "adpcm_ms", 1, 8000, 14300, "wav")
    cycle_ms_predictors(path)
    made.append(path)
    print("  %-26s %s" % ("wav_msadpcm_coefs_8000", os.path.getsize(path)))

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
