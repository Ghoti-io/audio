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
"""RFC 6716's conformance vectors, decoded with this library's Opus decoder.

Section 6 of RFC 6716 defines compliance in two parts, and this is the
only gate in the library whose standard is set by a document rather than
by a measurement someone here chose:

  1. the decoder's output must be accepted by `opus_compare` against the
     reference output, and
  2. the decoder "MUST have the same final range decoder state as that
     of the reference decoder".

The second is the stricter one. Every other decode differential here
compares samples and must pick a tolerance, because two decoders may
round differently and both be right. A final range state is an exact
equality over the entire sequence of symbols a frame contained: one
misread probability anywhere changes it, and nothing about it can be
"close". The vectors carry that state for every packet, so the strictest
half of conformance is 20,075 integer comparisons.

What this gate asserts, for each of the twelve vectors:

  - every packet is accepted, and the decoder's final range state after
    it is the one the vector states;
  - the number of samples is the reference's own output length, to the
    sample;
  - `opus_compare`, run in the pinned reference image, says the output
    passes against the vector's `.dec` file; and
  - the output is *identical*, bit for bit, to what RFC 6716's own
    fixed-point decoder writes. That last is stronger than section 6
    asks and is pinned in `opus_vectors.DECODED_SHA256`, with the reason
    those hashes are not the `.dec` files'.

**And it fails if the codec stops declaring GAUD_CAP_DECODE**, because a
decoder that has quietly become unreachable must not leave this gate
reporting on a file nobody decodes. planning/audio.md section 11.18
makes the same argument about capability bits; this is the enforcement.
"""

import hashlib
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import opus_vectors  # noqa: E402
import oracle_env  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))

# 48 kHz, two channels, 16-bit: what `opus_demo` wrote the .dec files as,
# and what RFC 6716 section 6.1's own command line asks for.
BYTES_PER_FRAME = 4


def probe_path():
    """The probe binary, which the Makefile builds and names."""
    path = os.environ.get("GAUD_OPUS_DECODE_PROBE")
    if not path:
        raise SystemExit(
            "check-opus-vectors: GAUD_OPUS_DECODE_PROBE is not set, so "
            "there is nothing to ask. Run this through `make "
            "check-opus-vectors`.")
    if not os.path.exists(path):
        raise SystemExit(f"check-opus-vectors: no probe at {path}")
    return path


def decode(probe, where, name, out_dir):
    """Run the probe on one vector; returns its numbers and the PCM path."""
    pcm = os.path.join(out_dir, name + ".pcm")
    result = subprocess.run(
        [probe, os.path.join(where, name + ".bit"), pcm],
        capture_output=True, text=True)
    if result.returncode != 0:
        raise SystemExit(
            f"check-opus-vectors: the probe exited {result.returncode} on "
            f"{name}, so nothing it printed is an answer.\n{result.stderr}")
    numbers = {}
    for line in result.stdout.splitlines():
        key, value = line.split()
        numbers[key] = value if key == "capabilities" else int(value)
    if numbers.get("capabilities") != "decode":
        raise SystemExit(
            "check-opus-vectors: the codec no longer declares "
            "GAUD_CAP_DECODE, so this gate would be reporting on a decoder "
            "nobody can reach.")
    for key in ("packets", "compared", "range_wrong", "refused", "samples"):
        if key not in numbers:
            raise SystemExit(
                f"check-opus-vectors: the probe printed no {key} for {name}")
    return numbers, pcm


def opus_compare(where, name, pcm, out_dir):
    """What `opus_compare` says of our output, in the pinned image."""
    reference = os.path.join(where, name + ".dec")
    command = oracle_env.command(
        "opus_compare", ["opus_compare", "-s", reference, pcm],
        scratch=[where, out_dir])
    result = subprocess.run(command, capture_output=True, text=True)
    # opus_compare reports on stderr; the engine adds a banner of its own.
    lines = [line for line in (result.stdout + result.stderr).splitlines()
             if line.startswith(("Test vector", "Opus quality"))]
    passed = result.returncode == 0 and "Test vector PASSES" in lines
    return passed, lines[-1:] or ["no verdict"]


def main(argv):
    where = opus_vectors.require()
    if subprocess.run([sys.executable,
                       os.path.join(HERE, "opus_vectors.py"),
                       "--check"]).returncode != 0:
        raise SystemExit(
            "check-opus-vectors: the vectors do not match their pins, so "
            "any verdict about them would be about different data.")

    probe = probe_path()
    try:
        print(oracle_env.provenance(["opus_compare"]))
    except oracle_env.OracleUnavailable as why:
        raise SystemExit(
            "check-opus-vectors: opus_compare is not reachable, and it is "
            "the normative half of RFC 6716 section 6, so this gate cannot "
            f"stand in for it.\n{why}")

    failures = []
    totals = {"packets": 0, "compared": 0, "samples": 0, "exact": 0}
    with tempfile.TemporaryDirectory(prefix="gaud-opus-") as out_dir:
        os.chmod(out_dir, 0o777)
        for name in opus_vectors.VECTORS:
            numbers, pcm = decode(probe, where, name, out_dir)
            want_samples = os.path.getsize(
                os.path.join(where, name + ".dec")) // BYTES_PER_FRAME
            for key in ("packets", "compared", "samples"):
                totals[key] += numbers[key]
            problems = []
            if numbers["refused"]:
                problems.append(f"{numbers['refused']} packets were refused")
            if numbers["range_wrong"]:
                problems.append(
                    f"{numbers['range_wrong']} of {numbers['compared']} "
                    "packets ended with a different range decoder state "
                    "than the reference")
            if numbers["compared"] == 0:
                problems.append("no final range state was compared")
            if numbers["samples"] != want_samples:
                problems.append(
                    f"{numbers['samples']} samples where the reference "
                    f"decoder produced {want_samples}")
            passed, said = opus_compare(where, name, pcm, out_dir)
            if not passed:
                problems.append(f"opus_compare says: {' '.join(said)}")
            sha = opus_vectors.digest(pcm)
            exact = sha == opus_vectors.DECODED_SHA256[name]
            if exact:
                totals["exact"] += 1
            else:
                problems.append(
                    "the output is not identical to the reference "
                    f"decoder's ({sha[:16]} where "
                    f"{opus_vectors.DECODED_SHA256[name][:16]} was pinned)")
            verdict = "exact" if exact else "DIFFERS"
            print(f"  {name}  {numbers['packets']:5d} packets  "
                  f"{numbers['samples']:8d} samples  range "
                  f"{numbers['compared'] - numbers['range_wrong']}/"
                  f"{numbers['compared']}  opus_compare "
                  f"{'pass' if passed else 'FAIL'}  samples {verdict}")
            failures += [f"{name}: {p}" for p in problems]

    print(f"check-opus-vectors: {totals['packets']} packets, "
          f"{totals['samples']} samples, {totals['compared']} final range "
          f"decoder states compared, {totals['exact']} of "
          f"{len(opus_vectors.VECTORS)} vectors identical to the "
          "reference")

    if failures:
        print("\n\033[0;31mcheck-opus-vectors: "
              f"{len(failures)} finding(s)\033[0m", file=sys.stderr)
        for one in failures:
            print(f"  - {one}", file=sys.stderr)
        return 1
    print("\033[0;32mcheck-opus-vectors: RFC 6716 section 6 holds on all "
          "twelve vectors\033[0m")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
