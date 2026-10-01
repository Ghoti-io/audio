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
"""Which arms of the MPEG audio decoder the corpus reaches.

    make mpeg-coverage

**Not a gate, and not pass/fail.** `make check-mpeg` says our decode
agrees with both reference decoders on every fixture; it says nothing
about how much of the decoder ran to produce them. An MPEG decoder is a
tree of branches selected by choices the *encoder* made - which of 32
Huffman tables, which of four window shapes, whether to use the bit
reservoir, whether to fold the stereo image - so a corpus of ordinary
music walks one path through it and leaves the rest dark.

planning/audio.md 11.14 asked for this in phase 5 from the first day
rather than after the fact, because phase 4 learned it the hard way: a
third of the FLAC decoder had never executed while every differential
passed.

A zero here is a question about the corpus, not a defect. Some rows can
be reached by no encoder in the oracle image at all - nothing writes
Layer I, nothing writes a mixed block, and nothing sets a CRC - and those
are covered by hand-built frames in `tests/unit/test_mp3.cpp` instead,
or not covered, which is the point of printing them.
"""

import collections
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DATA = os.path.join(ROOT, "tests", "data")


def counts(probe, paths):
    """Run the instrumented probe over each path and total the counters."""
    total = collections.OrderedDict()
    reached_by = collections.defaultdict(list)
    for path in paths:
        finished = subprocess.run([probe, path, "--pcm"],
            capture_output=True)
        here = {}
        for line in finished.stderr.decode("utf-8", "replace").splitlines():
            parts = line.split("\t")
            if len(parts) != 3 or parts[0] != "MPEGTRACE":
                continue
            name, value = parts[1], int(parts[2])
            here[name] = value
            total[name] = total.get(name, 0) + value
        for name, value in here.items():
            if value:
                reached_by[name].append(os.path.basename(path))
    return total, reached_by


def main(argv):
    if len(argv) < 2:
        raise SystemExit("usage: mpeg_coverage.py <instrumented probe>")
    probe = argv[1]
    files = sorted(os.path.join(DATA, f) for f in os.listdir(DATA)
                   if f.endswith((".mp3", ".mp2", ".mp1")))
    if not files:
        raise SystemExit("tests/data holds no MPEG audio to measure")

    total, reached_by = counts(probe, files)
    if not total:
        raise SystemExit(
            "the probe produced no counters, so this measured nothing. It "
            "has to be built with -DGAUD_MP3_TRACE=1; `make mpeg-coverage` "
            "does that.")

    reached = [name for name, value in total.items() if value]
    dark = [name for name, value in total.items() if not value]
    print("%d of %d named arms of the MPEG decoder are reached by the %d "
          "fixtures in tests/data.\n" % (len(reached), len(total),
              len(files)))
    width = max(len(name) for name in total)
    for name, value in total.items():
        mark = " " if value else "."
        where = reached_by.get(name, [])
        note = ""
        if value and len(where) <= 2:
            note = "  only %s" % ", ".join(where)
        print("  %s %-*s %10d%s" % (mark, width, name, value, note))

    if dark:
        print("\n%d arm(s) nothing reaches. A zero is a question about the "
              "corpus:" % len(dark))
        for name in dark:
            print("  %s" % name)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
