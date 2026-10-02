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
"""Score what this library says an Opus stream is, against three readers.

    make check-opus

**There is no decode here yet**, and planning/audio.md section 11.18 is
the argument for that being a commit of its own: the identification can
be gated on its own, and for Opus there is more of it to gate than for
any format so far.

An Opus stream states its length nowhere. The only statement is the
granule position on its last page - counted at 48 kHz, whatever rate the
encoder saw - and **that position includes samples the recording does
not contain**. Every Opus stream begins with a pre-skip, a number of
samples the encoder's own filters needed, stated in `OpusHead`; the
length is the position minus that. A reader that forgot reports every
file a few milliseconds long, and a reader that forgot the position
entirely has nothing at all.

## Three readings, and all three reconcile

Unusually for this library, there is nothing to exclude here. The three
disagree, and the disagreement is an identity rather than a defect:

  - **opusdec** decodes exactly the number of frames we report. It is
    libopus, which is the implementation RFC 6716 defines conformance
    against.
  - **opusinfo** prints a playback length equal to ours, to the
    millisecond it prints.
  - **ffprobe's `duration_ts`** is ours *plus the pre-skip*, on every
    fixture. ffmpeg reports the granule position; we report the
    recording. Checked as that equation rather than set aside, which is
    the stronger thing to do with a disagreement whose shape is known.

And a fourth reading that involves no decoder at all: every fixture is
made from raw PCM of a length the generator knows.

## What this does not check

**No sample values**, because there is no decoder. A decoder that
produced silence would pass everything here, and `opus_compare` - the
normative tool RFC 6716 defines conformance by, now in the oracle image -
is not yet called by anything. That is the hole the decode commit
closes.
"""

import json
import math
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import oracle_env as oracle  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(ROOT, "tests", "data")
PROBE = os.environ.get("GAUD_DUMP_PROBE") or os.path.join(
    ROOT, "build", "linux", "release", "apps", "oracle", "dump_probe")

#: How many frames each fixture's encoder was given. Duplicated from
#: make_corpus.py on purpose: a value read out of the generator agrees
#: with the generator by construction.
RECORDED = {
    "opus_celt_stereo_96k.opus": 9600,
    "opus_celt_lowdelay_2ms5.opus": 9600,
    "opus_celt_stereo_10ms.opus": 9600,
    "opus_celt_mono_swb.opus": 9600,
    "opus_silk_mono_nb.opus": 9600,
    "opus_silk_mono_mb.opus": 9600,
    "opus_silk_mono_wb.opus": 9600,
    "opus_silk_mono_40ms.opus": 9600,
    "opus_silk_stereo_60ms.opus": 9600,
    "opus_hybrid_mono_swb.opus": 9600,
    "opus_hybrid_mono_fb.opus": 9600,
    "opus_celt_mono_60ms.opus": 9600,
    "opus_celt_5dot1.opus": 4800,
    "opus_celt_silence.opus": 4800,
    "opus_tagged_stereo.opus": 4800,
}

#: Where a reference cannot answer, and why. **Empty, and that is the
#: result**: the three readers disagree about the length in a way that
#: reconciles exactly, so none of them has to be set aside. The table
#: stays because an exclusion that is not written down is
#: indistinguishable from a reference nobody thought to ask.
EXCLUSIONS = {}


def run(argv, **kw):
    return subprocess.run(argv, capture_output=True, **kw)


def clean(text):
    return "\n".join(
        line for line in text.splitlines() if "Emulate Docker" not in line)


def ours(path):
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
    finished = run(oracle.command("ffmpeg", [
        "ffprobe", "-hide_banner", "-v", "error", "-select_streams", "a:0",
        "-show_entries", "stream=sample_rate,channels,duration_ts",
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
    return {"rate": int(one["sample_rate"]),
            "channels": int(one["channels"]),
            "granule": int(one["duration_ts"])}


def opusdec_frames(path):
    """How many frames libopus's own front end decodes.

    Through a wav file and Python's `wave`, rather than a raw pipe,
    because the frame count is what is wanted and a wav states it.
    """
    script = (
        "import subprocess, wave, sys\n"
        "subprocess.run(['opusdec', '--quiet', %r, '/tmp/d.wav'],\n"
        "               check=True)\n"
        "w = wave.open('/tmp/d.wav')\n"
        "print('%%d %%d %%d' %% (w.getnframes(), w.getframerate(),\n"
        "                      w.getnchannels()))\n" % path)
    finished = run(oracle.command("opusdec", ["python3", "-c", script]),
                   text=True)
    if finished.returncode != 0:
        return None
    parts = clean(finished.stdout).split()
    if len(parts) != 3:
        return None
    return {"frames": int(parts[0]), "rate": int(parts[1]),
            "channels": int(parts[2])}


def opusinfo_facts(path):
    """The pre-skip and the playback length opusinfo prints."""
    finished = run(oracle.command("opusdec", ["opusinfo", path],
                                  scratch=DATA), text=True)
    if finished.returncode != 0:
        return None
    text = clean(finished.stdout) + clean(finished.stderr)
    pre = re.search(r"Pre-skip:\s*([0-9]+)", text)
    length = re.search(r"Playback length:\s*([0-9]+)m:([0-9.]+)s", text)
    if not pre or not length:
        return None
    seconds = int(length.group(1)) * 60 + float(length.group(2))
    return {"pre_skip": int(pre.group(1)), "seconds": seconds}


def compare(name, mine, ffprobe, opusdec, info):
    """Every disagreement, as a list. Shared with the controls."""
    complaints = []
    if ffprobe is not None:
        if ffprobe["rate"] != mine["rate"]:
            complaints.append("%s: ffprobe says %d Hz and we say %d"
                              % (name, ffprobe["rate"], mine["rate"]))
        if ffprobe["channels"] != mine["channels"]:
            complaints.append("%s: ffprobe says %d channels and we say %d"
                              % (name, ffprobe["channels"],
                                 mine["channels"]))
        # The identity, rather than an exclusion: ffmpeg reports the
        # granule position and we report the recording, so the two
        # differ by exactly the pre-skip.
        if info is not None:
            expected = mine["frames"] + info["pre_skip"]
            if ffprobe["granule"] != expected:
                complaints.append(
                    "%s: ffprobe's granule is %d and our length plus the "
                    "pre-skip is %d + %d = %d"
                    % (name, ffprobe["granule"], mine["frames"],
                       info["pre_skip"], expected))
    if opusdec is not None:
        if opusdec["frames"] != mine["frames"]:
            complaints.append("%s: opusdec decodes %d frames and we say %d"
                              % (name, opusdec["frames"], mine["frames"]))
        if opusdec["rate"] != mine["rate"]:
            complaints.append("%s: opusdec outputs %d Hz and we say %d"
                              % (name, opusdec["rate"], mine["rate"]))
        if opusdec["channels"] != mine["channels"]:
            complaints.append("%s: opusdec gives %d channels and we say %d"
                              % (name, opusdec["channels"],
                                 mine["channels"]))
    if info is not None:
        # opusinfo prints three decimal places, so the tolerance is half
        # a millisecond and not a sample.
        seconds = mine["frames"] / 48000.0
        if abs(seconds - info["seconds"]) > 0.0006:
            complaints.append("%s: opusinfo says %.3fs and we say %.6fs"
                              % (name, info["seconds"], seconds))
    return complaints


def main():
    print(oracle.provenance(["ffmpeg", "opusdec", "opus_compare"]))
    if not os.path.isfile(PROBE):
        raise SystemExit(
            "dump_probe is not built: %s\nRun `make oracle-probe`." % PROBE)
    files = sorted(f for f in os.listdir(DATA) if f.endswith(".opus"))
    if not files:
        raise SystemExit("no Opus fixtures in tests/data. Run `make "
                         "corpus`.")
    missing = [f for f in files if f not in RECORDED]
    if missing:
        raise SystemExit(
            "these fixtures have no recorded length in check_opus.py:\n  "
            + "\n  ".join(missing))

    bad = []
    comparisons = 0
    first = None
    for name in files:
        path = os.path.join(DATA, name)
        raw = ours(path)
        if raw.get("codec") != "opus":
            bad.append("%s: we identify it as %r, not opus"
                       % (name, raw.get("codec")))
            continue
        mine = {"rate": int(raw["rate"]), "channels": int(raw["channels"]),
                "frames": int(raw["frames"])}
        if mine["rate"] != 48000:
            bad.append("%s: we report %d Hz, and every Opus track is "
                       "48,000" % (name, mine["rate"]))
        if mine["frames"] != RECORDED[name]:
            bad.append("%s: we say %d frames and the encoder was given %d"
                       % (name, mine["frames"], RECORDED[name]))

        ffprobe = ffprobe_stream(path)
        opusdec = opusdec_frames(path)
        info = opusinfo_facts(path)
        for which, value in (("ffprobe", ffprobe), ("opusdec", opusdec),
                             ("opusinfo", info)):
            if value is None:
                bad.append("%s: %s could not read it, which is not an "
                           "exclusion this gate knows about"
                           % (name, which))
        bad += compare(name, mine, ffprobe, opusdec, info)
        comparisons += 3 * (ffprobe is not None) + 3 * (opusdec is not None)
        comparisons += 1 * (info is not None)
        print("  %-30s %dch %6d frames  pre-skip %-4s  opusdec %-6s  "
              "ffprobe granule %s"
              % (name, mine["channels"], mine["frames"],
                 info["pre_skip"] if info else "?",
                 opusdec["frames"] if opusdec else "?",
                 ffprobe["granule"] if ffprobe else "?"))
        if first is None:
            first = (name, mine, ffprobe, opusdec, info)

    controls = 0
    unseen = []
    if first is None:
        raise SystemExit("no fixture was read, so nothing above is a check")
    name, mine, ffprobe, opusdec, info = first
    for what, wrong in (
            ("a length one frame over", dict(mine,
                                             frames=mine["frames"] + 1)),
            ("a length one frame under", dict(mine,
                                              frames=mine["frames"] - 1)),
            ("the pre-skip left in", dict(mine, frames=mine["frames"]
                                          + (info["pre_skip"] if info
                                             else 312))),
            ("a channel count one too many",
             dict(mine, channels=mine["channels"] + 1)),
            ("the sample rate from OpusHead rather than 48,000",
             dict(mine, rate=16000)),
    ):
        controls += 1
        if not compare(name, wrong, ffprobe, opusdec, info):
            unseen.append(what)
    if unseen:
        bad.append("these controls were not rejected by compare(): "
                   + "; ".join(unseen))

    if bad:
        print()
        for line in bad:
            print("  FAIL %s" % line)
        raise SystemExit("check-opus: %d disagreement(s)" % len(bad))

    print()
    print("%d fixtures, %d comparisons against three readers - opusdec, "
          "which is libopus; opusinfo, which is its front end reading the "
          "headers; and ffprobe - plus %d against the frame count the "
          "encoder was given, which involves no decoder at all."
          % (len(files), comparisons, len(files)))
    print("%d controls run through the same comparison and rejected, one "
          "of them the pre-skip left in - which is the error this format "
          "invites and no other here can make." % controls)
    print()
    print("**Nothing above compares a sample.** `opus_compare` is in the "
          "image and is called by nothing, which is the hole the decode "
          "commit closes: RFC 6716 defines a conforming decoder as one "
          "whose output that tool accepts, and this library has no "
          "output for it to accept yet.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
