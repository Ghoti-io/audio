# FLAC {#format_flac}

RFC 9639, read and written, in both of its containers: the native stream
and Ogg. The first codec in this library that is a codec - WAV and AIFF
are chunk trees around samples, and FLAC is an actual compressor - and
the one chosen to be first because it is the only one whose correctness
can be settled rather than argued. It is lossless, so a reference decode
is a number to match, not a tolerance to agree on.

## What is implemented

**Read.** Every subframe type (CONSTANT, VERBATIM, the five fixed
predictors and LPC at every order from 1 to 32), both Rice coding
methods, escaped partitions, wasted bits, all four channel assignments,
and both blocking strategies. Bit depths of 8, 16, 24 and 32; see the
refusal below for the others. Metadata blocks: STREAMINFO, SEEKTABLE,
VORBIS_COMMENT, PICTURE, CUESHEET, APPLICATION, PADDING, and anything a
later revision defines, which is carried raw.

**Write.** The native container and Ogg. CONSTANT, VERBATIM and the fixed
predictors, with a search over Rice parameters, partition orders and all
four stereo decorrelations; escaped partitions and wasted bits where they
win. A seek table. A STREAMINFO MD5, which `flac -t` verifies.

**Seek.** Exact, to the sample. Natively through the seek table and then
by decoding forward; in Ogg by rewinding and decoding forward.

## Two decisions worth knowing before you use it

### The encoder has no LPC, and that is deliberate

Our encoder uses the format's fixed predictors - four integer differences
- and not linear prediction. LPC would compress better by a few percent.
It would also make this library's output depend on the host's floating
point: an autocorrelation, a Levinson-Durbin recursion and a
quantisation, every step of which can round differently on a different
machine or a different compiler. `planning/audio.md` §11.1 promises that
what this library writes is byte-identical across architectures, and
`make check-golden` tests it. Fixed predictors keep that promise for the
writer as well as the reader.

Measured against the references on the corpus in `tests/data/`, at the
same input: our output is larger than `flac -8`'s by roughly ten to
fifteen per cent on musical material and smaller on some synthetic
signals. Every file we write decodes, in libFLAC and in ffmpeg, to
exactly the samples that went in - which is the whole of what lossless
means, and the part that is not a trade.

### Bit depths other than 8, 16, 24 and 32 are refused

RFC 9639 allows 4 to 32 bits. ::GAUD_Buffer holds 8, 16, 24 and 32. For a
20-bit file there is no lossless landing place, and both of the obvious
answers lie:

- leaving the samples where they are makes the track quieter than the
  file by a factor the caller cannot see;
- shifting them up to fill the container - which is what ffmpeg does -
  changes every sample value, so the round trip this codec exists to
  promise no longer holds.

So `gaud_doc_load()` answers ::GAUD_ERR_UNSUPPORTED and raises a
diagnostic naming the depth. The reference implementation agrees to the
extent that it can: `flac --bps=20` is refused by the `flac` command line
with "must be 8/16/24/32". The fix, when someone has such a file, is a
bit depth on ::GAUD_Track - which WAV's `wValidBitsPerSample` already
wants, so it will have two callers and not one.

## CUESHEET is checked and carried, not modelled

A CUESHEET block is parsed far enough to know whether its own counts add
up, and then kept as a raw block and written back byte for byte. There is
no public cue or chapter API.

That is `planning/audio.md` §11.5's rule - a shape stays where it is
until a second caller wants it - applied to a vocabulary rather than a
module. Chapters and cues have **four spellings** across the formats this
library will reach: WAV's `cue `+`adtl`, FLAC's CUESHEET, MP4's
`chpl`/`chap`, and Matroska's chapters. Designing one model from the
first of them would be designing it from one example, and the three
later ones would each have to be bent to fit. FLAC's survives a round
trip exactly in the meantime, which is what a tagger needs.

What the check is for is the other half: a cuesheet whose track and index
counts do not tile its own length is corrupt, and carrying it forward
unexamined would hand the next reader a block this library has implicitly
vouched for. It gets a diagnostic and is dropped.

## Cover art: the first scheme that states dimensions

`planning/audio.md` §11.4 built a four-state picture status around a case
that did not exist yet. FLAC's PICTURE block is that case: it is the only
cover-art carrier of the three that states width, height, depth and
palette size, so it is the only one whose claim can be checked against
its own payload, and the only one that can be found lying.

With `image` present, a FLAC picture reaches ::GAUD_PICTURE_VERIFIED or
::GAUD_PICTURE_MISMATCH. Without it, ::GAUD_PICTURE_UNVERIFIED. ID3's
`APIC` states nothing and stays ::GAUD_PICTURE_NOT_STATED in either
build. The stated values are written back **as they were stated**, not as
this build measured them: silently correcting a file's own claim changes
a file the caller did not ask to change, and hides the defect from the
next reader.

## The vendor string is not a tag

A Vorbis comment block begins with a vendor string naming the software
that wrote it. This library reads past it and does not keep it.

It is not a property of the recording, and the comment list has an
`ENCODER` field for what a caller means by that. More to the point, the
writer replaces it: a reader that collected it into the tag model would
make the round trip gain a value every generation, because our
predecessor's signature becomes a tag and our own signature is stamped
beside it. **A field the writer overwrites must not also be a field the
reader collects.**

## Ogg FLAC

The same bitstream in Ogg pages, sharing every line of the frame decoder
with the native codec. The codec is registered as `ogg-flac`; the probe
looks inside the first page's packet, because `OggS` is shared with
Vorbis, Opus and Theora and a signature match alone would claim all of
them.

Two things differ from the native container and both follow from the
framing:

- **No seek table.** Its offsets are byte positions into a native
  stream's frames, and in Ogg the frames are scattered through page
  bodies at positions no such table describes. Ogg's granule positions
  are what a seeker reads instead, which is what they are for.
- **Seeking is linear.** Ogg is designed to be bisected and this does not
  bisect; it rewinds and decodes forward. A bisection is subtly wrong
  near the ends and on files with a false `OggS` inside a page body, and
  it belongs with phase 6, where Vorbis and Opus will want the same code
  and it can be written once against three codecs' worth of fixtures.

The STREAMINFO MD5 still applies, because it is over the samples rather
than over the file, so `flac -t --ogg` verifies an Ogg FLAC file the same
way it verifies a native one.

## How it is scored

`make check-corpus` compares our decode of every fixture in `tests/data/`
against each reference that can read it, byte for byte. `make
check-writer` re-encodes every fixture through our writer and asks the
references to read the result, and additionally runs `flac -t
--warnings-as-errors` on it.

**`--warnings-as-errors` is load-bearing, and it was added because
leaving it off hid a real defect.** Without it, `flac -t` exits zero
after printing "sample or frame number does not increase correctly ...
file might not be seekable" - and every FLAC this encoder had written was
in that state, because STREAMINFO's minimum block size was folded from
all the blocks including the short last one. A minimum that differs from
the maximum tells a reader the stream is variable-blocksize, and libFLAC
then expects a sample number where our frames carry a frame number. The
gate read the exit status and reported a pass.

It is also why that flag is not redundant with the byte comparison: the
samples were right the whole time. What was wrong was a claim about the
stream that only the format's own tool knew to check.

**For FLAC the reference count is two, not four**, and
`tools/oracle/check_corpus.py` says so in its output. sox reaches FLAC
through `libsox_fmt_flac`, libsndfile links libFLAC, and the `flac` tool
is libFLAC's own front end: three of the four names are one
implementation asked three times. ffmpeg is the one that is genuinely
separate - libavcodec has a native FLAC decoder and has never had a
libFLAC decoder wrapper. (`ldd /usr/bin/ffmpeg` does list libFLAC, which
is why this is written down rather than assumed: it arrives through
libavdevice → libpulse → libsndfile, an output-device path the demuxer
never enters.)

## Known gaps

| Gap | Why, and what would close it |
| --- | --- |
| LPC subframes are read and never written | Floating point in the encoder would break the cross-architecture promise; see above. An integer-only LPC search would close it. |
| Bit depths outside 8/16/24/32 | No lossless sample format; a bit depth on ::GAUD_Track closes it. |
| Variable-blocksize streams are read and never written | Nothing in the reference set writes one either, so the reading side is exercised only by hand-made fixtures. |
| CUESHEET has no public model | Waiting for the second of its four spellings. |
| Ogg seeking is linear | Waiting for phase 6, where bisection has three callers. |
| FLAC in MP4 and Matroska | Phase 9. |
