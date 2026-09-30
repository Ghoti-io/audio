# AIFF and AIFF-C {#format_aiff}

IFF rather than RIFF, so **big-endian**. The mirror image of WAV, which is
why both are in phase 1: each one's byte-swapping is dead code on the
architecture where the other's runs.

## What is implemented

**Read.** AIFF with signed 8-, 16-, 24- and 32-bit samples, and AIFF-C with
the compression types `NONE` and `twos` (big-endian PCM), `sowt`
(little-endian PCM), and `fl32`/`fl64` (big-endian floats). The `COMM` and
`SSND` chunks; anything else is skipped by its stated length.

**Coded samples.** `ulaw` and `alaw` (and SGI's `ULAW`/`ALAW`, accepted on
read), and `ima4`, Apple's IMA ADPCM in 34-byte packets. Read and written,
all decoding to ::GAUD_SAMPLE_S16 and reported by gaud_track_coding(). See
\ref format_coding "the codings page".

**Write.** The same, as plain AIFF for integer samples and AIFF-C for float
or any coding - because plain AIFF has no way to say a sample is not an
integer, and no way to name a compression at all. Unsigned 8-bit is refused:
the container cannot express it.

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

**`COMM`'s frame count means packets for `ima4`, against the specification.**
AIFF-C says the common chunk describes the uncompressed data, which would
make the field the sample-frame count. ffmpeg and libsndfile both *write* the
packet count - 32 for a 2048-frame file - and both *decoders* ignore the
field entirely, taking the length from `SSND`. Patching it to 32, 2048, 7 or
100000 changes neither reference's output, while ffmpeg's reported duration
does follow it, so writing the spec-literal value makes `ffprobe` report a
file 64 times too long. This library writes the packet count, derives the
length from `SSND`, and raises a diagnostic only when `COMM` matches neither
reading.

**`COMM`'s sampleSize for `ima4` has no agreed value.** ffmpeg writes 4, the
stored nibble width; libsndfile writes 16, the decoded width. Neither reader
appears to use it. The specification's reading is 16 and that is what goes
out; anything is accepted on read. afconvert, the reference that would settle
what AIFF-C means, is macOS-only and is not in the oracle image -
`tools/oracle/containers/IMAGES` records that gap.

**There is no `fact` chunk here.** WAV can state a coded track's true length
and AIFF-C cannot, so an `ima4` file is always a whole number of 64-frame
packets and its tail is padding nothing can mark.

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
