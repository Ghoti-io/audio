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
"""Score this library's reading and writing of tags against the references.

    make check-tags

Three questions, and they are different questions:

  1. **Does the genre table match?** ID3v1's genre is a byte index into a
     list no specification contains - 80 entries from ID3v1 and another
     112 from successive Winamp releases. It is compared against
     mutagen's row by row. The first draft of that table was typed out,
     had 148 rows where mutagen has 192, and three of the names it did
     have were wrong; reading it back over would not have found either
     problem, because one was an absence.

  2. **Can the references read what we write?** Every tag, both
     containers, scored by ffmpeg and by mutagen separately. This is the
     half that catches a frame built wrong.

  3. **Can we read what the references write?** ffmpeg writes the
     fixtures, so this is the half that catches a *reader* that only
     understands its own output. A library whose writer and reader share
     a misunderstanding passes question 2 perfectly.

### Why mutagen is here

ffmpeg generates the tag fixtures. Scoring them with ffmpeg alone would
be asking one implementation whether it agrees with itself, and a
misunderstanding held in both its writer and its reader would be
invisible. mutagen shares no code with it and exists for metadata
specifically. `containers/IMAGES` records the same reasoning.

### The control

`--self-check` corrupts *our* answer and requires the gate to reject it.
A differential that cannot be seen to fail is not a differential.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import oracle_env as oracle  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
PROBE = os.environ.get(
    "GAUD_TAG_PROBE",
    os.path.join(ROOT, "build", "linux", "release", "apps", "oracle",
                 "tag_probe"))
SCRATCH = os.path.join(ROOT, "tests", "out")

# The tags every scheme in phase 3 can carry, with a value chosen to be
# awkward in a way that matters: non-ASCII, a slash that ID3 packs into
# one frame, and a value that looks like a number.
CASES = [
    ("title", "A Title"),
    ("artist", "An Artist"),
    ("album", "An Album"),
    ("date", "2026"),
    ("genre", "Ambient"),
    ("comment", "caf\u00e9 \u65e5\u672c\u8a9e"),
    ("composer", "A Composer"),
    ("copyright", "(c) 2026 Nobody"),
]

# What each reference calls each of our tag names. ffmpeg and mutagen use
# different vocabularies and neither is ours, so the mapping is stated
# rather than guessed at by string similarity.
FFMPEG_NAMES = {
    "title": "title", "artist": "artist", "album": "album",
    "date": "date", "genre": "genre", "comment": "comment",
    "composer": "composer", "copyright": "copyright",
}
MUTAGEN_FRAMES = {
    "title": "TIT2", "artist": "TPE1", "album": "TALB", "date": "TDRC",
    "genre": "TCON", "composer": "TCOM", "copyright": "TCOP",
    "comment": "COMM::XXX",
}

# Where a reference cannot answer, why, and what was measured to show it.
#
# **Empty, and that is a result.** Every tag in CASES is read back by both
# references from both containers, so nothing here needs setting aside. The
# table stays because an exclusion that is not written down is
# indistinguishable from a reference nobody thought to ask - and because
# the first draft of it carried a speculative row for AIFF copyright that
# excluded a comparison which in fact passes. An exclusion with no
# measurement behind it is a hole in the denominator.
EXCLUSIONS = {}


def run(argv, **kw):
    return subprocess.run(argv, capture_output=True, text=True, **kw)


def clean(text):
    return "\n".join(
        line for line in text.splitlines() if "Emulate Docker" not in line)


def ours(path):
    """Our reading, as {tag: [values]} plus {'custom:KEY': [value]}."""
    finished = run([PROBE, "dump", path])
    if finished.returncode != 0:
        raise SystemExit("tag_probe dump failed on %s:\n%s"
                         % (path, finished.stderr[-800:]))
    out = {}
    for line in finished.stdout.splitlines():
        parts = line.split("\t")
        if parts[0] == "tag" and len(parts) >= 4:
            out.setdefault(parts[1], []).append(parts[3])
        elif parts[0] == "custom" and len(parts) >= 3:
            out.setdefault("custom:" + parts[1], []).append(parts[2])
    return out


def ffmpeg_tags(path):
    finished = run(oracle.command("ffmpeg", [
        "ffprobe", "-hide_banner", "-v", "error", "-show_entries",
        "format_tags", "-of", "default=nw=1", path], scratch=SCRATCH))
    out = {}
    for line in clean(finished.stdout).splitlines():
        if not line.startswith("TAG:"):
            continue
        key, _, value = line[4:].partition("=")
        out.setdefault(key.lower(), []).append(value)
    return out


MUTAGEN_DUMP = (
    "import sys, mutagen\n"
    "f = mutagen.File(sys.argv[1])\n"
    "if f is None or not f.tags:\n"
    "    raise SystemExit(0)\n"
    "for key in f.tags.keys():\n"
    "    frame = f.tags[key]\n"
    "    values = list(frame) if hasattr(frame, '__iter__') "
    "and not isinstance(frame, str) else [str(frame)]\n"
    "    for v in values:\n"
    "        print('%s\\t%s' % (key, v))\n")


def mutagen_tags(path):
    finished = run(oracle.command(
        "mutagen", ["python3", "-c", MUTAGEN_DUMP, path], scratch=SCRATCH))
    out = {}
    for line in clean(finished.stdout).splitlines():
        key, _, value = line.partition("\t")
        if key:
            out.setdefault(key, []).append(value)
    return out


def compare_reading(label, expected, got, lookup):
    """The gate's actual comparison, as a function both callers share.

    `expected` is the list of (name, value) we wrote, `got` is a
    reference's reading keyed by its own vocabulary, and `lookup` maps
    our name to theirs. Returns a list of complaints.

    Extracted rather than written inline because the control has to
    drive the real comparison. A control that re-implements the check it
    is controlling tests a copy, and the copy is the one thing that
    cannot be wrong in the way that matters.
    """
    complaints = []
    for name, value in expected:
        key = lookup(name)
        if key is None:
            continue
        values = got.get(key, [])
        if value not in values:
            complaints.append("%s/%s: read %r, wrote %r"
                              % (label, name, values, value))
    return complaints


def check_genre_table(bad):
    """Our ID3v1 genre table against mutagen's, row by row."""
    finished = run([PROBE, "genres"])
    if finished.returncode != 0:
        bad.append("tag_probe genres failed: %s" % finished.stderr[-200:])
        return
    mine = [line.split("\t", 1)[1]
            for line in finished.stdout.splitlines() if "\t" in line]
    theirs_raw = run(oracle.command("mutagen", [
        "python3", "-c",
        "from mutagen.id3 import TCON\n"
        "for g in TCON.GENRES: print(g)"], scratch=SCRATCH))
    theirs = [line for line in clean(theirs_raw.stdout).splitlines()]
    if not theirs:
        bad.append("mutagen produced no genre list, so the table is scored "
                   "by nothing")
        return
    if len(mine) != len(theirs):
        bad.append("the genre table has %d entries and mutagen's has %d"
                   % (len(mine), len(theirs)))
    for index in range(min(len(mine), len(theirs))):
        if mine[index] != theirs[index]:
            bad.append("genre %d is %r here and %r in mutagen"
                       % (index, mine[index], theirs[index]))
    print("  genre table: %d entries, %s mutagen's"
          % (len(mine), "identical to" if len(mine) == len(theirs)
             and mine == theirs else "DIFFERENT from"))


def check_we_are_read(bad, scratch):
    """Question 2: can the references read what we write?"""
    pairs = 0
    for container, extension in (("wav", "wav"), ("aiff", "aiff")):
        path = os.path.join(scratch, "ours.%s" % extension)
        argv = [PROBE, "write", path, container]
        argv += ["%s=%s" % (name, value) for name, value in CASES]
        argv += ["REPLAYGAIN_TRACK_GAIN=-3.21 dB"]
        finished = run(argv)
        if finished.returncode != 0:
            bad.append("%s: tag_probe write failed: %s"
                       % (container, finished.stderr[-200:]))
            continue

        from_ffmpeg = ffmpeg_tags(path)
        from_mutagen = mutagen_tags(path)
        if not from_ffmpeg:
            bad.append("%s: ffmpeg read no tags at all from what we wrote"
                       % container)
        if not from_mutagen:
            bad.append("%s: mutagen read no tags at all from what we wrote"
                       % container)

        scored = [(name, value) for name, value in CASES
                  if ("ffmpeg", container, name) not in EXCLUSIONS]
        bad += compare_reading("%s/ffmpeg" % container, scored, from_ffmpeg,
                               lambda n: FFMPEG_NAMES[n])
        pairs += len(scored)
        bad += compare_reading("%s/mutagen" % container, CASES, from_mutagen,
                               lambda n: MUTAGEN_FRAMES[n])
        pairs += len(CASES)

        # The custom key, which only ID3v2 can carry and only as TXXX.
        if "-3.21 dB" not in from_mutagen.get(
                "TXXX:REPLAYGAIN_TRACK_GAIN", []):
            bad.append("%s: mutagen did not find the TXXX custom key"
                       % container)
        pairs += 1
        print("  %-4s -> ffmpeg %2d tags, mutagen %2d frames"
              % (container, len(from_ffmpeg), len(from_mutagen)))
    return pairs


def check_we_read_theirs(bad, scratch):
    """Question 3: can we read what ffmpeg writes?

    The half a library whose writer and reader share a misunderstanding
    passes question 2 on and fails here.
    """
    pairs = 0
    for container, extension, codec in (
            ("wav", "wav", "pcm_s16le"), ("aiff", "aiff", "pcm_s16be")):
        path = os.path.join(scratch, "theirs.%s" % extension)
        argv = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
                "-f", "lavfi",
                "-i", "sine=frequency=440:sample_rate=8000:duration=0.05",
                "-c:a", codec]
        for name, value in CASES:
            argv += ["-metadata", "%s=%s" % (FFMPEG_NAMES[name], value)]
        argv += ["-fflags", "+bitexact", "-f", container, path]
        finished = run(oracle.command("ffmpeg", argv, scratch=scratch))
        if not os.path.exists(path):
            bad.append("%s: ffmpeg would not write the fixture: %s"
                       % (container, clean(finished.stderr)[-200:]))
            continue

        mine = ours(path)
        for name, value in CASES:
            got = mine.get(name, [])
            if value not in got:
                # AIFF has native chunks for only four of these and
                # ffmpeg writes no ID3 chunk into an AIFF, so the rest
                # genuinely are not in the file. That is ffmpeg's
                # limitation and not a reading failure - but it has to
                # be established rather than assumed, so the reference
                # is asked whether IT can read its own file back.
                theirs = ffmpeg_tags(path)
                if value not in theirs.get(FFMPEG_NAMES[name], []):
                    print("    %s/%s: not in the file - ffmpeg cannot "
                          "write it to this container either" %
                          (container, name))
                    continue
                bad.append("%s/%s: ffmpeg wrote %r and we read %r"
                           % (container, name, value, got))
            pairs += 1
        print("  %-4s <- ffmpeg's own tags, %d read back" % (container,
                                                             len(mine)))
    return pairs


def main():
    args = sys.argv[1:]
    self_check = "--self-check" in args
    print(oracle.provenance(["ffmpeg", "mutagen"]))
    if not os.path.isfile(PROBE):
        raise SystemExit(
            "tag_probe is not built: %s\nRun `make oracle-probe`." % PROBE)

    # Inside the repository, because the references run in a container
    # with the repository bind-mounted and nothing else - a directory in
    # /tmp is invisible to them, and the symptom is every reference
    # reading nothing, which looks like a broken writer.
    scratch = os.path.join(SCRATCH, "tags")
    os.makedirs(scratch, exist_ok=True)

    bad = []
    check_genre_table(bad)
    pairs = check_we_are_read(bad, scratch)
    pairs += check_we_read_theirs(bad, scratch)

    if self_check:
        print("\n  control: this gate must reject a wrong reading")
        # Mutate OUR answer, not the file: corrupting the fixture
        # corrupts it for the references too, and they would agree with
        # us about the damaged file. check_corpus.py learned that the
        # hard way and the same trap is here.
        path = os.path.join(scratch, "ours.wav")
        truth = ours(path)
        controls = [
            ("a tag dropped",
             {k: v for k, v in truth.items() if k != "title"}),
            ("a tag altered", dict(truth, title=["A Different Title"])),
            ("a value truncated", dict(truth, album=["An Albu"])),
            ("everything empty", {}),
        ]
        for label, wrong in controls:
            complaints = compare_reading(
                "control", CASES, wrong, lambda n: n)
            if not complaints:
                bad.append("CONTROL FAILED (%s): the comparison accepted a "
                           "wrong reading, so it is not measuring anything"
                           % label)
            else:
                print("    %-18s rejected: %s"
                      % (label, complaints[0].split(": ", 1)[-1][:56]))

        # And the control's control: the truth itself must be accepted,
        # or the four above would pass for a comparison that rejects
        # everything.
        if compare_reading("control", CASES, truth, lambda n: n):
            bad.append("CONTROL FAILED: the comparison rejected our own "
                       "correct reading, so rejecting the wrong ones "
                       "showed nothing")
        else:
            print("    %-18s accepted" % "the true reading")

    print("\n%d tag comparisons across two containers and two references."
          % pairs)
    if EXCLUSIONS:
        print("\nExcluded, with the reason each was established by:")
        for key, why in sorted(EXCLUSIONS.items()):
            print("  %s" % " / ".join(key))
            for line in _wrap(why, 70):
                print("      " + line)
    if bad:
        print("\n%d disagreement(s):" % len(bad), file=sys.stderr)
        for line in bad:
            print("  " + line, file=sys.stderr)
        return 1
    print("Both references read our tags, and we read theirs.")
    return 0


def _wrap(text, width):
    words, line, out = text.split(), "", []
    for word in words:
        if line and len(line) + 1 + len(word) > width:
            out.append(line)
            line = word
        else:
            line = word if not line else line + " " + word
    if line:
        out.append(line)
    return out


if __name__ == "__main__":
    sys.exit(main())
