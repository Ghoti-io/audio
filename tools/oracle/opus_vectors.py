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

These are not fixtures and they are not in the repository. They are 39 MB
of normative test data published alongside the codec, and `tests/data/`
holds 120 files totalling a few hundred kilobytes - putting these there
would multiply the checkout by a hundred to carry data that never changes
and that only one gate reads.

So they are fetched deliberately, by `make opus-vectors`, into a
directory outside the tree, and the gate that reads them fails with
instructions when they are absent rather than fetching them itself. That
is the same arrangement `make corpus` has and for the same reason: a gate
that silently downloads 39 MB the first time it runs is a gate whose
first run means something different from its second.

**The pin is a hash of the archive, not a version.** opus-codec.org
serves one file under one name and has replaced it before - the copy here
is dated 2024-04-12 - so a name and a date are not an identity. The
SHA-256 below is, and `--check` verifies it without unpacking anything.

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

URL = "https://opus-codec.org/static/testvectors/opus_testvectors.tar.gz"

ARCHIVE_SHA256 = \
    "94ac78ca4f74c4e43bc9fe4ec1ad0aa36f38ab90f45b0727c40dd1e96096e767"

ARCHIVE_BYTES = 39001148

# Twelve vectors, each a pair: the packets and the reference decoder's
# output at 48 kHz stereo. Both halves are hashed - the .dec files are
# what `opus_compare` is run against, so a wrong one would pass a
# decoder that was wrong in the same way.
VECTORS = [f"testvector{n:02d}" for n in range(1, 13)]

FILE_SHA256 = {
    "testvector01.bit":
        "00a4d34e12a2a32a047bfcaf762f663430b2232bc76a1c21e3b598f060f99b7e",
    "testvector01.dec":
        "917a3b81d0ad24b0c75a07e60d8bc043023895a9a7b4b662e68f840ceff700ef",
    "testvector02.bit":
        "285deaf78e6b1ebdde7dc6a76af279a08bf0fa7a13e3a84e87ac3f76213626a6",
    "testvector02.dec":
        "4ad3b7aeaea242acc96cafebee69edb0914abc2e46c0c884f812ce10bbee4dbc",
    "testvector03.bit":
        "811b3d28483c781e4d1a0f8008bf73c3882c6c9cf8dedc65c0c14fa4239bb9d9",
    "testvector03.dec":
        "e00f4ee27f007d849fb67f4386db93254631d2ff31b580394d854819560f1bc7",
    "testvector04.bit":
        "874b838699a471acd97bd13e11224a3ef71c95b018b85a9c93a41459a7185565",
    "testvector04.dec":
        "ce7450716f4911284332e76dab66518dcfd5c84e320c0a8a54014e658f5c6908",
    "testvector05.bit":
        "9b13f677c61cfad9a94460f986401b843e07e3245265067580539d9d8f1ec230",
    "testvector05.dec":
        "4a0299f4c7442ce8c6ae8f553db87bac40f1e00bd0bb99988331ede9529cbaf3",
    "testvector06.bit":
        "65a779507949842232cfb6119db3ec8c056c05b80d65abb7a3ae9b06b443e33f",
    "testvector06.dec":
        "dd5956ceb323238c724bfcd8303b3f1176d0662fc8ccfc8088f1d0493cf83e40",
    "testvector07.bit":
        "ae7ae8f569807d4d964c574d7b1556e8b930307adfa9928942fe44f5e2bcc5f1",
    "testvector07.dec":
        "ad56058474248fcf87c4f94a1fa55a68f0f971a2cfdd9f068e002db11e59f04b",
    "testvector08.bit":
        "4de258de02ccc08c805b3d41414c3a0f2f607e57ea3a7a5cb9bef26c2f01eb01",
    "testvector08.dec":
        "bff57a955e82c0bb099c80b8498273d420a1c62a0eb819bad8b49da73290aa4b",
    "testvector09.bit":
        "91a798186cd2483e47a6b3db44eb3f8fb7f5584c3ed2f2c8158ae8f9172a6b31",
    "testvector09.dec":
        "d19317c82a716e9c2bd340a0231859788978b225b0f2da8d9517ba0dc3eabf08",
    "testvector10.bit":
        "7288deb8d0e9e49f7f8ae50d6b4f2221294faa222b9db69161951b928e62435a",
    "testvector10.dec":
        "68727a2328d713158e4ed7fa64773b659641cc77b63d138064ccf726c1dfd887",
    "testvector11.bit":
        "ee3cd3c6f420d803f563af6518851316696fd121932a191045349a615df7a249",
    "testvector11.dec":
        "fc31d697a1b7f5d28ff439cd1882bbeda80412232ef1aefea069c849356dce5f",
    "testvector12.bit":
        "48df85083178e8fc9c31af3fa6d5b38377f0eb84da4931a8886af503b17a0218",
    "testvector12.dec":
        "8107b5e33122cd5e2aceae723d8a4ea2ebb30b3493bb11d7f021cd1d8e653aa0",
}

# How many packets of each vector end with exactly the range decoder
# state the vector file states. Pinned per vector rather than only in
# total, because a decoder that stopped answering for one configuration
# while another gained the same number of packets would keep a total
# intact. Every entry is a floor that has been reached, never a target:
# raising one is the measurement of a decoder that reaches further.
RANGE_MATCHED = {
    "testvector01": 2147,
    "testvector02": 1185,
    "testvector03": 998,
    "testvector04": 1265,
    "testvector07": 4186,
    "testvector08": 4,
    "testvector09": 4,
    "testvector10": 965,
    "testvector11": 553,
    "testvector12": 1056,
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
    return os.path.join(cache, "ghoti-audio", "opus_testvectors")


def digest(path):
    """SHA-256 of one file, read in blocks."""
    sha = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            sha.update(block)
    return sha.hexdigest()


def present(where=None):
    """Whether all 24 files are there. Says nothing about their contents."""
    where = where or directory()
    for name in VECTORS:
        for suffix in (".bit", ".dec"):
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
    archive = os.path.join(where, "opus_testvectors.tar.gz")
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
        "They are 39 MB and are deliberately not in the repository. Run\n"
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
