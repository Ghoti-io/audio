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

## And the samples

Every mono and stereo fixture is also decoded by `opusdec` and by this
library, through the whole path - Ogg pages, pre-skip, the end trimmed to
the last granule position - and the two are put to `opus_compare`, the
tool RFC 6716 defines conformance by. Two controls are run through the
same call and must be rejected: silence, and the right signal at half
amplitude. A comparison that cannot fail is
not one.

**The six-channel fixture is not compared**, because `opus_compare` takes
one or two channels. It is checked sample for sample, against RFC 6716's
own decoder, in the unit tests instead.
"""

import json
import math
import os
import re
import subprocess
import sys
import tempfile

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


def compare_samples(name, path, channels, frames, scratch):
    """What opus_compare says of our decode against opusdec's.

    Both run in one container call, because opus_compare reads raw files
    and the two decodes are the whole of the input. `opusdec` is told not
    to dither, so that what it writes is its decode and not its decode
    plus noise, and the rate is forced to 48,000 so that the same count
    of samples is compared.

    **The reference file is always read as stereo.** opus_compare takes
    `-s` to say the *test* file is, and for a mono one the reference is
    still two channels, which it averages - which is how RFC 6716's own
    mono runs are scored against its stereo `.dec` files. So a mono
    decode is written out as both channels for the reference side.

    Returns a dict of label -> (passed, text), or None and a reason. The
    controls are skipped, and say so, for a reference that is silence
    throughout: silence is the right answer there and a control built
    from it could not be wrong.
    """
    ours_path = os.path.join(scratch, name + ".raw")
    finished = run([PROBE, path, "--pcm-le"])
    if finished.returncode != 0:
        return None, "dump_probe --pcm-le failed"
    with open(ours_path, "wb") as handle:
        handle.write(finished.stdout)
    if len(finished.stdout) != frames * channels * 2:
        return None, ("we wrote %d bytes for %d frames of %d channels"
                      % (len(finished.stdout), frames, channels))
    flag = "-s " if channels == 2 else ""
    size = frames * channels * 2
    script = (
        "set -e\n"
        "opusdec --quiet --no-dither --rate 48000 %(path)s /tmp/native.raw\n"
        "python3 - <<'EOF'\n"
        "import struct\n"
        "raw = open('/tmp/native.raw', 'rb').read()\n"
        "ch = %(channels)d\n"
        "if ch == 1:\n"
        "    out = b''.join(raw[i:i + 2] * 2 for i in range(0, len(raw), 2))\n"
        "else:\n"
        "    out = raw\n"
        "open('/tmp/ref.raw', 'wb').write(out)\n"
        "silent = not any(raw)\n"
        "samples = struct.unpack('<%%dh' %% (len(raw) // 2), raw)\n"
        "quiet = struct.pack('<%%dh' %% len(samples), *[v // 2 for v in samples])\n"
        "open('/tmp/quiet.raw', 'wb').write(quiet)\n"
        "open('/tmp/zero.raw', 'wb').write(b'\\0' * len(raw))\n"
        "print('SILENT' if silent else 'LOUD')\n"
        "EOF\n"
        "set +e\n"
        "for t in %(ours)s /tmp/zero.raw /tmp/quiet.raw; do\n"
        "  echo \"== $t\"\n"
        "  opus_compare %(flag)s/tmp/ref.raw $t 2>&1\n"
        "  echo \"rc $?\"\n"
        "done\n" % {"path": path, "channels": channels, "ours": ours_path,
                    "flag": flag})
    finished = run(oracle.command("opus_compare", ["sh", "-c", script],
                                  scratch=[scratch]), text=True)
    text = clean(finished.stdout) + clean(finished.stderr)
    if finished.returncode != 0 and "== " not in text:
        return None, "the reference could not decode it: " + text[-200:]
    results = {"silent": "SILENT" in text}
    for label, marker in (("ours", ours_path), ("silence", "/tmp/zero.raw"),
                          ("quiet", "/tmp/quiet.raw")):
        block = text.split("== %s" % marker, 1)
        if len(block) < 2:
            return None, "no verdict for %s in: %s" % (label, text[-200:])
        verdict = block[1].split("== ", 1)[0]
        results[label] = ("Test vector PASSES" in verdict
                          and "rc 0" in verdict, verdict)
    return results, None


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

    sample_checks = 0
    sample_controls = 0
    with tempfile.TemporaryDirectory(prefix="gaud-opus-") as scratch:
        os.chmod(scratch, 0o777)
        for name in files:
            path = os.path.join(DATA, name)
            raw = ours(path)
            channels = int(raw["channels"])
            if channels > 2:
                continue
            results, why = compare_samples(
                name, path, channels, int(raw["frames"]), scratch)
            if results is None:
                bad.append("%s: %s" % (name, why))
                continue
            sample_checks += 1
            if not results["ours"][0]:
                bad.append("%s: opus_compare rejects our decode: %s"
                           % (name, results["ours"][1].strip()[-200:]))
            for control in ("silence", "quiet"):
                if results["silent"]:
                    continue
                sample_controls += 1
                if results[control][0]:
                    bad.append("%s: opus_compare accepted %s, so it cannot "
                               "tell a decode from one that is wrong"
                               % (name, control))

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
    print("%d fixtures (every one with one or two channels) decode to "
          "what libopus's own front end decodes, as `opus_compare` judges "
          "it, through the whole Ogg path; %d controls - silence, and the "
          "signal at half amplitude - are rejected by the same call."
          % (sample_checks, sample_controls))
    return 0


if __name__ == "__main__":
    sys.exit(main())
