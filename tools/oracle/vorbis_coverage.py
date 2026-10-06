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
"""Which arms of the Vorbis setup parser the corpus actually reaches.

    make vorbis-coverage

**Not pass/fail, and no reference is involved.** `make check-vorbis` asks
whether we agree with other readers about what a file is; this asks a
question no other reader can answer, which is which branches of *this*
parser a corpus of real files exercises at all.

It exists because of what a Vorbis stream is. Every table an MP3 decoder
needs is in a standard; a Vorbis stream states its own codebooks, floor
curves, residue layout, channel coupling and block modes. So the branches
of this parser are selected by the *encoder* that wrote each fixture, and
a corpus samples encoders rather than the format - more sharply than a
corpus of MP3s does, where at least the tables are fixed.
`flac-coverage` and `mpeg-coverage` make the same argument for their
formats; this is the third instance and the one where it bites hardest.

It also carries the measurement that decided VORBIS_Q. The extremes of
every codebook's vector values are printed, because the first draft of
that constant was chosen rather than measured and saturated on two of the
ten fixtures.

The output names what no encoder reaches. **That is the useful half**: five
arms of the format are reached by nothing any encoder in the oracle image
produces at any setting, and knowing which before the decoder is written is
the difference between a documented gap and a coverage hole with an excuse
attached. They are reached instead by streams written by hand
(tools/oracle/vorbis_synth.py), and the gate fails if one of the five is
reached by neither.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(ROOT, "tests", "data")
PROBE = os.environ.get("GAUD_VORBIS_PROBE") or os.path.join(
    ROOT, "build", "linux", "release", "apps", "oracle", "vorbis_probe")

#: Arms of the format that no encoder in the oracle image produces at any
#: setting, why, and which check reaches each instead.
#:
#: **Each is a feature of the format that no encoder writes**, which is a
#: different thing from a feature nobody has got round to testing. Saying
#: so here, with the reason, is what keeps the counts below honest: a
#: denominator that quietly excluded these would read as complete coverage.
#: They are reached by `tools/oracle/vorbis_synth.py`, which writes streams
#: from the specification, and scored by `make check-vorbis-synth` against
#: ffmpeg's decoder and libvorbis.
HAND_WRITTEN_ONLY = {
    "floor type 0": (
        "Line spectral pair floors. In the specification and produced by "
        "neither libvorbis nor libavcodec, because floor 1 superseded it "
        "before Vorbis I was finished. Decoded in integer arithmetic "
        "(src/codec/vorbis/vorbis_floor0.c) and scored against two "
        "decoders on streams written for the purpose."),
    "residue type 0": (
        "The original residue layout, which lays a partition's vectors "
        "across it where types 1 and 2 keep them end to end. Same history "
        "as floor 0. The first version of it here was wrong, and nothing "
        "noticed until a stream was written that used it."),
    "codebook lookup type 2": (
        "A codebook that states every entry's vector explicitly rather "
        "than as a lattice. Legal and enormous - entries times "
        "dimensions multiplicands rather than the lattice's one per axis "
        "- so no encoder chooses it. ffmpeg's native decoder refuses it; "
        "libvorbis reads it."),
    "a sequential codebook": (
        "`sequence_p` set, where a vector's values accumulate rather "
        "than standing alone. Used by floor 0's codebooks."),
    "one side of a coupled pair silent": (
        "A coupling step reads both of its channels, so if either "
        "carries something both must have their residue decoded, and "
        "the silent one's spectrum is zero because it has no floor. No "
        "signal in the corpus makes an encoder leave one channel of a "
        "pair empty while the other is not. Reached by the coupled "
        "streams, where reading the curve it never wrote changes the "
        "output."),
}


def rows():
    files = sorted(f for f in os.listdir(DATA) if f.startswith("vorbis_")
                   and f.endswith(".ogg"))
    if not files:
        raise SystemExit("no Vorbis fixtures in tests/data. Run `make "
                         "corpus`.")
    if not os.path.isfile(PROBE):
        raise SystemExit("vorbis_probe is not built: %s\nRun `make "
                         "oracle-probe`." % PROBE)
    finished = subprocess.run(
        [PROBE] + [os.path.join(DATA, f) for f in files],
        capture_output=True, text=True)
    if finished.returncode != 0:
        raise SystemExit("vorbis_probe failed:\n%s"
                         % finished.stderr[-2000:])
    out = []
    current = None
    for line in finished.stdout.splitlines():
        parts = line.split("\t")
        if parts[0] == "file":
            current = os.path.basename(parts[1])
            continue
        out.append((current, parts))
    return files, out


def main():
    files, parsed = rows()

    books = 0
    lookups = {0: 0, 1: 0, 2: 0}
    sequential = 0
    sparse = 0
    longest = 0
    widest = 0
    most_entries = 0
    floors = {0: 0, 1: 0}
    residues = {0: 0, 1: 0, 2: 0}
    multipliers = {}
    passes = {}
    submaps = {}
    coupling = {}
    modes = {0: 0, 1: 0}
    blocksizes = set()
    low = 0
    high = 0
    smallest = 0

    hand = set()      # arms the hand-written streams reach
    encoder = set()   # and the encoders' streams
    for name, parts in parsed:
        kind = parts[0]
        synthetic = name.startswith("vorbis_syn_")
        if kind == "info":
            blocksizes.add((int(parts[3]), int(parts[4])))
        elif kind == "book":
            books += 1
            lookups[int(parts[4])] += 1
            if int(parts[4]) == 2:
                (hand if synthetic else encoder).add("codebook lookup type 2")
            if int(parts[5]):
                sequential += 1
                (hand if synthetic else encoder).add("a sequential codebook")
            if int(parts[7]):
                sparse += 1
            longest = max(longest, int(parts[6]))
            widest = max(widest, int(parts[2]))
            most_entries = max(most_entries, int(parts[3]))
            if not synthetic:
                low = min(low, int(parts[8]))
                high = max(high, int(parts[9]))
                one = int(parts[10])
                if one and (smallest == 0 or one < smallest):
                    smallest = one
        elif kind == "floor":
            floors[int(parts[2])] += 1
            if int(parts[2]) == 0:
                (hand if synthetic else encoder).add("floor type 0")
            if int(parts[2]) == 1:
                multipliers[int(parts[4])] = multipliers.get(
                    int(parts[4]), 0) + 1
        elif kind == "residue":
            residues[int(parts[2])] += 1
            if int(parts[2]) == 0:
                (hand if synthetic else encoder).add("residue type 0")
            passes[int(parts[7])] = passes.get(int(parts[7]), 0) + 1
        elif kind == "mapping":
            if synthetic and int(parts[3]):
                hand.add("one side of a coupled pair silent")
            submaps[int(parts[2])] = submaps.get(int(parts[2]), 0) + 1
            coupling[int(parts[3])] = coupling.get(int(parts[3]), 0) + 1
        elif kind == "mode":
            modes[int(parts[2])] += 1

    scale = float(1 << 16)
    print("%d fixtures, %d codebooks." % (len(files), books))
    print()
    print("  codebook lookup      none %-4d  lattice %-4d  explicit %-4d"
          % (lookups[0], lookups[1], lookups[2]))
    print("  sequential vectors   %d of %d" % (sequential, books))
    print("  sparse (some entry unused)  %d of %d" % (sparse, books))
    print("  longest codeword     %d bits     widest vector %d values"
          % (longest, widest))
    print("  most entries         %d" % most_entries)
    print()
    print("  floor types          1: %-4d  0: %-4d" % (floors[1], floors[0]))
    print("  floor 1 multipliers  %s"
          % "  ".join("%d:%d" % kv for kv in sorted(multipliers.items())))
    print("  residue types        1: %-4d  2: %-4d  0: %-4d"
          % (residues[1], residues[2], residues[0]))
    print("  residue passes used  %s"
          % "  ".join("%d:%d" % kv for kv in sorted(passes.items())))
    print("  mapping submaps      %s"
          % "  ".join("%d:%d" % kv for kv in sorted(submaps.items())))
    print("  coupling steps       %s"
          % "  ".join("%d:%d" % kv for kv in sorted(coupling.items())))
    print("  modes                short %d  long %d" % (modes[0], modes[1]))
    print("  block size pairs     %s"
          % "  ".join("%d/%d" % pair for pair in sorted(blocksizes)))
    print()
    print("  codebook vector values, which is what VORBIS_Q is sized for:")
    print("    range   [%+.4f, %+.4f]  in Q16 [%d, %d]"
          % (low / scale, high / scale, low, high))
    print("    step    %.6f, the smallest nonzero magnitude anywhere"
          % (smallest / scale))
    print("    **every value is an integer**, because a residue codebook")
    print("    quantises the spectrum and the floor carries the scale, so")
    print("    the encoders choose a delta of one. Q16 holds %+.0f with"
          % (high / scale))
    print("    %.1f times that in reserve; Q20 saturated on two fixtures,"
          % (32768.0 / (high / scale)))
    print("    which is how this came to be measured rather than assumed.")

    print()
    print("Reached by no encoder's stream, with the reason and what reaches "
          "it:")
    for arm in sorted(HAND_WRITTEN_ONLY):
        by = []
        if arm in encoder:
            by.append("an encoder")
        if arm in hand:
            by.append("a hand-written stream")
        print("  %s  [reached by: %s]" % (arm, ", ".join(by) or "nothing"))
        for line in _wrap(HAND_WRITTEN_ONLY[arm], 68):
            print("      %s" % line)
    dark = [arm for arm in HAND_WRITTEN_ONLY
            if arm not in hand and arm not in encoder]
    print()
    print("%d of %d arms are reached only by streams `vorbis_synth.py` "
          "wrote; %d by an encoder's; %d by nothing."
          % (len([a for a in HAND_WRITTEN_ONLY if a in hand
                  and a not in encoder]), len(HAND_WRITTEN_ONLY),
             len([a for a in HAND_WRITTEN_ONLY if a in encoder]),
             len(dark)))
    if dark:
        raise SystemExit("vorbis-coverage: reached by nothing: %s"
                         % ", ".join(dark))

    # The one assertion this instrument makes. Everything above is a
    # count; this is a bound, because VORBIS_Q depends on it and a
    # fixture added later could break it silently.
    if high >= (1 << 30) or low <= -(1 << 30):
        raise SystemExit(
            "vorbis-coverage: a codebook value is within a factor of two "
            "of saturating Q16 (%d of %d). VORBIS_Q needs re-measuring "
            "before the corpus grows further." % (high, 1 << 31))
    return 0


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
    sys.exit(main())
