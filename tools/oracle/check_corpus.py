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
"""Score this library's reading of tests/data/ against the pinned references.

    make check-corpus

**PCM is the exact class**, so this compares bytes and not a tolerance. Every
fixture is lossless: whatever ffmpeg, sox and libsndfile decode, this library
must decode identically, and any difference is a defect in one of us with no
tolerance argument available. planning/audio.md 12 puts PCM in that class for
exactly this reason - it is the one place where the oracle cannot be argued
with, which is why the decoder interface is proved here before meeting a
codec where it can.

### Why a byte comparison is not the whole gate

A decoder that emits silence produces the right number of bytes and no
crash. One that is 6 dB down differs from the reference by a small amount
everywhere. One that is a frame short passes any comparison that aligns two
signals before measuring. So the byte comparison is joined by, and reported
beside:

  frames    asserted exactly, before anything is compared
  peak      the ABSOLUTE level of our output, so a uniformly quiet decode
            fails even though its difference from the reference is small
  dc        the mean, which catches a sign or bias error that RMS forgives
  channels  measured separately, so a swapped or silent channel cannot hide
            behind the others' energy

### The control

`--self-check` corrupts a fixture's samples and requires the gate to fail on
it. A gate never seen to fail is not known to work, and this one would report
success for every file in the corpus if the probe silently produced nothing.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import oracle_env as oracle  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(ROOT, "tests", "data")
PROBE = os.environ.get(
    "GAUD_DUMP_PROBE",
    os.path.join(ROOT, "build", "linux", "release", "apps", "oracle",
                 "dump_probe"))

HOST_LE = sys.byteorder == "little"

# Our decoded samples are in HOST order, so the reference is asked for the
# host's order too. Spelled from sys.byteorder rather than assumed, because
# the whole reason AIFF is in this corpus is that byte order is a thing this
# library gets wrong if nobody looks.
RAW = {
    "u8":  ("u8",   "unsigned-integer", 1, 1),
    "s8":  ("s8",   "signed-integer",   1, 1),
    "s16": ("s16" + ("le" if HOST_LE else "be"), "signed-integer", 2, 2),
    "s24": ("s24" + ("le" if HOST_LE else "be"), "signed-integer", 3, 3),
    "s32": ("s32" + ("le" if HOST_LE else "be"), "signed-integer", 4, 4),
    "f32": ("f32" + ("le" if HOST_LE else "be"), "floating-point", 4, 4),
    "f64": ("f64" + ("le" if HOST_LE else "be"), "floating-point", 8, 8),
}


# Where a reference cannot answer, why, and what was measured to establish
# it. **Named here rather than skipped quietly**: an exclusion that is not
# written down is indistinguishable from a reference nobody thought to ask,
# and a score whose denominator shrank without saying so is inflated. Every
# file stays in the corpus and is still scored exactly by whoever can read
# it; only the one reference is set aside, for the one format.
EXCLUSIONS = {
    ("sox", "f32"): (
        "SoX routes samples through its own 32-bit fixed-point "
        "representation, so a float round trip loses the last bits. "
        "Measured on wav_f32_stereo_48000: max |ours-sox| = 3.0e-08, "
        "ratio within 1.2e-06 of 1.0, while ffmpeg agrees with us byte for "
        "byte. This is a property of SoX, not a disagreement about the file."),
    ("sox", "f64"): (
        "As for f32: SoX's internal representation is narrower than the "
        "samples, so it cannot be an exact reference for them."),
    ("pywave", "f32"): (
        "Python's `wave` reads WAVE_FORMAT_PCM only and raises on "
        "WAVE_FORMAT_IEEE_FLOAT. It is a reference for integer WAV."),
    ("pywave", "f64"): (
        "As for f32: `wave` has no float support at all."),

    # Phase 2. These are keyed by (reference, container, coding) rather
    # than by sample format, because the sample format of every coded
    # fixture is s16 and the thing a reference cannot do is not about the
    # samples. sox reads mu-law in WAV perfectly well and cannot read the
    # same coding in AIFF-C, so a per-format key would have excluded it
    # from both or neither, and either answer is wrong.
    ("sox", "aiff", "ulaw"): (
        "sox refuses every AIFF-C compression type: `sox FAIL formats: "
        "can't open input file ...: Unsupported AIFC compression type "
        "`ulaw''. Measured on aifc_ulaw_mono_8000. It reads the same "
        "coding in WAV, where it is still scored."),
    ("sox", "aiff", "alaw"): (
        "As for ulaw: `Unsupported AIFC compression type `alaw''."),
    ("sox", "aiff", "ima-qt"): (
        "As for ulaw: `Unsupported AIFC compression type `ima4''. sox has "
        "no reader for QuickTime's IMA packets in any container."),

    ("pywave", "wav", "ulaw"): (
        "Python's `wave` reads WAVE_FORMAT_PCM only and raises "
        "wave.Error on every other tag. It is a reference for "
        "uncompressed integer WAV and for nothing else."),
    ("pywave", "wav", "alaw"): (
        "As for ulaw: `wave` refuses WAVE_FORMAT_ALAW."),
    ("pywave", "wav", "ima-wav"): (
        "As for ulaw: `wave` refuses WAVE_FORMAT_IMA_ADPCM."),
    ("pywave", "wav", "ms-adpcm"): (
        "As for ulaw: `wave` refuses WAVE_FORMAT_ADPCM."),

    # The two IMA exclusions are the only ones here where the reference
    # can read the file perfectly well and simply decodes it to different
    # numbers. Both were reproduced exactly before being written down: a
    # model of ffmpeg's arithmetic matches ffmpeg on every sample of the
    # corpus, which turns "ffmpeg disagrees" into a statement about which
    # of two documented algorithms each side implements. We match
    # libsndfile byte for byte in both cases, on all four fixtures.
    ("ffmpeg", "wav", "ima-wav"): (
        "ffmpeg expands an IMA nibble by direct multiplication, "
        "diff = ((2*delta+1)*step) >> 3; this library and libsndfile use "
        "the additive form the IMA reference states, "
        "diff = step>>3 (+step>>2, +step>>1, +step by bit). ffmpeg's own "
        "source calls the multiplication a substitute for `the series of "
        "jumps proposed by the reference ADPCM implementation`. Measured "
        "on wav_imaadpcm_mono_22050: ours vs ffmpeg 1996/2003 samples "
        "differ, max 40 of 32768 (0.12%); ours vs libsndfile 0/2003. "
        "Re-decoding the same blocks with the multiplicative form "
        "reproduces ffmpeg on 2041/2041 samples, which is what "
        "establishes the mechanism."),
    # Keyed by FIXTURE, not by coding: sox and libsndfile agree with us on
    # every MS ADPCM file an encoder actually produces, and disagree only
    # on the synthetic one that names coefficient pairs no encoder picks.
    # Excluding them from "ms-adpcm" would throw away four correct
    # comparisons to accommodate one.
    ("sox", "wav_msadpcm_coefs_8000.wav"): (
        "MS ADPCM computes its prediction as "
        "(sample1*coef1 + sample2*coef2) / 256, and C's division truncates "
        "toward zero where an arithmetic shift floors. The two differ only "
        "for a negative numerator with a remainder - and coefficient pairs "
        "0, 1 and 2 ({256,0} {512,-256} {0,0}) all give exact multiples of "
        "256, so no file an encoder produces can tell them apart. This "
        "fixture names pairs 3 to 6 deliberately and does. Measured: "
        "truncation reproduces ffmpeg on 16288/16288 samples and the shift "
        "reproduces libsndfile on 16288/16288. This library truncates, "
        "with ffmpeg; sox and libsndfile shift. Two against two, and "
        "Microsoft's own description of the format is a division."),
    ("libsndfile", "wav_msadpcm_coefs_8000.wav"): (
        "As for sox, which it agrees with exactly: an arithmetic shift "
        "where this library and ffmpeg truncate toward zero."),

    ("ffmpeg", "aiff", "ima-qt"): (
        "A different mechanism from the WAV case, and a more consequential "
        "one: ffmpeg CARRIES the predictor across `ima4` packets instead "
        "of reloading it from each packet header. Apple's 34-byte packet "
        "begins with a predictor and step index precisely so that packets "
        "are independent, and this library reloads both - which is what "
        "makes gaud_decoder_seek() able to land in the middle of a file "
        "without decoding what precedes it. Measured on "
        "aifc_ima4_mono_22050: identical for the first packet and "
        "divergent from sample 64 onward, 960/1024 samples differing, max "
        "117 of 32768 (0.36%); ours vs libsndfile 0/1024. Re-decoding "
        "with the predictor carried across packets reproduces ffmpeg on "
        "1024/1024 samples."),
}

# How far a reference is allowed to run past us, and why.
#
# A coded WAV's final block is padded to its full length, and `fact`
# carries the true frame count. This library honours `fact`; **no
# reference in the image does** - ffmpeg, sox and libsndfile all decode
# the padding, and patching `fact` to any value changes none of their
# output (notes/audio/phase2-calibration.md). AIFF-C has no equivalent
# field at all, so an `ima4` file is always a whole number of 64-frame
# packets and everyone agrees on the padded length.
#
# So the comparison is: the frames we both have must agree EXACTLY, and
# the excess must be less than one block. Both halves matter. Dropping
# the first would stop scoring the samples; dropping the second would let
# a decoder that stopped early pass by calling the missing frames
# padding.
# Frames in one block, by (coding, channels-independent formula). Only
# used to bound the padding, so an upper bound is enough and the exact
# geometry does not have to be re-derived here.
MAX_BLOCK_FRAMES = {
    "ulaw": 1,
    "alaw": 1,
    "ima-wav": 4096,
    "ima-qt": 64,
    "ms-adpcm": 4096,
}


def run(argv, **kw):
    return subprocess.run(argv, capture_output=True, **kw)


def ours_meta(path):
    finished = run([PROBE, path])
    if finished.returncode != 0:
        raise SystemExit("dump_probe failed on %s:\n%s"
                         % (path, finished.stderr.decode()[-800:]))
    fields = {}
    for pair in finished.stdout.decode().strip().split():
        key, _, value = pair.partition("=")
        fields[key] = value
    return fields


def ours_pcm(path):
    finished = run([PROBE, path, "--pcm"])
    if finished.returncode != 0:
        raise SystemExit("dump_probe --pcm failed on %s" % path)
    return finished.stdout


def ffmpeg_pcm(path, fmt):
    raw = RAW[fmt][0]
    return run(oracle.command("ffmpeg", [
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-i", path,
        "-f", raw, "-c:a", "pcm_" + raw, "-"])).stdout


def sox_pcm(path, fmt, channels, rate):
    _, encoding, width, _ = RAW[fmt]
    argv = ["sox", path, "-t", "raw", "-e", encoding, "-b", str(width * 8)]
    # sox writes raw in the host's order by default; -L/-B state it anyway so
    # the answer does not depend on where this runs.
    argv += ["-L" if HOST_LE else "-B", "-"]
    return run(oracle.command("sox", argv)).stdout


def libsndfile_pcm(path, fmt):
    # soundfile reads into numpy, whose dtype spelling carries the order.
    dtype = {"u8": "int16", "s8": "int16", "s16": "int16", "s24": "int32",
             "s32": "int32", "f32": "float32", "f64": "float64"}[fmt]
    script = (
        "import sys,soundfile as sf,numpy as np;"
        "d,_=sf.read(sys.argv[1],dtype=%r,always_2d=True);"
        "sys.stdout.buffer.write(d.tobytes())" % dtype)
    return run(oracle.command("libsndfile",
                              ["python3", "-c", script, path])).stdout


def pywave_pcm(path):
    script = (
        "import sys,wave;"
        "w=wave.open(sys.argv[1],'rb');"
        "sys.stdout.buffer.write(w.readframes(w.getnframes()))")
    return run(oracle.command("pywave",
                              ["python3", "-c", script, path])).stdout


def measure(data, fmt, channels):
    """Peak, RMS and DC per channel, as fractions of full scale."""
    import struct
    scale = {"u8": 128.0, "s8": 128.0, "s16": 32768.0, "s24": 8388608.0,
             "s32": 2147483648.0, "f32": 1.0, "f64": 1.0}[fmt]
    width = RAW[fmt][2]
    n = len(data) // width
    vals = [[] for _ in range(channels)]
    for i in range(n):
        raw = data[i * width:(i + 1) * width]
        if fmt == "u8":
            v = (raw[0] - 128) / scale
        elif fmt == "s8":
            v = struct.unpack("b", raw)[0] / scale
        elif fmt == "s24":
            b = raw if HOST_LE else raw[::-1]
            u = b[0] | (b[1] << 8) | (b[2] << 16)
            if u & 0x800000:
                u -= 1 << 24
            v = u / scale
        elif fmt in ("f32", "f64"):
            v = struct.unpack(("<" if HOST_LE else ">")
                              + ("f" if fmt == "f32" else "d"), raw)[0]
        else:
            code = {"s16": "h", "s32": "i"}[fmt]
            v = struct.unpack(("<" if HOST_LE else ">") + code, raw)[0] / scale
        vals[i % channels].append(v)
    out = []
    for ch in vals:
        if not ch:
            out.append((0.0, 0.0, 0.0))
            continue
        peak = max(abs(x) for x in ch)
        rms = (sum(x * x for x in ch) / len(ch)) ** 0.5
        dc = sum(ch) / len(ch)
        out.append((peak, rms, dc))
    return out


def scaled_pcm(data, factor):
    """Every sample multiplied by `factor`. Only used by the control, to
    manufacture the uniformly-quiet decode that a difference threshold
    scaled to the signal would forgive."""
    import struct
    out = bytearray(len(data))
    for i in range(0, len(data) - 1, 2):
        v = struct.unpack_from("<h", data, i)[0]
        struct.pack_into("<h", out, i, int(v * factor))
    return bytes(out)


# The 89-entry IMA step table, mirrored here so that the coverage check
# below is independent of the C. A copy that drifted would make the check
# report on the wrong table, so `make check-corpus` compares the two
# through the fixtures themselves: if this list were wrong, the decoded
# samples would disagree with the references and the differential above
# would fail first.
IMA_STEP_COUNT = 89
IMA_INDEX_ADJUST = [-1, -1, -1, -1, 2, 4, 6, 8] * 2


def ima_observable_indices(path):
    """Which step-table entries this fixture loads with a nonzero nibble.

    **Loading an entry is not testing it.** At nibble magnitude 0 the
    decoder computes only `step >> 3`, so most of a wrong step value is
    invisible; the entry has to be loaded with a magnitude that reads the
    other shifted terms before a differential can see it. Counting loads
    rather than observable loads was this corpus's own mistake: it
    reported 89 of 89 entries reached while a deliberately wrong entry
    still decoded identically, because every one of its loads was at
    magnitude 0.
    """
    import struct as _s
    data = open(path, "rb").read()
    pairs = set()
    if data[:4] == b"RIFF":
        i, body, block, channels = 12, None, None, None
        while i + 8 <= len(data):
            cid = data[i:i + 4]
            size = _s.unpack_from("<I", data, i + 4)[0]
            if cid == b"fmt ":
                channels = _s.unpack_from("<H", data, i + 10)[0]
                block = _s.unpack_from("<H", data, i + 20)[0]
            elif cid == b"data":
                body = data[i + 8:i + 8 + size]
            i += 8 + size + (size & 1)
        if body is None or not block or not channels:
            return pairs
        for start in range(0, len(body), block):
            chunk = body[start:start + block]
            if len(chunk) < 4 * channels:
                break
            index = [min(88, chunk[4 * c + 2]) for c in range(channels)]
            at = 4 * channels
            while at + 4 * channels <= len(chunk):
                for c in range(channels):
                    for k in range(4):
                        byte = chunk[at + 4 * c + k]
                        for nibble in (byte & 0x0F, byte >> 4):
                            pairs.add((index[c], nibble & 7))
                            index[c] = max(0, min(88,
                                index[c] + IMA_INDEX_ADJUST[nibble]))
                at += 4 * channels
    else:
        i, body = 12, None
        while i + 8 <= len(data):
            cid = data[i:i + 4]
            size = _s.unpack_from(">I", data, i + 4)[0]
            if cid == b"SSND":
                body = data[i + 16:i + 8 + size]
            i += 8 + size + (size & 1)
        if body is None:
            return pairs
        for packet in range(0, len(body) // 34 * 34, 34):
            word = _s.unpack_from(">H", body, packet)[0]
            index = min(88, word & 0x7F)
            for n in range(32):
                byte = body[packet + 2 + n]
                for nibble in (byte & 0x0F, byte >> 4):
                    pairs.add((index, nibble & 7))
                    index = max(0, min(88,
                        index + IMA_INDEX_ADJUST[nibble]))
    return set(i for (i, magnitude) in pairs if magnitude != 0)


# Fixtures that are quiet ON PURPOSE, with the level each one actually
# decodes to.
#
# The generic floor below ("silent or far down") exists to catch a decoder
# returning nothing, and it cannot tell that from a fixture written at
# -60 dBFS to drive the bottom of the IMA step table. Exempting them would
# lose the check exactly where the samples are smallest and a scaling
# error is easiest to make, so instead each one states its measured peak
# and RMS and is held to a BAND around them. That is stricter than the
# floor, not looser: a decode 6 dB down fails here and would have passed
# the floor for every loud fixture in the corpus.
QUIET_FIXTURES = {
    "aifc_ima4_quietnoise_8000.aifc": (0.00162, 0.000645),
    "wav_imaadpcm_lownoise_8000.wav": (0.01099, 0.005921),
    "wav_imaadpcm_lsbnoise_8000.wav": (0.00052, 0.000191),
    "wav_imaadpcm_midnoise_8000.wav": (0.05371, 0.028858),
    "wav_imaadpcm_quietnoise_8000.wav": (0.00262, 0.001459),
}
# +/- 30%: wide enough that a regenerated fixture does not trip it,
# narrow enough that the nearest interesting error (6 dB, a factor of two)
# is outside it in both directions.
QUIET_BAND = 0.30


def classify_excess(coding, excess_frames):
    """Is a reference reading `excess_frames` more than us allowed?

    Returns a complaint, or None when the difference is padding past the
    `fact` chunk. Pulled out of compare() so that the self-check can drive
    it directly: every way of reaching this through a real fixture is
    caught first by the frame-count check, so a control that goes through
    compare() proves the frame-count check works and says nothing about
    this. A branch that can only ACCEPT a difference has to be shown to
    reject one somehow, and this is the only honest way to show it.
    """
    limit = MAX_BLOCK_FRAMES.get(coding, 0)
    if coding == "pcm":
        return ("decoded %d frames more than we did, and PCM has no padding "
                "to explain it" % excess_frames)
    if excess_frames < 0:
        return ("decoded %d frames FEWER than we did; we are producing "
                "frames the file does not hold" % -excess_frames)
    if excess_frames >= limit:
        return ("decoded %d frames more than we did, which is a whole block "
                "or more (%s pads at most %d) - that is a decoder stopping "
                "early, not padding" % (excess_frames, coding, limit))
    return None


def compare(path, ours, quiet=False, excluded=None, scored=None,
            pad_notes=None, as_name=None):
    """Score one reading of one fixture. Returns a list of complaints.

    `ours` is passed in rather than read here so that the control can hand
    over a deliberately wrong reading and require this to object.
    """
    bad = []
    if excluded is None:
        excluded = set()
    if scored is None:
        scored = []
    if pad_notes is None:
        pad_notes = []
    name = os.path.basename(path)
    # A round trip's output is a temporary file, and the level band that
    # belongs to the fixture it came from has to follow it there or the
    # quiet fixtures fail the generic floor every time they are rewritten.
    level_name = as_name or name
    meta = ours_meta(path)
    fmt = meta["format"]
    coding = meta.get("coding", "pcm")
    container = "wav" if name.endswith(".wav") else "aiff"
    channels = int(meta["channels"])
    frames = int(meta["frames"])
    rate = int(meta["rate"])

    width = RAW[fmt][2]
    expect_bytes = frames * channels * width
    if len(ours) != expect_bytes:
        bad.append("%s: decoded %d bytes, the header says %d frames means %d"
                   % (name, len(ours), frames, expect_bytes))

    wanted = ["ffmpeg", "sox"]
    # libsndfile widens u8/s8 to int16 and s24 to int32, so for those its
    # bytes are a different width and cannot be compared directly.
    if fmt in ("s16", "s32", "f32", "f64"):
        wanted.append("libsndfile")
    if name.endswith(".wav"):
        wanted.append("pywave")

    getters = {
        "ffmpeg": lambda: ffmpeg_pcm(path, fmt),
        "sox": lambda: sox_pcm(path, fmt, channels, rate),
        "libsndfile": lambda: libsndfile_pcm(path, fmt),
        "pywave": lambda: pywave_pcm(path),
    }
    refs = {}
    for ref in wanted:
        key = None
        if (ref, name) in EXCLUSIONS:
            key = (ref, name)
        elif (ref, container, coding) in EXCLUSIONS:
            key = (ref, container, coding)
        elif (ref, fmt) in EXCLUSIONS:
            key = (ref, fmt)
        if key:
            excluded.add(key)
            continue
        refs[ref] = getters[ref]()
    scored.append((name, sorted(refs)))

    if not refs:
        bad.append("%s: no reference could read this, so nothing scored it"
                   % name)

    for ref, data in refs.items():
        if not data:
            bad.append("%s: %s produced nothing" % (name, ref))
            continue
        if ref == "pywave" and not HOST_LE:
            # `wave` hands back the file's bytes untouched, and WAV is
            # little-endian; on a big-endian host that is not the host order
            # everything else here is in.
            continue
        common = min(len(data), len(ours))
        first = next((i for i in range(common) if data[i] != ours[i]), None)
        if first is not None:
            bad.append("%s: %s disagrees (%d vs %d bytes, first at %d)"
                       % (name, ref, len(data), len(ours), first))
            continue
        if len(data) == len(ours):
            continue
        # The prefix is identical and the lengths are not. For a coded
        # WAV that is the padded final block: `fact` told us where the
        # signal stops and told the reference nothing it acted on. It is
        # allowed, bounded, and only in that direction - a reference
        # SHORTER than us means we invented frames.
        excess_frames = (len(data) - len(ours)) // (channels * width)
        complaint = classify_excess(coding, excess_frames)
        if complaint:
            bad.append("%s: %s %s" % (name, ref, complaint))
        else:
            pad_notes.append("%s: %s read %d padding frame%s past the "
                             "fact chunk's count (identical over the %d "
                             "frames we share)"
                             % (name, ref, excess_frames,
                                "" if excess_frames == 1 else "s",
                                len(ours) // (channels * width)))

    # Absolute level, per channel. A decoder returning silence, or uniformly
    # down, agrees with nothing here even when its difference from a
    # reference looks small - which is the case a threshold scaled to the
    # signal forgives.
    stats = measure(ours, fmt, channels)
    band = QUIET_FIXTURES.get(level_name)
    for index, (peak, rms, dc) in enumerate(stats):
        if band:
            want_peak, want_rms = band
            for label, got, want in (("peak", peak, want_peak),
                                     ("RMS", rms, want_rms)):
                if not (want * (1.0 - QUIET_BAND) <= got
                        <= want * (1.0 + QUIET_BAND)):
                    bad.append("%s: channel %d %s %.6f is outside the "
                               "%.0f%% band around the %.6f this quiet "
                               "fixture decodes to"
                               % (name, index, label, got,
                                  QUIET_BAND * 100, want))
        else:
            if peak < 0.05:
                bad.append("%s: channel %d peaks at %.4f - silent or far "
                           "down" % (name, index, peak))
            if rms < 0.01:
                bad.append("%s: channel %d RMS %.5f - silent or far down"
                           % (name, index, rms))
        if abs(dc) > 0.05:
            bad.append("%s: channel %d DC offset %.4f - sign or bias error"
                       % (name, index, dc))

    if not quiet:
        levels = " ".join("ch%d peak=%.3f rms=%.3f dc=%+.4f" % (i, p, r, d)
                          for i, (p, r, d) in enumerate(stats))
        print("  %-27s %-4s %uch %6u %5u frames  %-4s  %s"
              % (name, fmt, channels, rate, frames,
                 "OK" if not bad else "FAIL", levels))
    return bad


def check(path, quiet=False, excluded=None, scored=None, pad_notes=None,
          as_name=None):
    return compare(path, ours_pcm(path), quiet, excluded, scored, pad_notes,
                   as_name)


def main():
    args = sys.argv[1:]
    self_check = "--self-check" in args
    print(oracle.provenance(["ffmpeg", "sox", "libsndfile", "pywave"]))

    if not os.path.isfile(PROBE):
        raise SystemExit(
            "dump_probe is not built: %s\nRun `make oracle-probe`." % PROBE)
    files = sorted(f for f in os.listdir(DATA)
                   if f.endswith((".wav", ".aiff", ".aifc")))
    if not files:
        raise SystemExit("tests/data is empty, so this gate is measuring "
                         "nothing. Run `make corpus`.")

    bad = []
    excluded = set()
    scored = []
    pad_notes = []
    for f in files:
        bad += check(os.path.join(DATA, f), excluded=excluded, scored=scored,
                     pad_notes=pad_notes)

    if self_check:
        # The control, and the first draft of it was wrong in a way worth
        # recording: it corrupted the FIXTURE, which corrupts it for the
        # references too, so they agreed with us about the damaged file and
        # the gate passed. A differential cannot see a bad input; what it
        # sees is a bad *reader*. So the control mutates our own answer
        # instead, which is what a defect in this library would look like.
        print("\n  control: this gate must reject a wrong reading")
        victim = os.path.join(DATA, "wav_s16_stereo_44100.wav")
        truth = ours_pcm(victim)
        cases = [
            ("one sample changed",
             truth[:200] + bytes([truth[200] ^ 0x01]) + truth[201:]),
            ("one frame short", truth[:-4]),
            # The two a byte comparison alone would catch but that the level
            # checks exist for: silence, and a uniformly quiet decode.
            ("all silence", bytes(len(truth))),
            ("6 dB down", scaled_pcm(truth, 0.5)),
        ]
        for label, wrong in cases:
            complaints = compare(victim, wrong, quiet=True)
            if not complaints:
                bad.append("CONTROL FAILED (%s): this gate accepted a wrong "
                           "reading, so it is not measuring anything" % label)
                continue
            print("    %-20s rejected: %s"
                  % (label, complaints[0].split(": ", 1)[-1][:66]))

        # The level checks need their own control. Against a lossless format
        # the byte comparison catches everything first, so "silence was
        # rejected" does not show that measuring the level works - and the
        # level checks are the half that will carry the gate from phase 5,
        # where no reference is exact and the comparison becomes a tolerance.
        # So: require that silence trips a LEVEL complaint specifically.
        # The padding branch is the one that ACCEPTS a difference, so it
        # needs a control of its own or it is a hole shaped like a
        # feature. Two ways to abuse it: stop a whole block early and
        # call the gap padding, or return more frames than the file
        # holds. Run against a coded fixture, because padding is only
        # allowed there.
        for label, coding, excess, want_reject in [
            ("padding, one frame", "ms-adpcm", 1, False),
            ("padding, near a block", "ms-adpcm", 499, False),
            ("a whole block over", "ms-adpcm", 4096, True),
            ("reference is shorter", "ms-adpcm", -1, True),
            ("any excess on PCM", "pcm", 1, True),
            ("ima-qt, 63 frames", "ima-qt", 63, False),
            ("ima-qt, 64 frames", "ima-qt", 64, True),
        ]:
            got = classify_excess(coding, excess)
            if want_reject and got is None:
                bad.append("CONTROL FAILED (%s): the padding allowance "
                           "accepted %d excess frames of %s, so it is a "
                           "hole rather than a bounded difference"
                           % (label, excess, coding))
            elif not want_reject and got is not None:
                bad.append("CONTROL FAILED (%s): the padding allowance "
                           "rejected %d excess frames of %s, which is "
                           "padding and must be allowed"
                           % (label, excess, coding))
            else:
                print("    %-22s %s"
                      % (label, "rejected" if want_reject else "allowed"))

        silent = compare(victim, bytes(len(truth)), quiet=True)
        level_said = [c for c in silent
                      if "peaks at" in c or "RMS" in c or "DC offset" in c]
        if not level_said:
            bad.append("CONTROL FAILED: silence did not trip the level "
                       "checks, so peak/RMS/DC are not measuring anything")
        else:
            print("    %-20s rejected: %s"
                  % ("(level check)", level_said[0].split(": ", 1)[-1][:66]))

    # Step-table coverage, reported every run rather than measured once.
    # A fixture regenerated with different content, or one removed, can
    # take entries out of the corpus without changing any verdict - and
    # the entries it takes out are then validated by nothing.
    covered = set()
    for f in files:
        if "ima" in f:
            covered |= ima_observable_indices(os.path.join(DATA, f))
    uncovered = sorted(set(range(IMA_STEP_COUNT)) - covered)
    print("\nIMA step table: %d of %d entries loaded with a nonzero nibble "
          "magnitude, which is what makes a wrong value visible."
          % (len(covered), IMA_STEP_COUNT))
    if uncovered:
        bad.append("the IMA step table has %d entries no fixture exercises "
                   "observably (%s); a wrong value in any of them would "
                   "decode identically here"
                   % (len(uncovered),
                      ", ".join(str(x) for x in uncovered[:12])
                      + ("..." if len(uncovered) > 12 else "")))

    if pad_notes:
        # Printed, not silent. This is a real disagreement with every
        # reference about how long a file is, and it is resolved in our
        # favour by the file's own `fact` chunk. A reader of this output
        # has to be able to see that it happened and how often.
        print("\n%d comparison%s ended in padding past the fact chunk. The "
              "shared frames were identical in every one:"
              % (len(pad_notes), "" if len(pad_notes) == 1 else "s"))
        for line in pad_notes:
            print("  %s" % line)

    # The denominator, stated. A score whose exclusions are invisible is
    # inflated, so both halves are printed whether or not anything failed.
    total = sum(len(refs) for _, refs in scored)
    print("\n%d fixtures, %d fixture-reference comparisons." % (len(files),
                                                                total))
    if excluded:
        print("\nExcluded, with the reason each was established by:")
        for key in sorted(excluded):
            print("  %s" % " / ".join(key))
            for line in _wrap(EXCLUSIONS[key], 70):
                print("      " + line)

    if bad:
        print("\n%d disagreement(s):" % len(bad), file=sys.stderr)
        for line in bad:
            print("  " + line, file=sys.stderr)
        return 1
    print("\nEvery reference that can read a fixture agrees with us byte "
          "for byte.")
    return 0


def _wrap(text, width):
    words, line, out = text.split(), "", []
    for word in words:
        if line and len(line) + 1 + len(word) > width:
            out.append(line)
            line = word
        else:
            line = (line + " " + word) if line else word
    if line:
        out.append(line)
    return out


if __name__ == "__main__":
    sys.exit(main())
