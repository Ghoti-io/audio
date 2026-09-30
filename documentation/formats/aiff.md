# AIFF and AIFF-C {#format_aiff}

IFF rather than RIFF, so **big-endian**. The mirror image of WAV, which is
why both are in phase 1: each one's byte-swapping is dead code on the
architecture where the other's runs.

## What is implemented

**Read.** AIFF with signed 8-, 16-, 24- and 32-bit samples, and AIFF-C with
the compression types `NONE` and `twos` (big-endian PCM), `sowt`
(little-endian PCM), and `fl32`/`fl64` (big-endian floats). The `COMM` and
`SSND` chunks; anything else is skipped by its stated length.

**Write.** The same, as plain AIFF for integer samples and AIFF-C for float -
because plain AIFF has no way to say a sample is not an integer. Unsigned
8-bit is refused: the container cannot express it.

## Things that are easy to get wrong, and what this does

**The sample rate is an 80-bit extended float.** Nothing else in either
format is, and there is no C type for it on every platform - `long double` is
80-bit on x86, 64-bit on aarch64 and 128-bit elsewhere. It is decoded by hand
into a double. The significand's top bit is **explicit**, unlike every other
IEEE format, which is the detail a hand-rolled decoder gets wrong. The result
is rounded rather than truncated on the way to an integer, so a writer that
lost a bit does not turn 44100 into 44099.

**8-bit AIFF is signed**, where WAV's is unsigned. See wav.md; the same
paragraph applies from the other side.

**`sowt` is "twos" backwards** and means the samples are little-endian - the
case where the container's default byte order and the data's disagree.
Whatever the file says, the decoded buffer is in the **host's** order, which
is what lets WAV and AIFF produce the same buffer.

**`SSND` has an 8-byte preamble** - an offset and a block size - before the
samples. The offset is almost always zero and is almost always ignored by
readers, which then get it wrong for the file where it is not. It is honoured
here.

**`COMM` states a frame count and `SSND` carries the bytes**, and they can
disagree. The bytes win, because they are what can actually be read, and a
diagnostic records the disagreement because it means something truncated the
file.

**An unrecognised compression type is refused by name.** An IMA or µ-law
AIFF-C read as though it were PCM produces loud noise, which is a worse
answer than saying no.

## Not implemented

- Any compressed AIFF-C payload. Refused by name; see above.
- `MARK`, `INST`, `COMT` and the other metadata chunks - phase 3. Skipped by
  length today.
- AIFF-C's `FVER` is written for float output and is not required on input,
  which is what every reader does in practice.

## Specification

Apple's *Audio Interchange File Format: AIFF* version 1.3 (1989) and *AIFF-C*
(1991). Both are informal by modern standards and neither is maintained;
where they are ambiguous, the behaviour here is what ffmpeg, sox and
libsndfile agree on, which `make check-corpus` requires.

## How it is checked

As WAV, except that Python's `wave` is not a reference here: **Python 3.13
removed `aifc`** under PEP 594, so there is no standard-library AIFF reader
any more. AIFF is scored by ffmpeg, sox and libsndfile - three independent
parsers, which is one fewer than WAV gets and is stated in
`tools/oracle/containers/IMAGES` rather than left to be noticed.
