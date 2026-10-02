# Opus {#format_opus}

Opus in Ogg, as RFC 6716 and RFC 7845 define them. **Identified, not yet
decoded.** The codec is registered as `opus`, and ::GAUD_Sample_Coding
spells it `GAUD_CODING_OPUS`.

This page is written to be rewritten. What is here is the half of the
format with no decoder in it, which planning/audio.md section 11.18
argues is a commit of its own and warns must not become a resting place.

## Four things with no analogue elsewhere here

**The sample rate is always 48,000 and the file's own rate is advice.**
RFC 6716 lets a decoder output at 8, 12, 16, 24 or 48 kHz whatever the
encoder was given; the Ogg mapping counts granule positions at 48 kHz;
and `OpusHead` records the original rate in a field the specification
marks informational. So `gaud_track_sample_rate()` answers 48,000 for
every Opus file and the header's number is reported nowhere.

**The length needs the pre-skip subtracted.** Every Opus stream begins
with samples the encoder's own filters needed and the recording does not
contain - a number of them stated in `OpusHead` - and the granule
positions count those. So the length is the last page's position minus
the pre-skip. This is the gapless arithmetic MPEG audio needed a Xing
tag for, except that here it is in the format and is not optional; a
reader that forgot would report every file several milliseconds long.

**A packet may hold several frames.** One byte of table of contents says
which of thirty-two configurations the packet uses and how many frames
follow, in one of four framings. A 60 ms packet may be three 20 ms
frames, so counting packets is not counting anything.

**The mode changes within a stream.** A packet is SILK, CELT, or both at
once, chosen by the configuration, and may differ from the packet before
it. That is the point of the design and is why ::GAUD_CODING_OPUS is one
value rather than three.

## What is implemented

**Both headers.** `OpusHead` with every field and every bound - including
the version byte, whose **major version is the top nibble**: RFC 7845
requires a decoder to refuse a major version it does not know and to
accept any minor one, because a minor bump only ever appends fields. A
reader comparing the byte to 1 would refuse every future minor revision
of a format that promised they would keep working.

**All three channel mapping families.** 0 is one or two channels; 1 is
the Vorbis orders up to eight, so the permutation \ref format_vorbis
"formats/vorbis.md" describes applies here too; 255 is channels with no
defined positions. 2 to 254 are reserved and refused, because guessing
at speaker positions is worse than declining.

**The packet framing**, all four codes, with what each cannot say
refused: a code 3 packet of zero frames, which would advance a stream by
nothing; a packet past 120 milliseconds, which the format caps; a code 1
packet whose remaining bytes do not divide in two; a code 2 packet whose
stated first length runs past its end.

**The tags**, out of `OpusTags`, through the same Vorbis comment reader
FLAC and Vorbis use - and **without the framing bit** Vorbis's own
comment header ends with, which RFC 7845 drops. That costs nothing
because the reader stops where the comment list ends rather than at the
end of its buffer.

## What is not

**The decoder.** `GAUD_CAP_DECODE` is not declared, so
`gaud_decoder_create()` answers ::GAUD_ERR_UNSUPPORTED. A caller can ask
rather than read a paragraph.

**Writing.** Phase 8.

## The gate, and the one that is waiting

`make check-opus` scores the identification against **three** readers,
and unusually for this library there is nothing to exclude: the three
disagree, and the disagreement reconciles.

- `opusdec` decodes exactly the number of frames we report. It is
  libopus, the implementation RFC 6716 defines conformance against.
- `opusinfo` prints a playback length equal to ours.
- `ffprobe`'s `duration_ts` is ours **plus the pre-skip**, on every
  fixture - ffmpeg reports the granule position and we report the
  recording. That is checked as an equation rather than set aside, which
  is the stronger thing to do with a disagreement whose shape is known.

And a fourth reading with no decoder in it: every fixture is made from
raw PCM of a length the generator knows.

**`opus_compare` is in the oracle image and is called by nothing.** RFC
6716 does not define Opus by a bitstream and a transform; it defines a
conforming decoder as one whose output that tool accepts against the
reference decoder's. It is the only normative reference in this tree -
planning/audio.md section 11.15 is the long apology for MPEG audio not
having one, because ISO/IEC 11172-4's vectors cannot be obtained - and
it will have something to accept when the decoder lands.

## The corpus

Fifteen fixtures from one encoder, and the one encoder is the format
rather than a gap: libopus is the only Opus encoder that exists in any
meaningful sense, and conformance is defined against its own decoder
anyway.

What varies is the mode, steered by `-application` and `-cutoff`
together. The corpus reaches eleven of the thirty-two configurations:
SILK at all three bandwidths and at 20, 40 and 60 ms; hybrid at both of
its; CELT at four bandwidths and at 2.5, 10 and 20 ms. Framing codes 0
and 3. One, two and six channels - the last being mapping family 1.

**The other twenty-one configurations are checked by hand**, against RFC
6716's Table 2 transcribed into the unit tests, because an encoder
chooses the handful its settings produce and a configuration decoded as
the wrong frame size is a stream that plays at the wrong speed rather
than one that fails.
