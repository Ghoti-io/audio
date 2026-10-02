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
"""RFC 6716's conformance vectors, read with this library's Opus code.

Section 6 of RFC 6716 defines compliance in two parts, and this is the
only gate in the library whose standard is set by a document rather than
by a measurement someone here chose:

  1. the decoder's output must be within the thresholds `opus_compare`
     reports against the reference output, and
  2. the decoder "MUST have the same final range decoder state as that
     of the reference decoder".

The second is the interesting one. Every other decode differential here
compares samples and must pick a tolerance, because two decoders may
round differently and both be right. A final range state is an exact
equality over the entire sequence of symbols a frame contained: one
misread probability anywhere changes it, and nothing about it can be
"close". The vectors carry that state for every packet, so the strictest
half of conformance is 20,075 integer comparisons.

**What this gate can say today.** There is no Opus decoder yet, so
neither half of section 6 is reachable. What is reachable is already
worth asserting, and is asserted:

  - every packet in every vector parses, and
  - the duration this library computes for each vector - frame count
    times the frame size each of the 32 configurations implies - equals
    the reference decoder's own output length, to the sample.

That second one is a real differential rather than arithmetic checked
against itself. The `.dec` files are what the reference decoder actually
produced, so their length is its opinion of how long each vector is, and
it is reached through 20,075 packets of four different framings. A wrong
frame size for one configuration, or one misread padding chain, shows up
as a mismatched total.

**And what it must not become.** The gate fails if the codec declares
GAUD_CAP_DECODE while no range states are being compared. Without that,
the day a decoder lands this file would keep printing a cheerful summary
of the framing and never notice that the harder half of its job had
become possible. planning/audio.md section 11.18 makes the same argument
about capability bits; this is the enforcement.
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import opus_vectors  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))

# 48 kHz, two channels, 16-bit: what `opus_demo` wrote the .dec files as,
# and what RFC 6716 section 6.1's own command line asks for.
BYTES_PER_FRAME = 4


def probe_path():
    """The probe binary, which the Makefile builds and names."""
    path = os.environ.get("GAUD_OPUS_PROBE")
    if not path:
        raise SystemExit(
            "check-opus-vectors: GAUD_OPUS_PROBE is not set, so there is "
            "nothing to ask. Run this through `make check-opus-vectors`.")
    if not os.path.exists(path):
        raise SystemExit(f"check-opus-vectors: no probe at {path}")
    return path


def run_probe(probe, paths):
    """Our reading of every vector, as a dict keyed by vector name."""
    result = subprocess.run([probe] + paths, capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(
            "check-opus-vectors: the probe exited "
            f"{result.returncode}, so nothing it printed is an answer.\n"
            f"{result.stderr}")
    out = {}
    capabilities = None
    current = None
    for line in result.stdout.splitlines():
        parts = line.split("\t")
        if parts[0] == "capabilities":
            capabilities = parts[1]
        elif parts[0] == "file":
            current = os.path.basename(parts[1]).replace(".bit", "")
            out[current] = {"packets": 0, "refused": 0, "samples": 0,
                            "configs": set(), "ranges": 0, "matched": 0,
                            "stale": 0, "absent": 0, "redundant": 0,
                            "truncated": False}
        elif parts[0] == "packet" and current:
            entry = out[current]
            entry["packets"] += 1
            if "refused" in parts:
                entry["refused"] += 1
                continue
            fields = dict(zip(parts[2::2], parts[3::2]))
            entry["configs"].add(int(fields["config"]))
            entry["samples"] += int(fields["samples"])
            # "none" is a packet in a mode that has no decoder yet.
            # "stale" is a CELT packet in a stream that has already
            # carried one of those, so the state it should have built
            # on is missing. "redundant" is a SILK packet with room
            # after its frames for the mode-switch handover, which
            # reads one more symbol and folds a second decoder's range
            # into the answer. None of the three is a failure, and
            # none is a pass either.
            state = fields.get("range", "none")
            if state == "none":
                entry["absent"] += 1
            elif state == "stale":
                entry["stale"] += 1
            elif state == "redundant":
                entry["redundant"] += 1
            else:
                entry["ranges"] += 1
                if int(state) == int(fields["want_range"]):
                    entry["matched"] += 1
        elif parts[0] == "truncated" and current:
            out[current]["truncated"] = True
    if capabilities is None:
        raise SystemExit(
            "check-opus-vectors: the probe printed no capability line, so "
            "this gate cannot tell whether a decoder exists.")
    return capabilities, out


def main(argv):
    where = opus_vectors.require()
    if subprocess.run([sys.executable,
                       os.path.join(HERE, "opus_vectors.py"),
                       "--check"]).returncode != 0:
        raise SystemExit(
            "check-opus-vectors: the vectors do not match their pins, so "
            "any verdict about them would be about different data.")

    probe = probe_path()
    paths = [os.path.join(where, name + ".bit")
             for name in opus_vectors.VECTORS]
    capabilities, ours = run_probe(probe, paths)

    failures = []
    total_packets = 0
    total_samples = 0
    total_ranges = 0
    total_matched = 0
    total_stale = 0
    total_redundant = 0
    total_absent = 0
    seen_configs = set()

    print(f"check-opus-vectors: the codec declares {capabilities}")
    for name in opus_vectors.VECTORS:
        entry = ours.get(name)
        if entry is None:
            failures.append(f"{name}: the probe said nothing about it")
            continue
        reference = os.path.getsize(os.path.join(where, name + ".dec"))
        want_samples = reference // BYTES_PER_FRAME
        total_packets += entry["packets"]
        total_samples += entry["samples"]
        total_ranges += entry["ranges"]
        total_matched += entry["matched"]
        total_stale += entry["stale"]
        total_redundant += entry["redundant"]
        total_absent += entry["absent"]
        seen_configs |= entry["configs"]

        # Each vector's own count, so a regression says which stream it
        # is in rather than only that the total moved.
        want_matched = opus_vectors.RANGE_MATCHED.get(name)
        if want_matched is not None and entry["matched"] != want_matched:
            failures.append(
                f"{name}: {entry['matched']} packets ended with the "
                f"reference's range state, and {want_matched} did when "
                "this was last measured")

        if entry["truncated"]:
            failures.append(f"{name}: the vector's own framing ran out")
        if entry["refused"]:
            failures.append(
                f"{name}: {entry['refused']} of {entry['packets']} packets "
                "were refused")
        if entry["samples"] != want_samples:
            failures.append(
                f"{name}: we make it {entry['samples']} samples and the "
                f"reference decoder produced {want_samples} "
                f"({entry['samples'] - want_samples:+d})")
        want_configs = set(opus_vectors.CONFIGS[name])
        if entry["configs"] != want_configs:
            failures.append(
                f"{name}: reaches configurations "
                f"{sorted(entry['configs'])}, recorded as "
                f"{sorted(want_configs)}")

        note = ""
        if entry["ranges"]:
            note = (f"  range {entry['matched']}/{entry['ranges']}"
                    if entry["matched"] != entry["ranges"]
                    else f"  range {entry['ranges']}/{entry['ranges']} exact")
        print(f"  {name}  {entry['packets']:5d} packets  "
              f"{entry['samples']:8d} samples{note}")

    # Every one of the 32 configurations must be reached by some vector,
    # or the claim this gate rests on is smaller than it sounds.
    missing = sorted(set(range(32)) - seen_configs)
    if missing:
        failures.append(
            f"the vectors reach {len(seen_configs)} of 32 configurations; "
            f"missing {missing}")

    print(f"check-opus-vectors: {total_packets} packets, "
          f"{total_samples} samples, "
          f"{len(seen_configs)} of 32 configurations")

    if total_ranges == 0:
        if capabilities == "decode":
            failures.append(
                "the codec declares GAUD_CAP_DECODE and yet not one final "
                "range decoder state was compared. That is the harder half "
                "of RFC 6716 section 6 and it is now reachable: wire the "
                "decoder into tools/oracle/opus_probe.c rather than leaving "
                "this gate reporting on the framing alone.")
        else:
            print("check-opus-vectors: no final range states compared - "
                  "there is no decoder yet, and the codec does not claim "
                  "one. This is the half of RFC 6716 section 6 that is "
                  "still ahead; 0 of "
                  f"{total_packets} packets decode.")
    else:
        if total_matched != total_ranges:
            failures.append(
                f"{total_ranges - total_matched} of {total_ranges} packets "
                "ended with a different range decoder state than the "
                "reference, so a different sequence of symbols was read")
        else:
            print(f"check-opus-vectors: all {total_ranges} final range "
                  "decoder states match the reference exactly")
        # The categories that are not yet claimable, named so that the
        # number above cannot be read as "all of them".
        print(f"check-opus-vectors: {total_absent} packets are hybrid, "
              f"which has no decoder; {total_stale} are CELT in a stream "
              "that already carried another mode, so the state they would "
              f"build on is missing; {total_redundant} are SILK with room "
              "for a mode-switch handover this does not read yet. None of "
              "the three is compared.")
        if (total_ranges + total_stale + total_absent + total_redundant
                != total_packets):
            failures.append(
                "the four categories do not add up to the packet count, "
                "so some packets are being counted twice or not at all")

    if failures:
        print("\n\033[0;31mcheck-opus-vectors: "
              f"{len(failures)} finding(s)\033[0m", file=sys.stderr)
        for one in failures:
            print(f"  - {one}", file=sys.stderr)
        return 1
    print("\033[0;32mcheck-opus-vectors: every assertion this gate can "
          "make today holds\033[0m")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
