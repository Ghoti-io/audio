# Vorbis {#format_vorbis}

Vorbis I in Ogg, as the Xiph specification defines it. **Identified, not
yet decoded.** The codec is registered as `vorbis`, and
::GAUD_Sample_Coding spells it `GAUD_CODING_VORBIS`.

This page is written to be rewritten. What is here is the half of the
format with no decoder in it, which planning/audio.md section 11.18
argues is a commit of its own and warns must not become a resting place.

## What is implemented

**The three header packets.** A Vorbis stream begins with exactly three:
identification, comment and setup, in that order, with the first alone on
the first page. All three are read; the first two are kept and the third
is validated and discarded, because everything in it - the codebooks, the
floor curves, the residue layout, the channel coupling and the block
modes - is a property of how the audio is coded and not of what the track
is. A stream missing any of them is refused at open rather than at the
first read.

**Every field of the identification header, with every bound.** The
version, which must be zero and is refused otherwise as a different
bitstream wearing the same signature; the channel count and sample rate,
which must be nonzero and are otherwise unconstrained - Vorbis tabulates
no list of legal rates, so 12,345 Hz is as valid as 44,100; the three
bitrate hints, which are advisory and are not reported as anything; the
two block sizes; and the framing bit, whose only job is to be set.

**The tags**, out of the comment header, through the same Vorbis comment
reader FLAC uses - it is the same structure, which is where FLAC got it.
The vendor string is read past and deliberately not turned into a tag;
src/meta/vorbis_comment.c says why, and the short version is that a field
the writer overwrites must not be a field the reader collects.

**The length.** See below; it is the only interesting thing on this page.

## What is not

**The decoder.** `GAUD_CAP_DECODE` is not declared, so
`gaud_decoder_create()` answers ::GAUD_ERR_UNSUPPORTED for a Vorbis
track. A caller can ask rather than read a paragraph.

**Writing.** Phase 8 brings the perceptual encoders with the two-gate
harness their output needs.

**Floor type 0 and residue type 0** will be unexercised by any corpus
this tree can build, and that is worth recording before the decoder
arrives rather than after. Both are in the specification; neither is
produced by libvorbis or by libavcodec at any setting, because both were
superseded before Vorbis I was finished. Whatever this library does with
them will be scored by reading the specification and by hand-built
streams, not by a fixture.

## The length is not in the file

Nowhere in a Vorbis stream is there a sample count. Not in the
identification header, not in a trailer, not in a tag. And it cannot be
computed from the packets either, because a packet's contribution depends
on the block size of the packet *before* it: a block of N samples
overlaps its predecessor by half, so a packet contributes
`(previous + this) / 4` samples and the first packet of a stream
contributes nothing at all.

What answers it is the granule position on the last page, which Ogg
carries and attaches no meaning to beyond "monotonic". So
`gaud_ogg_last_granule()` had to exist before this codec could, and it
scans backwards in widening windows from the end of the file - because
the last page of a *file* need not belong to the logical stream being
asked about.

Two consequences:

- **An unseekable stream cannot be opened into a document.** The same
  refusal MPEG audio makes, for a stronger reason: an MPEG stream's
  length can at least be estimated from its bitrate, and a Vorbis
  stream's cannot be estimated from anything.
- **A file whose last page states no position opens with its length
  unknown**, reported as `UINT64_MAX` with a diagnostic, rather than as
  zero. A caller cannot tell a length of zero from a length nobody knows.

## Two references, and they are not the two you would pick

ffmpeg has two Vorbis decoders - libavcodec's own and a wrapper around
libvorbis - and asking it twice feels like two readings. For *sample
values* it is: given the same file the two differ by up to 2 of 32,768 on
7,216 of 8,562 samples, which is two floating-point implementations
rounding differently. For the *length* it is one reading, because the trim
is applied in ffmpeg's Ogg demuxer, which both of them sit behind.

And ffmpeg's answer there disagrees with everything else. For a
4,409-frame file ffmpeg decodes 4,281 samples - 128 short, which is half
the short block size - and for a 1,601-frame file it decodes 1,792, which
is 191 *over*. libsndfile, which reaches Vorbis through libvorbis's own
`vorbisfile` layer, decodes exactly 4,409 and exactly 1,601. So does
`ffprobe`'s `duration_ts`, which is ffmpeg reading the granule position
itself rather than computing a trim from it.

The specification is not ambiguous: Vorbis I section A.2 makes the final
granule position the length of the stream. `make check-vorbis` scores us
against ffprobe and libsndfile, names ffmpeg's decoded sample count as an
exclusion with the numbers above, and checks all of it a third way that
involves no decoder at all - against the frame count the generator fed
each encoder.

## Two writers in the corpus

Ten fixtures, from libvorbis and from libavcodec's native encoder, and
the second one matters here for the reason a second MP3 encoder mattered:
**a Vorbis stream defines its own codebooks, floor curves, residue layout
and block modes**, so a decoder that has only ever read libvorbis output
has met one encoder's taste in all four rather than the format.

The axis worth naming is the block sizes, because no single setting
reaches more than one pair. The corpus has 256/2048, 512/1024, 512/512,
1024/1024 and 2048/2048 - and **the three pairs where the two are equal
are the interesting ones**: a stream whose long and short blocks are the
same size never switches, so its window shape and overlap are constant
and a decoder that assumed switching always happens works on it
perfectly. libvorbis chooses these from the sample rate and the quality,
which is why the corpus reaches them by way of 8 kHz and 22.05 kHz rather
than by asking for them.

One fixture's stated length is deliberately not its recording's:
`vorbis_ff_stereo_44100.ogg` is 4,416 frames for 4,409 in, because
libavcodec's encoder pads the final block and states the padded length
rather than setting the last granule position to its input count. It is
in the corpus *because* of that - a granule position that is not a round
number of input frames is the only thing that distinguishes reading the
field from computing it.
