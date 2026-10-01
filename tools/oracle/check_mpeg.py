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
"""Score this library's MPEG audio decode against the reference decoders.

    make check-mpeg

**This gate is not a byte comparison, and the difference from
`check-corpus` is the whole design.** PCM and FLAC are exact: a decode
either matches or it is a defect. MPEG audio is not - the format defines
no sample values, only a transform, and this library's is integer where
both references are floating point. So the comparison is a tolerance, and
planning/audio.md section 12 says what has to be checked *besides* the
difference, because every one of these failures passes a difference-only
gate:

  - **a decoder that emits silence** produces the right number of bytes,
    crashes nothing, and sounds like a quiet passage;
  - **a decoder 6 dB down** passes any threshold scaled to the signal;
  - **a decoder one frame short** passes any comparison that aligns the
    two signals before measuring, which is what every convenient
    comparison does;
  - **a decoder with a swapped or silent channel** hides behind the
    other channel's energy.

So: the frame count is asserted exactly before anything is compared, the
absolute level of our own output is checked and not only the difference,
the DC offset is checked, and all of it is per channel. And the whole
thing is run against a deliberately wrong version of our own answer,
which must fail - a gate never seen to fail is not known to work.

The measured agreement, for context on the thresholds below: every
fixture in the corpus agrees with ffmpeg to **within one least
significant bit** of 16, and the difference is 88 to 106 dB below the
signal. The thresholds are an order of magnitude looser than that, which
leaves room for a reference upgrade to move the last bit and no room for
a decoder defect.
"""

import math
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import oracle_env as oracle  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(ROOT, "tests", "data")
PROBE = os.path.join(ROOT, "build", "linux", "release", "apps", "oracle",
    "dump_probe")
OUT = os.path.join(ROOT, "tests", "out")

#: The most a sample may differ from a reference's, in units of the
#: 16-bit output. One is what is measured everywhere; two is the gate.
MAX_SAMPLE = 2

#: The most the difference's RMS may be, relative to the signal's.
#: Measured worst case is 3.9e-05; this is 1e-04.
MAX_RELATIVE_RMS = 1.0e-4

#: How far the absolute level may differ, relative. A decoder 6 dB down
#: is 0.5 here, and 1e-3 is far inside what rounding can produce.
MAX_LEVEL = 1.0e-3

#: How far the DC offset may differ, in output units.
MAX_DC = 1.0

# Where a reference cannot answer, why, and what was measured to
# establish it. Named here rather than skipped quietly: an exclusion that
# is not written down is indistinguishable from a reference nobody
# thought to ask, and a score whose denominator shrank without saying so
# is inflated.
EXCLUSIONS = {
    ("ffmpeg", "mp3_tagged_stereo_44100.mp3"): (
        "ffmpeg's end-of-stream trim is defeated by a 128-byte ID3v1 "
        "trailer: it decodes this file to 2,351 frames where libsndfile "
        "decodes 2,003, and 2,351 is 3,456 minus the 1,105-frame start "
        "trim - the start applied and the end not. The minimal pair "
        "settles it: the same file written with -write_id3v1 0 decodes "
        "to 2,003 in ffmpeg too, and 2,003 is the number of samples the "
        "encoder was given. We agree with libsndfile and with the "
        "generator. The *samples* still agree with ffmpeg to one bit over "
        "the frames both produce, so only the length is excluded."),
    ("ffmpeg", "mp2_twolame_joint_44100.mp2"): (
        "ffmpeg's mp3 demuxer drops this file's first frame - it says "
        "'Skipping 626 bytes of junk at 0' - because TwoLAME switches the "
        "channel mode frame by frame: frame 0 is joint stereo, frames 1 "
        "and 2 are plain stereo, frame 3 is joint stereo with a different "
        "bound. All of that is legal; the mode is a per-frame field. "
        "ffmpeg's probe requires it to be stable and ours deliberately "
        "does not, because a real encoder changes it. "
        "**The samples are not in dispute.** Decoded from frame 1 and "
        "with ffmpeg's own first 600 samples set aside - where its "
        "filterbank is cold because it skipped a frame and ours is not - "
        "ffmpeg agrees with us to a rms difference of 0.5 in a signal of "
        "15,020, which is the same one-bit agreement as everywhere else. "
        "libsndfile reads all four frames and agrees with us exactly, so "
        "this fixture is still scored."),
    ("ffmpeg", "mp3_lame_mono_11025.mp3"): (
        "MPEG-2.5, which this library identifies and does not decode: "
        "its Layer III scalefactor band tables are in no standard, so "
        "tools/tables/gen_mp3_tables.py has nothing to generate them "
        "from. There is no decode of ours to compare."),
    ("libsndfile", "mp3_lame_mono_11025.mp3"): (
        "As above: there is no decode of ours to compare."),
}


def run(argv, **kw):
    return subprocess.run(argv, capture_output=True, **kw)


def ours_meta(path):
    """The one metadata line dump_probe prints, as a dictionary."""
    finished = run([PROBE, path], text=True)
    if finished.returncode != 0:
        raise SystemExit("dump_probe failed on %s:\n%s"
                         % (path, finished.stderr.strip()))
    out = {}
    for pair in finished.stdout.split():
        if "=" in pair:
            key, value = pair.split("=", 1)
            out[key] = value
    return out


def ours_pcm(path):
    """Our decode, as a list of 16-bit samples."""
    finished = run([PROBE, path, "--pcm"])
    if finished.returncode != 0:
        return None
    raw = finished.stdout
    return list(struct.unpack("<%dh" % (len(raw) // 2), raw))


def ffmpeg_pcm(path):
    argv = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", path,
            "-f", "s16le", "-"]
    finished = run(oracle.command("ffmpeg", argv))
    if finished.returncode != 0:
        return None
    return list(struct.unpack("<%dh" % (len(finished.stdout) // 2),
        finished.stdout))


def libsndfile_pcm(path):
    script = (
        "import soundfile, sys\n"
        "data, rate = soundfile.read(%r, dtype='int16')\n"
        "sys.stdout.buffer.write(data.tobytes())\n" % path)
    finished = run(oracle.command("libsndfile", ["python3", "-c", script]))
    if finished.returncode != 0:
        return None
    return list(struct.unpack("<%dh" % (len(finished.stdout) // 2),
        finished.stdout))


def trim(samples, delay, padding, channels):
    """Our untrimmed decode, cut down to the recording.

    **This library hands back the file's contents and the references hand
    back the recording**, which is not a disagreement: gaud_track_frames()
    is documented as the contents and gaud_track_trim() as the difference,
    so the comparison applies the trim rather than asking either side to
    change. The frame-count assertions below are what keep this from being
    a free parameter - the trimmed length has to equal the reference's
    exactly, so a wrong trim fails here rather than hiding here.
    """
    out = samples[delay * channels:]
    if padding:
        cut = padding * channels
        out = out[:-cut] if cut < len(out) else []
    return out


def measure(samples, channels):
    """Per-channel RMS and DC, and the same over everything."""
    out = []
    for ch in range(channels):
        column = samples[ch::channels]
        if not column:
            out.append((0.0, 0.0))
            continue
        out.append((math.sqrt(sum(x * x for x in column) / len(column)),
                    sum(column) / len(column)))
    return out


def compare(name, ours, theirs, channels, reference, quiet=False):
    """Every check in planning/audio.md section 12, in its order."""
    bad = []
    label = "%s / %s" % (name, reference)

    # The frame count, before anything is compared. A decoder a frame
    # short passes every comparison that aligns first.
    if len(ours) != len(theirs):
        bad.append("%s: we produce %d sample frames and it produces %d"
                   % (label, len(ours) // channels, len(theirs) // channels))
        return bad
    if not ours:
        bad.append("%s: nothing was decoded, so nothing was compared"
                   % label)
        return bad

    mine = measure(ours, channels)
    yours = measure(theirs, channels)
    for ch in range(channels):
        # The absolute level of our own output, not only the difference.
        # A decode 6 dB down fails here and nowhere else.
        if yours[ch][0] > 1.0:
            ratio = mine[ch][0] / yours[ch][0]
            if abs(ratio - 1.0) > MAX_LEVEL:
                bad.append("%s: channel %d is at %.1f and it says %.1f, a "
                           "ratio of %.6f" % (label, ch, mine[ch][0],
                               yours[ch][0], ratio))
        elif mine[ch][0] > 1.0:
            bad.append("%s: channel %d is at %.1f where it is silent"
                       % (label, ch, mine[ch][0]))
        # The DC offset, which catches a sign or bias error that an RMS
        # comparison forgives.
        if abs(mine[ch][1] - yours[ch][1]) > MAX_DC:
            bad.append("%s: channel %d has a DC offset of %.2f and it says "
                       "%.2f" % (label, ch, mine[ch][1], yours[ch][1]))

    # And the difference itself, per channel.
    for ch in range(channels):
        column = [ours[i] - theirs[i]
                  for i in range(ch, len(ours), channels)]
        worst = max(abs(x) for x in column)
        rms = math.sqrt(sum(x * x for x in column) / len(column))
        signal = yours[ch][0]
        if worst > MAX_SAMPLE:
            at = max(range(len(column)), key=lambda i: abs(column[i]))
            bad.append("%s: channel %d differs by %d at sample %d, and the "
                       "most a rounding difference can be is %d"
                       % (label, ch, worst, at, MAX_SAMPLE))
        if signal > 1.0 and rms / signal > MAX_RELATIVE_RMS:
            bad.append("%s: channel %d's difference is %.3f against a "
                       "signal of %.1f, which is %.1f dB down and the gate "
                       "is %.1f" % (label, ch, rms, signal,
                           20 * math.log10(rms / signal) if rms else -999,
                           20 * math.log10(MAX_RELATIVE_RMS)))
    if not bad and not quiet:
        worst = max(abs(ours[i] - theirs[i]) for i in range(len(ours)))
        print("  %-34s %-11s %6d frames, worst sample %d"
              % (name, reference, len(ours) // channels, worst))
    return bad


def scaled(samples, factor):
    return [int(round(x * factor)) for x in samples]


def main():
    print(oracle.provenance(["ffmpeg", "libsndfile"]))
    if not os.path.isfile(PROBE):
        raise SystemExit("dump_probe is not built: %s\nRun `make "
                         "oracle-probe`." % PROBE)
    files = sorted(f for f in os.listdir(DATA)
                   if f.endswith((".mp3", ".mp2", ".mp1")))
    if not files:
        raise SystemExit("tests/data holds no MPEG audio, so this gate is "
                         "measuring nothing. Run `make corpus`.")

    bad = []
    scored = 0
    excluded = set()
    undecodable = []
    for name in files:
        path = os.path.join(DATA, name)
        meta = ours_meta(path)
        channels = int(meta["channels"])
        stated = int(meta["frames"])
        delay = int(meta["delay"])
        padding = int(meta["padding"])

        raw = ours_pcm(path)
        if raw is None:
            undecodable.append(name)
            continue
        # The file's own contents, before the trim: the loader states a
        # frame count and the decoder has to produce exactly it. This is
        # the one assertion that needs no reference at all.
        if stated != 0xFFFFFFFFFFFFFFFF and len(raw) // channels != stated:
            bad.append("%s: the track states %d sample frames and the "
                       "decoder produced %d" % (name, stated,
                           len(raw) // channels))
        ours = trim(raw, delay, padding, channels)

        for reference, decode in (("ffmpeg", ffmpeg_pcm),
                                  ("libsndfile", libsndfile_pcm)):
            if (reference, name) in EXCLUSIONS:
                excluded.add((reference, name))
                continue
            theirs = decode(path)
            if theirs is None:
                bad.append("%s / %s: the reference could not read a file "
                           "this library decoded, which is a finding and "
                           "not a skip" % (name, reference))
                continue
            bad += compare(name, ours, theirs, channels, reference)
            scored += 1

    # The control. It mutates **our** answer rather than the fixture: a
    # differential cannot see a bad input, because the references read the
    # same bad input and agree with us about it. What it has to be able to
    # see is a bad reader.
    print("\n  control: this gate must reject each of these")
    victim = os.path.join(DATA, "mp3_lame_stereo_44100.mp3")
    meta = ours_meta(victim)
    channels = int(meta["channels"])
    truth = trim(ours_pcm(victim), int(meta["delay"]), int(meta["padding"]),
        channels)
    reference = ffmpeg_pcm(victim)
    cases = [
        ("all silence", [0] * len(truth)),
        ("6 dB down", scaled(truth, 0.5)),
        ("0.1 dB down", scaled(truth, 0.98851)),
        ("one frame short", truth[:-channels]),
        ("one sample changed by three", truth[:200] + [truth[200] + 3]
            + truth[201:]),
        ("a DC offset of two", [x + 2 for x in truth]),
        ("the channels swapped", [truth[i ^ 1] for i in range(len(truth))]),
        ("the right channel silenced",
            [0 if i % 2 else truth[i] for i in range(len(truth))]),
    ]
    for what, mutated in cases:
        objections = compare("control", mutated, reference, channels,
            "ffmpeg", quiet=True)
        print("    %-30s %s" % (what,
            "rejected" if objections else "ACCEPTED - the gate is blind"))
        if not objections:
            bad.append("the control '%s' passed, so this gate cannot see "
                       "that failure" % what)

    print("\n%d fixtures, %d fixture-reference comparisons." % (len(files),
        scored))
    print("  Two references and two implementations: ffmpeg's native "
          "decoder in libavcodec, and libsndfile's, which is minimp3. "
          "Unlike FLAC, no name here is another's front end - and sox is "
          "absent because the build in the image has no MPEG handler at "
          "all, which was checked rather than assumed.")
    if undecodable:
        print("\n%d fixture(s) this library identifies and does not decode:"
              % len(undecodable))
        for name in undecodable:
            print("  %s" % name)
    if excluded:
        print("\nExcluded, with the reason each was established by:")
        for key in sorted(excluded):
            print("  %s" % " / ".join(key))
            for line in _wrap(EXCLUSIONS[key], 70):
                print("      " + line)

    if bad:
        print("\n%d disagreement(s):" % len(bad), file=sys.stderr)
        for line in bad:
            print("  " + line, file=sys.stderr)
        return 1
    print("\nBoth reference decoders agree with us to within a least "
          "significant bit, per channel, in level, in offset and in "
          "length.")
    return 0


def _wrap(text, width):
    words = text.split()
    lines = []
    line = ""
    for word in words:
        if len(line) + len(word) + 1 > width:
            lines.append(line)
            line = word
        else:
            line = (line + " " + word) if line else word
    if line:
        lines.append(line)
    return lines


if __name__ == "__main__":
    sys.exit(main())
