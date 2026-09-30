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
"""Score this library's reading of tests/data/ against the pinned references.

    make check-corpus

**PCM is the exact class**, so this compares bytes and not a tolerance. Every
fixture is lossless: whatever ffmpeg, sox and libsndfile decode, this library
must decode identically, and any difference is a defect in one of us with no
tolerance argument available. planning/audio.md 12 puts PCM in that class for
exactly this reason - it is the one place where the oracle cannot be argued
with, which is why the decoder interface is proved here before meeting a
codec where it can.

### Why a byte comparison is not the whole gate

A decoder that emits silence produces the right number of bytes and no
crash. One that is 6 dB down differs from the reference by a small amount
everywhere. One that is a frame short passes any comparison that aligns two
signals before measuring. So the byte comparison is joined by, and reported
beside:

  frames    asserted exactly, before anything is compared
  peak      the ABSOLUTE level of our output, so a uniformly quiet decode
            fails even though its difference from the reference is small
  dc        the mean, which catches a sign or bias error that RMS forgives
  channels  measured separately, so a swapped or silent channel cannot hide
            behind the others' energy

### The control

`--self-check` corrupts a fixture's samples and requires the gate to fail on
it. A gate never seen to fail is not known to work, and this one would report
success for every file in the corpus if the probe silently produced nothing.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import oracle_env as oracle  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(ROOT, "tests", "data")
PROBE = os.environ.get(
    "GAUD_DUMP_PROBE",
    os.path.join(ROOT, "build", "linux", "release", "apps", "oracle",
                 "dump_probe"))

HOST_LE = sys.byteorder == "little"

# Our decoded samples are in HOST order, so the reference is asked for the
# host's order too. Spelled from sys.byteorder rather than assumed, because
# the whole reason AIFF is in this corpus is that byte order is a thing this
# library gets wrong if nobody looks.
RAW = {
    "u8":  ("u8",   "unsigned-integer", 1, 1),
    "s8":  ("s8",   "signed-integer",   1, 1),
    "s16": ("s16" + ("le" if HOST_LE else "be"), "signed-integer", 2, 2),
    "s24": ("s24" + ("le" if HOST_LE else "be"), "signed-integer", 3, 3),
    "s32": ("s32" + ("le" if HOST_LE else "be"), "signed-integer", 4, 4),
    "f32": ("f32" + ("le" if HOST_LE else "be"), "floating-point", 4, 4),
    "f64": ("f64" + ("le" if HOST_LE else "be"), "floating-point", 8, 8),
}


# Where a reference cannot answer, why, and what was measured to establish
# it. **Named here rather than skipped quietly**: an exclusion that is not
# written down is indistinguishable from a reference nobody thought to ask,
# and a score whose denominator shrank without saying so is inflated. Every
# file stays in the corpus and is still scored exactly by whoever can read
# it; only the one reference is set aside, for the one format.
EXCLUSIONS = {
    ("sox", "f32"): (
        "SoX routes samples through its own 32-bit fixed-point "
        "representation, so a float round trip loses the last bits. "
        "Measured on wav_f32_stereo_48000: max |ours-sox| = 3.0e-08, "
        "ratio within 1.2e-06 of 1.0, while ffmpeg agrees with us byte for "
        "byte. This is a property of SoX, not a disagreement about the file."),
    ("sox", "f64"): (
        "As for f32: SoX's internal representation is narrower than the "
        "samples, so it cannot be an exact reference for them."),
    ("pywave", "f32"): (
        "Python's `wave` reads WAVE_FORMAT_PCM only and raises on "
        "WAVE_FORMAT_IEEE_FLOAT. It is a reference for integer WAV."),
    ("pywave", "f64"): (
        "As for f32: `wave` has no float support at all."),
}


def run(argv, **kw):
    return subprocess.run(argv, capture_output=True, **kw)


def ours_meta(path):
    finished = run([PROBE, path])
    if finished.returncode != 0:
        raise SystemExit("dump_probe failed on %s:\n%s"
                         % (path, finished.stderr.decode()[-800:]))
    fields = {}
    for pair in finished.stdout.decode().strip().split():
        key, _, value = pair.partition("=")
        fields[key] = value
    return fields


def ours_pcm(path):
    finished = run([PROBE, path, "--pcm"])
    if finished.returncode != 0:
        raise SystemExit("dump_probe --pcm failed on %s" % path)
    return finished.stdout


def ffmpeg_pcm(path, fmt):
    raw = RAW[fmt][0]
    return run(oracle.command("ffmpeg", [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-i", path,
        "-f", raw, "-c:a", "pcm_" + raw, "-"])).stdout


def sox_pcm(path, fmt, channels, rate):
    _, encoding, width, _ = RAW[fmt]
    argv = ["sox", path, "-t", "raw", "-e", encoding, "-b", str(width * 8)]
    # sox writes raw in the host's order by default; -L/-B state it anyway so
    # the answer does not depend on where this runs.
    argv += ["-L" if HOST_LE else "-B", "-"]
    return run(oracle.command("sox", argv)).stdout


def libsndfile_pcm(path, fmt):
    # soundfile reads into numpy, whose dtype spelling carries the order.
    dtype = {"u8": "int16", "s8": "int16", "s16": "int16", "s24": "int32",
             "s32": "int32", "f32": "float32", "f64": "float64"}[fmt]
    script = (
        "import sys,soundfile as sf,numpy as np;"
        "d,_=sf.read(sys.argv[1],dtype=%r,always_2d=True);"
        "sys.stdout.buffer.write(d.tobytes())" % dtype)
    return run(oracle.command("libsndfile",
                              ["python3", "-c", script, path])).stdout


def pywave_pcm(path):
    script = (
        "import sys,wave;"
        "w=wave.open(sys.argv[1],'rb');"
        "sys.stdout.buffer.write(w.readframes(w.getnframes()))")
    return run(oracle.command("pywave",
                              ["python3", "-c", script, path])).stdout


def measure(data, fmt, channels):
    """Peak, RMS and DC per channel, as fractions of full scale."""
    import struct
    scale = {"u8": 128.0, "s8": 128.0, "s16": 32768.0, "s24": 8388608.0,
             "s32": 2147483648.0, "f32": 1.0, "f64": 1.0}[fmt]
    width = RAW[fmt][2]
    n = len(data) // width
    vals = [[] for _ in range(channels)]
    for i in range(n):
        raw = data[i * width:(i + 1) * width]
        if fmt == "u8":
            v = (raw[0] - 128) / scale
        elif fmt == "s8":
            v = struct.unpack("b", raw)[0] / scale
        elif fmt == "s24":
            b = raw if HOST_LE else raw[::-1]
            u = b[0] | (b[1] << 8) | (b[2] << 16)
            if u & 0x800000:
                u -= 1 << 24
            v = u / scale
        elif fmt in ("f32", "f64"):
            v = struct.unpack(("<" if HOST_LE else ">")
                              + ("f" if fmt == "f32" else "d"), raw)[0]
        else:
            code = {"s16": "h", "s32": "i"}[fmt]
            v = struct.unpack(("<" if HOST_LE else ">") + code, raw)[0] / scale
        vals[i % channels].append(v)
    out = []
    for ch in vals:
        if not ch:
            out.append((0.0, 0.0, 0.0))
            continue
        peak = max(abs(x) for x in ch)
        rms = (sum(x * x for x in ch) / len(ch)) ** 0.5
        dc = sum(ch) / len(ch)
        out.append((peak, rms, dc))
    return out


def scaled_pcm(data, factor):
    """Every sample multiplied by `factor`. Only used by the control, to
    manufacture the uniformly-quiet decode that a difference threshold
    scaled to the signal would forgive."""
    import struct
    out = bytearray(len(data))
    for i in range(0, len(data) - 1, 2):
        v = struct.unpack_from("<h", data, i)[0]
        struct.pack_into("<h", out, i, int(v * factor))
    return bytes(out)


def compare(path, ours, quiet=False, excluded=None, scored=None):
    """Score one reading of one fixture. Returns a list of complaints.

    `ours` is passed in rather than read here so that the control can hand
    over a deliberately wrong reading and require this to object.
    """
    bad = []
    if excluded is None:
        excluded = set()
    if scored is None:
        scored = []
    name = os.path.basename(path)
    meta = ours_meta(path)
    fmt = meta["format"]
    channels = int(meta["channels"])
    frames = int(meta["frames"])
    rate = int(meta["rate"])

    width = RAW[fmt][2]
    expect_bytes = frames * channels * width
    if len(ours) != expect_bytes:
        bad.append("%s: decoded %d bytes, the header says %d frames means %d"
                   % (name, len(ours), frames, expect_bytes))

    wanted = ["ffmpeg", "sox"]
    # libsndfile widens u8/s8 to int16 and s24 to int32, so for those its
    # bytes are a different width and cannot be compared directly.
    if fmt in ("s16", "s32", "f32", "f64"):
        wanted.append("libsndfile")
    if name.endswith(".wav"):
        wanted.append("pywave")

    getters = {
        "ffmpeg": lambda: ffmpeg_pcm(path, fmt),
        "sox": lambda: sox_pcm(path, fmt, channels, rate),
        "libsndfile": lambda: libsndfile_pcm(path, fmt),
        "pywave": lambda: pywave_pcm(path),
    }
    refs = {}
    for ref in wanted:
        if (ref, fmt) in EXCLUSIONS:
            excluded.add((ref, fmt))
            continue
        refs[ref] = getters[ref]()
    scored.append((name, sorted(refs)))

    if not refs:
        bad.append("%s: no reference could read this, so nothing scored it"
                   % name)

    for ref, data in refs.items():
        if not data:
            bad.append("%s: %s produced nothing" % (name, ref))
            continue
        if ref == "pywave" and not HOST_LE:
            # `wave` hands back the file's bytes untouched, and WAV is
            # little-endian; on a big-endian host that is not the host order
            # everything else here is in.
            continue
        if data != ours:
            first = next((i for i in range(min(len(data), len(ours)))
                          if data[i] != ours[i]), min(len(data), len(ours)))
            bad.append("%s: %s disagrees (%d vs %d bytes, first at %d)"
                       % (name, ref, len(data), len(ours), first))

    # Absolute level, per channel. A decoder returning silence, or uniformly
    # down, agrees with nothing here even when its difference from a
    # reference looks small - which is the case a threshold scaled to the
    # signal forgives.
    stats = measure(ours, fmt, channels)
    for index, (peak, rms, dc) in enumerate(stats):
        if peak < 0.05:
            bad.append("%s: channel %d peaks at %.4f - silent or far down"
                       % (name, index, peak))
        if rms < 0.01:
            bad.append("%s: channel %d RMS %.5f - silent or far down"
                       % (name, index, rms))
        if abs(dc) > 0.05:
            bad.append("%s: channel %d DC offset %.4f - sign or bias error"
                       % (name, index, dc))

    if not quiet:
        levels = " ".join("ch%d peak=%.3f rms=%.3f dc=%+.4f" % (i, p, r, d)
                          for i, (p, r, d) in enumerate(stats))
        print("  %-27s %-4s %uch %6u %5u frames  %-4s  %s"
              % (name, fmt, channels, rate, frames,
                 "OK" if not bad else "FAIL", levels))
    return bad


def check(path, quiet=False, excluded=None, scored=None):
    return compare(path, ours_pcm(path), quiet, excluded, scored)


def main():
    args = sys.argv[1:]
    self_check = "--self-check" in args
    print(oracle.provenance(["ffmpeg", "sox", "libsndfile", "pywave"]))

    if not os.path.isfile(PROBE):
        raise SystemExit(
            "dump_probe is not built: %s\nRun `make oracle-probe`." % PROBE)
    files = sorted(f for f in os.listdir(DATA)
                   if f.endswith((".wav", ".aiff", ".aifc")))
    if not files:
        raise SystemExit("tests/data is empty, so this gate is measuring "
                         "nothing. Run `make corpus`.")

    bad = []
    excluded = set()
    scored = []
    for f in files:
        bad += check(os.path.join(DATA, f), excluded=excluded, scored=scored)

    if self_check:
        # The control, and the first draft of it was wrong in a way worth
        # recording: it corrupted the FIXTURE, which corrupts it for the
        # references too, so they agreed with us about the damaged file and
        # the gate passed. A differential cannot see a bad input; what it
        # sees is a bad *reader*. So the control mutates our own answer
        # instead, which is what a defect in this library would look like.
        print("\n  control: this gate must reject a wrong reading")
        victim = os.path.join(DATA, "wav_s16_stereo_44100.wav")
        truth = ours_pcm(victim)
        cases = [
            ("one sample changed",
             truth[:200] + bytes([truth[200] ^ 0x01]) + truth[201:]),
            ("one frame short", truth[:-4]),
            # The two a byte comparison alone would catch but that the level
            # checks exist for: silence, and a uniformly quiet decode.
            ("all silence", bytes(len(truth))),
            ("6 dB down", scaled_pcm(truth, 0.5)),
        ]
        for label, wrong in cases:
            complaints = compare(victim, wrong, quiet=True)
            if not complaints:
                bad.append("CONTROL FAILED (%s): this gate accepted a wrong "
                           "reading, so it is not measuring anything" % label)
                continue
            print("    %-20s rejected: %s"
                  % (label, complaints[0].split(": ", 1)[-1][:66]))

        # The level checks need their own control. Against a lossless format
        # the byte comparison catches everything first, so "silence was
        # rejected" does not show that measuring the level works - and the
        # level checks are the half that will carry the gate from phase 5,
        # where no reference is exact and the comparison becomes a tolerance.
        # So: require that silence trips a LEVEL complaint specifically.
        silent = compare(victim, bytes(len(truth)), quiet=True)
        level_said = [c for c in silent
                      if "peaks at" in c or "RMS" in c or "DC offset" in c]
        if not level_said:
            bad.append("CONTROL FAILED: silence did not trip the level "
                       "checks, so peak/RMS/DC are not measuring anything")
        else:
            print("    %-20s rejected: %s"
                  % ("(level check)", level_said[0].split(": ", 1)[-1][:66]))

    # The denominator, stated. A score whose exclusions are invisible is
    # inflated, so both halves are printed whether or not anything failed.
    total = sum(len(refs) for _, refs in scored)
    print("\n%d fixtures, %d fixture-reference comparisons." % (len(files),
                                                                total))
    if excluded:
        print("\nExcluded, with the reason each was established by:")
        for ref, fmt in sorted(excluded):
            print("  %s / %s" % (ref, fmt))
            for line in _wrap(EXCLUSIONS[(ref, fmt)], 70):
                print("      " + line)

    if bad:
        print("\n%d disagreement(s):" % len(bad), file=sys.stderr)
        for line in bad:
            print("  " + line, file=sys.stderr)
        return 1
    print("\nEvery reference that can read a fixture agrees with us byte "
          "for byte.")
    return 0


def _wrap(text, width):
    words, line, out = text.split(), "", []
    for word in words:
        if line and len(line) + 1 + len(word) > width:
            out.append(line)
            line = word
        else:
            line = (line + " " + word) if line else word
    if line:
        out.append(line)
    return out


if __name__ == "__main__":
    sys.exit(main())
