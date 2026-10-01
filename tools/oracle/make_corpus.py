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


TAGS = [
    ("title", "A Title"),
    ("artist", "An Artist"),
    ("album", "An Album"),
    ("date", "2026"),
    ("genre", "Ambient"),
    ("comment", "caf\u00e9 \u65e5\u672c\u8a9e"),
]


def tagged_make(path, container, codec):
    """A short tagged file, with an ID3 chunk grafted on.

    ffmpeg writes its own scheme into each container and will not write
    an ID3 chunk into either, so the ID3 block is taken from an MP3
    ffmpeg *does* write one into and moved across. That keeps the bytes
    ffmpeg's own writer produced - a fixture this library authored would
    agree with this library's reader by construction.
    """
    import struct
    argv = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi",
            "-i", "aevalsrc=0.5*sin(2*PI*440*t):s=44100:d=0.05",
            "-c:a", codec, "-af", "atrim=end_sample=2205"]
    for key, value in TAGS:
        argv += ["-metadata", "%s=%s" % (key, value)]
    # -map_metadata 0 keeps the tags asked for above; the other fixtures
    # pass -1 to strip everything, which is what makes them byte-stable
    # across ffmpeg upgrades. A tagged fixture cannot have both.
    argv += ["-fflags", "+bitexact", "-flags:a", "+bitexact",
             "-map_metadata", "0", "-f", container, path]
    run(oracle.command("ffmpeg", argv, scratch=DATA))

    # The ID3 block, from an MP3 ffmpeg will write one into.
    source = os.path.join(DATA, ".id3-source.mp3")
    argv = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi",
            "-i", "aevalsrc=0.5*sin(2*PI*440*t):s=44100:d=0.05",
            "-c:a", "libmp3lame", "-write_id3v2", "1"]
    for key, value in TAGS:
        argv += ["-metadata", "%s=%s" % (key, value)]
    argv += ["-fflags", "+bitexact", "-f", "mp3", source]
    run(oracle.command("ffmpeg", argv, scratch=DATA))
    with open(source, "rb") as handle:
        mp3 = handle.read()
    os.remove(source)
    if mp3[:3] != b"ID3":
        raise SystemExit("ffmpeg wrote no ID3v2 block to graft")
    body = (((mp3[6] & 0x7F) << 21) | ((mp3[7] & 0x7F) << 14)
            | ((mp3[8] & 0x7F) << 7) | (mp3[9] & 0x7F))
    block = mp3[:10 + body]

    with open(path, "rb") as handle:
        data = bytearray(handle.read())
    big = container != "wav"
    identifier = b"ID3 " if big else b"id3 "
    order = ">I" if big else "<I"
    chunk = identifier + struct.pack(order, len(block)) + block
    if len(block) & 1:
        chunk += b"\0"
    data.extend(chunk)
    struct.pack_into(order, data, 4, len(data) - 8)
    with open(path, "wb") as handle:
        handle.write(bytes(data))


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


# Phase 4's FLAC fixtures, and **two writers rather than one**.
#
# ffmpeg's FLAC encoder is native to libavcodec and the `flac` tool is
# libFLAC's own, and they make different choices: different LPC orders,
# different partition orders, different stereo decorrelations on the same
# input. A decoder that had only ever met one of them has met an encoder's
# habits rather than the format. This matters more for FLAC than for PCM,
# where there is nothing to choose.
#
# The signals are chosen to reach subframe types a musical signal never
# does. Four of the five arms of the subframe decoder - CONSTANT, VERBATIM,
# FIXED and LPC - are selected by the encoder from the signal, so the only
# way to exercise them from a corpus is to supply signals that force each.
#
#   name, writer, bits, channels, rate, frames, signal, extra flags
FLAC_CASES = [
    # ffmpeg's encoder, which is the independent implementation of the two.
    ("flac_ff_s16_stereo_44100", "ffmpeg", 16, 2, 44100, 4409, "tone", []),
    ("flac_ff_s24_stereo_48000", "ffmpeg", 24, 2, 48000, 2399, "tone", []),
    # libFLAC, at both ends of its effort range. -0 picks fixed predictors
    # and -8 picks LPC with an exhaustive search, so the two differ in which
    # subframe type almost every block uses.
    ("flac_lib0_s16_stereo_44100", "flac", 16, 2, 44100, 4409, "tone",
     ["-0"]),
    ("flac_lib8_s16_stereo_44100", "flac", 16, 2, 44100, 4409, "tone",
     ["-8"]),
    # -e is exhaustive model search, which is where libFLAC emits escaped
    # Rice partitions: without it the escape arm of the residual reader is
    # reached by nothing in this corpus.
    ("flac_lib8e_s16_mono_44100", "flac", 16, 1, 44100, 3001, "tone",
     ["-8", "-e"]),
    ("flac_lib_s8_mono_8000", "flac", 8, 1, 8000, 2003, "tone", ["-5"]),
    ("flac_lib_s32_stereo_96000", "flac", 32, 2, 96000, 1777, "tone", ["-5"]),
    ("flac_lib_s16_5dot1_48000", "flac", 16, 6, 48000, 1499, "tone", ["-5"]),
    # Silence: every subframe CONSTANT, which no musical fixture produces.
    ("flac_lib_silence_44100", "flac", 16, 2, 44100, 2003, "silence",
     ["-5"]),
    # Full-scale noise: nothing predicts, so the encoder falls back to
    # VERBATIM or a zero-order fixed predictor with large residuals.
    ("flac_lib_noise_44100", "flac", 16, 1, 44100, 3001, "noise", ["-5"]),
    # Every sample a multiple of 256, which is what 8-bit material carried
    # in a 16-bit file looks like - and the only thing that exercises the
    # wasted-bits path on either side.
    ("flac_lib_wasted_44100", "flac", 16, 2, 44100, 2003, "wasted", ["-5"]),

    # The six below are not "more coverage" in the vague sense. Each was
    # added because an instrumented decode of the corpus above showed a
    # named arm of the subframe reader that nothing reached, and each was
    # kept only after the measurement showed it now does. The arms, and
    # what forces them, are in notes/audio/flac-coverage.md.
    #
    # A square wave: long constant runs inside a block that is not
    # constant, which is what makes libFLAC pick the first-order
    # predictor and a deep partition order.
    ("flac_lib_square_44100", "flac", 16, 1, 44100, 4409, "square",
     ["-8", "-e"]),
    # A sawtooth with LPC switched off, so the fixed orders compete
    # against each other rather than all losing to LPC.
    ("flac_lib_ramp_44100", "flac", 16, 1, 44100, 4409, "ramp",
     ["-0", "--max-lpc-order=0"]),
    # Noise so quiet that nothing predicts it: the zeroth-order fixed
    # predictor, which predicts zero, is the one that wins.
    ("flac_lib_tiny_44100", "flac", 16, 1, 44100, 4409, "tiny",
     ["-0", "--max-lpc-order=0"]),
    # The right channel plus a little independent noise in the left. The
    # side is then cheap and the right is cheap and the left is not,
    # which is the only arrangement that makes **side/right** the
    # cheapest of the four channel assignments. A stereo corpus of
    # ordinary music reaches independent, left/side and mid/side and
    # never this one.
    ("flac_lib_sideright_44100", "flac", 16, 2, 44100, 4409, "sideright",
     ["-8"]),
    # Seven closely spaced partials, and an exhaustive search up to order
    # 32 - which is outside the FLAC Subset, hence --lax. Nothing at the
    # default maximum of 12 reaches the top half of the coefficient loop.
    ("flac_lib_rich_44100", "flac", 16, 1, 44100, 8819, "rich",
     ["-8", "-e", "-l", "32", "--lax"]),
    # A sample rate the frame header has no code for, so it is written as
    # a 16-bit field at the end of the header. Every rate in the rest of
    # the corpus is one of the twelve the format tabulates.
    ("flac_lib_rate12345", "flac", 16, 1, 12345, 2003, "tone", ["-5"]),
]

# The same bitstream in Ogg, from both writers. The frame decoder is shared
# with the native container, so what these score is the page layer: lacing,
# the granule positions, and the mapping's first packet.
FLAC_OGG_CASES = [
    ("oggflac_ff_s16_stereo_44100", "ffmpeg", 16, 2, 44100, 4409, "tone"),
    ("oggflac_lib_s16_stereo_44100", "flac", 16, 2, 44100, 4409, "tone"),
    ("oggflac_lib_s24_mono_48000", "flac", 24, 1, 48000, 2399, "tone"),
]

SIGNALS = {
    "tone": "0.6*sin(2*PI*%(f)d*t)+0.25*sin(2*PI*%(g)d*t)",
    "silence": "0",
    # Bipolar, and the `2*x-1` is the whole of why. ffmpeg's `random()`
    # returns [0,1), so `0.98*random()` is a signal with a DC offset of
    # half its amplitude - and the gate's DC check, which exists to catch
    # a sign error in a decoder, correctly reported 0.49 on it. A fixture
    # that forces a gate to be relaxed is a bad fixture, not a reason to
    # relax the gate.
    "noise": "0.98*(2*random(%(c)d)-1)",
    # Quantised to 256, so every sample has eight low zero bits. The
    # rounding has to happen in the signal rather than afterwards, because
    # ffmpeg's own sample conversion would reintroduce the low bits.
    "wasted": "floor(256*(0.5*sin(2*PI*%(f)d*t)))*0.0078125",
    # No commas in any of these: ffmpeg's filtergraph splits its arguments
    # on a comma before the expression parser ever sees it, so `mod(a,b)`
    # silently becomes two filters. It is spelled out as
    # `a-b*floor(a/b)` instead, which is the same function.
    "square": "0.7*(2*floor(2*(t*50-floor(t*50)))-1)",
    "ramp": "2*(t*20-floor(t*20))-1",
    "tiny": "0.004*(2*random(%(c)d)-1)",
    "sideright": "0.5*sin(2*PI*220*t)+0.02*(2*random(1)-1)"
                 "|0.5*sin(2*PI*220*t)",
    "rich": "0.15*(sin(2*PI*110*t)+sin(2*PI*113*t)+sin(2*PI*220*t)"
            "+sin(2*PI*227*t)+sin(2*PI*331*t)+sin(2*PI*337*t)"
            "+sin(2*PI*447*t))",
}


def raw_signal(path, signal, bits, channels, rate, frames):
    """Write raw signed little-endian PCM for an encoder to read."""
    duration = frames / float(rate)
    if "|" in SIGNALS[signal]:
        # An expression that already names every channel - the only way
        # to make two channels that are deliberately related rather than
        # independent, which is what the side/right case needs.
        exprs = SIGNALS[signal]
    else:
        exprs = "|".join(
            SIGNALS[signal] % {"c": i, "f": 220 * (i + 1), "g": 1330 + 97 * i}
            for i in range(channels))
    fmt = {8: "s8", 16: "s16le", 24: "s32le", 32: "s32le"}[bits]
    argv = [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
        "-f", "lavfi",
        "-i", "aevalsrc=%s:s=%d:d=%.9f" % (exprs, rate, duration),
        "-af", "atrim=end_sample=%d" % frames,
        "-fflags", "+bitexact", "-flags:a", "+bitexact", "-map_metadata", "-1",
        "-f", fmt, path,
    ]
    run(oracle.command("ffmpeg", argv, scratch=DATA))
    if bits == 24:
        # ffmpeg has no packed 24-bit raw output, so the samples come out
        # as 32-bit and the low byte is dropped here. Dropping the LOW byte
        # and not the high one: a 24-bit sample is the top 24 bits of the
        # 32-bit one ffmpeg produced, and taking the other three bytes
        # would be a signal with a quarter of the amplitude and a sawtooth
        # of noise on it.
        with open(path, "rb") as handle:
            wide = handle.read()
        narrow = bytearray()
        for i in range(0, len(wide), 4):
            narrow += wide[i + 1:i + 4]
        with open(path, "wb") as handle:
            handle.write(bytes(narrow))


def flac_make(path, writer, bits, channels, rate, frames, signal, flags,
              ogg=False):
    """One FLAC fixture, from whichever reference writes it."""
    raw = path + ".raw"
    raw_signal(raw, signal, bits, channels, rate, frames)
    if writer == "flac":
        argv = ["flac", "-s", "-f", "--force-raw-format", "--endian=little",
                "--sign=signed", "--channels=%d" % channels,
                "--bps=%d" % bits, "--sample-rate=%d" % rate,
                "--no-padding", "--no-seektable"]
        if ogg:
            argv.append("--ogg")
        argv += list(flags) + ["-o", path, raw]
        run(oracle.command("flac", argv, scratch=DATA))
    else:
        fmt = {8: "s8", 16: "s16le", 24: "s24le", 32: "s32le"}[bits]
        argv = [
            "ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-f", fmt, "-ar", str(rate), "-ac", str(channels), "-i", raw,
            "-c:a", "flac", "-bits_per_raw_sample", str(bits),
            "-fflags", "+bitexact", "-flags:a", "+bitexact",
            "-map_metadata", "-1",
            "-f", "ogg" if ogg else "flac", path,
        ]
        if bits == 32:
            argv[-3:-3] = ["-strict", "-2"]
        run(oracle.command("ffmpeg", argv, scratch=DATA))
    os.remove(raw)


def main():
    os.makedirs(DATA, exist_ok=True)
    print(oracle.provenance(
        ["ffmpeg", "sox", "libsndfile", "pywave", "flac"]))

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

    # Tagged fixtures, so that every gate that loads the corpus walks
    # the metadata path as well as the sample path - and so the
    # container fuzzers have seeds with real chunks in them rather than
    # only `fmt ` and `data`. ffmpeg writes LIST/INFO into a WAV and
    # NAME/ANNO into an AIFF; the ID3 chunk is grafted on afterwards,
    # because ffmpeg will not put one in either container itself and
    # that is the chunk most of this library's reader is about.
    for name, container, extension, codec in (
            ("wav_tagged_s16_44100", "wav", "wav", "pcm_s16le"),
            ("aiff_tagged_s16_44100", "aiff", "aiff", "pcm_s16be")):
        path = os.path.join(DATA, "%s.%s" % (name, extension))
        tagged_make(path, container, codec)
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

    # FLAC, from both writers.
    for name, writer, bits, channels, rate, frames, signal, flags \
            in FLAC_CASES:
        path = os.path.join(DATA, "%s.flac" % name)
        flac_make(path, writer, bits, channels, rate, frames, signal, flags)
        made.append(path)
        print("  %-30s %s" % (name, os.path.getsize(path)))

    for name, writer, bits, channels, rate, frames, signal in FLAC_OGG_CASES:
        path = os.path.join(DATA, "%s.oga" % name)
        flac_make(path, writer, bits, channels, rate, frames, signal, [],
                  ogg=True)
        made.append(path)
        print("  %-30s %s" % (name, os.path.getsize(path)))

    print("%d fixtures in tests/data/" % len(made))


if __name__ == "__main__":
    main()
