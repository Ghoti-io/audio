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
import shutil
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
# Phase 4. FLAC carries signed integers of 8, 16, 24 or 32 bits and
# nothing else, so three of the corpus's formats have no spelling in it.
# Named rather than skipped, for the reason the two above are: a refusal
# that is not recorded is indistinguishable from a combination nobody
# tried.
for _container in ("flac", "ogg-flac"):
    CANNOT[("u8", _container)] = (
        "FLAC's 8-bit samples are signed; writing WAV's unsigned bytes "
        "would move every sample by half the scale")
    CANNOT[("f32", _container)] = "FLAC carries integers only"
    CANNOT[("f64", _container)] = "FLAC carries integers only"

# Phase 2: the coded writers, and planning/audio.md 11.3's two gates.
#
# A coded round trip cannot be exact, so the PCM assertion above does not
# apply and something has to replace it. 11.3 says what: **validity**, which
# is exact and binary - if every reference can decode our output at all,
# the bitstream is conformant - and **quality**, which is a metric with a
# stated floor.
#
# Waveform SNR is the right metric HERE and will not be in phase 8. These
# four codings are sample-by-sample quantisers with no phase behaviour and
# no psychoacoustic model, so an error measured against the input means
# what it appears to mean. For MP3, Vorbis and Opus it will not, which is
# why 11.3 pins ViSQOL for those rather than extending this.
#
# The floors are set from measurement with margin, not from theory. The
# numbers each coding actually achieves on this corpus are printed every
# run, so a floor drifting away from what the encoder does is visible
# rather than something to rediscover.
CODED_TARGETS = [
    ("wav", "ulaw", 33.0),
    ("wav", "alaw", 33.0),
    ("wav", "ima-wav", 36.0),
    ("wav", "ms-adpcm", 36.0),
    ("aiff", "ulaw", 33.0),
    ("aiff", "alaw", 33.0),
    ("aiff", "ima-qt", 36.0),
]

# Combinations that are codings and are not spellable in that container.
# Named so that the refusal is a recorded expectation: a writer that
# started emitting one of these would be producing files labelled with a
# framing they are not in, and nothing else here would notice.
# The three that carry per-channel predictor state in a block header.
ADPCM_CODINGS = {"ima-wav", "ima-qt", "ms-adpcm"}

CODED_CANNOT = {
    ("aiff", "ima-wav"): "WAV's IMA block framing has no AIFF-C type; "
                         "ima-qt is the spelling for this container",
    ("aiff", "ms-adpcm"): "Microsoft ADPCM has no AIFF-C compression type "
                          "at all; ffmpeg refuses the same combination",
    ("wav", "ima-qt"): "QuickTime's IMA packet framing has no WAVE format "
                       "tag; ima-wav is the spelling for this container",
}


def ms_coefficients(path):
    """The coefficient pairs in an MS ADPCM file's `fmt ` extension."""
    import struct
    data = open(path, "rb").read()
    i = 12
    while i + 8 <= len(data):
        cid = data[i:i + 4]
        size = struct.unpack_from("<I", data, i + 4)[0]
        if cid == b"fmt ":
            if struct.unpack_from("<H", data, i + 8)[0] != 0x0002 or size < 22:
                return None
            count = struct.unpack_from("<H", data, i + 28)[0]
            if size < 22 + 4 * count:
                return None
            return [(struct.unpack_from("<h", data, i + 30 + 4 * k)[0],
                     struct.unpack_from("<h", data, i + 32 + 4 * k)[0])
                    for k in range(count)]
        i += 8 + size + (size & 1)
    return None


def snr_db(want, got):
    """Signal-to-noise in dB over the frames the two share."""
    import struct
    n = min(len(want), len(got)) // 2
    if n == 0:
        return None
    a = struct.unpack("<%dh" % n, want[:n * 2])
    b = struct.unpack("<%dh" % n, got[:n * 2])
    error = sum((x - y) * (x - y) for x, y in zip(a, b))
    power = sum(x * x for x in a)
    if error == 0:
        return 999.0
    if power == 0:
        return None
    import math
    return 10.0 * math.log10(power / error)


def flac_verify(path, ogg):
    """`flac -t`: does the file's own MD5 match what libFLAC decodes?

    Returns a complaint, or None. **An all-zero signature is a failure
    here, not a pass.** RFC 9639 allows one and `flac -t` then prints a
    warning and exits zero, so a writer that stopped computing the digest
    would silently turn this gate off - which is exactly the shape of
    gate that reports success forever.
    """
    argv = ["flac", "-t", "--silent", "--warnings-as-errors"]
    if ogg:
        argv.append("--ogg")
    argv.append(path)
    finished = subprocess.run(oracle.command("flac", argv),
                              capture_output=True)
    if finished.returncode != 0:
        message = (finished.stderr.decode() or finished.stdout.decode())
        return "flac -t rejected it: %s" % " ".join(message.split())[:160]
    return None


def main():
    print(oracle.provenance(
        ["ffmpeg", "sox", "libsndfile", "pywave", "flac"]))
    if not os.path.isfile(WRITE_PROBE):
        raise SystemExit("write_probe is not built: %s" % WRITE_PROBE)
    files = sorted(f for f in os.listdir(DATA)
                   if f.endswith((".wav", ".aiff", ".aifc", ".flac", ".oga")))
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
    md5_checked = 0
    # One of the FLAC files we write, kept for the control below. It has
    # to be a file this encoder produced: the control exists to show that
    # the check would catch OUR digest going missing.
    md5_sample = None
    with tempfile.TemporaryDirectory(dir=scratch_root) as tmp:
        for name in files:
            source = os.path.join(DATA, name)
            meta = corpus.ours_meta(source)
            fmt = meta["format"]
            original = corpus.ours_pcm(source)

            for codec, ext in (("wav", "wav"), ("aiff", "aiff"),
                               ("flac", "flac"), ("ogg-flac", "oga")):
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
                complaints = corpus.check(out, quiet=True,
                                          as_name=name)
                back = corpus.ours_pcm(out)
                if back != original:
                    bad.append("%s -> %s: the round trip changed the samples"
                               % (name, codec))
                for line in complaints:
                    bad.append("%s -> %s: %s" % (name, codec,
                                                 line.split(": ", 1)[-1]))
                # **FLAC's own whole-decoder check, which no other format
                # here offers.** STREAMINFO carries an MD5 of the
                # unencoded audio; `flac -t` decodes the file and compares
                # the digest. That scores every subframe type, every
                # predictor order and every stereo mode at once against a
                # number computed by a different implementation - and it
                # is the thing that would catch an encoder whose output we
                # also decode wrongly, which the comparisons above cannot.
                verified = True
                if codec in ("flac", "ogg-flac"):
                    problem = flac_verify(out, codec == "ogg-flac")
                    if problem:
                        bad.append("%s -> %s: %s" % (name, codec, problem))
                        verified = False
                    else:
                        md5_checked += 1
                        if md5_sample is None and codec == "flac":
                            md5_sample = os.path.join(
                                scratch_root, "md5-sample.flac")
                            shutil.copyfile(out, md5_sample)
                pairs += 1
                print("  %-27s -> %-8s  %s"
                      % (name, codec, "OK" if not complaints
                         and back == original and verified else "FAIL"))

    # ---- the coded writers -------------------------------------------
    #
    # Only s16 sources are scored for quality, and the count is printed:
    # every coding decodes to s16, so comparing a u8 or f32 source against
    # the decode would be measuring the format conversion as well as the
    # coding, and attributing the sum to the coding. Validity is checked
    # for every source regardless, because that half does not care what
    # the samples were.
    coded_pairs = 0
    quality = {}
    quality_sources = 0
    wide = []
    with tempfile.TemporaryDirectory(dir=scratch_root) as tmp:
        for name in files:
            source = os.path.join(DATA, name)
            meta = corpus.ours_meta(source)
            if meta.get("coding", "pcm") != "pcm":
                # Re-coding an already-coded fixture would measure two
                # generations of loss and call it one.
                continue
            original = corpus.ours_pcm(source)
            scored_here = (meta["format"] == "s16"
                           and int(meta["channels"]) <= 2)
            if scored_here:
                quality_sources += 1

            channels = int(meta["channels"])
            for container, coding, floor in CODED_TARGETS:
                ext = "wav" if container == "wav" else "aiff"
                if coding in ADPCM_CODINGS and channels > 2:
                    # Refused on purpose, and the refusal is checked below
                    # rather than assumed: a writer that started emitting
                    # these would produce files ffmpeg cannot open.
                    wide.append("  %-27s -> %-4s %-9s refused: ADPCM is "
                                "mono/stereo; ffmpeg refuses %d channels "
                                "both ways" % (name, container, coding,
                                               channels))
                    probe = subprocess.run(
                        [WRITE_PROBE, source,
                         os.path.join(tmp, "wide.%s" % ext), container,
                         coding], capture_output=True)
                    if probe.returncode == 0:
                        bad.append("%s -> %s/%s: %d channels was written "
                                   "rather than refused"
                                   % (name, container, coding, channels))
                    continue
                out = os.path.join(tmp, "%s.%s.%s" % (name, coding, ext))
                finished = subprocess.run(
                    [WRITE_PROBE, source, out, container, coding],
                    capture_output=True)
                if finished.returncode != 0:
                    bad.append("%s -> %s/%s: write_probe failed: %s"
                               % (name, container, coding,
                                  finished.stderr.decode().strip()[:120]))
                    continue

                # Gate one, exact: every reference that can read this
                # coding must read it, and must agree with us about the
                # samples. corpus.check() carries the exclusions, so a
                # reference that cannot read the coding at all is not
                # counted as a failure here either.
                complaints = corpus.check(out, quiet=True,
                                          as_name=name)
                for line in complaints:
                    bad.append("%s -> %s/%s: %s"
                               % (name, container, coding,
                                  line.split(": ", 1)[-1]))

                # Gate two, by metric with a stated floor.
                verdict = "OK"
                if scored_here:
                    back = corpus.ours_pcm(out)
                    value = snr_db(original, back)
                    if value is None:
                        bad.append("%s -> %s/%s: nothing decoded back"
                                   % (name, container, coding))
                    else:
                        quality.setdefault(coding, []).append(value)
                        if value < floor:
                            bad.append(
                                "%s -> %s/%s: %.1f dB, below the %.1f dB "
                                "floor this coding's bit rate allows"
                                % (name, container, coding, value, floor))
                            verdict = "FAIL %.1f dB" % value
                        else:
                            verdict = "%.1f dB" % value
                coded_pairs += 1
                if complaints:
                    verdict = "FAIL"
                print("  %-27s -> %-4s %-9s %s"
                      % (name, container, coding, verdict))

            for (container, coding), why in sorted(CODED_CANNOT.items()):
                ext = "wav" if container == "wav" else "aiff"
                out = os.path.join(tmp, "no.%s.%s" % (coding, ext))
                finished = subprocess.run(
                    [WRITE_PROBE, source, out, container, coding],
                    capture_output=True)
                if finished.returncode == 0:
                    bad.append("%s -> %s/%s: written, and it should have "
                               "been refused: %s"
                               % (name, container, coding, why))

    # The coefficient table we WRITE, against the one ffmpeg writes.
    #
    # Nothing else can see this. A file's own `fmt ` extension carries the
    # table, so our decoder reads the file's and never consults the
    # built-in one; and our encoder only ever names pair 0, so pairs 1 to
    # 6 are bytes we emit and never use. A wrong value in them would
    # travel in every MS ADPCM file this library writes and be found by
    # whoever eventually decoded a block that named it. A byte comparison
    # against a second writer is the whole check, and it is exact.
    ms_ours = None
    ms_theirs = None
    with tempfile.TemporaryDirectory(dir=scratch_root) as tmp:
        seed = None
        for name in files:
            if name.endswith(".wav") and "s16" in name and "5dot1" not in name:
                seed = os.path.join(DATA, name)
                break
        if seed:
            mine = os.path.join(tmp, "coef_ours.wav")
            if subprocess.run([WRITE_PROBE, seed, mine, "wav", "ms-adpcm"],
                              capture_output=True).returncode == 0:
                ms_ours = ms_coefficients(mine)
        for name in files:
            if "msadpcm" in name and "coefs" not in name:
                ms_theirs = ms_coefficients(os.path.join(DATA, name))
                break
    if ms_ours is None or ms_theirs is None:
        bad.append("could not compare the MS ADPCM coefficient table "
                   "against ffmpeg's; that table is write-only here and "
                   "this is the only thing that checks it")
    elif ms_ours != ms_theirs:
        bad.append("the MS ADPCM coefficient table we write differs from "
                   "ffmpeg's: ours %s, ffmpeg %s" % (ms_ours, ms_theirs))
    else:
        print("\nMS ADPCM coefficient table: %d pairs, byte-identical to "
              "ffmpeg's. (Nothing else checks these: our decoder uses the "
              "table in the file and our encoder only names pair 0.)"
              % len(ms_ours))

    if quality:
        print("\nQuality, over %d s16 source%s, against each coding's floor:"
              % (quality_sources, "" if quality_sources == 1 else "s"))
        for container, coding, floor in CODED_TARGETS:
            values = quality.get(coding)
            if not values:
                continue
            print("  %-9s %5.1f - %5.1f dB  (floor %.1f, margin %.1f)"
                  % (coding, min(values), max(values), floor,
                     min(values) - floor))
            quality.pop(coding, None)

    if wide:
        print("\nRefused by design, because the coding is mono/stereo:")
        for line in wide:
            print(line)

    if CODED_CANNOT:
        print("\nRefused by design, because the container cannot spell it:")
        for (container, coding), why in sorted(CODED_CANNOT.items()):
            print("  %-4s / %-9s %s" % (container, coding, why))

    if refused:
        print("\nRefused by design, and why:")
        for line in refused:
            print(line)

    # The figure, with the instrument beside it. A count collected and not
    # printed is one nobody can tell went to zero.
    print("\n%d FLAC file(s) additionally verified by `flac -t "
          "--warnings-as-errors`, which checks the STREAMINFO MD5 we wrote "
          "against libFLAC's own decode." % md5_checked)
    if md5_checked == 0:
        bad.append("no FLAC file reached `flac -t`, so that half of this "
                   "gate measured nothing")
    else:
        # **The control, and it is not optional.** Without
        # --warnings-as-errors, `flac -t` prints "cannot check MD5
        # signature since it was unset" and exits ZERO - so a writer that
        # stopped computing the digest would turn this check off and the
        # gate would keep reporting a pass. The control proves the flag is
        # doing the work, by handing the reference a file whose digest has
        # been zeroed and requiring a refusal.
        probe = os.path.join(scratch_root, "md5-control.flac")
        shutil.copyfile(md5_sample, probe)
        with open(probe, "r+b") as handle:
            # STREAMINFO's body starts at 8 and its last 16 bytes are the
            # digest: four magic bytes, four of block header, eighteen of
            # fields.
            handle.seek(8 + 18)
            handle.write(bytes(16))
        if flac_verify(probe, False) is None:
            bad.append("CONTROL FAILED: `flac -t` accepted a FLAC whose "
                       "MD5 signature was zeroed, so this check is not "
                       "measuring the digest")
        else:
            print("  control: a zeroed MD5 signature is rejected")
        os.remove(probe)
        os.remove(md5_sample)

    print("\n%d lossless round trips and %d coded ones through this "
          "library's writer, each read back by every reference that can."
          % (pairs, coded_pairs))
    if bad:
        print("\n%d disagreement(s):" % len(bad), file=sys.stderr)
        for line in bad:
            print("  " + line, file=sys.stderr)
        return 1
    print("Every reference reads what we wrote, and the samples survived.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
