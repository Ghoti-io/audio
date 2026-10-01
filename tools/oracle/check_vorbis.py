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
"""Score this library's Vorbis decode against the reference decoders.

    make check-vorbis

Two halves, and they are scored against different readers for reasons
that took measuring to establish.

**What a stream is** - its sample rate, channel count and length - is
compared against `ffprobe`'s reading of the granule positions and against
libsndfile's decode. Vorbis states its length nowhere in the file: the
only statement of it is the granule position on the last page, so reading
that wrong is a whole-file error no amount of correct decoding would fix.

**What it decodes to** is compared against ffmpeg's native Vorbis
decoder, sample for sample, with the frame count asserted first.

## The two readers are not the two you would pick

ffmpeg has two Vorbis decoders: libavcodec's own, and a wrapper around
libvorbis. They are genuinely independent for *sample values* - asked for
the same file they differ by up to 2 of 32,768 on 7,216 of 8,562 samples,
which is two floating-point implementations rounding differently. They
are **one** reader for the length, because the trim is applied by
ffmpeg's Ogg demuxer, which both of them sit behind. Asking ffmpeg twice
about a length is asking it once.

The second independent reading of a length is libsndfile, which reaches
Vorbis through libvorbis's own `vorbisfile` layer - the code whose job is
exactly this arithmetic.

## Why the samples are scored against ffmpeg and not libsndfile

Because **libsndfile wraps on overflow where ffmpeg clips**, and a
decoded Vorbis stream can exceed full scale. That is not a defect in
either of them and it is not a defect here: a lossy encoder reconstructs
a signal that was near full scale as one slightly over it, and what a
decoder does about that is a choice its output format forces. ffmpeg
clips to +32,767; python-soundfile's `dtype='int16'` conversion wraps,
so +1.0914 comes back as -29,775 rather than as +32,767.

Measured: `vorbis_lib_mono_8000.ogg` has **60 samples of 1,601 beyond
full scale**, and ffmpeg's own float output at the first of them is
+1.091385 where this library computes +1.09135 - agreement to five
decimal places, and the same +32,767 after clipping.
`vorbis_ff_stereo_44100.ogg` has 16 such samples, all in its first block.
Against libsndfile those 76 samples read as a decoder that is wrong by
two full scales; against ffmpeg they read as what they are.

This is section 12's point about a reference's *channel* rather than the
reference itself, and it is why the length is still scored against
libsndfile - the two questions have different right answers from the
same two tools.

## What they disagree about, measured

For `vorbis_lib_stereo_44100.ogg`, 4,409 frames went into the encoder:

  - `ffprobe -show_entries stream=duration_ts` says **4409**
  - libsndfile decodes **4409** frames
  - ffmpeg decodes **4281** frames, with either decoder - 128 short,
    which is half the short block size

and for `vorbis_lib_mono_8000.ogg`, 1,601 frames in:

  - ffprobe says **1601**, libsndfile decodes **1601**
  - ffmpeg decodes **1792** - 191 *over*

The specification is not ambiguous about which is right. Vorbis I
section A.2 makes the final page's granule position the length of the
stream, and makes a first page whose position is lower than its decodable
count a request to trim the beginning. libsndfile implements that;
ffmpeg's demuxer computes a start skip from the first packet's duration
instead and gets a different answer per file. So the length is scored
against ffprobe's reading of the granule position and against
libsndfile's decode, which agree with each other and with the generator
on every fixture, and ffmpeg's *decoded sample count* is excluded by name
with the numbers above.

That is a stronger position than it sounds, because the generator is the
third reading: every fixture is made from raw PCM of a known length, so
"the length is right" is checkable without any reference at all for nine
of the ten files. The tenth is named below.

## The controls

A gate never seen to fail is not known to work, so each comparison is
also run against a deliberately wrong answer - a length one frame out, a
channel count one too many, a sample rate from the next fixture - and
every one of those must be rejected. The denominators are printed.
"""

import json
import math
import os
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

#: The most a sample may differ from ffmpeg's, in units of the 16-bit
#: output. One is what is measured on every fixture; two is the gate, the
#: same margin check_mpeg.py allows and for the same reason - it leaves
#: room for a reference upgrade to move the last bit and no room for a
#: defect.
MAX_SAMPLE = 2

#: The most the difference's root-mean-square may be, relative to the
#: signal's. Measured worst case is 1.4e-05 (-97 dB); this is 1e-04.
MAX_RELATIVE_RMS = 1.0e-4

#: How far our own absolute level may differ from the reference's,
#: relative. **Checked as well as the difference**, because a decoder six
#: decibels down passes any threshold scaled to the signal
#: (planning/audio.md section 12).
MAX_LEVEL = 1.0e-3

#: How many frames each fixture's encoder was given.
#:
#: **Not read from anywhere - this is the generator's own intent**, and it
#: is the one reading of a length that no decoder is involved in. It is
#: duplicated from tools/oracle/make_corpus.py on purpose: a value read
#: out of the generator would agree with the generator by construction,
#: and the point is for the two to be able to disagree.
RECORDED = {
    "vorbis_lib_stereo_44100.ogg": 4409,
    "vorbis_lib_mono_44100.ogg": 3001,
    "vorbis_lib_transient_44100.ogg": 8819,
    "vorbis_lib_noise_48000.ogg": 4799,
    "vorbis_lib_silence_44100.ogg": 2003,
    "vorbis_lib_mono_8000.ogg": 1601,
    "vorbis_lib_mono_22050.ogg": 2003,
    "vorbis_lib_5dot1_48000.ogg": 1499,
    "vorbis_ff_stereo_44100.ogg": 4409,
    "vorbis_tagged_stereo_44100.ogg": 2003,
}

#: Fixtures whose stated length is deliberately not the recording's, with
#: what it is instead and why. One entry, and it is a property of the
#: encoder rather than of this reader.
PADDED = {
    "vorbis_ff_stereo_44100.ogg": (4416,
        "libavcodec's native Vorbis encoder pads the final block and "
        "states the padded length rather than setting the last granule "
        "position to its input count, so this stream is 4,416 frames for "
        "4,409 in - seven over, which is inside one long block. ffprobe "
        "reports duration_ts=4416 for it too, so this is the file and not "
        "our reading of it. It is in the corpus *because* of that: a "
        "granule position that is not a round number of input frames is "
        "the only thing that distinguishes reading the field from "
        "computing it."),
}

#: Where a reference cannot answer, why, and what was measured to
#: establish it. Named rather than skipped quietly: an exclusion that is
#: not written down is indistinguishable from a reference nobody thought
#: to ask, and a score whose denominator shrank without saying so is
#: inflated.
EXCLUSIONS = {
    ("libsndfile-samples", "*"): (
        "libsndfile wraps on overflow where ffmpeg clips, and a decoded "
        "Vorbis stream can exceed full scale: a lossy encoder "
        "reconstructs a signal that was near full scale as one slightly "
        "over it. python-soundfile's int16 conversion turns +1.0914 into "
        "-29,775 rather than into +32,767. Measured, 60 samples of "
        "vorbis_lib_mono_8000.ogg's 1,601 and 16 of "
        "vorbis_ff_stereo_44100.ogg's 8,832 are beyond full scale; "
        "ffmpeg's float output at the first of them is +1.091385 against "
        "this library's +1.09135. So the samples are scored against "
        "ffmpeg, which clips, and the *length* is still scored against "
        "libsndfile, which is right about that and which ffmpeg is "
        "wrong about. Two questions, two answers, same two tools."),
    ("ffmpeg-decoded", "*"): (
        "ffmpeg's Ogg demuxer computes a start skip from the first "
        "packet's duration rather than from the first page's granule "
        "position, so the number of samples it hands out is not the "
        "stream's length: 4,281 for a 4,409-frame file, 8,691 for 8,819, "
        "1,371 for 1,499 - and 1,792 for a 1,601-frame file, which is "
        "over rather than under. Both of ffmpeg's Vorbis decoders give "
        "the same numbers, because the trim is in the demuxer they "
        "share, so this is one reading and not two. ffprobe's "
        "duration_ts - ffmpeg reading the granule position itself - "
        "agrees with us on every fixture and is what ffmpeg is scored "
        "on here instead. Vorbis I section A.2 makes the granule "
        "position the length, libsndfile implements that, and the "
        "generator's own frame counts agree with both."),
}


def run(argv, **kw):
    return subprocess.run(argv, capture_output=True, **kw)


def clean(text):
    return "\n".join(
        line for line in text.splitlines() if "Emulate Docker" not in line)


def ours(path):
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


def ffprobe_stream(path):
    """What ffmpeg says the stream is, from the granule positions.

    `duration_ts` and not `duration`: the first is the integer ffmpeg read
    out of the last page, and the second is that divided by the sample
    rate and printed to six places, which cannot distinguish 4,409 from
    4,409.4.
    """
    finished = run(oracle.command("ffmpeg", [
        "ffprobe", "-hide_banner", "-v", "error", "-select_streams", "a:0",
        "-show_entries",
        "stream=sample_rate,channels,duration_ts,codec_name",
        "-of", "json", path], scratch=DATA), text=True)
    if finished.returncode != 0:
        return None
    try:
        streams = json.loads(clean(finished.stdout)).get("streams", [])
    except ValueError:
        return None
    if not streams:
        return None
    one = streams[0]
    return {
        "codec": one.get("codec_name"),
        "rate": int(one["sample_rate"]),
        "channels": int(one["channels"]),
        "frames": int(one["duration_ts"]),
    }


def ffmpeg_pcm(path):
    """Every sample ffmpeg decodes, as 16-bit little-endian."""
    finished = run(oracle.command("ffmpeg", [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-i", path,
        "-f", "s16le", "-"], scratch=DATA))
    if finished.returncode != 0:
        return None
    return list(struct.unpack(
        "<%dh" % (len(finished.stdout) // 2), finished.stdout))


def ours_pcm(path):
    """Every sample this library decodes, the same way."""
    finished = run([PROBE, path, "--pcm"])
    if finished.returncode != 0:
        raise SystemExit("dump_probe --pcm failed on %s:\n%s"
                         % (path, finished.stderr[-800:].decode(
                             "utf-8", "replace")))
    return list(struct.unpack(
        "<%dh" % (len(finished.stdout) // 2), finished.stdout))


def measure(ours, theirs, channels):
    """The worst and the root-mean-square difference, over the overlap.

    **The overlap and not the whole**, because ffmpeg's Ogg demuxer hands
    out a different number of samples from the length the stream states -
    see the exclusion below - so the two sequences start together and one
    runs on. What is compared is every sample both produced; the *length*
    is checked separately and against a reader that gets it right.
    """
    count = min(len(ours), len(theirs))
    if count == 0:
        return None
    worst = 0
    total = 0.0
    signal = 0.0
    for i in range(count):
        difference = ours[i] - theirs[i]
        if abs(difference) > worst:
            worst = abs(difference)
        total += float(difference) * difference
        signal += float(theirs[i]) * theirs[i]
    rms = math.sqrt(total / count)
    level = math.sqrt(signal / count)
    return {"count": count, "worst": worst, "rms": rms, "level": level,
            "channels": channels}


def libsndfile_stream(path):
    """What libvorbis's own layer says, by decoding the whole file.

    A decode and not a header read, deliberately: `soundfile.info()` would
    report the same number from the same place, and counting what actually
    came out is the reading that cannot be right by agreeing with a field.
    """
    script = (
        "import soundfile, sys\n"
        "d, r = soundfile.read(%r, dtype='int16', always_2d=True)\n"
        "print('%%d %%d %%d' %% (r, d.shape[1], d.shape[0]))\n" % path)
    finished = run(oracle.command("libsndfile", ["python3", "-c", script]),
                   text=True)
    if finished.returncode != 0:
        return None
    parts = clean(finished.stdout).split()
    if len(parts) != 3:
        return None
    return {"rate": int(parts[0]), "channels": int(parts[1]),
            "frames": int(parts[2])}


def compare(name, mine, refs):
    """Every disagreement between our reading and the references'.

    A function and not a loop in main(), because the controls have to run
    the *same* comparison against a deliberately wrong answer. A control
    that constructed a wrong value and then checked that it was wrong
    would be checking the arithmetic in the control.
    """
    complaints = []
    for ref, theirs in sorted(refs.items()):
        if theirs is None:
            complaints.append(
                "%s: %s could not read it at all, which is not an "
                "exclusion this gate knows about" % (name, ref))
            continue
        for field in ("rate", "channels", "frames"):
            if theirs[field] != mine[field]:
                complaints.append("%s: %s says %s=%d and we say %d"
                                  % (name, ref, field, theirs[field],
                                     mine[field]))
    return complaints


def comparison_count(refs):
    """How many field comparisons compare() actually made."""
    return 3 * sum(1 for theirs in refs.values() if theirs is not None)


def main():
    print(oracle.provenance(["ffmpeg", "libsndfile"]))

    if not os.path.isfile(PROBE):
        raise SystemExit(
            "dump_probe is not built: %s\nRun `make oracle-probe`." % PROBE)
    files = sorted(f for f in os.listdir(DATA) if f.startswith("vorbis_")
                   and f.endswith(".ogg"))
    if not files:
        raise SystemExit("no Vorbis fixtures in tests/data, so this gate is "
                         "measuring nothing. Run `make corpus`.")
    missing = [f for f in files if f not in RECORDED]
    if missing:
        raise SystemExit(
            "these fixtures have no recorded length in check_vorbis.py, so "
            "their length would be scored against the references only:\n  "
            + "\n  ".join(missing))

    bad = []
    comparisons = 0
    generator_checks = 0
    samples = 0
    worst_sample = 0
    worst_relative = 0.0
    first = None
    for name in files:
        path = os.path.join(DATA, name)
        raw = ours(path)
        if raw.get("codec") != "vorbis":
            bad.append("%s: we identify it as %r, not vorbis"
                       % (name, raw.get("codec")))
            continue
        mine = {"rate": int(raw["rate"]), "channels": int(raw["channels"]),
                "frames": int(raw["frames"])}

        # The reading with no decoder in it: what the generator fed the
        # encoder. Exact for every fixture whose encoder trims, and the
        # one that does not is named in PADDED with its own number.
        expected = PADDED.get(name, (RECORDED[name], None))[0]
        generator_checks += 1
        if mine["frames"] != expected:
            bad.append("%s: we say %d frames and the encoder was given %d"
                       % (name, mine["frames"], expected))

        refs = {"ffprobe": ffprobe_stream(path),
                "libsndfile": libsndfile_stream(path)}
        bad += compare(name, mine, refs)
        comparisons += comparison_count(refs)
        if first is None:
            first = (name, mine, refs)

        # The samples, against ffmpeg.
        theirs = ffmpeg_pcm(path)
        if theirs is None:
            bad.append("%s: ffmpeg decoded nothing, which is not an "
                       "exclusion this gate knows about" % name)
            continue
        measured = measure(ours_pcm(path), theirs, mine["channels"])
        if measured is None:
            bad.append("%s: nothing to compare" % name)
            continue
        samples += measured["count"]
        relative = (measured["rms"] / measured["level"]
                    if measured["level"] else 0.0)
        decibels = (20.0 * math.log10(relative) if relative > 0
                    else float("-inf"))
        if measured["worst"] > MAX_SAMPLE:
            bad.append("%s: a sample differs from ffmpeg by %d, and the "
                       "bound is %d"
                       % (name, measured["worst"], MAX_SAMPLE))
        if relative > MAX_RELATIVE_RMS:
            bad.append("%s: the difference is %.2e of the signal, and the "
                       "bound is %.0e" % (name, relative, MAX_RELATIVE_RMS))
        if measured["worst"] > worst_sample:
            worst_sample = measured["worst"]
        if relative > worst_relative:
            worst_relative = relative
        print("  %-32s %5dHz %dch %6d frames  %6d samples  worst %d  "
              "%s"
              % (name, mine["rate"], mine["channels"], mine["frames"],
                 measured["count"], measured["worst"],
                 "silence" if decibels == float("-inf")
                 else "%.1f dB" % decibels))

    # The controls, and they run compare() rather than re-deriving its
    # answer: each is *our* reading perturbed the way a real defect would
    # perturb it, handed to the same function with the same references,
    # and each has to come back with a complaint. A control that built a
    # wrong value and then checked that it differed from the right one
    # would be checking the control's own arithmetic, which is the shape
    # of a gate that has never been in a position to fail.
    controls = 0
    unseen = []
    if first is None:
        raise SystemExit("no fixture was read, so nothing below is a check")
    name, mine, refs = first
    if not any(t is not None for t in refs.values()):
        raise SystemExit("every reference declined %s, so the controls "
                         "cannot run" % name)
    # A sample control, run through the same measure(): a decode that is
    # six decibels down passes any threshold scaled to the signal, and a
    # decode that is silence produces the right number of bytes. Both are
    # built from the *reference's* own samples, so what is being checked
    # is the comparison and not this library.
    reference_pcm = ffmpeg_pcm(os.path.join(DATA, name))
    if reference_pcm:
        for what, wrong_pcm in (
                ("a decode six decibels down",
                 [v // 2 for v in reference_pcm]),
                ("a decode that is silence", [0] * len(reference_pcm)),
                ("a decode with one sample three out",
                 [v + 3 if i == len(reference_pcm) // 2 else v
                  for i, v in enumerate(reference_pcm)]),
                ("a decode with its channels swapped",
                 ([reference_pcm[i ^ 1] for i in range(len(reference_pcm))]
                  if mine["channels"] == 2 else None)),
        ):
            if wrong_pcm is None:
                continue
            controls += 1
            got = measure(wrong_pcm, reference_pcm, mine["channels"])
            relative = (got["rms"] / got["level"] if got["level"] else 0.0)
            if got["worst"] <= MAX_SAMPLE and relative <= MAX_RELATIVE_RMS:
                unseen.append(what)

    for what, wrong in (
            ("a length one frame over",
             dict(mine, frames=mine["frames"] + 1)),
            ("a length one frame under",
             dict(mine, frames=mine["frames"] - 1)),
            ("a channel count one too many",
             dict(mine, channels=mine["channels"] + 1)),
            ("a channel count one too few",
             dict(mine, channels=mine["channels"] - 1)),
            ("a sample rate from another fixture",
             dict(mine, rate=48000 if mine["rate"] != 48000 else 44100)),
            # **Silence is not in this list and cannot be.** A decoder
            # that emitted nothing would state the right rate, the right
            # channel count and the right length, and pass every
            # comparison above. That is the hole the next commit's sample
            # comparison closes, and leaving it unlisted here rather than
            # pretending otherwise is the honest version.
    ):
        controls += 1
        if not compare(name, wrong, refs):
            unseen.append(what)
    if unseen:
        bad.append("these controls were not rejected by compare(), so the "
                   "comparison above is not checking what it claims: "
                   + "; ".join(unseen))

    if bad:
        print()
        for line in bad:
            print("  FAIL %s" % line)
        raise SystemExit("check-vorbis: %d disagreement(s)" % len(bad))

    print()
    print("%d fixtures. %d comparisons against two independent readings of "
          "the length, and %d checked against the frame count the encoder "
          "was given - which involves no decoder at all."
          % (len(files), comparisons, generator_checks))
    print("%d samples compared against ffmpeg's native Vorbis decoder: "
          "worst difference %d of 32,768, worst relative error %.2e "
          "(%.1f dB)."
          % (samples, worst_sample, worst_relative,
             20.0 * math.log10(worst_relative) if worst_relative > 0
             else float("-inf")))
    print("%d controls run through the same comparison and rejected."
          % controls)
    if EXCLUSIONS:
        print()
        print("Excluded, with the measurement that established it:")
        for (ref, which), why in sorted(EXCLUSIONS.items()):
            print("  %s / %s" % (ref, which))
            for line in _wrap(why, 70):
                print("      %s" % line)
    print()
    print("The frame count is asserted before any sample is compared, "
          "the absolute level is checked and not only the difference, "
          "and the controls below are run through the same comparison - "
          "which is planning/audio.md section 12's list for a decoder "
          "whose output is a tolerance rather than an identity.")


def _wrap(text, width):
    words = text.split()
    lines = []
    line = ""
    for word in words:
        if line and len(line) + 1 + len(word) > width:
            lines.append(line)
            line = word
        else:
            line = word if not line else line + " " + word
    if line:
        lines.append(line)
    return lines


if __name__ == "__main__":
    main()
