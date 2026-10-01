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
"""Score what this library says a Vorbis stream *is*, against two readers.

    make check-vorbis

**There is no decode here yet, and this gate is the reason that is an
honest place to stop.** planning/audio.md section 11.18 argues that
identification lands before decoding as a commit of its own, and the
second half of that argument is that the identification can be *gated* on
its own. For Vorbis there is more to gate than there was for MPEG,
because Vorbis states its length nowhere in the file: the only statement
of it is the granule position on the last page, and reading that wrong is
a whole-file error that no amount of correct decoding would fix.

So this compares, per fixture: the sample rate, the channel count, and
the length. Against two readers, and **the two disagree about the length
of every file in the corpus** - which is the finding this gate exists to
hold still rather than a problem with it.

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
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import oracle_env as oracle  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(ROOT, "tests", "data")
PROBE = os.path.join(ROOT, "build", "linux", "release", "apps", "oracle",
    "dump_probe")

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
        row = " | ".join("%s %dHz %dch %d" % (ref, t["rate"], t["channels"],
                                              t["frames"])
                         for ref, t in sorted(refs.items())
                         if t is not None)
        print("  %-32s %5dHz %dch %6d frames   %s"
              % (name, mine["rate"], mine["channels"], mine["frames"], row))
        if first is None:
            first = (name, mine, refs)

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
    print("%d fixtures, %d comparisons against two independent readings of "
          "the length, and %d checked against the frame count the encoder "
          "was given - which involves no decoder at all."
          % (len(files), comparisons, generator_checks))
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
    print("**No sample values are compared, because there is no decoder "
          "yet.** This gate scores identification: the rate, the channel "
          "count and the length. A decoder that produced silence would "
          "pass every check above, and that is the hole phase 6's next "
          "commit closes rather than a weakness in these numbers.")


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
