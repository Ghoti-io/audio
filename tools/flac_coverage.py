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
"""Which arms of the FLAC frame decoder the corpus reaches.

    make flac-coverage

**Not a gate.** It prints a table and always exits zero, because a zero in
it is a question about the corpus rather than a defect in the library, and
because three of the rows can only ever be reached by a hand-built frame -
no encoder in the oracle image writes a variable-blocksize stream, and
none defers the bit depth or the sample rate to STREAMINFO. Those are
covered by `tests/unit/test_flac.cpp`, which builds the frames itself, and
a gate that failed on them here would be demanding something impossible.

What it is for is the question a differential cannot answer. `make
check-corpus` says our decode matches the references on every fixture; it
says nothing about how much of the decoder ran to produce them. A FLAC
decoder is a tree of branches selected by choices the *encoder* made, so a
corpus of ordinary music walks one path through it and leaves the rest
dark - which is exactly what the first run of this found: eight named arms
that nothing reached.

Two populations are measured, because they answer different questions:

  corpus     the committed fixtures, written by ffmpeg and by libFLAC.
             What OTHER encoders make this decoder do.
  round trip the same fixtures re-encoded by this library and read back.
             What OUR encoder makes it do - which is how the escaped-
             partition arm is reached at all, since libFLAC's encoder
             never emits one.
"""

import collections
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA = os.path.join(ROOT, "tests", "data")
SCRATCH = os.path.join(ROOT, "tests", "out")
WRITE_PROBE = os.path.join(ROOT, "build", "linux", "release", "apps",
                           "oracle", "write_probe")


def counts(probe, paths):
    """Run the instrumented probe over each path and total the counters."""
    total = collections.OrderedDict()
    reached_by = collections.defaultdict(list)
    environment = dict(os.environ)
    for path in paths:
        finished = subprocess.run([probe, path, "--pcm"],
                                  capture_output=True, env=environment)
        for line in finished.stderr.decode().splitlines():
            parts = line.split("\t")
            if len(parts) != 3 or parts[0] != "FLACTRACE":
                continue
            value = int(parts[2])
            total[parts[1]] = total.get(parts[1], 0) + value
            if value:
                reached_by[parts[1]].append(os.path.basename(path))
    return total, reached_by


def report(label, total, reached_by):
    if not total:
        print("  %s: the probe produced no counters at all - it was not "
              "built with -DGAUD_FLAC_TRACE" % label)
        return 0, 0
    hit = sum(1 for value in total.values() if value)
    print("\n%s: %d of %d arms reached" % (label, hit, len(total)))
    for name, value in total.items():
        if value:
            first = reached_by[name][0]
            print("  reached   %-36s %7d  (first: %s)" % (name, value, first))
        else:
            print("  NOT SEEN  %-36s" % name)
    return hit, len(total)


def main():
    if len(sys.argv) < 2:
        raise SystemExit("usage: flac_coverage.py <instrumented probe>")
    probe = sys.argv[1]
    fixtures = sorted(
        os.path.join(DATA, f) for f in os.listdir(DATA)
        if f.endswith((".flac", ".oga")))
    if not fixtures:
        raise SystemExit("tests/data has no FLAC fixtures. Run `make corpus`.")

    total, reached_by = counts(probe, fixtures)
    hit, arms = report("corpus (%d fixtures)" % len(fixtures), total,
                       reached_by)

    # The same fixtures through our own writer. Needs the ordinary
    # write_probe, which is not the instrumented build - what is measured
    # is the instrumented READER over our writer's output.
    if os.path.isfile(WRITE_PROBE):
        os.makedirs(SCRATCH, exist_ok=True)
        rewritten = []
        for path in fixtures:
            out = os.path.join(
                SCRATCH, "cov-" + os.path.basename(path) + ".flac")
            if subprocess.run([WRITE_PROBE, path, out, "flac"],
                              capture_output=True).returncode == 0:
                rewritten.append(out)
        ours, ours_by = counts(probe, rewritten)
        report("round trip (%d re-encoded)" % len(rewritten), ours, ours_by)
        for path in rewritten:
            os.remove(path)
        for name, value in ours.items():
            if value and not total.get(name):
                hit += 1
        print("\n%d of %d arms reached by one population or the other."
              % (hit, arms))
    else:
        print("\nwrite_probe is not built, so only the corpus was "
              "measured. Run `make oracle-probe` for the other half.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
