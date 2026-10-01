# MPEG audio {#format_mpeg}

MPEG-1, MPEG-2 and MPEG-2.5 audio, Layers I, II and III - which is to say
MP3 and the two layers nobody calls by name. **Identified, measured and
tagged; not decoded.** This page says exactly where that line falls and
why the measuring half is worth having on its own.

The codec is registered as `mp3`, because that is what the files are
called. It reads all three layers because they share a frame header, but
::GAUD_Sample_Coding keeps them apart - `GAUD_CODING_MPEG_LAYER1`,
`_LAYER2` and `_LAYER3` - since a Layer II frame and a Layer III frame of
the same length at the same rate have four bytes in common and nothing
after them.

## What is implemented

**Identification.** The four-byte frame header in every combination the
standards define, with every reserved encoding refused. A stream is found
past an ID3v2 tag of any size, or past leading bytes that are not a frame
at all, and a candidate is believed only when the frames its own stated
length points at are there too.

**Length, with its provenance.** gaud_track_frames() answers, and the
diagnostics say how it was arrived at: stated by a Xing, Info or VBRI
frame, counted exactly, estimated from the bitrate, or not known. The
table below is the whole of it.

**Encoder delay and padding.** From the LAME extension, where there is
one, as a ::GAUD_Trim - so gaud_track_duration() is the recording's
length rather than the file's contents', and gapless playback is possible
on top of it.

**Tags.** ID3v2.2, 2.3 and 2.4 at the front, ID3v1 in the last 128 bytes,
through the same readers the WAV and AIFF codecs use for their ID3 chunks.
An APE trailer is detected, excluded from the audio, and not interpreted.

**No decoder.** The codec declares no `GAUD_CAP_DECODE` and has no decoder
entry point, so gaud_decoder_create() answers `GAUD_ERR_UNSUPPORTED`.
That is deliberate rather than unfinished-and-hidden: a decoder that
returned silence would produce the right number of bytes, crash nothing,
and sound like a quiet passage.

## The length is a judgement, and it says which one

Everything else this library reads states its own length in a header.
**An MPEG audio stream states nothing anywhere**: it is a run of frames
with no index, no count and no end marker, and the file may have been cut
with a text editor. So four answers are possible and they are not equally
good:

| How | When | How exact |
| --- | --- | --- |
| **Stated** | A Xing, Info or VBRI frame says so | Exact, if the writer was honest |
| **Counted** | The stream is short enough to walk | Exact |
| **Estimated** | The bitrate does not change over the frames looked at | Exact for a constant-rate file, wrong for a variable-rate one with no tag |
| **Unknown** | The bitrate varies and nothing states a length | `UINT64_MAX`, and a duration of -1 |

Two of those carry a diagnostic saying what was assumed. The fourth is
the one worth dwelling on: a library that produced a number there would be
*usually* right, and the shape of its wrongness - a few percent, on files
whose encoder wrote no tag - is the shape nobody notices and nobody can
debug.

**A stated length is bounded before it is believed.** A Xing frame count
is a number an encoder wrote once and nothing has checked since, and a
file that has been cut or rewritten still carries the original's. So it is
compared against what the bytes could hold at the lowest bitrate the
format defines, and a count larger than that is ignored with a
diagnostic. A fabricated duration is not a cosmetic problem: a caller
allocates from it and a player scrubs against it.

## The delay nobody writes down, and the 529 everybody adds

An MPEG encoder primes its filterbank before the recording's first
sample, so a decode is longer than what went in, and **nothing in the
format says by how much.** LAME writes the number into its extension of
the Xing tag, and that is the only reason gapless playback of MP3 works
at all.

What a player needs is the delay in the samples a *decoder* produces,
which is the encoder's delay plus the decoder's own - 529 frames, being
the group delay of the analysis and synthesis filterbanks in series. That
constant is not in any file and is the same for every decoder. So
::GAUD_Trim::encoder_delay is the stated delay plus 529 and the padding is
the stated padding minus it, floored at zero.

::GAUD_Trim::stated is what separates a file whose delay is zero from one
whose delay was never mentioned. The second MP3 encoder in this library's
corpus writes a length tag whose delay and padding are both zero, and both
reference decoders still subtract the 529 - so "stated as zero" and "not
stated" really are different facts with different consequences.

**The extension is believed only when it is there.** A writer that filled
in the Xing fields and stopped leaves the frame's own data where the
extension would be, which for a silent first frame is zeros; reading a
delay out of that trims a track by an arbitrary number of frames, which
sounds like a click rather than like a bug. The test is that the nine
characters of the encoder string are printable. It was a list of two names
first - `LAME` and `Lavc` - and that was wrong: ffmpeg asked for bit-exact
output writes `Lavf lame`, which is the spelling every fixture in this
library's corpus carries, and both reference decoders read the delay out
of all three.

## What is refused, and why that is the answer

- **The free format.** Bitrate index 0 means the frame states no rate, so
  its length is the distance to the next sync word. It is legal, it is
  vanishingly rare, and this library does not search for it:
  `GAUD_ERR_UNSUPPORTED` with a diagnostic naming the format, rather than
  `GAUD_ERR_FORMAT`, because "I know what this is and cannot read it" is a
  different thing to be told.
- **A stream that is not seekable.** Every other codec here reads its
  container forwards. This one cannot, and the reason is the format: the
  length comes from the file's size and from trailers at the *end*, and a
  candidate frame is confirmed by reading ahead and then carrying on. A
  pipe could be decoded frame by frame; it cannot be opened into a
  document that answers gaud_track_frames().
- **A stream that ends inside its own tag frame.** The frame holding a
  Xing tag is not audio, so it is skipped - and if the skip runs off the
  end there is no audio at all. `GAUD_ERR_CORRUPT`, rather than a document
  whose data offset is outside its own stream.

## How it is scored

**Two independent decoders, not four names for one.** For FLAC this
library's oracle image holds four references that are two implementations;
for MPEG audio it holds ffmpeg's native decoder in libavcodec and
libsndfile's, which is minimp3, and those are genuinely separate. **sox is
not a reference here at all** - the build in the image has no MP3 handler,
which is a fact about the package and not about sox, and it was measured
rather than assumed.

The corpus is written by four encoders: LAME and libshine for Layer III,
TwoLAME and libavcodec's own for Layer II. That matters more for a
perceptual codec than it did for FLAC, because an encoder's output is a
set of *choices* - window switching, stereo mode, how much of the bit
reservoir to use - and a decoder that has only ever read LAME has met one
encoder's taste rather than the format.

What is checked today, with no decoder, is the length: every fixture's
stated frame count minus its stated trim is compared against the number of
samples ffmpeg and libsndfile actually produce from it. Fifteen of the
sixteen agree with both.

**The sixteenth is a disagreement with ffmpeg, and it is resolved against
it.** The fixture carrying an ID3v1 trailer decodes to 2,351 frames in
ffmpeg and 2,003 in libsndfile, and 2,003 is the number of samples the
encoder was given. 2,351 is 3,456 minus 1,105, which is the start trim
applied and the end trim not. A minimal pair settles it: the same file
written with `-write_id3v1 0` decodes to 2,003 in ffmpeg too, so the
128-byte trailer is what defeats its end trim.

## Known gaps

- **No decoder.** The rest of phase 5.
- **No encoder.** Phase 8, with the two-gate harness perceptual output
  needs.
- **The free format**, as above.
- **VBRI's seek table is read as a length and not as a table.** Its
  geometry is stated rather than fixed, unlike Xing's hundred entries, and
  nothing seeks yet.
- **APEv2 is excluded, not read.** It is tier 3 metadata; the diagnostic
  says it was found.
