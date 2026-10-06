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
"""Score the Vorbis decode of streams written for the parts of the format
no encoder writes.

    make check-vorbis-synth

`tools/oracle/vorbis_synth.py` writes streams that use a floor of type 0, a
residue of type 0 and codebooks that state every vector, from the
specification and a seeded generator. Nothing in the oracle image encodes
any of those, so this is the only place a decoder's handling of them meets
a reference. Three things are checked:

  - **The committed fixtures are what the generator writes.** The bytes are
    regenerated and compared, so a fixture cannot drift from the script that
    says where it came from.
  - **Our decode against two independent decoders**: ffmpeg's native Vorbis
    decoder and libvorbis (through ffmpeg's wrapper, which is the only way
    the oracle image reaches it). They are two implementations, in two code
    bases, in different languages' idioms; asked for the same stream they
    differ by at most one least significant bit here. We are held to two
    of either, which is what `check_vorbis.py` allows.
  - **The frame count**, which the generator knows from the specification's
    own arithmetic and writes into the last page.

One exclusion, named: ffmpeg's native decoder refuses a codebook whose
lookup type is 2, so the two fixtures that use one are scored against
libvorbis alone. The gate asserts the refusal rather than assuming it, so a
future ffmpeg that reads them fails this check and the exclusion is removed.

**The controls** are the same comparison run on answers that are wrong - our
decode of another stream, our decode a frame late, and our decode six
decibels down - and each must be rejected, or the comparison would pass a
decoder that produced anything at all.
"""

import math
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import oracle_env as oracle  # noqa: E402
import vorbis_synth  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(ROOT, "tests", "data")
PROBE = os.environ.get("GAUD_DUMP_PROBE") or os.path.join(
    ROOT, "build", "linux", "release", "apps", "oracle", "dump_probe")

#: The most a sample may differ from either reference, in 16-bit units.
MAX_SAMPLE = 2

#: The most the difference's root-mean-square may be, relative to the
#: signal's. Measured worst is 1.2e-3: these streams are an order of
#: magnitude quieter than music (a root mean square of a thousand or so),
#: so the one least significant bit by which two decoders differ is a larger
#: share of them than check_vorbis.py's 1e-4 allows for.
MAX_RELATIVE_RMS = 2.0e-3

#: How far our level may differ from a reference's, relative. Checked as
#: well as the difference, because a decoder six decibels down passes any
#: threshold scaled to the signal. Measured worst 5.8e-3, for the same
#: reason; six decibels is 5e-1.
MAX_LEVEL = 1.0e-2

#: Fixtures ffmpeg's native decoder refuses, and why.
NATIVE_REFUSES = {
    "vorbis_syn_floor0_explicit_seq":
        "codebook lookup type 2: ffmpeg's decoder reads types 0 and 1 only",
    "vorbis_syn_floor0_explicit_ramp":
        "codebook lookup type 2: ffmpeg's decoder reads types 0 and 1 only",
}


def run(argv, **kw):
    return subprocess.run(argv, capture_output=True, **kw)


def samples_of(raw):
    return struct.unpack("<%dh" % (len(raw) // 2), raw[:len(raw) // 2 * 2])


def ours(path):
    finished = run([PROBE, path, "--pcm-le"])
    if finished.returncode != 0:
        raise SystemExit("dump_probe failed on %s:\n%s"
                         % (path, finished.stderr.decode(errors="replace")))
    return samples_of(finished.stdout)


def ffmpeg(path, extra, scratch):
    """What ffmpeg decodes the file to, or None if it declines."""
    out = path + ".ref.pcm"
    if os.path.exists(out):
        os.remove(out)
    finished = run(oracle.command("ffmpeg", [
        "ffmpeg", "-v", "error", "-y"] + extra + [
        "-i", path, "-f", "s16le", "-acodec", "pcm_s16le", out],
        scratch=scratch))
    if finished.returncode != 0 or not os.path.exists(out):
        return None
    with open(out, "rb") as f:
        data = f.read()
    os.remove(out)
    return samples_of(data) if data else None


def compare(mine, ref):
    """(max difference, relative rms difference, relative level error)."""
    if len(mine) != len(ref):
        return (math.inf, math.inf, math.inf)
    worst = 0
    square = 0.0
    signal = 0.0
    level_mine = 0.0
    level_ref = 0.0
    for a, b in zip(mine, ref):
        d = abs(a - b)
        worst = max(worst, d)
        square += d * d
        signal += b * b
        level_mine += abs(a)
        level_ref += abs(b)
    rms = math.sqrt(square / max(1, len(mine)))
    signal_rms = math.sqrt(signal / max(1, len(mine)))
    relative = rms / signal_rms if signal_rms else (0.0 if rms == 0 else math.inf)
    level = abs(level_mine - level_ref) / level_ref if level_ref else 0.0
    return (worst, relative, level)


def passes(result):
    worst, relative, level = result
    return (worst <= MAX_SAMPLE and relative <= MAX_RELATIVE_RMS
            and level <= MAX_LEVEL)


def main():
    oracle.check_pin("ffmpeg")
    print(oracle.provenance(["ffmpeg"]))
    failures = []
    committed = 0
    # The fixtures are what the generator writes.
    written = vorbis_synth.write_cases(os.path.join(ROOT, "build", "synth-check"))
    for name, (data, frames) in written.items():
        path = os.path.join(DATA, name + ".ogg")
        with open(path, "rb") as f:
            if f.read() != data:
                failures.append("%s: differs from what vorbis_synth.py "
                                "writes" % name)
            else:
                committed += 1
    frames_expected = {name: frames for name, (_, frames) in written.items()}
    print("%d of %d fixtures are byte-identical to the generator's output"
          % (committed, len(frames_expected)))

    channels_of = {name: dict(kw).get("channels", 1)
                   for name, kw in vorbis_synth.CASES}
    decoded = {}
    compared = 0
    total_samples = 0
    worst_overall = 0
    for name in frames_expected:
        path = os.path.join(DATA, name + ".ogg")
        mine = ours(path)
        decoded[name] = mine
        channels = channels_of[name]
        if len(mine) != frames_expected[name] * channels:
            failures.append("%s: we decode %d samples, the stream's last "
                            "granule position says %d"
                            % (name, len(mine),
                               frames_expected[name] * channels))
        native = ffmpeg(path, [], DATA)
        lib = ffmpeg(path, ["-c:a", "libvorbis"], DATA)
        if name in NATIVE_REFUSES:
            if native is not None:
                failures.append("%s: ffmpeg's native decoder now reads this, "
                                "so the exclusion (%s) is stale"
                                % (name, NATIVE_REFUSES[name]))
            native = None
        elif native is None:
            failures.append("%s: ffmpeg's native decoder declined it" % name)
        if lib is None:
            failures.append("%s: libvorbis declined it" % name)
        for label, ref in (("ffmpeg-native", native), ("libvorbis", lib)):
            if ref is None:
                continue
            result = compare(mine, ref)
            compared += 1
            total_samples += len(mine)
            if result[0] != math.inf:
                worst_overall = max(worst_overall, result[0])
            if not passes(result):
                failures.append(
                    "%s against %s: max %s, relative rms %.2e, level %.2e"
                    % (name, label, result[0], result[1], result[2]))
        # The two references agree with each other, which is what makes
        # either of them evidence.
        if native is not None and lib is not None:
            if compare(native, lib)[0] > MAX_SAMPLE:
                failures.append("%s: the two references disagree by more "
                                "than %d" % (name, MAX_SAMPLE))

    # The controls: wrong answers, which the same comparison must reject.
    controls = 0
    names = sorted(decoded)
    sample = decoded[names[0]]
    reference = ffmpeg(os.path.join(DATA, names[0] + ".ogg"),
                       ["-c:a", "libvorbis"], DATA)
    wrong = {
        "another stream's decode":
            tuple((list(decoded[names[1]]) + [0] * len(sample))[:len(sample)]),
        "a decode one frame late": (0,) + tuple(sample[:-1]),
        "a decode six decibels down": tuple(int(round(v / 2)) for v in sample),
    }
    for label, bad in wrong.items():
        controls += 1
        if passes(compare(bad, reference)):
            failures.append("control passed that must fail: %s" % label)

    print("%d comparisons over %d samples; worst difference from either "
          "reference %d (of 32,768); %d controls rejected"
          % (compared, total_samples, worst_overall,
             controls - sum(1 for f in failures if f.startswith("control"))))
    if failures:
        print("\n".join("FAIL " + f for f in failures))
        return 1
    print("check-vorbis-synth: every stream decodes as two independent "
          "references do")
    return 0


if __name__ == "__main__":
    sys.exit(main())
