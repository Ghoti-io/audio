# Opus {#format_opus}

Opus in Ogg, as RFC 6716 and RFC 7845 define them, **read and decoded**.
The codec is registered as `opus`, and ::GAUD_Sample_Coding spells it
`GAUD_CODING_OPUS`.

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

## The decoder

**SILK, CELT and hybrid**, mono and stereo, at every one of the 32
configurations (2.5 to 60 ms frames, narrow band to full band), with the
mode changing from one packet to the next as it does in a real stream.
The output is always 16-bit at 48 kHz, whatever rate the encoder was
given: SILK runs at 8, 12 or 16 kHz and is resampled up by the same
filter the reference uses, and that filter is part of what conformance
compares.

**It is bit-identical to RFC 6716's own decoder**, which is the fixed-point
build of the code printed in Appendix A, and not merely close. Section 6
asks for two things: that `opus_compare` accepts the output, and that the
decoder's final range decoder state after every packet is the
reference's. The second is an exact equality over every symbol a frame
contained and cannot be approximated. Both hold for all twelve
conformance vectors, and so does something section 6 does not ask for:
every output sample equals the reference's.

**Redundancy frames.** At a switch between CELT and SILK or hybrid an
encoder may include a 5 ms CELT copy of the audio around the switch, and
the decoder cross-fades with it. When it did not, the decoder conceals a
packet that was never lost to make the boundary smooth anyway, so
concealment is part of decoding a stream that loses nothing.

**Packet-loss concealment**, in all three modes, in the reference's
exact arithmetic. SILK repeats the last pitch pulse through the last
filter with noise drawn from its own excitation, faded; CELT either does
that or, from the fifth consecutive loss, generates noise with the
background spectrum; and a good frame after a loss is blended in rather
than cut in. RFC 6716 calls this informative, and a decoder is free to do
something else - but then it would not reproduce the transitions above.
The decoder entry point conceals a packet when asked for one with no
data, which the track decoder does not do: Ogg Opus stores every packet,
and a gap in a *file* is corruption rather than loss.

**Multistream.** Mapping family 1 (the Vorbis channel orders up to
eight, mapped onto the WAV order the rest of the library uses) and
family 255 (channels with no defined positions) multiplex up to 255
Opus streams in one Ogg packet, all but the last in Appendix B's
self-delimiting framing. Streams may be coupled pairs or single channels,
and a channel mapped to 255 is silent.

**The output gain** in `OpusHead` is applied, as RFC 7845 section 5.1
says a decoder should, in the fixed-point form libopus uses: the gain in
Q7.8 decibels becomes a base-two logarithm, then a multiplier, and the
result is rounded and clamped to plus or minus 32,767.

**Seeking decodes from the start.** An Opus decoder's state converges
rather than resets, which is why RFC 7845 suggests 80 ms of pre-roll
before the target. Starting part-way through would give samples that
differ in their last bits from a straight read's, and nothing here could
then say which was right. So a seek decodes forward from the beginning
and reads exactly what reading would have read, at the cost of a seek
taking as long as decoding to the target. The page bisection in the Ogg
layer would remove the cost, and using it is a decision about giving up
that guarantee.

## What is not

**Forward error correction.** An Opus packet may carry a low-bitrate copy
of the *previous* frame so that a lost one can be recovered from its
successor. The decoder reads and discards it, as it must, and never uses
it: a file has no lost packets, and the live-stream API that would ask
for it does not exist here.

**RFC 8251's corrections.** The decoder is RFC 6716's reference, bit
for bit. RFC 8251 changes how hybrid frames fold the second CELT band
and zeroes SILK's stereo state on a mode switch, both of which alter
output for the streams they touch, and tightens several checks that
matter only for invalid input. None is applied; doing so is a decision
to re-pin against a patched reference, not a defect fix.

**Custom modes**, RFC 6716's `opus_custom`: frame sizes and sample rates
outside the 32 configurations. No encoder in common use makes them.

**Writing.** Phase 8.

## The gates

`make check-opus-vectors` is section 6 of RFC 6716 as a gate, and the
strictest in the library. It decodes the twelve conformance vectors
(39 MB, fetched deliberately by `make opus-vectors` and pinned by hash)
and requires, for each: every packet accepted; every one of the 20,075
final range decoder states equal to the vector's; `opus_compare`, run in
the pinned reference image, accepting the output; and the output equal
to the bit to what the RFC's fixed-point reference writes, which is
pinned per vector. It was seen to fail on purpose: one wrong all-pass
coefficient in the SILK resampler leaves every range state intact - the
resampler reads no symbols - and is caught by the other two.

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
raw PCM of a length the generator knows. Then, since the decoder exists,
the samples: each mono and stereo fixture is decoded by `opusdec` and by
this library and put to `opus_compare`, with controls that must be
rejected.

**The unit tests go further than either gate**, because they can run
RFC 6716's decoder on the same bytes. They hold the digests of its output
for every fixture (the six-channel one included, which `opus_compare`
cannot take), and for 15,000 random packets - with and without losses,
through every mode change, every redundancy form and every stereo
transition - from a harness that compared each sample and each final
range state to the reference's before the digest was recorded.

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
