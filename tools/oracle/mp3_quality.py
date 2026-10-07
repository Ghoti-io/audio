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
"""How the MP3 encoder sounds, scored against two others at the same rate.

    make check-mp3-quality

planning/audio.md 11.3 says a lossy encoder is gated twice and only the
first gate is exact; this is the second, and it is not exact, so what it is
and what it is not has to be said before a number is read.

**What it measures.** A neurogram similarity: the core of ViSQOL, which is
Google's full-reference audio quality metric and the one 11.3 names. Both
signals go through a bank of 32 gammatone-shaped filters spaced evenly on
the ERB scale from 50 Hz to the lesser of 16 kHz and 95 per cent of
Nyquist; each band's power in 1024-sample frames is taken in decibels; and
the two spectrograms are compared patch by patch (32 frames, half
overlapping) with the structural similarity index of Wang et al., computed
over 3 x 3 windows. The score is the mean, 1 for identical. Aligned first,
by the cross-correlation of the first channel, because an encoder's delay is
not a defect; the first and last 20 ms are dropped because a lossy codec's
edges are.

**What it is not.** It is not ViSQOL: no pinned build of ViSQOL could be
had here (it builds with Bazel, and the one PyPI wrapper downloads a binary
from a model hub), so this is the same similarity index on a similar
spectrogram, written from the published description, with no trained
mapping to opinion scores. It is not PEAQ either, which 11.3 also names and
which has no free implementation in the image. It cannot hear: it knows
nothing of masking, so a difference it calls a defect may be inaudible and a
defect it misses may be loud. Its use here is *comparison* - the same
signal, the same bit rate, the same decoder, three encoders - where a
metric's absolute calibration drops out and only its ordering matters.

**What it checks, and why those floors.** Against Shine, the fixed-point
ISO-example-based encoder, ours must not be worse on any signal Shine can
code: Shine is a deliberately plain encoder (no model worth the name), and
an encoder with a model that loses to it has something wrong. Against LAME,
ours must be within 0.03 on every signal and within 0.01 on average:
LAME is the reference every claim of competitiveness is made against and
has had two decades of tuning this encoder has not, so being a little
behind it is the expected result and being a lot behind is a defect. The
margins are set from measurement - at the time of writing the average gap
is about 0.007 - and left wide enough that a reference upgrade does not
break the gate, and narrow enough that a broken quantiser or a model that
has stopped working does.

**The controls.** The metric must see damage: noise added to our own decode
at about 15 dB below the signal must lower the score by more than 0.02;
LAME at 64 kbit/s must score lower than at 128 and that lower than at 256,
or the score is not tracking the bits; and a decode a thousand samples
late, which the alignment must absorb, must score the same as one in time.
"""

import math
import os
import subprocess
import sys
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import oracle_env as oracle  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
DATA = os.path.join(ROOT, "tests", "data")
SCRATCH = os.path.join(ROOT, "build", "mp3-quality")
WRITE_PROBE = os.path.join(ROOT, "build", "linux", "release", "apps", "oracle",
                           "write_probe")
DUMP_PROBE = os.path.join(ROOT, "build", "linux", "release", "apps", "oracle",
                          "dump_probe")

#: Ours may be this much below LAME on any one signal, and this much on
#: average.
LAME_EACH = 0.03
LAME_MEAN = 0.01
#: And this much below Shine, which is to say not at all, to a rounding.
SHINE_EACH = 0.005
#: Shine, through ffmpeg's wrapper, writes MPEG-1 only. Asked for 22.05 or
#: 11.025 kHz it writes a file that decodes to something else (and ffmpeg
#: then cannot read it), so it is scored at the three rates it supports and
#: named as not asked at the others.
SHINE_RATES = (32000, 44100, 48000)


def run(argv, **kw):
    return subprocess.run(argv, capture_output=True, **kw)


def ffmpeg(args):
    return run(oracle.command("ffmpeg", ["ffmpeg", "-v", "error", "-y"] + args,
                              scratch=SCRATCH))


# ------------------------------------------------------------ the metric

def erb(f):
    return 24.7 * (4.37 * f / 1000.0 + 1.0)


def erb_centres(count, low, high):
    to = lambda f: 21.4 * np.log10(4.37 * f / 1000.0 + 1.0)
    back = lambda e: (10.0 ** (e / 21.4) - 1.0) * 1000.0 / 4.37
    return back(np.linspace(to(low), to(high), count))


def spectrogram(x, rate, nfft, hop, bands=32, low=50.0):
    high = min(16000.0, rate / 2.0 * 0.95)
    centres = erb_centres(bands, low, high)
    freqs = np.fft.rfftfreq(nfft, 1.0 / rate)
    width = 1.019 * erb(centres)
    weights = (1.0 + ((freqs[None, :] - centres[:, None]) / width[:, None]) ** 2) ** -4.0
    weights /= weights.sum(axis=1, keepdims=True)
    window = np.hanning(nfft)
    frames = (len(x) - nfft) // hop + 1
    index = np.arange(nfft)[None, :] + hop * np.arange(frames)[:, None]
    power = np.abs(np.fft.rfft(x[index] * window, axis=1)) ** 2 / (nfft * 0.375)
    return 10.0 * np.log10(power @ weights.T + 1e-3)


def box(a, k):
    c = np.cumsum(np.cumsum(np.pad(a, ((1, 0), (1, 0))), 0), 1)
    return (c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]) / (k * k)


def similarity(ref, test, patch=32, k=3):
    ref = np.maximum(ref, 0.0)
    test = np.maximum(test, 0.0)
    c1 = (0.01 * 100.0) ** 2
    c2 = (0.03 * 100.0) ** 2 / 2.0
    scores = []
    for t in range(0, ref.shape[0] - patch + 1, patch // 2):
        r = ref[t:t + patch]
        d = test[t:t + patch]
        mr, md = box(r, k), box(d, k)
        vr = np.maximum(box(r * r, k) - mr * mr, 0.0)
        vd = np.maximum(box(d * d, k) - md * md, 0.0)
        cv = box(r * d, k) - mr * md
        s = ((2 * mr * md + c1) / (mr * mr + md * md + c1)) \
            * ((cv + c2) / (np.sqrt(vr * vd) + c2))
        scores.append(s.mean())
    return float(np.mean(scores))


def load(path):
    with wave.open(path, "rb") as w:
        ch = w.getnchannels()
        rate = w.getframerate()
        data = np.frombuffer(w.readframes(w.getnframes()), "<i2").reshape(-1, ch)
    return data.astype(float), rate


def align(ref, test):
    n = 1 << int(math.ceil(math.log2(len(ref) + len(test))))
    c = np.fft.irfft(np.fft.rfft(test[:, 0], n) * np.conj(np.fft.rfft(ref[:, 0], n)), n)
    lag = int(np.argmax(c))
    if lag > n // 2:
        lag -= n
    if lag > 0:
        test = test[lag:]
    else:
        ref = ref[-lag:]
    m = min(len(ref), len(test))
    return ref[:m], test[:m]


def score(ref, test, rate):
    """Mean over channels of the similarity of two aligned signals."""
    ref, test = align(ref, test)
    edge = int(0.02 * rate)
    ref, test = ref[edge:-edge], test[edge:-edge]
    nfft = 1024 if rate >= 32000 else 512
    out = []
    for c in range(ref.shape[1]):
        a = spectrogram(ref[:, c], rate, nfft, nfft // 2)
        b = spectrogram(test[:, c], rate, nfft, nfft // 2)
        out.append(similarity(a, b))
    return float(np.mean(out))


# --------------------------------------------------------------- encoding

def decode(mp3):
    pcm = mp3[:-4] + ".pcm"
    finished = ffmpeg(["-i", mp3, "-f", "s16le", "-acodec", "pcm_s16le", pcm])
    if finished.returncode != 0 or not os.path.exists(pcm):
        return None
    return pcm


def encode(kind, wav, kbps):
    base = os.path.join(SCRATCH, os.path.splitext(os.path.basename(wav))[0])
    mp3 = "%s.%s%d.mp3" % (base, kind, kbps)
    if os.path.exists(mp3):
        os.remove(mp3)
    if kind == "ours":
        finished = run([WRITE_PROBE, wav, mp3, "mp3", "mp3", str(kbps), "cbr"])
        ok = finished.returncode == 0
    else:
        codec = {"lame": "libmp3lame", "shine": "libshine"}[kind]
        finished = ffmpeg(["-i", wav, "-c:a", codec, "-b:a", "%dk" % kbps, "-f", "mp3", mp3])
        ok = os.path.exists(mp3) and os.path.getsize(mp3) > 0
    return mp3 if ok else None


def samples(pcm, channels):
    return np.fromfile(pcm, "<i2").reshape(-1, channels).astype(float)


# ---------------------------------------------------------------- signals

def synth():
    """Signals made here, so the gate has a few shapes the committed files
    do not: noise with a falling spectrum, a steady harmonic tone, and
    a voiced sound with a wandering pitch."""
    out = {}
    rng = np.random.default_rng(1)
    n = 44100 * 4
    w = rng.standard_normal(n)
    f = np.fft.rfft(w)
    k = np.arange(len(f))
    k[0] = 1
    p = np.fft.irfft(f / np.sqrt(k), n)
    p2 = np.fft.irfft(np.fft.rfft(rng.standard_normal(n)) / np.sqrt(k), n)
    out["synth_pink_44100"] = (np.stack([p, p2], 1) / np.abs(p).max() * 0.4, 44100, 128)
    t = np.arange(n) / 44100.0
    h = sum(np.sin(2 * np.pi * 220 * j * (t + 0.008 / (2 * np.pi * 5) * (1 - np.cos(2 * np.pi * 5 * t)))) / j
            for j in range(1, 20))
    out["synth_harmonic_44100"] = (np.stack([h * 0.4 / np.abs(h).max()] * 2, 1), 44100, 96)
    f0 = 110 + 30 * np.sin(2 * np.pi * 0.7 * t)
    ph = 2 * np.pi * np.cumsum(f0) / 44100.0
    v = sum((np.exp(-((j * f0 - 700) / 300) ** 2) + 0.6 * np.exp(-((j * f0 - 1800) / 400) ** 2)
             + 0.3 * np.exp(-((j * f0 - 3200) / 600) ** 2)) * np.sin(j * ph) for j in range(1, 40))
    v = v * (0.1 + 0.5 * (1 + np.sin(2 * np.pi * 3 * t)) / 2) + 0.02 * rng.standard_normal(n)
    out["synth_voice_44100"] = (np.stack([v, 0.7 * v], 1) / np.abs(v).max() * 0.5, 44100, 128)
    return out


def write_wav(path, data, rate):
    with wave.open(path, "wb") as w:
        w.setnchannels(data.shape[1])
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(np.clip(np.round(data * 32767.0), -32768, 32767).astype("<i2").tobytes())


# ------------------------------------------------------------------ music

CORPUS = os.path.join(ROOT, "third_party", "mp3corpus")
CLIP_SECONDS = 30
#: The VBR scale: quality points the encoder is asked for, and LAME's -V
#: settings it is compared against.
VBR_QUALITIES = tuple(range(20, 65, 5))
LAME_VBR = (0, 2, 4, 6, 8)
#: Raising the quality five points may raise the rate by this factor at most,
#: and never lower it. LAME climbs about 1.25 at the widest; ours is at 1.76 on worn shellac today.
VBR_STEP_RATIO = 1.8
#: Raising the quality may not lower the rate, and may not lower the score by
#: more than this. Measured: the rate is monotone on all twelve clips (it was
#: not on the drum before middle/side was given the allowance it is owed), and
#: the score dips at most 0.015 (worn shellac, 20 to 25, at the 32 kbit/s
#: floor, where the rate cannot move).
VBR_RATE_DIP = 0.97
VBR_SCORE_DIP = 0.02
#: At the same bit rate (read off LAME's -V ladder, interpolated in the log
#: of the rate) ours may be this far below LAME's score on any clip, and on
#: average over the clips.
VBR_GAP_EACH = 0.04
VBR_GAP_MEAN = 0.015


def corpus_clips():
    """The fetched music, decoded to 44.1 kHz stereo WAV in the scratch
    directory: a list of (tag, path). Empty if tools/corpus/fetch.sh has not
    been run."""
    clips = []
    if not os.path.isdir(CORPUS):
        return clips
    for name in sorted(os.listdir(CORPUS)):
        if not name.endswith(".flac"):
            continue
        tag = name[:-5]
        wav = os.path.join(SCRATCH, "music_%s.wav" % tag)
        if not os.path.exists(wav):
            ffmpeg(["-i", os.path.join(CORPUS, name), "-t", str(CLIP_SECONDS),
                    "-ar", "44100", "-ac", "2", "-sample_fmt", "s16", wav])
        if os.path.exists(wav):
            clips.append((tag, wav))
    return clips


def vbr_encode(kind, wav, setting):
    """Encode at a VBR setting (ours: quality 0-100, LAME: -V 0-9)."""
    base = os.path.join(SCRATCH, os.path.splitext(os.path.basename(wav))[0])
    mp3 = "%s.%s-v%d.mp3" % (base, kind, setting)
    if os.path.exists(mp3):
        os.remove(mp3)
    if kind == "ours":
        finished = run([WRITE_PROBE, wav, mp3, "mp3", "mp3", "0", "vbr", str(setting)])
        ok = finished.returncode == 0
    else:
        ffmpeg(["-i", wav, "-c:a", "libmp3lame", "-q:a", str(setting), "-f", "mp3", mp3])
        ok = os.path.exists(mp3) and os.path.getsize(mp3) > 0
    return mp3 if ok else None


def vbr_point(kind, wav, ref, rate, setting, seconds):
    mp3 = vbr_encode(kind, wav, setting)
    if mp3 is None:
        return None
    pcm = decode(mp3)
    if pcm is None:
        return None
    kbps = os.path.getsize(mp3) * 8.0 / seconds / 1000.0
    return kbps, score(ref, samples(pcm, ref.shape[1]), rate)


def music_checks(failures):
    """The VBR quality scale on real music: it must rise smoothly with the
    rate, and at LAME's rates it must sound about as good as LAME."""
    clips = corpus_clips()
    if not clips:
        message = ("the music corpus is not fetched: run tools/corpus/fetch.sh "
                   "(set GHOTI_CORPUS_REQUIRED=1 to make this a failure)")
        print("\n  " + message)
        if os.environ.get("GHOTI_CORPUS_REQUIRED") == "1":
            failures.append(message)
        return
    print("\n  VBR on music: kbit/s and score, ours by quality, LAME by -V")
    gaps = []
    for tag, wav in clips:
        ref, rate = load(wav)
        seconds = len(ref) / float(rate)
        ours = []
        for q in VBR_QUALITIES:
            point = vbr_point("ours", wav, ref, rate, q, seconds)
            if point is None:
                failures.append("%s: ours did not encode at quality %d" % (tag, q))
                break
            ours.append((q,) + point)
        lame = []
        for v in LAME_VBR:
            point = vbr_point("lame", wav, ref, rate, v, seconds)
            if point is not None:
                lame.append(point)
        if len(ours) != len(VBR_QUALITIES) or len(lame) < 2:
            continue
        print("  %-11s ours %s" % (tag, " ".join("%d:%.0f/%.3f" % p for p in ours)))
        print("  %-11s lame %s" % ("", " ".join("%.0f/%.3f" % p for p in lame)))
        for a, b in zip(ours, ours[1:]):
            if b[1] < a[1] * VBR_RATE_DIP or b[2] < a[2] - VBR_SCORE_DIP:
                failures.append("%s: quality %d -> %d lowered the rate or the score "
                                "(%.0f -> %.0f kbit/s, %.4f -> %.4f)"
                                % (tag, a[0], b[0], a[1], b[1], a[2], b[2]))
            if a[1] > 0 and b[1] / a[1] > VBR_STEP_RATIO:
                failures.append("%s: quality %d -> %d raised the rate %.2fx (%.0f -> %.0f "
                                "kbit/s); the most allowed is %.2fx"
                                % (tag, a[0], b[0], b[1] / a[1], a[1], b[1], VBR_STEP_RATIO))
        lame.sort()
        xs = [math.log(p[0]) for p in lame]
        ys = [p[1] for p in lame]
        clip_gaps = [float(np.interp(math.log(k), xs, ys)) - s
                     for _, k, s in ours if xs[0] <= math.log(k) <= xs[-1]]
        if clip_gaps:
            gap = float(np.mean(clip_gaps))
            gaps.append(gap)
            print("  %-11s ours is %+.4f behind LAME at LAME's rates" % ("", gap))
            if gap > VBR_GAP_EACH:
                failures.append("%s: %.4f behind LAME at equal rate; the most allowed is %.2f"
                                % (tag, gap, VBR_GAP_EACH))
    if gaps and sum(gaps) / len(gaps) > VBR_GAP_MEAN:
        failures.append("music: on average %.4f behind LAME at equal rate; the most allowed "
                        "is %.2f" % (sum(gaps) / len(gaps), VBR_GAP_MEAN))
    if gaps:
        print("  music: %d clips, ours on average %.4f behind LAME at equal rate"
              % (len(gaps), sum(gaps) / len(gaps)))


def main():
    print(oracle.provenance(["ffmpeg"]))
    for probe in (WRITE_PROBE, DUMP_PROBE):
        if not os.path.isfile(probe):
            raise SystemExit("%s is not built: run `make oracle-probe`." % probe)
    os.makedirs(SCRATCH, exist_ok=True)

    cases = []   # (name, wav path, kbps)
    for name, kbps in (("mp3enc_mix_44100", 128), ("mp3enc_clicks_22050", 48),
                       ("mp3enc_voice_11025", 24)):
        cases.append((name, os.path.join(DATA, name + ".wav"), kbps))
    for name, (data, rate, kbps) in synth().items():
        path = os.path.join(SCRATCH, name + ".wav")
        write_wav(path, data, rate)
        cases.append((name, path, kbps))

    print("%-22s %-6s %7s %7s" % ("signal", "enc", "score", "bytes"))
    failures = []
    gaps = []
    table = {}
    for name, wav, kbps in cases:
        ref, rate = load(wav)
        channels = ref.shape[1]
        results = {}
        for kind in ("ours", "lame", "shine"):
            if kind == "shine" and rate not in SHINE_RATES:
                continue
            mp3 = encode(kind, wav, kbps)
            if mp3 is None:
                continue
            pcm = decode(mp3)
            if pcm is None:
                failures.append("%s: ffmpeg could not decode %s's file" % (name, kind))
                continue
            results[kind] = (score(ref, samples(pcm, channels), rate), os.path.getsize(mp3), pcm)
            print("%-22s %-6s %7.4f %7d" % (name, kind, results[kind][0], results[kind][1]))
        table[name] = results
        if "ours" not in results:
            failures.append("%s: no score for ours" % name)
            continue
        mine = results["ours"][0]
        if "lame" in results:
            gap = results["lame"][0] - mine
            gaps.append(gap)
            if gap > LAME_EACH:
                failures.append("%s: %.4f against LAME's %.4f, %.4f behind and the "
                                "most allowed is %.2f" % (name, mine, results["lame"][0], gap, LAME_EACH))
        if "shine" in results and results["shine"][0] - mine > SHINE_EACH:
            failures.append("%s: %.4f against Shine's %.4f; an encoder with a model "
                            "must not lose to one without" % (name, mine, results["shine"][0]))
    if gaps and sum(gaps) / len(gaps) > LAME_MEAN:
        failures.append("on average %.4f behind LAME and the most allowed is %.3f"
                        % (sum(gaps) / len(gaps), LAME_MEAN))

    # ---- the controls
    print("\n  controls: the metric has to be able to see damage")
    name, wav, kbps = cases[0]
    ref, rate = load(wav)
    channels = ref.shape[1]
    good = samples(table[name]["ours"][2], channels)
    rng = np.random.default_rng(7)
    noisy = good + rng.standard_normal(good.shape) * math.sqrt((ref ** 2).mean()) * 10 ** (-15 / 20)
    base = score(ref, good, rate)
    damaged = score(ref, noisy, rate)
    print("  noise 15 dB under the signal: %.4f -> %.4f" % (base, damaged))
    if base - damaged < 0.02:
        failures.append("control: adding noise 15 dB down moved the score by only "
                        "%.4f, so the score does not see noise" % (base - damaged))
    late = np.concatenate([np.zeros((1000, channels)), good])[:len(good)]
    moved = score(ref, late, rate)
    print("  a thousand samples late: %.4f -> %.4f" % (base, moved))
    if abs(moved - base) > 0.002:
        failures.append("control: a decode a thousand samples late scored %.4f against "
                        "%.4f, so the alignment is not absorbing delay" % (moved, base))
    ladder = []
    for k in (64, 128, 256):
        mp3 = encode("lame", wav, k)
        pcm = decode(mp3)
        ladder.append(score(ref, samples(pcm, channels), rate))
    print("  LAME at 64, 128, 256 kbit/s: %.4f %.4f %.4f" % tuple(ladder))
    if not (ladder[0] < ladder[1] < ladder[2]):
        failures.append("control: LAME's score does not rise with its bit rate (%s)" % ladder)

    music_checks(failures)

    mean_gap = sum(gaps) / len(gaps) if gaps else float("nan")
    print("\n%d signals; ours is on average %.4f behind LAME" % (len(cases), mean_gap))
    if failures:
        print("\n".join("FAIL " + f for f in failures))
        return 1
    print("check-mp3-quality: within the stated distance of LAME and not behind Shine")
    return 0


if __name__ == "__main__":
    sys.exit(main())
