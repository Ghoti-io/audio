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
"""Score this library's writer against the pinned references.

    make check-writer

Each fixture is read by this library, written back out by this library, and
the result handed to ffmpeg, sox and libsndfile. Two things are then required
of every one of them:

  1. **They can read it at all.** For an encoder this is the exact, binary
     half of the gate - bitstream validity - and it stays available for every
     format this library will ever write, including the ones where quality
     becomes a matter of a metric and an argument.
  2. **The samples are unchanged.** Every format here is lossless, so a round
     trip that altered a sample is a defect, not a tolerance.

Both directions are covered: a WAV is rewritten as WAV *and* as AIFF, and an
AIFF as both too. The cross pairs are the interesting ones, because they are
where a byte-order mistake cannot hide - reading big-endian and writing
little-endian has to be wrong in two places to look right.

planning/audio.md 11.3 is why this is a separate gate from check-corpus: a
reader and a writer that share a misunderstanding agree with each other
perfectly, and only something outside this library can see it.
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import oracle_env as oracle  # noqa: E402
import check_corpus as corpus  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(ROOT, "tests", "data")
WRITE_PROBE = os.environ.get(
    "GAUD_WRITE_PROBE",
    os.path.join(ROOT, "build", "linux", "release", "apps", "oracle",
                 "write_probe"))

# WAV has no signed 8-bit and AIFF has no unsigned 8-bit; the two formats
# genuinely disagree, and this library refuses rather than shifting every
# sample by 128 without saying so. Named here so the refusal is a recorded
# expectation rather than a silent gap in the matrix.
CANNOT = {
    ("u8", "aiff"): "AIFF's 8-bit is signed; there is no unsigned spelling",
    ("s8", "wav"): "WAV's 8-bit is unsigned; there is no signed spelling",
}


def main():
    print(oracle.provenance(["ffmpeg", "sox", "libsndfile", "pywave"]))
    if not os.path.isfile(WRITE_PROBE):
        raise SystemExit("write_probe is not built: %s" % WRITE_PROBE)
    files = sorted(f for f in os.listdir(DATA)
                   if f.endswith((".wav", ".aiff", ".aifc")))
    if not files:
        raise SystemExit("tests/data is empty, so this gate measures nothing.")

    # The scratch directory lives INSIDE the repository, under tests/out
    # which .gitignore already covers. The references run in a container
    # with the repository bind-mounted and nothing else, so a directory in
    # /tmp is invisible to them - and the symptom is every reference
    # "producing nothing", which reads like a broken writer rather than a
    # mount that does not reach.
    scratch_root = os.path.join(ROOT, "tests", "out")
    os.makedirs(scratch_root, exist_ok=True)

    bad, refused, pairs = [], [], 0
    with tempfile.TemporaryDirectory(dir=scratch_root) as tmp:
        for name in files:
            source = os.path.join(DATA, name)
            meta = corpus.ours_meta(source)
            fmt = meta["format"]
            original = corpus.ours_pcm(source)

            for codec, ext in (("wav", "wav"), ("aiff", "aiff")):
                if (fmt, codec) in CANNOT:
                    refused.append("  %-27s -> %-4s  refused: %s"
                                   % (name, codec, CANNOT[(fmt, codec)]))
                    continue
                out = os.path.join(tmp, "%s.%s" % (name, ext))
                finished = subprocess.run(
                    [WRITE_PROBE, source, out, codec], capture_output=True)
                if finished.returncode != 0:
                    bad.append("%s -> %s: write_probe failed: %s"
                               % (name, codec,
                                  finished.stderr.decode().strip()[:120]))
                    continue

                # Our own reading of what we wrote, then the references'.
                # Ours first only to get the metadata; theirs is the verdict.
                complaints = corpus.check(out, quiet=True)
                back = corpus.ours_pcm(out)
                if back != original:
                    bad.append("%s -> %s: the round trip changed the samples"
                               % (name, codec))
                for line in complaints:
                    bad.append("%s -> %s: %s" % (name, codec,
                                                 line.split(": ", 1)[-1]))
                pairs += 1
                print("  %-27s -> %-4s  %s"
                      % (name, codec, "OK" if not complaints
                         and back == original else "FAIL"))

    if refused:
        print("\nRefused by design, and why:")
        for line in refused:
            print(line)

    print("\n%d round trips through this library's writer, each read back by "
          "every reference that can." % pairs)
    if bad:
        print("\n%d disagreement(s):" % len(bad), file=sys.stderr)
        for line in bad:
            print("  " + line, file=sys.stderr)
        return 1
    print("Every reference reads what we wrote, and the samples survived.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
