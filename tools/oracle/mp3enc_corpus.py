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
"""Write the signals the MP3 encoder is exercised on.

    tools/oracle/mp3enc_corpus.py [--check]

A lossy encoder is judged on what it is given, and the encoder's own unit
tests synthesise their inputs in C++. These are the same kind of thing for
the places that need a file: the cross-architecture gate, which encodes
them on big-endian machines and compares bytes, and the quality gate, which
scores them against other encoders.

Three signals, each chosen for the part of the encoder it reaches that a
plain tone does not:

  - **mp3enc_mix_44100.wav**, stereo: a harmonic tone with vibrato, loud
    decaying noise bursts, and a quiet pink-ish floor, panned differently
    on each side. It reaches block switching, mid/side against left/right,
    the reservoir, and short blocks in stereo.
  - **mp3enc_clicks_22050.wav**, mono: bursts and silence. MPEG-2's single
    granule, and the case where a short block follows digital silence.
  - **mp3enc_voice_11025.wav**, mono: pulses through formant bands with a
    wandering pitch, at an MPEG-2.5 rate.

The noise is a fixed linear congruential generator and the tones are sums of
sines, so the files are the same on every machine that runs this; they are
committed all the same, because the gates that read them must not depend on
a regeneration.
"""

import math
import os
import struct
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(os.path.dirname(os.path.dirname(HERE)), "tests", "data")


class Lcg:
    """The same generator the C++ tests use, so an input can be reasoned
    about on either side."""

    def __init__(self, seed):
        self.state = seed & 0xFFFFFFFF

    def next(self):
        self.state = (self.state * 1664525 + 1013904223) & 0xFFFFFFFF
        return ((self.state >> 8) - 8388608) / 8388608.0


def pink(count, rng):
    """Noise with a falling spectrum: white noise through a leaky
    integrator and a one-pole highpass of its own, which is rough but
    stationary and cheap."""
    out = []
    a = 0.0
    b = 0.0
    for _ in range(count):
        w = rng.next()
        a = 0.97 * a + 0.2 * w
        b = 0.7 * b + 0.3 * a
        out.append(b)
    peak = max(abs(v) for v in out) or 1.0
    return [v / peak for v in out]


def bursts(count, rate, rng, spacing, level):
    out = [0.0] * count
    t = int(rate * 0.08)
    while t < count - rate // 10:
        length = int(rate * 0.04)
        for i in range(length):
            if t + i < count:
                out[t + i] += rng.next() * math.exp(-i / (0.006 * rate)) * level
        t += int(rate * spacing)
    return out


def harmonic(count, rate, f0, vibrato):
    out = []
    for n in range(count):
        t = n / rate
        phase = t + vibrato / (2 * math.pi * 5.0) * (1.0 - math.cos(2 * math.pi * 5.0 * t))
        v = 0.0
        for h in range(1, 16):
            v += math.sin(2 * math.pi * f0 * h * phase) / h
        out.append(v)
    peak = max(abs(v) for v in out)
    return [v / peak for v in out]


def voice(count, rate, rng):
    out = []
    phase = 0.0
    for n in range(count):
        t = n / rate
        f0 = 120.0 + 30.0 * math.sin(2 * math.pi * 0.7 * t)
        phase += 2 * math.pi * f0 / rate
        v = 0.0
        for h in range(1, 30):
            fc = h * f0
            g = (math.exp(-((fc - 700.0) / 300.0) ** 2)
                 + 0.6 * math.exp(-((fc - 1800.0) / 400.0) ** 2)
                 + 0.3 * math.exp(-((fc - 3200.0) / 600.0) ** 2))
            v += g * math.sin(h * phase)
        out.append(v * (0.1 + 0.5 * (1 + math.sin(2 * math.pi * 3.0 * t)) / 2)
                   + 0.02 * rng.next())
    peak = max(abs(v) for v in out)
    return [v / peak * 0.8 for v in out]


def write(name, rate, channels):
    path = os.path.join(DATA, name)
    frames = len(channels[0])
    data = bytearray()
    for i in range(frames):
        for c in channels:
            v = int(round(max(-1.0, min(1.0, c[i])) * 32767.0))
            data += struct.pack("<h", v)
    return path, rate, len(channels), bytes(data)


def build():
    files = []
    rng = Lcg(1)
    n = 44100 * 2
    h = harmonic(n, 44100, 196.0, 0.008)
    p = [pink(n, Lcg(10)), pink(n, Lcg(11))]
    c = [bursts(n, 44100, Lcg(20), 0.31, 0.5), bursts(n, 44100, Lcg(21), 0.27, 0.5)]
    left = [0.3 * h[i] + c[0][i] * 0.7 + 0.08 * p[0][i] for i in range(n)]
    right = [0.24 * h[i] + c[1][i] * 0.5 + 0.08 * p[1][i] for i in range(n)]
    files.append(write("mp3enc_mix_44100.wav", 44100, [left, right]))
    n = 22050 * 2
    files.append(write("mp3enc_clicks_22050.wav", 22050,
                       [bursts(n, 22050, Lcg(30), 0.23, 0.7)]))
    n = 11025 * 3
    files.append(write("mp3enc_voice_11025.wav", 11025, [voice(n, 11025, Lcg(40))]))
    return files


def render(path, rate, channels, data):
    import io
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(data)
    return buffer.getvalue()


def main(argv):
    status = 0
    for path, rate, channels, data in build():
        blob = render(path, rate, channels, data)
        if "--check" in argv:
            ok = os.path.exists(path) and open(path, "rb").read() == blob
            print("%s: %s" % (os.path.basename(path), "current" if ok else "DIFFERS"))
            status |= 0 if ok else 1
        else:
            with open(path, "wb") as f:
                f.write(blob)
            print("wrote %s (%d bytes)" % (path, len(blob)))
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv))
