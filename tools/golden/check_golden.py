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
"""Decode the corpus on big-endian machines and require the same samples.

    make check-golden

planning/audio.md 11.1 promises that this library's decoders produce
**byte-identical PCM on every platform and architecture**. On one machine
that promise is unfalsifiable. This builds the library for three big-endian
targets, runs the corpus under qemu, and requires the samples to hash the
same as they do here.

### Why this matters in phase 1 rather than later

It would be easy to file this as scaffolding for the fixed-point decoders
that arrive in phase 5. It is not. **WAV is little-endian and AIFF is
big-endian**, so the byte-swapping in each codec runs on exactly the inputs
the other one does not:

    on x86-64 (little)   WAV: no swap     AIFF: swap
    on s390x  (big)      WAV: swap        AIFF: no swap

Every swap path this library has is dead code on one of the two, and a gate
that only ran here would leave half of them never executed. That is the
whole reason AIFF is in phase 1 beside WAV.

### What is compared

The SHA-256 of the decoded **sample values**, per fixture, against what this
host produces. Values and not raw bytes, and the difference is the whole
subtlety: this library decodes to the host's byte order on purpose, so that a
caller can do arithmetic without swapping first - which means the raw bytes of
a correct decode necessarily differ between a little-endian and a big-endian
machine. `dump_probe --pcm-le` puts them in one spelling so the comparison is
about the numbers. A first draft of this gate compared the raw bytes and
reported a difference on every multi-byte fixture, all of them correct. - computed now rather than committed, because the host's answer is
itself checked against ffmpeg, sox and libsndfile by `make check-corpus`.
Committing a golden file would freeze one machine's answer; comparing
against a reading three independent implementations have agreed with is
stronger.
"""

import hashlib
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))   # .../libs/audio/tools/golden
ROOT = os.path.dirname(os.path.dirname(HERE))      # .../libs/audio
WORKSPACE = os.path.dirname(os.path.dirname(ROOT)) # the workspace
SHIM = os.path.join(HERE, "cross_shim.c")
IMAGE = os.environ.get("GHOTI_XARCH_IMAGE", "localhost/ghoti-xarch:deb13")
ENGINE = os.environ.get("GHOTI_CONTAINER_ENGINE", "docker")
PROBE = os.path.join(ROOT, "build", "linux", "release", "apps", "oracle",
                     "dump_probe")

# Every one of them big-endian, which is the axis that matters here. A
# little-endian cross target would be a second copy of this host.
TARGETS = {
    "s390x": ("s390x-linux-gnu-gcc", "qemu-s390x", "s390x-linux-gnu",
              "64-bit big-endian"),
    "powerpc64": ("powerpc64-linux-gnu-gcc", "qemu-ppc64",
                  "powerpc64-linux-gnu", "64-bit big-endian, a second one"),
}


def fixtures():
    data = os.path.join(ROOT, "tests", "data")
    return sorted(f for f in os.listdir(data)
                  if f.endswith((".wav", ".aiff", ".aifc")))


def host_hashes():
    """What this machine decodes, per fixture."""
    out = {}
    for name in fixtures():
        path = os.path.join(ROOT, "tests", "data", name)
        finished = subprocess.run([PROBE, path, "--pcm-le"],
                                  capture_output=True)
        if finished.returncode != 0:
            raise SystemExit("dump_probe failed on %s" % name)
        out[name] = hashlib.sha256(finished.stdout).hexdigest()
    return out


def script(compiler, qemu, triple):
    out = "/tmp/golden"
    return r"""
set -e
mkdir -p %(out)s
cd %(root)s
%(cc)s -std=c17 -O2 -w -fno-strict-aliasing \
    -I include -I src -I build/linux/release/generated -I %(cutil)s \
    -o %(out)s/dump_probe \
    $(find src -name '*.c') tools/oracle/dump_probe.c %(shim)s -lm
for f in tests/data/*.wav tests/data/*.aiff tests/data/*.aifc; do
    printf '%%s ' "$(basename "$f")"
    %(qemu)s -L /usr/%(triple)s %(out)s/dump_probe "$f" --pcm-le | sha256sum \
        | cut -d' ' -f1
done
""" % {"out": out, "root": ROOT, "cc": compiler, "qemu": qemu,
       "triple": triple, "shim": SHIM,
       "cutil": os.path.join(WORKSPACE, ".local", "include", "ghoti.io",
                             "cutil-0")}


def main():
    if not os.path.isfile(PROBE):
        raise SystemExit("dump_probe is not built. Run `make oracle-probe`.")
    names = fixtures()
    if not names:
        raise SystemExit("tests/data is empty, so this gate measures nothing.")

    host = host_hashes()
    print("host (%s): %d fixtures decoded" % (os.uname().machine, len(host)))

    wanted = sys.argv[1:] or list(TARGETS)
    bad = []
    for label in wanted:
        if label not in TARGETS:
            raise SystemExit("unknown target %r" % label)
        compiler, qemu, triple, described = TARGETS[label]
        print("\n%s (%s)" % (label, described))
        finished = subprocess.run(
            [ENGINE, "run", "--rm", "--network", "none",
             "--volume", "%s:%s:ro" % (WORKSPACE, WORKSPACE),
             "--workdir", ROOT, IMAGE,
             "sh", "-c", script(compiler, qemu, triple)],
            capture_output=True, text=True)
        if finished.returncode != 0:
            raise SystemExit("%s: the cross build or run failed:\n%s"
                             % (label, finished.stderr[-3000:]))

        seen = {}
        for line in finished.stdout.strip().splitlines():
            parts = line.split()
            if len(parts) == 2:
                seen[parts[0]] = parts[1]
        if not seen:
            # A gate that compared nothing would report success.
            bad.append("%s: produced no hashes at all" % label)
            continue
        for name in names:
            if name not in seen:
                bad.append("%s: %s was not decoded there" % (label, name))
                continue
            if seen[name] != host[name]:
                bad.append("%s: %s differs - host %s, target %s"
                           % (label, name, host[name][:16], seen[name][:16]))
            else:
                print("  %-27s %s" % (name, seen[name][:16]))

    if bad:
        print("\n%d difference(s):" % len(bad), file=sys.stderr)
        for line in bad:
            print("  " + line, file=sys.stderr)
        return 1
    print("\n%d fixtures decode identically on %d big-endian targets and "
          "here." % (len(names), len(wanted)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
