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
"""Where RFC 6716's conformance vectors are, and how they are pinned.

These are not fixtures and they are not in the repository. They are 75 MB
of normative test data published alongside the codec, and `tests/data/`
holds 120 files totalling a few hundred kilobytes - putting these there
would multiply the checkout by a hundred to carry data that never changes
and that only one gate reads.

So they are fetched deliberately, by `make opus-vectors`, into a
directory outside the tree, and the gate that reads them fails with
instructions when they are absent rather than fetching them itself. That
is the same arrangement `make corpus` has and for the same reason: a gate
that silently downloads 75 MB the first time it runs is a gate whose
first run means something different from its second.

**These are RFC 8251's vectors**, which replace RFC 6716's: the
bitstreams are the same twelve, and each has two decoded outputs, the
second (`m`) without the 180-degree phase shift of intensity stereo that
RFC 8251 section 10 lets a decoder skip when downmixing. A decoder
passes by matching either. **The pin is a hash of the archive, not a
version**: a name and a date are not an identity. The SHA-256 below is,
and `--check` verifies it without unpacking anything.

The per-file hashes are listed as well, because the archive is unpacked
once and read many times; a gate that trusted the tarball hash and then
read a directory somebody had edited would be pinning the download
rather than the data.
"""

import hashlib
import os
import sys
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

URL = ("https://www.ietf.org/proceedings/98/slides/"
       "materials-98-codec-opus-newvectors-00.tar.gz")

ARCHIVE_SHA256 = \
    "6b26a22f9ba87b2b836906a9bb7afec5f8e54d49553b1200382520ee6fedfa55"

ARCHIVE_BYTES = 74624664

# Twelve vectors, each a pair: the packets and the reference decoder's
# output at 48 kHz stereo. Both halves are hashed - the .dec files are
# what `opus_compare` is run against, so a wrong one would pass a
# decoder that was wrong in the same way.
VECTORS = [f"testvector{n:02d}" for n in range(1, 13)]

FILE_SHA256 = {
    "testvector01.bit":
        "00a4d34e12a2a32a047bfcaf762f663430b2232bc76a1c21e3b598f060f99b7e",
    "testvector01.dec":
        "c4b8b31ede5a2f32e0daf445073d2765533a93e45656a8eb36bc288f05bafd34",
    "testvector01m.dec":
        "43ed3123cf34d9c45c784a6e2658c75a934783d13f4708fbe32922e1755a167a",
    "testvector02.bit":
        "285deaf78e6b1ebdde7dc6a76af279a08bf0fa7a13e3a84e87ac3f76213626a6",
    "testvector02.dec":
        "4ad3b7aeaea242acc96cafebee69edb0914abc2e46c0c884f812ce10bbee4dbc",
    "testvector02m.dec":
        "4ad3b7aeaea242acc96cafebee69edb0914abc2e46c0c884f812ce10bbee4dbc",
    "testvector03.bit":
        "811b3d28483c781e4d1a0f8008bf73c3882c6c9cf8dedc65c0c14fa4239bb9d9",
    "testvector03.dec":
        "e00f4ee27f007d849fb67f4386db93254631d2ff31b580394d854819560f1bc7",
    "testvector03m.dec":
        "e00f4ee27f007d849fb67f4386db93254631d2ff31b580394d854819560f1bc7",
    "testvector04.bit":
        "874b838699a471acd97bd13e11224a3ef71c95b018b85a9c93a41459a7185565",
    "testvector04.dec":
        "ce7450716f4911284332e76dab66518dcfd5c84e320c0a8a54014e658f5c6908",
    "testvector04m.dec":
        "ce7450716f4911284332e76dab66518dcfd5c84e320c0a8a54014e658f5c6908",
    "testvector05.bit":
        "9b13f677c61cfad9a94460f986401b843e07e3245265067580539d9d8f1ec230",
    "testvector05.dec":
        "21a63bd7b9eefc228cf18c312f9fbe144e02a8d696306cb93575ece97e05ea0f",
    "testvector05m.dec":
        "2dccbba51402ffae88e68a060a18bf719c3becbbb745cdc4b19faccc49ca2bf3",
    "testvector06.bit":
        "65a779507949842232cfb6119db3ec8c056c05b80d65abb7a3ae9b06b443e33f",
    "testvector06.dec":
        "e216d2a721c74d01ae9d1147c75178800b865b3388efcc6b2bb5db96d50a64e8",
    "testvector06m.dec":
        "7a593d17e0702184c30fd2db9240d0fffc03ddbf2e6e6a3847e360158c5d4a4e",
    "testvector07.bit":
        "ae7ae8f569807d4d964c574d7b1556e8b930307adfa9928942fe44f5e2bcc5f1",
    "testvector07.dec":
        "e0161343bef57f81f07183ba58f336c7845246553bdf4e6d64fe6f5af46d48bd",
    "testvector07m.dec":
        "09faeaa61c70fe451dfd05e34f7a796ecdb12f786964dcbb617e75aaffd27cda",
    "testvector08.bit":
        "4de258de02ccc08c805b3d41414c3a0f2f607e57ea3a7a5cb9bef26c2f01eb01",
    "testvector08.dec":
        "8b8435b88ea5f8c5fc1b6ed355454324cd7c5e81d7ab3ba055511979be6c6358",
    "testvector08m.dec":
        "8b6ded1b08cf40e8e377c65ddd7ae21245045ee4b146540be21aac7dcceafc43",
    "testvector09.bit":
        "91a798186cd2483e47a6b3db44eb3f8fb7f5584c3ed2f2c8158ae8f9172a6b31",
    "testvector09.dec":
        "9baedeac28470f1b275c3a1b6a89725ed9a00370460dd4ab03cdaac09387049f",
    "testvector09m.dec":
        "026a3961c57e96aeab19026de0d0dd113162675bf404b56b6abbbad35933492f",
    "testvector10.bit":
        "7288deb8d0e9e49f7f8ae50d6b4f2221294faa222b9db69161951b928e62435a",
    "testvector10.dec":
        "8529af01c64acf8a1a10cc9f1664a78508c392078ac165f77da6a3846f6b1d48",
    "testvector10m.dec":
        "8529af01c64acf8a1a10cc9f1664a78508c392078ac165f77da6a3846f6b1d48",
    "testvector11.bit":
        "ee3cd3c6f420d803f563af6518851316696fd121932a191045349a615df7a249",
    "testvector11.dec":
        "4f3e16ddef55e84a7776469cba161e0ed647dadc1e36271fad1bc43a70112813",
    "testvector11m.dec":
        "d8e0595a896cb134bf3d4a5ac1138354981222e476b985e01459f7bae9f98896",
    "testvector12.bit":
        "48df85083178e8fc9c31af3fa6d5b38377f0eb84da4931a8886af503b17a0218",
    "testvector12.dec":
        "bd7f89a4cb02fb8dcb8fca4425bbcfdbdb3487beed9cd9d49883beb0960e617b",
    "testvector12m.dec":
        "bd7f89a4cb02fb8dcb8fca4425bbcfdbdb3487beed9cd9d49883beb0960e617b",
}

# What RFC 6716's own decoder with RFC 8251's patch applied - Appendix A's
# fixed-point build, then the update's changes to it - writes for each
# vector at 48 kHz stereo, as a SHA-256 of the raw 16-bit samples.
#
# **These are not the `.dec` files and are not interchangeable with them.**
# `opus_compare` is what accepts a decoder against the `.dec` files, with
# a tolerance; three of the twelve (02, 03, 04, which are SILK only) are
# byte-identical to what the reference writes and nine differ in the last
# bits of nearly every sample, because the `.dec` files come from a
# floating-point build. A decoder passes `opus_compare` either way. These
# pins say something stronger and different: this decoder's output is
# *identical* to the reference's, bit for bit, on all 15,390,480
# samples. They were made by building the tarball inside RFC 6716
# (Appendix A.1) with FIXED_POINT, applying the patch RFC 8251 names
# (SHA-1 029e3aa88fc342c91e67a21e7bfbc9458661cd5f), and running
# `opus_demo -d 48000 2` over each `.bit` file. Four of them (05, 06, 10,
# 12) differ from what RFC 6716's unpatched reference writes, which is
# the hybrid folding change; the other eight do not.
DECODED_SHA256 = {
    "testvector01": "9afd77e4ef6865c06bdc105a5761c64702ff6a3c5eb1e0ac23e5f6d409529045",
    "testvector02": "4ad3b7aeaea242acc96cafebee69edb0914abc2e46c0c884f812ce10bbee4dbc",
    "testvector03": "e00f4ee27f007d849fb67f4386db93254631d2ff31b580394d854819560f1bc7",
    "testvector04": "ce7450716f4911284332e76dab66518dcfd5c84e320c0a8a54014e658f5c6908",
    "testvector05": "594ea1228c8806c0be2449d549c94ffdf91383d39cb80d2576a4d81c82f8f04b",
    "testvector06": "5d8028eb137c8917024eb2ceb3d2a55ef95cdbb1a63cb81a1c8d31e3b47cc20c",
    "testvector07": "79d5c6b8a552ee4bc68cf8a18c9fc64d37e006ec24b4fef6faacf0e0b456641c",
    "testvector08": "aba1e0f8287f9dffd95345031f5241d231ae78c15f3ac956c9290854b42f474a",
    "testvector09": "8c4c795adbaa04f8583f387c505c61f2e4d08dce9b518e4bec406ff3dd1576ca",
    "testvector10": "d376fa063b620219f6dfdc796cd35074c836632779dd359851ad23f8732f1d93",
    "testvector11": "57afdb53a68c9571b9d99d3c78d57d3c7411153bb97b86f90b9ad4948a35c0a9",
    "testvector12": "9d371ba6d76b37f133bbdec0ded5457c25c917c6a8a2c340d3b5f811e1072ec2",
}

CONFIGS = {
    "testvector01": [28, 29, 30, 31],
    "testvector02": [0, 1, 2, 3],
    "testvector03": [4, 5, 6, 7],
    "testvector04": [8, 9, 10, 11],
    "testvector05": [12, 13],
    "testvector06": [14, 15],
    "testvector07": list(range(16, 32)),
    "testvector08": [1, 16, 17, 18, 20, 21, 22, 23, 24, 25, 26, 27, 29, 30,
                     31],
    "testvector09": [1, 17, 20, 21, 24, 25, 27, 28, 29, 30, 31],
    "testvector10": [14, 15, 28, 29, 30, 31],
    "testvector11": [31],
    "testvector12": [1, 5, 9, 13],
}


def directory():
    """Where the unpacked vectors are, or should go.

    GAUD_OPUS_VECTORS overrides it, so a machine that already has them
    for another reason does not fetch them twice.
    """
    override = os.environ.get("GAUD_OPUS_VECTORS")
    if override:
        return override
    cache = os.environ.get("XDG_CACHE_HOME") or os.path.join(
        os.path.expanduser("~"), ".cache")
    return os.path.join(cache, "ghoti-audio", "opus_newvectors")


def digest(path):
    """SHA-256 of one file, read in blocks."""
    sha = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            sha.update(block)
    return sha.hexdigest()


def present(where=None):
    """Whether all 36 files are there. Says nothing about their contents."""
    where = where or directory()
    for name in VECTORS:
        for suffix in (".bit", ".dec", "m.dec"):
            if not os.path.exists(os.path.join(where, name + suffix)):
                return False
    return True


def fetch(where=None):
    """Download, verify the archive's hash, and unpack.

    The hash is checked **before** unpacking, so a truncated or replaced
    archive never reaches the filesystem as files that look like data.
    """
    where = where or directory()
    os.makedirs(where, exist_ok=True)
    archive = os.path.join(where, "opus_newvectors.tar.gz")
    if not os.path.exists(archive) or digest(archive) != ARCHIVE_SHA256:
        print(f"opus-vectors: fetching {ARCHIVE_BYTES} bytes from {URL}")
        with urllib.request.urlopen(URL, timeout=600) as source:
            payload = source.read()
        got = hashlib.sha256(payload).hexdigest()
        if got != ARCHIVE_SHA256:
            raise SystemExit(
                "opus-vectors: the archive does not match its pin.\n"
                f"  expected {ARCHIVE_SHA256}\n  got      {got}\n"
                "This is a different file from the one these gates were "
                "written against. Do not update the pin without reading "
                "what changed.")
        with open(archive, "wb") as handle:
            handle.write(payload)
    with tarfile.open(archive, "r:gz") as tar:
        for member in tar.getmembers():
            # Never trust a path out of an archive.
            name = os.path.basename(member.name)
            if not member.isfile() or not name:
                continue
            target = os.path.join(where, name)
            extracted = tar.extractfile(member)
            if extracted is None:
                continue
            with open(target, "wb") as handle:
                handle.write(extracted.read())
    print(f"opus-vectors: unpacked into {where}")
    return where


def require(where=None):
    """The directory, or a failure that says how to get one."""
    where = where or directory()
    if present(where):
        return where
    raise SystemExit(
        "opus-vectors: RFC 6716's conformance vectors are not at\n"
        f"  {where}\n"
        "They are 75 MB and are deliberately not in the repository. Run\n"
        "  make opus-vectors\n"
        "to fetch them, or set GAUD_OPUS_VECTORS to a directory that has\n"
        "them already.")


def main(argv):
    if "--fetch" in argv:
        fetch()
        return 0
    where = directory()
    if "--path" in argv:
        print(where)
        return 0
    if "--check" in argv:
        require(where)
        bad = 0
        for name, want in sorted(FILE_SHA256.items()):
            got = digest(os.path.join(where, name))
            if got == want:
                print(f"opus-vectors: {name} matches its pin")
            else:
                bad += 1
                print(f"opus-vectors: {name} DOES NOT match its pin\n"
                      f"  expected {want}\n  got      {got}",
                      file=sys.stderr)
        if bad:
            print(f"opus-vectors: {bad} of {len(FILE_SHA256)} files differ "
                  "from the data these gates were written against",
                  file=sys.stderr)
            return 1
        print(f"opus-vectors: all {len(FILE_SHA256)} files match their pins")
        return 0
    print(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
