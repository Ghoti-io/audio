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
WRITE_PROBE = os.path.join(ROOT, "build", "linux", "release", "apps",
                           "oracle", "write_probe")

# Every one of them big-endian, which is the axis that matters here. A
# little-endian cross target would be a second copy of this host.
#
# **Phase 5 is what this gate was built for.** Until the MPEG decoder
# there was little in the tree whose output could plausibly differ
# between architectures: PCM and FLAC are integer by construction. An
# MPEG decoder is a filterbank, an inverse transform and a requantiser,
# and planning/audio.md 11.1 promises byte-identical output everywhere -
# which is a promise about *this library* that no reference comparison
# can check, because the references are floating point and are allowed
# to differ from themselves. These hashes are the only thing that checks
# it.
TARGETS = {
    "s390x": ("s390x-linux-gnu-gcc", "qemu-s390x", "s390x-linux-gnu",
              "64-bit big-endian"),
    "powerpc64": ("powerpc64-linux-gnu-gcc", "qemu-ppc64",
                  "powerpc64-linux-gnu", "64-bit big-endian, a second one"),
}


#: Fixtures this library identifies and does not decode, so there is no
#: decode to hash. **Named rather than filtered by extension**: an
#: exclusion that is a missing glob is the most invisible kind there is.
#:
#: It held one entry in phase 5 - MPEG-2.5 Layer III, refused because its
#: scalefactor band tables are in no standard - ten in phase 6, the
#: Vorbis fixtures, between identification landing and the decoder
#: landing, and fifteen after that, the Opus fixtures, for the same
#: reason. **It is empty now**, the third time, and emptying it is the
#: direction that means anything: the denominator went up rather than the
#: numerator. It is kept, empty, because an exclusion that is not written
#: down is indistinguishable from a format nobody thought to decode.
UNDECODABLE = set()

#: Every extension the corpus holds that this gate reasons about. A file
#: with an extension not in here is not excluded - it is invisible, which
#: is why the two sets are checked against each other below.
#:
#: **Read twice: once to decide what to hash here, and once to build the
#: glob the cross-architecture script walks.** Those were two lists until
#: the Vorbis fixtures arrived, and the second one did not have `.ogg` in
#: it - so ten fixtures were expected on the target, never decoded there,
#: and reported as "did not happen there". A missing glob again, ten
#: lines below the comment saying so.
EXTENSIONS = (".wav", ".aiff", ".aifc", ".flac", ".oga", ".ogg", ".opus",
              ".mp3", ".mp2", ".mp1")


def fixtures():
    data = os.path.join(ROOT, "tests", "data")
    present = sorted(f for f in os.listdir(data) if f.endswith(EXTENSIONS))
    stale = sorted(UNDECODABLE - set(present))
    if stale:
        raise SystemExit(
            "check-golden: these files are excluded as undecodable and are "
            "not in the corpus, so the exclusion is hiding nothing and the "
            "name has probably changed:\n  " + "\n  ".join(stale))
    unknown = sorted(f for f in os.listdir(data)
                     if not f.endswith(EXTENSIONS)
                     and not f.startswith(".")
                     and os.path.isfile(os.path.join(data, f)))
    if unknown:
        raise SystemExit(
            "check-golden: these corpus files have an extension this gate "
            "does not know, so they are neither hashed nor excluded - "
            "which is the invisible kind of hole:\n  "
            + "\n  ".join(unknown))
    return [f for f in present if f not in UNDECODABLE]


# Fixtures that are also ENCODED on the target, and the file hashed.
#
# **The writer's half of section 11.1's promise, and the reason the FLAC
# encoder has no floating point in it.** Decoding identically everywhere
# is one claim; producing identical bytes everywhere is another, and
# until phase 4 nothing tested it - WAV and AIFF writers copy samples
# around, so there was little to differ. A FLAC encoder makes choices:
# predictor orders, Rice parameters, partition orders, stereo modes. Made
# in integers those choices are the same on every machine; made in
# floating point they would not be, and the difference would be a file
# that decodes correctly and hashes differently, which no other gate here
# would see.
#
# A handful rather than all of them, because each one is a cross-compiled
# encode under qemu and the gate already takes minutes. One per shape
# that makes a different choice: mono and stereo (the decorrelation
# search), a wide depth (the 64-bit accumulation), and silence (the
# escaped-partition arm).
ENCODE_FIXTURES = [
    "wav_s16_stereo_44100.wav",
    "wav_s24_stereo_48000.wav",
    "aiff_s16_stereo_44100.aiff",
    "flac_lib_silence_44100.flac",
    "flac_lib_s32_stereo_96000.flac",
]


def host_hashes():
    """What this machine decodes, and what it encodes, per fixture."""
    out = {}
    for name in fixtures():
        path = os.path.join(ROOT, "tests", "data", name)
        finished = subprocess.run([PROBE, path, "--pcm-le"],
                                  capture_output=True)
        if finished.returncode != 0:
            raise SystemExit("dump_probe failed on %s" % name)
        out["decode " + name] = hashlib.sha256(finished.stdout).hexdigest()

    scratch = os.path.join(ROOT, "tests", "out")
    os.makedirs(scratch, exist_ok=True)
    for name in ENCODE_FIXTURES:
        source = os.path.join(ROOT, "tests", "data", name)
        target = os.path.join(scratch, "golden-host.flac")
        finished = subprocess.run([WRITE_PROBE, source, target, "flac"],
                                  capture_output=True)
        if finished.returncode != 0:
            raise SystemExit("write_probe failed on %s:\n%s"
                             % (name, finished.stderr.decode()[-600:]))
        with open(target, "rb") as handle:
            out["encode " + name] = hashlib.sha256(handle.read()).hexdigest()
        os.remove(target)
    return out


def script(compiler, qemu, triple):
    out = "/tmp/golden"
    return r"""
set -e
mkdir -p %(out)s
cd %(root)s
SRC="$(find src -name '*.c') %(md5)s %(shim)s"
%(cc)s -std=c17 -O2 -w -fno-strict-aliasing \
    -I include -I src -I build/linux/release/generated -I %(cutil)s \
    -I %(security)s \
    -o %(out)s/dump_probe $SRC tools/oracle/dump_probe.c -lm
%(cc)s -std=c17 -O2 -w -fno-strict-aliasing \
    -I include -I src -I build/linux/release/generated -I %(cutil)s \
    -I %(security)s \
    -o %(out)s/write_probe $SRC tools/oracle/write_probe.c -lm
for f in %(globs)s; do
    case " %(skip)s " in *" $(basename "$f") "*) continue;; esac
    printf 'decode %%s ' "$(basename "$f")"
    %(qemu)s -L /usr/%(triple)s %(out)s/dump_probe "$f" --pcm-le | sha256sum \
        | cut -d' ' -f1
done
for f in %(encode)s; do
    printf 'encode %%s ' "$(basename "$f")"
    %(qemu)s -L /usr/%(triple)s %(out)s/write_probe \
        "tests/data/$(basename "$f")" %(out)s/out.flac flac >/dev/null
    sha256sum %(out)s/out.flac | cut -d' ' -f1
done
""" % {"out": out, "root": ROOT, "cc": compiler, "qemu": qemu,
       "triple": triple, "shim": SHIM,
       "encode": " ".join(ENCODE_FIXTURES),
       "globs": " ".join("tests/data/*" + one for one in EXTENSIONS),
       "skip": " ".join(sorted(UNDECODABLE)),
       "md5": os.path.join(WORKSPACE, "libs", "security", "src", "md5",
                           "md5.c"),
       "cutil": os.path.join(WORKSPACE, ".local", "include", "ghoti.io",
                             "cutil-0"),
       "security": os.path.join(WORKSPACE, ".local", "include", "ghoti.io",
                                "security-0")}


def main():
    if not os.path.isfile(PROBE) or not os.path.isfile(WRITE_PROBE):
        raise SystemExit(
            "the oracle probes are not built. Run `make oracle-probe`.")
    names = ["decode " + name for name in fixtures()]
    names += ["encode " + name for name in ENCODE_FIXTURES]
    if not names:
        raise SystemExit("tests/data is empty, so this gate measures nothing.")

    host = host_hashes()
    print("host (%s): %d fixtures decoded, %d re-encoded"
          % (os.uname().machine, len(fixtures()), len(ENCODE_FIXTURES)))

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
            if len(parts) == 3:
                seen["%s %s" % (parts[0], parts[1])] = parts[2]
        if not seen:
            # A gate that compared nothing would report success.
            bad.append("%s: produced no hashes at all" % label)
            continue
        for name in names:
            if name not in seen:
                bad.append("%s: %s did not happen there" % (label, name))
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
    print("\n%d fixtures decode identically on %d big-endian target(s) and "
          "here, and %d re-encode to identical BYTES - which is section "
          "11.1's promise applied to the writer, and what the FLAC "
          "encoder's integer-only predictors are for."
          % (len(fixtures()), len(wanted), len(ENCODE_FIXTURES)))
    if UNDECODABLE:
        print("\n%d fixture(s) are identified and not decoded, so they are "
              "not in that number:" % len(UNDECODABLE))
        for name in sorted(UNDECODABLE):
            print("  %s" % name)
        print("  Named rather than left out of the extension list,"
              " because a hole that is a missing glob is one nobody can"
              " see.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
