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
"""Score the MPEG decode against the signal the encoder was given.

    make check-mpeg-input

**Every other gate here compares this library against another decoder.
This one compares it against the input**, and it exists for one reason:
MPEG-2.5's scalefactor band tables are in no standard, so they were taken
from an implementation - and the two reference decoders carry byte-identical
copies of the same values. A differential against them is therefore blind
to exactly one class of error: a table that every implementation agrees on
and that is wrong. `check-mpeg` cannot see it, `check-golden` cannot see it,
and no amount of either adds up to seeing it.

What makes a comparison against the input possible at all is that the
corpus is *synthesised*: tools/oracle/make_corpus.py builds each fixture
from a closed-form `aevalsrc` expression, so the exact samples the encoder
was handed can be regenerated instead of stored. What makes it meaningful
is that a scalefactor band table is the decoder's map from coded values to
*where in the spectrum they go*: get it wrong and the spectral envelope is
wrong, whatever the reference decoders think.

So the measure is per-band energy against the input's, in bands wide
enough that a perceptual encoder's own decisions do not move them:

  - **below the encoder's cutoff, every band must track the input.** The
    measured worst case is 1.96 dB, at 12 kHz and 48 kbit/s where the
    encoder is working hardest; the 8 kHz fixtures are within 0.71 dB and
    the 48 kHz calibration within 0.65. The threshold is 3 dB, and the
    separation that matters is the other side of it: with the 8 kHz band
    row deliberately replaced by the 16 kHz one, the same measurement
    reads 5.24 and 6.33 dB. So 3 dB sits about a decibel above the worst
    correct answer and two below the wrong one.
  - **the overall level** must match, which is what the phase 5 band-table
    defect moved: 8.6 dB, on exactly this kind of signal.
  - **the spectral centroid** must land near the input's. This is the one
    number that moves when the envelope is warped but the total energy is
    not, which is what a wrong band *map* does as distinct from a wrong
    band *gain*.

**Which bands are compared is derived from our own decode, not from the
input**, and getting that backwards is a trap worth naming: keying it on
the input's energy looked right and was useless, because the input here is
white noise whose every band is full, so the encoder's own lowpass - the
top two or three bands, 30 dB down - was judged a decoder defect. The
cutoff is a fact about the encoder's output. It is found as the first band
more than 12 dB below the median of the lower half of our own spectrum,
and the band below it is dropped as the lowpass transition, which leaves
13 to 14 of the 16 bands actually compared. The count is printed, because
a sweep whose denominator is not printed can shrink to nothing and still
say "pass" - which this one did, at 0 of 16, on the way to here.

**The fixtures have to be broadband and from an encoder that lowpasses.**
libshine does not lowpass, so its in-band region runs to Nyquist where
the comparison measures the encoder rather than the band map; its fixture
is deliberately not in the list below.

The thresholds are set from measurement and are listed beside each check.
As everywhere in this directory the gate is run against deliberately wrong
versions of our own answer, every one of which it must reject - including
a spectral tilt, which is the shape of the failure this gate alone can see.
"""

import cmath
import math
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import make_corpus  # noqa: E402
import oracle_env as oracle  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(ROOT, "tests", "data")
PROBE = os.path.join(ROOT, "build", "linux", "release", "apps", "oracle",
    "dump_probe")
OUT = os.path.join(ROOT, "tests", "out")

#: How many equal-width bands the spectrum is cut into. Sixteen puts about
#: 250 Hz in a band at 8 kHz, which is several scalefactor bands wide - so
#: a band here cannot be moved by one quantiser decision, and a band *map*
#: error cannot hide inside one.
BANDS = 16

#: Below the cutoff: the most a band may differ from the input's, in dB.
#: Measured worst case is 1.96 over every fixture here and 5.24 with a
#: wrong band table; see the separation argument in the file comment.
MAX_BAND_DB = 3.0

#: How far below the robust in-band level a band must fall to count as
#: being past the encoder's lowpass. The drop at the cutoff is 25 to 40 dB
#: and the in-band variation is under 1 dB, so this sits between them with
#: an order of magnitude to spare either way.
CUTOFF_DB = 12.0

#: The fewest bands a fixture must actually contribute. Below this the
#: comparison is not measuring the spectrum any more and the right answer
#: is to fail rather than to pass on three bands.
MIN_BANDS = 10

#: How far the overall level may be from the input's, in dB. Measured
#: worst case is 1.24; the phase 5 band-table defect was 8.6.
MAX_LEVEL_DB = 3.0

#: How far the spectral centroid may be from the input's, relatively.
#: Measured worst case is 0.152; a wrong band map moves it much further.
MAX_CENTROID = 0.30

#: The signal table, **imported and not copied.** An earlier version of
#: this file repeated it, with a comment claiming make_corpus.py could not
#: be imported safely. That was wrong - nothing runs at its module level
#: but a `sys.path.insert` - and the copy was a second place for the same
#: definition to live, which is how a gate comes to regenerate a signal
#: that no longer matches the fixture it is scoring.
SIGNALS = make_corpus.SIGNALS

#: name, channels, rate, frames, signal. The three MPEG-2.5 fixtures,
#: because they are the ones whose band tables came from an
#: implementation - plus two whose tables came from the standards, which
#: are the calibration: this gate has to pass on a decode that is known
#: good for an independent reason before its verdict on the others means
#: anything.
CASES = [
    # MPEG-2.5. The 8 kHz pair is the only decode in this library whose
    # band table came from an implementation rather than a document.
    ("mp3_lame_mpeg25_mono_8000.mp3", 1, 8000, 1601, "noise", False),
    ("mp3_lame_mpeg25_stereo_8000.mp3", 2, 8000, 1601, "noisestereo", False),
    ("mp3_lame_mpeg25_12000.mp3", 2, 12000, 1201, "noisestereo", False),
    # The calibration: MPEG-1 and MPEG-2, whose tables come from the two
    # standards, measured the same way by the same code.
    ("mp3_lame_stereo_320_48000.mp3", 2, 48000, 4799, "noisestereo", True),
    ("mp3_lame_noise_22050.mp3", 2, 22050, 2003, "noisestereo", True),
]

WINDOW = 1024


def run(argv, **kw):
    return subprocess.run(argv, capture_output=True, **kw)


def meta(path):
    finished = run([PROBE, path], text=True)
    if finished.returncode != 0:
        raise SystemExit("dump_probe failed on %s:\n%s"
                         % (path, finished.stderr.strip()))
    return dict(pair.split("=", 1)
                for pair in finished.stdout.split() if "=" in pair)


def ours(path):
    finished = run([PROBE, path, "--pcm"])
    if finished.returncode != 0:
        raise SystemExit("dump_probe --pcm failed on %s" % path)
    raw = finished.stdout
    return list(struct.unpack("<%dh" % (len(raw) // 2), raw))


def source(signal, channels, rate, frames):
    """The samples the encoder was handed, regenerated not stored."""
    template = SIGNALS[signal]
    exprs = "|".join(template % {"c": i, "f": 220 * (i + 1),
                                 "g": 1330 + 97 * i}
                     for i in range(channels))
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, "mpeg-input-source.raw")
    argv = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
            "-f", "lavfi",
            "-i", "aevalsrc=%s:s=%d:d=%.9f"
                  % (exprs, rate, frames / float(rate)),
            "-af", "atrim=end_sample=%d" % frames,
            "-fflags", "+bitexact", "-flags:a", "+bitexact",
            "-map_metadata", "-1",
            "-f", "s16le", "-c:a", "pcm_s16le", path]
    finished = run(oracle.command("ffmpeg", argv, scratch=OUT))
    if finished.returncode != 0:
        raise SystemExit("could not regenerate the source signal for %s:\n%s"
                         % (signal, finished.stderr.decode()[-800:]))
    with open(path, "rb") as handle:
        raw = handle.read()
    os.remove(path)
    return list(struct.unpack("<%dh" % (len(raw) // 2), raw))


def trim(samples, delay, padding, channels):
    out = samples[delay * channels:]
    if padding:
        cut = padding * channels
        out = out[:-cut] if cut < len(out) else []
    return out


def transform(values):
    """Radix-2 FFT, written here rather than imported.

    No numpy and no scipy: the whole claim of this gate is that it depends
    on the input signal and on nothing else, and a gate that needs a
    numerical library has acquired a dependency whose version is not
    pinned anywhere in this repository. WINDOW is a power of two so the
    recursion is exact.
    """
    count = len(values)
    if count == 1:
        return list(values)
    even = transform(values[0::2])
    odd = transform(values[1::2])
    out = [0j] * count
    for k in range(count // 2):
        turn = cmath.exp(-2.0j * math.pi * k / count) * odd[k]
        out[k] = even[k] + turn
        out[k + count // 2] = even[k] - turn
    return out


def spectrum(samples):
    """Energy in BANDS equal-width bands, in dB, over one Hann window."""
    values = [float(v) for v in samples[:WINDOW]]
    values += [0.0] * (WINDOW - len(values))
    window = [0.5 - 0.5 * math.cos(2.0 * math.pi * i / (WINDOW - 1))
              for i in range(WINDOW)]
    spectra = transform([values[i] * window[i] for i in range(WINDOW)])
    per = (WINDOW // 2) // BANDS
    return [10.0 * math.log10(
                sum(abs(spectra[k]) ** 2
                    for k in range(band * per, (band + 1) * per)) + 1e-9)
            for band in range(BANDS)]


def in_band(mine):
    """Which bands the encoder kept, from our own spectrum.

    The median of the lower half is the in-band level: robust, because one
    band of a noise signal wanders by a few dB and the mean of a spectrum
    that includes the discarded bands is pulled down by them. The band
    immediately below the cutoff is the lowpass transition and is dropped.
    """
    ordered = sorted(mine[:BANDS // 2])
    middle = len(ordered) // 2
    level = (ordered[middle] if len(ordered) % 2
             else 0.5 * (ordered[middle - 1] + ordered[middle]))
    cutoff = BANDS
    for band in range(BANDS):
        if mine[band] < level - CUTOFF_DB:
            cutoff = band
            break
    return list(range(max(0, cutoff - 1)))


def centroid(bands):
    """The energy-weighted mean band index, as a fraction of the spectrum."""
    weights = [10.0 ** (v / 10.0) for v in bands]
    total = sum(weights)
    if total <= 0.0:
        return 0.0
    return sum((i + 0.5) * w for i, w in enumerate(weights)) / total / BANDS


def level(samples):
    """RMS, in dB, of one channel."""
    if not samples:
        return -999.0
    power = sum(float(v) * float(v) for v in samples) / len(samples)
    return 10.0 * math.log10(power + 1e-9)


def compare(ours_ch, source_ch):
    """(complaints, bands compared, worst band difference) for one channel."""
    bad = []
    mine = spectrum(ours_ch)
    theirs = spectrum(source_ch)
    bands = in_band(mine)
    worst = 0.0
    if len(bands) < MIN_BANDS:
        bad.append("only %d of %d bands are below the encoder's cutoff, and "
                   "a comparison over fewer than %d is not measuring the "
                   "spectrum" % (len(bands), BANDS, MIN_BANDS))
    for band in bands:
        difference = mine[band] - theirs[band]
        worst = max(worst, abs(difference))
        if abs(difference) > MAX_BAND_DB:
            bad.append("band %d of %d holds %.1f dB and the input holds "
                       "%.1f, a difference of %.1f and the gate is %.1f"
                       % (band, BANDS, mine[band], theirs[band], difference,
                          MAX_BAND_DB))
    gap = level(ours_ch) - level(source_ch)
    if abs(gap) > MAX_LEVEL_DB:
        bad.append("the level is %.2f dB from the input's and the gate is "
                   "%.1f" % (gap, MAX_LEVEL_DB))
    one = centroid([mine[b] for b in bands] or mine)
    two = centroid([theirs[b] for b in bands] or theirs)
    if two > 0.0 and abs(one - two) / two > MAX_CENTROID:
        bad.append("the spectral centroid is at %.3f of the compared bands "
                   "and the input's is at %.3f, a relative difference of "
                   "%.3f and the gate is %.2f"
                   % (one, two, abs(one - two) / two, MAX_CENTROID))
    return bad, len(bands), worst


def channel(samples, index, count):
    return samples[index::count]


def wrong_answers(truth, channels):
    """Deliberately wrong decodes this gate must reject.

    The tilt is the one that matters and the reason this gate exists: it
    is the shape a misplaced scalefactor band produces, and it is the one
    shape a comparison against another decoder carrying the same table
    cannot see.
    """
    out = [
        ("silence", [0] * len(truth)),
        ("six dB down", [v // 2 for v in truth]),
    ]
    # A spectral tilt: a one-pole high-pass, which moves the envelope
    # without changing the total energy much. Per channel, or the
    # filter would mix the channels together.
    tilted = list(truth)
    for ch in range(channels):
        previous = 0
        for i in range(ch, len(truth), channels):
            current = truth[i]
            tilted[i] = max(-32768, min(32767, current - previous // 2))
            previous = current
    out.append(("a spectral tilt", tilted))
    # A low-pass, which is the phase 5 band-table defect's own shape: the
    # top of the spectrum quieter than the encoder sent. It is the
    # mirror of the tilt and the two together bracket an envelope error
    # in both directions.
    rolled = list(truth)
    for ch in range(channels):
        previous = 0
        for i in range(ch, len(truth), channels):
            current = (truth[i] + previous) // 2
            rolled[i] = current
            previous = truth[i]
    out.append(("a low-pass", rolled))
    return out


def main():
    oracle.check_pin("ffmpeg")
    if not os.path.exists(PROBE):
        raise SystemExit("%s is missing; run `make oracle-probe`" % PROBE)

    failures = []
    print("Our decode against the signal the encoder was given.")
    print("The last two rows are the calibration: their band tables come "
          "from the")
    print("standards, so this gate's verdict on the others means nothing "
          "unless they pass.\n")
    print("  %-34s %7s %4s  %-13s %s"
          % ("fixture", "rate", "ch", "bands compared", "worst band"))
    scored = 0
    for name, channels, rate, frames, signal, calibration in CASES:
        path = os.path.join(DATA, name)
        if not os.path.exists(path):
            failures.append("%s: the fixture is missing" % name)
            continue
        info = meta(path)
        raw = ours(path)
        mine = trim(raw, int(info["delay"]), int(info["padding"]), channels)
        theirs = source(signal, channels, rate, frames)
        if len(mine) != len(theirs):
            failures.append("%s: our decode is %d samples and the input was "
                            "%d; a length difference makes every spectral "
                            "comparison below meaningless"
                            % (name, len(mine) // channels,
                               len(theirs) // channels))
            continue
        worst = 0.0
        counted = 0
        for ch in range(channels):
            bad, bands, high = compare(channel(mine, ch, channels),
                channel(theirs, ch, channels))
            for complaint in bad:
                failures.append("%s / channel %d: %s" % (name, ch, complaint))
            worst = max(worst, high)
            counted = bands
            scored += 1
        print("  %-34s %7d %4d  %2d of %2d       %.2f dB%s"
              % (name, rate, channels, counted, BANDS, worst,
                 "   (calibration)" if calibration else ""))

    # The controls. A gate never seen to fail is not known to work, and
    # this one is new: its thresholds have never been tested against
    # anything but a correct decode until here.
    print("\n  control: this gate must reject each of these")
    name, channels, rate, frames, signal, _ = CASES[0]
    info = meta(os.path.join(DATA, name))
    truth = trim(ours(os.path.join(DATA, name)), int(info["delay"]),
        int(info["padding"]), channels)
    theirs = source(signal, channels, rate, frames)
    for label, wrong in wrong_answers(truth, channels):
        complaints = []
        for ch in range(channels):
            bad, _bands, _high = compare(channel(wrong, ch, channels),
                channel(theirs, ch, channels))
            complaints += bad
        if complaints:
            print("      %-32s rejected: %s" % (label, complaints[0][:70]))
        else:
            failures.append("the control %r was ACCEPTED, so this gate does "
                            "not measure what it claims to" % label)

    print("\n%d fixture-channel comparisons against the input signal."
          % scored)
    if failures:
        print("\nFAILED")
        for line in failures:
            print("  %s" % line)
        return 1
    print("Every band the encoder kept tracks the input, and the four "
          "wrong answers are rejected.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
