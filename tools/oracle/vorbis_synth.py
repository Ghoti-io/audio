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
"""Write Ogg Vorbis streams that use the parts of the format no encoder does.

    vorbis_synth.py OUTDIR

No encoder in the oracle image emits a floor of type 0 or a residue of type
0, at any setting, so the decoder's handling of them has nothing to be
compared against - unless a stream that uses them is written by hand. This
is that writer. It is **not an encoder**: it never looks at a signal. It
chooses codebook entries, amplitudes and line spectral pair vectors with a
seeded generator and writes them down in the order the specification says a
decoder will read them, so that what comes out is a legal stream whose
meaning is whatever the specification says it is. Two independent decoders
(ffmpeg's native one and libvorbis) are then asked what that is, and this
library's answer is scored against theirs.

The streams are deterministic: the same arguments write the same bytes.

Everything here is from the Vorbis I specification (xiph.org), sections
3 (bitpacking and codebooks), 4.2 (the three headers), 6 (floors), 8
(residues), and the packet layout of 4.3. The Ogg framing is RFC 3533.
"""

import math
import os
import random
import struct
import sys

# ---------------------------------------------------------------- bit I/O


class Bits:
    """Bits packed least-significant first, as Vorbis does."""

    def __init__(self):
        self.data = bytearray()
        self.used = 0  # bits in the last byte

    def write(self, value, width):
        value &= (1 << width) - 1 if width else 0
        for i in range(width):
            if self.used == 0:
                self.data.append(0)
            if (value >> i) & 1:
                self.data[-1] |= 1 << self.used
            self.used = (self.used + 1) % 8

    def code(self, word, length):
        """A Huffman codeword: its first bit is its most significant."""
        for i in range(length - 1, -1, -1):
            self.write((word >> i) & 1, 1)

    def bytes(self):
        return bytes(self.data)


def ilog(x):
    """The specification's ilog: how many bits `x` occupies."""
    n = 0
    while x > 0:
        n += 1
        x >>= 1
    return n


def float32_pack(value):
    """The specification's float32_pack: 21-bit mantissa, 10-bit exponent."""
    if value == 0:
        return 0
    sign = 0x80000000 if value < 0 else 0
    mantissa, exponent = math.frexp(abs(value))  # 0.5 <= m < 1
    mantissa = int(round(mantissa * (1 << 21)))
    exponent -= 21
    if mantissa == (1 << 21):
        mantissa >>= 1
        exponent += 1
    return sign | ((exponent + 788) << 21) | mantissa


def float32_unpack(packed):
    mantissa = packed & 0x1FFFFF
    exponent = (packed & 0x7FE00000) >> 21
    value = mantissa * 2.0 ** (exponent - 788)
    return -value if packed & 0x80000000 else value


# ------------------------------------------------------------- codebooks


class Book:
    """One codebook: a prefix code and, optionally, a vector per entry.

    `lengths` has a codeword length per entry (0 for unused). `lookup` is
    None, or a dict with `type` 1 or 2, `minimum`, `delta`, `bits`,
    `sequence` and `multiplicands`.
    """

    def __init__(self, dimensions, lengths, lookup=None):
        self.dimensions = dimensions
        self.lengths = list(lengths)
        self.entries = len(lengths)
        self.lookup = lookup
        self.words = self._assign()

    def _assign(self):
        """The specification's codeword assignment (libvorbis's _make_words)."""
        marker = [0] * 33
        words = [None] * self.entries
        for i, length in enumerate(self.lengths):
            if length == 0:
                continue
            entry = marker[length]
            if length < 32 and (entry >> length):
                raise ValueError("overspecified codebook")
            words[i] = entry
            for j in range(length, 0, -1):
                if marker[j] & 1:
                    if j == 1:
                        marker[1] += 1
                    else:
                        marker[j] = marker[j - 1] << 1
                    break
                marker[j] += 1
            for j in range(length + 1, 33):
                if (marker[j] >> 1) == entry:
                    entry = marker[j]
                    marker[j] = marker[j - 1] << 1
                else:
                    break
        return words

    def lookup_values(self):
        """Multiplicands a lookup states."""
        if self.lookup["type"] == 2:
            return self.entries * self.dimensions
        v = int(round(self.entries ** (1.0 / self.dimensions)))
        while v ** self.dimensions > self.entries:
            v -= 1
        while (v + 1) ** self.dimensions <= self.entries:
            v += 1
        return v

    def vector(self, entry):
        """The floats a decoder computes for an entry (float64)."""
        lk = self.lookup
        minimum = float32_unpack(float32_pack(lk["minimum"]))
        delta = float32_unpack(float32_pack(lk["delta"]))
        out = []
        last = 0.0
        if lk["type"] == 1:
            lv = self.lookup_values()
            divisor = 1
            for _ in range(self.dimensions):
                offset = (entry // divisor) % lv
                value = lk["multiplicands"][offset] * delta + minimum + last
                out.append(value)
                if lk["sequence"]:
                    last = value
                divisor *= lv
        else:
            base = entry * self.dimensions
            for j in range(self.dimensions):
                value = lk["multiplicands"][base + j] * delta + minimum + last
                out.append(value)
                if lk["sequence"]:
                    last = value
        return out

    def pack(self, bits):
        bits.write(0x564342, 24)
        bits.write(self.dimensions, 16)
        bits.write(self.entries, 24)
        bits.write(0, 1)  # not ordered
        sparse = 1 if any(length == 0 for length in self.lengths) else 0
        bits.write(sparse, 1)
        for length in self.lengths:
            if sparse:
                bits.write(1 if length else 0, 1)
                if length:
                    bits.write(length - 1, 5)
            else:
                bits.write(length - 1, 5)
        if self.lookup is None:
            bits.write(0, 4)
            return
        lk = self.lookup
        bits.write(lk["type"], 4)
        bits.write(float32_pack(lk["minimum"]), 32)
        bits.write(float32_pack(lk["delta"]), 32)
        bits.write(lk["bits"] - 1, 4)
        bits.write(1 if lk["sequence"] else 0, 1)
        for m in lk["multiplicands"]:
            bits.write(m, lk["bits"])

    def write_entry(self, bits, entry):
        if self.lengths[entry] == 0:
            raise ValueError("unused entry")
        bits.code(self.words[entry], self.lengths[entry])


def uniform_book(dimensions, entries, lookup=None):
    """A book whose entries all have the same length; entries a power of 2."""
    length = ilog(entries - 1) if entries > 1 else 1
    if (1 << length) != entries and entries > 1:
        raise ValueError("entries must be a power of two")
    return Book(dimensions, [length] * entries, lookup)


# --------------------------------------------------------------- the setup


class Spec:
    """What to write: a mono or stereo stream with one floor and one residue."""

    def __init__(self, **kw):
        self.channels = kw.get("channels", 1)
        self.rate = kw.get("rate", 44100)
        self.blocksizes = kw.get("blocksizes", (256, 2048))
        self.frames_blocks = kw.get("blocks", 24)
        self.seed = kw.get("seed", 1)
        # Floor 0.
        self.order = kw.get("order", 16)
        self.floor_rate = kw.get("floor_rate", self.rate)
        self.bark_map = kw.get("bark_map", 256)
        self.amp_bits = kw.get("amp_bits", 6)
        self.amp_offset = kw.get("amp_offset", 100)
        self.amp_range = kw.get("amp_range", (1, 4))
        self.floor_books = kw.get("floor_books", [0])  # indices into books
        # Residue.
        self.residue_type = kw.get("residue_type", 1)
        self.partition_size = kw.get("partition_size", 16)
        self.residue_scale = kw.get("residue_scale", 250.0)
        # Which block sizes appear: list of 0/1 per block (None = random).
        self.block_pattern = kw.get("block_pattern")
        self.floor_type = kw.get("floor_type", 0)
        # Stereo with the two channels polar-coupled (section 1.3.3).
        self.coupled = kw.get("coupled", False)
        if self.coupled and self.channels != 2:
            raise ValueError("coupling needs two channels")
        self.dimension_a = kw.get("dimension_a", 4)
        self.lattice_a = kw.get("lattice_a", 8)
        self.sequence_a = kw.get("sequence_a", True)
        self.lookup_a = kw.get("lookup_a", 1)
        # Line spectral pairs spread evenly over the half circle with a little
        # jitter, which is what keeps the curve's denominator away from zero:
        # each step between neighbours is `average` give or take `spread`.
        self.average = 3.0 / (self.order + 1)
        self.spread = kw.get("spread", 0.14)


def make_books(spec):
    """The codebooks: 0 for the floor's vectors, 1 the residue's classes, 2 its values."""
    rng = random.Random(spec.seed * 7919 + 1)
    books = []
    dim = spec.dimension_a
    avg = spec.average
    if spec.lookup_a == 1:
        if not spec.sequence_a:
            raise ValueError("a lattice cannot state a rising run without sequence_p")
        lv = spec.lattice_a
        entries = lv ** dim
        # Entries must be a power of two for the uniform code.
        if entries & (entries - 1):
            raise ValueError("lattice^dimension must be a power of two")
        lookup = {
            "type": 1, "minimum": avg * (1 - spec.spread),
            "delta": 2 * spec.spread * avg / (lv - 1) if lv > 1 else 0.0,
            "bits": ilog(lv - 1) or 1, "sequence": True,
            "multiplicands": list(range(lv)),
        }
        books.append(uniform_book(dim, entries, lookup))
    else:
        entries = 64
        if spec.sequence_a:
            # Each element is a step on the one before it.
            lv = 16
            mults = [rng.randrange(0, lv) for _ in range(entries * dim)]
            lookup = {
                "type": 2, "minimum": avg * (1 - spec.spread),
                "delta": 2 * spec.spread * avg / (lv - 1), "bits": 4,
                "sequence": True, "multiplicands": mults,
            }
        else:
            # Each vector is a ramp, so that the next one, offset by the
            # last element of this, carries on rising.
            mults = []
            for _ in range(entries):
                for j in range(dim):
                    jitter = 1 + spec.spread * (2 * rng.random() - 1)
                    mults.append(int(round((j + 1) * 8 * jitter)))
            lookup = {
                "type": 2, "minimum": 0.0, "delta": avg / 8, "bits": 8,
                "sequence": False, "multiplicands": mults,
            }
        books.append(uniform_book(dim, entries, lookup))
    # The residue's classification book: one dimension, two classes.
    books.append(uniform_book(1, 2))
    # The residue's value book: dimension 2, a 3-value lattice, 9 entries.
    lookup = {
        "type": 1, "minimum": -spec.residue_scale, "delta": spec.residue_scale,
        "bits": 2,
        "sequence": False, "multiplicands": [0, 1, 2],
    }
    books.append(Book(2, [3, 3, 3, 3, 3, 3, 3, 4, 4], lookup))
    return books


class Setup:
    def __init__(self, spec):
        self.spec = spec
        self.books = make_books(spec)

    def identification(self):
        s = self.spec
        b = Bits()
        b.write(1, 8)
        for c in b"vorbis":
            b.write(c, 8)
        b.write(0, 32)
        b.write(s.channels, 8)
        b.write(s.rate, 32)
        b.write(0, 32)
        b.write(0, 32)
        b.write(0, 32)
        b.write(ilog(s.blocksizes[0]) - 1, 4)
        b.write(ilog(s.blocksizes[1]) - 1, 4)
        b.write(1, 1)
        return b.bytes()

    def comment(self):
        b = Bits()
        b.write(3, 8)
        for c in b"vorbis":
            b.write(c, 8)
        vendor = b"vorbis_synth.py"
        b.write(len(vendor), 32)
        for c in vendor:
            b.write(c, 8)
        b.write(0, 32)
        b.write(1, 1)
        return b.bytes()

    def setup(self):
        s = self.spec
        b = Bits()
        b.write(5, 8)
        for c in b"vorbis":
            b.write(c, 8)
        b.write(len(self.books) - 1, 8)
        for book in self.books:
            book.pack(b)
        b.write(0, 6)   # one time domain transform
        b.write(0, 16)
        # One floor.
        b.write(0, 6)
        b.write(s.floor_type, 16)
        if s.floor_type != 0:
            raise ValueError("only floor 0 is written here")
        b.write(s.order, 8)
        b.write(s.floor_rate, 16)
        b.write(s.bark_map, 16)
        b.write(s.amp_bits, 6)
        b.write(s.amp_offset, 8)
        b.write(len(s.floor_books) - 1, 4)
        for n in s.floor_books:
            b.write(n, 8)
        # Two residues, one for each block size, because the specification
        # clips a residue's end to the block's spectrum and ffmpeg's decoder
        # instead refuses a block it is longer than; encoders write one per
        # size, so that is what is written.
        b.write(1, 6)
        for flag in (0, 1):
            b.write(s.residue_type, 16)
            b.write(0, 24)                          # begin
            b.write(self.residue_end(flag), 24)     # end
            b.write(s.partition_size - 1, 24)
            b.write(1, 6)                           # two classes
            b.write(1, 8)                           # the classification book
            # Class 0 has no value book, class 1 has book 2 in pass 0.
            b.write(0, 3)
            b.write(0, 1)
            b.write(1, 3)
            b.write(0, 1)
            b.write(2, 8)
        # Two mappings, one per block size, no coupling, one submap each.
        b.write(1, 6)
        for flag in (0, 1):
            b.write(0, 16)
            b.write(0, 1)                     # one submap
            if s.coupled:
                b.write(1, 1)                 # coupling
                b.write(0, 8)                 # one step
                b.write(0, ilog(s.channels - 1))   # magnitude: channel 0
                b.write(1, ilog(s.channels - 1))   # angle: channel 1
            else:
                b.write(0, 1)                 # no coupling
            b.write(0, 2)
            b.write(0, 8)                     # time
            b.write(0, 8)                     # floor 0
            b.write(flag, 8)                  # residue for this size
        # Two modes, short and long, each with its own mapping.
        b.write(1, 6)
        for flag in (0, 1):
            b.write(flag, 1)
            b.write(0, 16)
            b.write(0, 16)
            b.write(flag, 8)
        b.write(1, 1)
        return b.bytes()

    def residue_end(self, flag):
        """Where the residue stops: the block's spectrum, all channels for type 2."""
        s = self.spec
        lines = s.blocksizes[flag] // 2
        return lines * s.channels if s.residue_type == 2 else lines

    # ---------------------------------------------------------- the audio

    def audio_packets(self):
        s = self.spec
        rng = random.Random(s.seed)
        pattern = s.block_pattern
        if pattern is None:
            pattern = [rng.randrange(2) if k else 0 for k in range(s.frames_blocks)]
        packets = []
        sizes = []
        for k, flag in enumerate(pattern):
            prev_long = pattern[k - 1] if k else flag
            next_long = pattern[k + 1] if k + 1 < len(pattern) else flag
            n = s.blocksizes[flag]
            sizes.append(n)
            b = Bits()
            b.write(0, 1)
            b.write(flag, 1)  # mode number: one bit for two modes
            if flag:
                b.write(prev_long, 1)
                b.write(next_long, 1)
            used = [self._floor(b, rng) for _ in range(s.channels)]
            if s.coupled and (used[0] or used[1]):
                # A coupling step reads both of its channels, so if either
                # carries something both have their residue decoded - the
                # silent one's spectrum is still zero, having no floor.
                used = [True, True]
            self._residue(b, rng, n // 2, used, flag)
            packets.append(b.bytes())
        return packets, sizes

    def _floor(self, b, rng):
        s = self.spec
        # One packet in six has a silent channel.
        if rng.randrange(6) == 0:
            b.write(0, s.amp_bits)
            return False
        amplitude = rng.randrange(s.amp_range[0], min(s.amp_range[1], (1 << s.amp_bits) - 1) + 1)
        b.write(amplitude, s.amp_bits)
        b.write(rng.randrange(len(s.floor_books)), ilog(len(s.floor_books)))
        book = self.books[s.floor_books[0]]
        count = 0
        while count < s.order:
            book.write_entry(b, rng.randrange(book.entries))
            count += book.dimensions
        return True

    def _residue(self, b, rng, lines, used, flag):
        """One packet's residue. Channels whose floor was unused are not coded."""
        s = self.spec
        part = s.partition_size
        if s.residue_type == 2:
            # One interleaved vector, decoded unless every channel is silent.
            if not any(used):
                return
            vectors = 1
            total = lines * s.channels
        else:
            vectors = sum(1 for u in used if u)
            if vectors == 0:
                return
            total = lines
        end = min(self.residue_end(flag), total)
        partitions = end // part
        vbook = self.books[2]
        cbook = self.books[1]
        classes = [[rng.randrange(2) for _ in range(partitions)]
                   for _ in range(vectors)]
        for p in range(partitions):
            for v in range(vectors):
                cbook.write_entry(b, classes[v][p])
            for v in range(vectors):
                if classes[v][p] == 1:
                    # Type 0 interleaves the vectors across the partition and
                    # type 1 lays them end to end; the same count either way.
                    for _ in range(part // vbook.dimensions):
                        vbook.write_entry(b, rng.randrange(vbook.entries))


# ------------------------------------------------------------------- Ogg

_CRC = []
for i in range(256):
    r = i << 24
    for _ in range(8):
        r = ((r << 1) ^ 0x04C11DB7) if r & 0x80000000 else (r << 1)
        r &= 0xFFFFFFFF
    _CRC.append(r)


def crc32(data):
    r = 0
    for byte in data:
        r = ((r << 8) ^ _CRC[((r >> 24) & 0xFF) ^ byte]) & 0xFFFFFFFF
    return r


def page(serial, sequence, flags, granule, packets):
    segments = bytearray()
    body = bytearray()
    for packet in packets:
        n = len(packet)
        while n >= 255:
            segments.append(255)
            n -= 255
        segments.append(n)
        body += packet
    header = bytearray(b"OggS")
    header += bytes([0, flags])
    header += struct.pack("<q", granule)
    header += struct.pack("<III", serial, sequence, 0)
    header.append(len(segments))
    header += segments
    out = bytearray(header + body)
    out[22:26] = struct.pack("<I", crc32(out))
    return bytes(out)


def write_stream(spec):
    setup = Setup(spec)
    packets, sizes = setup.audio_packets()
    serial = 0x6F726B73
    out = bytearray()
    out += page(serial, 0, 2, 0, [setup.identification()])
    out += page(serial, 1, 0, 0, [setup.comment(), setup.setup()])
    # The granule position after each packet, by the specification.
    granule = 0
    previous = None
    positions = []
    for n in sizes:
        if previous is not None:
            granule += previous // 4 + n // 4
        previous = n
        positions.append(granule)
    sequence = 2
    per_page = 6
    for first in range(0, len(packets), per_page):
        chunk = packets[first:first + per_page]
        last = first + per_page >= len(packets)
        out += page(serial, sequence, 4 if last else 0,
                    positions[first + len(chunk) - 1], chunk)
        sequence += 1
    return bytes(out), positions[-1]


#: The fixtures, by name. Each is deterministic: the same arguments always
#: write the same bytes, which `make check-vorbis-synth` asserts against
#: what is committed. **Each is chosen for one thing it reaches that the
#: encoders' streams do not**, and a stream is not asked to sound like
#: anything.
#:
#: Loudness is chosen too: every one of these is well inside full scale,
#: because a stream that clips tells two decoders nothing - a sample of
#: +32,767 is the same answer however far over it each of them was.
CASES = [
    # The default: order 16, a four-dimensional lattice book, mono, both
    # block sizes, residue type 1.
    ("vorbis_syn_floor0_mono", dict(seed=1)),
    ("vorbis_syn_floor0_stereo", dict(seed=2, channels=2)),
    # Residue type 2 interleaves the channels into one vector.
    ("vorbis_syn_floor0_stereo_res2",
     dict(seed=3, channels=2, residue_type=2)),
    # Residue type 0, which no encoder writes: a partition's vectors are
    # laid across it rather than end to end.
    ("vorbis_syn_res0_mono", dict(seed=4, residue_type=0)),
    ("vorbis_syn_res0_stereo",
     dict(seed=5, channels=2, residue_type=0, partition_size=8,
          dimension_a=2, lattice_a=8)),
    # Orders and book dimensions, odd and even, one to sixty-four: the
    # product in the curve has a different last factor for each parity.
    ("vorbis_syn_floor0_order8", dict(seed=6, order=8, dimension_a=2,
                                      lattice_a=8)),
    ("vorbis_syn_floor0_order17_dim1", dict(seed=7, order=17, dimension_a=1,
                                            lattice_a=16)),
    ("vorbis_syn_floor0_order32_dim8", dict(seed=8, order=32, dimension_a=8,
                                            lattice_a=2, spread=0.1,
                                            residue_scale=3.0)),
    ("vorbis_syn_floor0_order64", dict(seed=9, order=64, spread=0.03,
                                       amp_bits=8, amp_range=(1, 2))),
    # Books that state every entry's vector (lookup type 2), and one whose
    # vectors are ramps rather than steps (no sequence_p). ffmpeg's native
    # decoder refuses the first kind; libvorbis reads both.
    ("vorbis_syn_floor0_explicit_seq", dict(seed=10, lookup_a=2)),
    ("vorbis_syn_floor0_explicit_ramp",
     dict(seed=11, lookup_a=2, sequence_a=False)),
    # A low sampling rate, and a floor whose own rate is not the stream's.
    ("vorbis_syn_floor0_8000", dict(seed=12, rate=8000, floor_rate=8000,
                                    blocksizes=(256, 1024))),
    ("vorbis_syn_floor0_rate_mismatch",
     dict(seed=13, rate=44100, floor_rate=16000)),
    # Equal block sizes, so the window never changes; and a one-band map.
    ("vorbis_syn_floor0_equal_blocks",
     dict(seed=14, blocksizes=(512, 512), block_pattern=[0] * 24, rate=22050,
          floor_rate=22050)),
    ("vorbis_syn_floor0_barkmap1", dict(seed=15, bark_map=1)),
    # Two channels polar-coupled, one of them often silent while the other
    # is not: the arm where a channel is used for the residue and has no
    # floor of its own, which no encoder here produces.
    ("vorbis_syn_floor0_coupled",
     dict(seed=17, channels=2, coupled=True, blocks=40)),
    ("vorbis_syn_floor0_coupled_res2",
     dict(seed=18, channels=2, coupled=True, residue_type=2, blocks=40,
          residue_scale=120.0)),
    ("vorbis_syn_floor0_big_blocks", dict(seed=16, blocksizes=(256, 8192),
                                          blocks=16)),
]


def write_cases(outdir):
    """Write every fixture; return {name: (bytes, frames)}."""
    os.makedirs(outdir, exist_ok=True)
    made = {}
    for name, kw in CASES:
        data, frames = write_stream(Spec(**kw))
        with open(os.path.join(outdir, name + ".ogg"), "wb") as f:
            f.write(data)
        made[name] = (data, frames)
    return made


def main(argv):
    outdir = argv[1] if len(argv) > 1 else "."
    for name, (data, frames) in write_cases(outdir).items():
        print("%-36s %6d bytes %6d frames" % (name, len(data), frames))


if __name__ == "__main__":
    main(sys.argv)
