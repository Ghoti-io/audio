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
"""Score the MP3 encoder's files with three decoders that are not ours.

    make check-mp3-encode

planning/audio.md 11.3 puts it in two gates and only one of them is fuzzy.
This is the exact one: **if ffmpeg's decoder, libsndfile's and mpg123's
decode what the encoder wrote, to what our own decoder says, then the file is
a conformant stream.** No tolerance on quality, none on the encoder's
choices; only on the decoders' own last bit. That is *two* implementations
and not three: libsndfile 1.2.2 decodes MP3 through libmpg123, so it and
mpg123 are one decoder asked twice, and ffmpeg's native one is the other.

Every case below is encoded, and then:

  - **Our decode, ffmpeg's, libsndfile's and mpg123's agree**, sample for
    sample to two least significant bits, on the whole recording. Each of
    them returns the *recording* and not the file's contents, because each
    honours the delay and padding the tag states - ffmpeg only because the
    encoder's name begins `LAME` (see mp3enc_tag.c), mpg123 and libsndfile
    whatever it says - so the lengths are compared before anything else and
    must equal the frames that were given.
  - **The tag tells the truth**: its frame count is the frames in the file,
    its byte count is the file's, and delay plus the recording plus
    padding is the decoded length.
  - **The reservoir is respected in the bytes**: every frame's back-pointer
    is within what the frames before it left, and the granules' lengths fit
    what the frame and that back-pointer hold. A stream whose first frame
    starts before the file does is the commonest way for a reservoir to be
    wrong, and the decoders handle it by silence, so it would pass a
    comparison that did not look.
  - **The recording is in there**: aligned by the tag's delay, the decode
    correlates with the input to a floor chosen per signal. This is the
    check that a stream decoding to silence, or to someone else's samples,
    cannot pass.

**The controls** are the same checks run on files that are wrong - a frame
dropped, a back-pointer larger than the data before it, a granule longer
than its frame - and each must be rejected, or the checks would pass an
encoder that wrote anything at all.

The signals are the ones a plain tone does not make: full-scale noise and
square waves, the highest frequency the format can hold, silence between
bursts, a channel that is silent or the other's negative, a recording one
least significant bit deep. They are generated here, in pure Python, so the
gate depends on nothing but the interpreter.
"""

import math
import os
import struct
import subprocess
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import check_mpeg  # noqa: E402
import oracle_env as oracle  # noqa: E402

ROOT = os.path.dirname(HERE)
ROOT = os.path.dirname(ROOT)
SCRATCH = os.path.join(ROOT, "build", "mp3enc-check")
WRITE_PROBE = os.path.join(ROOT, "build", "linux", "release", "apps", "oracle",
                           "write_probe")

#: How far apart two decoders of one file may be, in units of 16 bits.
MAX_SAMPLE = 2


class Lcg:
    def __init__(self, seed):
        self.state = seed & 0xFFFFFFFF

    def next(self):
        self.state = (self.state * 1664525 + 1013904223) & 0xFFFFFFFF
        return ((self.state >> 8) - 8388608) / 8388608.0


def clip(v):
    return max(-32768, min(32767, int(round(v))))


# ------------------------------------------------------------- the signals
# Each returns a list of channels, each a list of floats in 16-bit units.

def tone(n, rate, ch):
    return [[0.5 * 32767 * math.sin(2 * math.pi * 1000.0 * i / rate + c)
             for i in range(n)] for c in range(ch)]


def white(n, rate, ch):
    r = Lcg(5)
    return [[32767 * r.next() for _ in range(n)] for _ in range(ch)]


def square(n, rate, ch):
    period = max(2, int(rate / 100))
    return [[32767.0 if (i % period) < period // 2 else -32768.0
             for i in range(n)] for _ in range(ch)]


def nyquist(n, rate, ch):
    return [[32767.0 if i % 2 == 0 else -32768.0 for i in range(n)]
            for _ in range(ch)]


def bursts(n, rate, ch):
    out = []
    for c in range(ch):
        r = Lcg(9 + c)
        x = [0.0] * n
        t = rate // 8
        while t < n - rate // 8:
            for i in range(int(rate * 0.03)):
                if t + i < n:
                    x[t + i] = 24000 * r.next() * math.exp(-i / (0.005 * rate))
            t += int(rate * 0.3)
        out.append(x)
    return out


def lsb(n, rate, ch):
    r = Lcg(3)
    return [[float(round(r.next() * 1.4)) for _ in range(n)]
            for _ in range(ch)]


def left_only(n, rate, ch):
    t = tone(n, rate, 1)[0]
    return [t] + [[0.0] * n for _ in range(ch - 1)]


def anti_phase(n, rate, ch):
    t = tone(n, rate, 1)[0]
    return [t, [-v for v in t]] if ch == 2 else [t]


def chirp(n, rate, ch):
    return [[0.4 * 32767 * math.sin(math.pi * (rate / 2.2) * (i / rate) ** 2 / (n / rate))
             for i in range(n)] for _ in range(ch)]


def silence(n, rate, ch):
    return [[0.0] * n for _ in range(ch)]


#: (name, generator, minimum correlation with the input after alignment).
#: The floor is deliberately low where the signal is one a lossy encoder is
#: allowed to wreck - full-scale noise, one bit deep - and high where it is
#: not.
SIGNALS = [
    ("tone", tone, 0.98),
    ("white", white, 0.5),
    ("square", square, 0.85),
    ("nyquist", nyquist, 0.0),
    ("bursts", bursts, 0.5),
    ("lsb", lsb, 0.0),
    ("left_only", left_only, 0.98),
    ("anti_phase", anti_phase, 0.98),
    ("chirp", chirp, 0.9),
    ("silence", silence, 0.0),
]

#: (rate, channels, [(mode arguments, label)]).
CONFIGS = [
    (44100, 2, [(["128", "cbr"], "cbr128"), (["0", "vbr", "50"], "vbr50"),
                (["112", "abr"], "abr112")]),
    (48000, 2, [(["192", "cbr"], "cbr192"), (["0", "vbr", "20"], "vbr20")]),
    (32000, 1, [(["64", "cbr"], "cbr64")]),
    (22050, 1, [(["48", "cbr"], "cbr48"), (["0", "vbr", "70"], "vbr70")]),
    (16000, 2, [(["64", "cbr"], "cbr64")]),
    (11025, 1, [(["32", "cbr"], "cbr32")]),
    (8000, 1, [(["16", "abr"], "abr16"), (["8", "cbr"], "cbr8")]),
]


def write_wav(path, rate, channels):
    n = len(channels[0])
    with wave.open(path, "wb") as w:
        w.setnchannels(len(channels))
        w.setsampwidth(2)
        w.setframerate(rate)
        data = bytearray()
        for i in range(n):
            for c in channels:
                data += struct.pack("<h", clip(c[i]))
        w.writeframes(bytes(data))


def read_wav(path):
    with wave.open(path, "rb") as w:
        ch = w.getnchannels()
        raw = w.readframes(w.getnframes())
    flat = struct.unpack("<%dh" % (len(raw) // 2), raw)
    return [list(flat[c::ch]) for c in range(ch)]


# ----------------------------------------------------------- the file check

BITRATES = {
    3: [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],
    2: [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
    0: [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
}
RATES = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000],
         0: [11025, 12000, 8000]}


class Reader:
    def __init__(self, data):
        self.data = data
        self.at = 0

    def read(self, n):
        v = 0
        for _ in range(n):
            byte = self.data[self.at >> 3]
            v = (v << 1) | ((byte >> (7 - (self.at & 7))) & 1)
            self.at += 1
        return v


def frames_of(blob):
    """(offset, size, header fields, side info) of every frame."""
    out = []
    at = 0
    if blob[:3] == b"ID3":
        at = 10 + ((blob[6] & 127) << 21 | (blob[7] & 127) << 14
                   | (blob[8] & 127) << 7 | (blob[9] & 127))
    while at + 4 <= len(blob):
        h = blob[at:at + 4]
        if h[0] != 0xFF or (h[1] & 0xE0) != 0xE0:
            raise ValueError("no frame at %d" % at)
        version = (h[1] >> 3) & 3
        index = h[2] >> 4
        kbps = BITRATES[version][index]
        rate = RATES[version][(h[2] >> 2) & 3]
        pad = (h[2] >> 1) & 1
        mode = h[3] >> 6
        channels = 1 if mode == 3 else 2
        size = (144 if version == 3 else 72) * kbps * 1000 // rate + pad
        lsf = version != 3
        side_size = (9 if channels == 1 else 17) if lsf else (17 if channels == 1 else 32)
        side = blob[at + 4:at + 4 + side_size]
        out.append((at, size, dict(version=version, kbps=kbps, rate=rate,
                                   channels=channels, lsf=lsf, mode=mode,
                                   ext=(h[3] >> 4) & 3, side_size=side_size),
                    side))
        at += size
    if at != len(blob):
        raise ValueError("the last frame ends at %d, the file at %d" % (at, len(blob)))
    return out


def parse_side(info, side):
    r = Reader(side)
    lsf = info["lsf"]
    channels = info["channels"]
    back = r.read(8 if lsf else 9)
    r.read((1 if channels == 1 else 2) if lsf else (5 if channels == 1 else 3))
    if not lsf:
        for _ in range(channels):
            r.read(4)
    lengths = []
    for _ in range(1 if lsf else 2):
        for _ in range(channels):
            lengths.append(r.read(12))
            r.read(9 + 8 + (9 if lsf else 4))
            if r.read(1):
                r.read(2 + 1 + 10 + 9)
            else:
                r.read(15 + 4 + 3)
            r.read(0 if lsf else 1)
            r.read(2)
    return back, lengths


def check_reservoir(blob):
    """Problems with the back-pointers and granule lengths, as strings."""
    bad = []
    frames = frames_of(blob)
    available = 0  # main-data bytes the frames so far have supplied
    for number, (at, size, info, side) in enumerate(frames[1:], 1):
        back, lengths = parse_side(info, side)
        payload = size - 4 - info["side_size"]
        if back > available:
            bad.append("frame %d reaches back %d bytes and only %d precede it"
                       % (number, back, available))
        limit = 255 if info["lsf"] else 511
        if back > limit:
            bad.append("frame %d's back-pointer %d is past %d" % (number, back, limit))
        if sum(lengths) > 8 * (back + payload):
            bad.append("frame %d's granules hold %d bits and it has %d"
                       % (number, sum(lengths), 8 * (back + payload)))
        available += payload
    return bad


def tag_check(blob, input_frames, delay_expected):
    bad = []
    frames = frames_of(blob)
    at, size, info, side = frames[0]
    pos = at + 4 + info["side_size"]
    kind = blob[pos:pos + 4]
    if kind not in (b"Info", b"Xing"):
        return ["no Info or Xing tag in the first frame"]
    flags = int.from_bytes(blob[pos + 4:pos + 8], "big")
    n_frames = int.from_bytes(blob[pos + 8:pos + 12], "big")
    n_bytes = int.from_bytes(blob[pos + 12:pos + 16], "big")
    if n_frames != len(frames) - 1:
        bad.append("the tag says %d frames and the file has %d" % (n_frames, len(frames) - 1))
    if n_bytes != len(blob):
        bad.append("the tag says %d bytes and the file is %d" % (n_bytes, len(blob)))
    cursor = pos + 16
    if flags & 4:
        cursor += 100
    if flags & 8:
        cursor += 4
    lame = blob[cursor:cursor + 36]
    delay = (lame[21] << 4) | (lame[22] >> 4)
    padding = ((lame[22] & 15) << 8) | lame[23]
    samples_per_frame = 576 if info["lsf"] else 1152
    decoded = n_frames * samples_per_frame
    if delay != delay_expected:
        bad.append("the tag's delay is %d, expected %d" % (delay, delay_expected))
    if delay + 529 + input_frames + (padding - 529 if padding > 529 else 0) != decoded:
        bad.append("delay %d + 529 + %d samples + padding %d does not make %d"
                   % (delay, input_frames, padding, decoded))
    return bad


# --------------------------------------------------------------- the checks

def correlation(a, b):
    """Normalised correlation of two equal-length sample lists."""
    sa = sum(x * x for x in a)
    sb = sum(x * x for x in b)
    if sa == 0 and sb == 0:
        return 1.0
    if sa == 0 or sb == 0:
        return 0.0
    return sum(x * y for x, y in zip(a, b)) / math.sqrt(sa * sb)


def mpg123_pcm(path):
    """libmpg123's decode, through its command-line front end.

    Gapless is on by default and is the point: it trims by the delay and
    padding a LAME extension states, and - measured against the pinned image
    - without looking at the name the tag carries, unlike ffmpeg.
    """
    finished = subprocess.run(
        oracle.command("mpg123", ["mpg123", "-q", "-s", path]),
        capture_output=True)
    if finished.returncode != 0 or not finished.stdout:
        return None
    raw = finished.stdout
    return list(struct.unpack("<%dh" % (len(raw) // 2), raw))


def decode_all(path):
    out = {}
    out["mpg123"] = mpg123_pcm(path)
    out["ours"] = check_mpeg.ours_pcm(path)
    out["ffmpeg"] = check_mpeg.ffmpeg_pcm(path)
    out["libsndfile"] = check_mpeg.libsndfile_pcm(path)
    return out


def run_case(label, wav, mp3, rate, channels, mode_args, floor, signal_name):
    bad = []
    finished = subprocess.run([WRITE_PROBE, wav, mp3, "mp3", "mp3"] + mode_args,
                              capture_output=True, text=True)
    if finished.returncode != 0:
        return ["%s: the encoder refused: %s" % (label, finished.stderr.strip())]
    blob = open(mp3, "rb").read()
    try:
        bad += ["%s: %s" % (label, p) for p in check_reservoir(blob)]
        wav_channels = read_wav(wav)
        n = len(wav_channels[0])
        bad += ["%s: %s" % (label, p) for p in tag_check(blob, n, 1057 - 529)]
    except ValueError as why:
        return ["%s: %s" % (label, why)]
    pcm = decode_all(mp3)
    if pcm["ours"] is None:
        return bad + ["%s: our decoder could not read the file" % label]
    meta = check_mpeg.ours_meta(mp3)
    # Every reference returns the recording: ffmpeg honours the delay and
    # padding of a tag whose encoder name begins LAME (ours does, on purpose;
    # see mp3enc_tag.c), and libsndfile and mpg123 honour any tag that states
    # them. Each is compared with our decode cut to match, and the cut has to
    # leave exactly the frames that were given.
    trimmed = check_mpeg.trim(pcm["ours"], int(meta["delay"]),
                              int(meta["padding"]), channels)
    expected = {"ffmpeg": trimmed, "libsndfile": trimmed, "mpg123": trimmed}
    if len(trimmed) != n * channels:
        bad.append("%s: the tag's delay and padding leave %d frames of the "
                   "%d that were given" % (label, len(trimmed) // channels, n))
    for name in ("ffmpeg", "libsndfile", "mpg123"):
        if pcm[name] is None:
            bad.append("%s: %s could not read a file we wrote" % (label, name))
            continue
        mine = expected[name]
        if len(pcm[name]) != len(mine):
            bad.append("%s: %s decodes %d frames and we decode %d"
                       % (label, name, len(pcm[name]) // channels,
                          len(mine) // channels))
            continue
        worst = max((abs(a - b) for a, b in zip(mine, pcm[name])), default=0)
        if worst > MAX_SAMPLE:
            at = max(range(len(mine)), key=lambda i: abs(mine[i] - pcm[name][i]))
            bad.append("%s: %s differs from us by %d at sample %d"
                       % (label, name, worst, at))
    # The recording, by the tag's delay.
    skip = 1057
    for c in range(channels):
        decoded = pcm["ours"][c::channels][skip:skip + n]
        original = wav_channels[c][:len(decoded)]
        r = correlation(decoded, original)
        if r < floor:
            bad.append("%s: channel %d correlates %.3f with the input, and "
                       "the floor for %s is %.2f" % (label, c, r, signal_name, floor))
    return bad


def controls(good_mp3):
    """Files that are wrong, which the checks must reject."""
    bad = []
    blob = bytearray(open(good_mp3, "rb").read())
    frames = frames_of(bytes(blob))
    if check_reservoir(bytes(blob)):
        return ["control: the file it starts from is not clean"]

    # A back-pointer larger than what precedes the frame.
    broken = bytearray(blob)
    at, size, info, side = frames[1]
    broken[at + 4] = 0xFF
    if not check_reservoir(bytes(broken)):
        bad.append("control passed that must fail: a back-pointer past the start")

    # Granules longer than their frame (59 bits of side information each,
    # for an ordinary granule): every length field set to its
    # largest, which no frame can hold four of.
    broken = bytearray(blob)
    at, size, info, side = frames[2]
    first = 9 + (5 if info["channels"] == 1 else 3) + 4 * info["channels"]
    fields = 2 * info["channels"]
    for i in range(fields):
        bit = 8 * (at + 4) + first + i * 59
        for k in range(12):
            broken[(bit + k) >> 3] |= 0x80 >> ((bit + k) & 7)
    if not check_reservoir(bytes(broken)):
        bad.append("control passed that must fail: a granule longer than its frame")

    # A frame dropped: the tag then counts one too many.
    dropped = bytes(blob[:frames[3][0]] + blob[frames[4][0]:])
    if not tag_check(dropped, 1, 1057 - 529):
        bad.append("control passed that must fail: a frame dropped")
    return bad


def main():
    print(oracle.provenance(["ffmpeg", "libsndfile", "mpg123"]))
    if not os.path.isfile(WRITE_PROBE):
        raise SystemExit("write_probe is not built: run `make oracle-probe`.")
    os.makedirs(SCRATCH, exist_ok=True)
    failures = []
    cases = 0
    good = None
    for rate, channels, modes in CONFIGS:
        n = int(rate * 1.2)
        for name, generator, floor in SIGNALS:
            if name in ("left_only", "anti_phase") and channels == 1:
                continue
            wav = os.path.join(SCRATCH, "%s_%d_%d.wav" % (name, rate, channels))
            write_wav(wav, rate, generator(n, rate, channels))
            for args, label in modes:
                mp3 = os.path.join(SCRATCH, "%s_%d_%d_%s.mp3"
                                   % (name, rate, channels, label))
                problems = run_case("%s %d Hz x%d %s" % (name, rate, channels, label),
                                    wav, mp3, rate, channels, args, floor, name)
                failures += problems
                cases += 1
                if good is None and name == "tone" and rate == 44100:
                    good = mp3
    failures += controls(good)
    print("%d encodes, each read by three decoders that are not ours (two implementations: libsndfile reaches MP3 through libmpg123); every "
          "frame's back-pointer, granule lengths and tag checked in the bytes; "
          "3 controls rejected" % cases)
    if failures:
        print("\n".join("FAIL " + f for f in failures))
        return 1
    print("check-mp3-encode: every file the encoder wrote is read the same way by"
          " ffmpeg, libsndfile, mpg123 and this library")
    return 0


if __name__ == "__main__":
    sys.exit(main())
