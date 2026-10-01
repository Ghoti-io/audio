# MPEG audio {#format_mpeg}

MPEG-1, MPEG-2 and MPEG-2.5 audio, Layers I, II and III - which is to say
MP3 and the two layers nobody calls by name. **Read, in all three
layers.** There is no encoder; phase 8 brings those.

The codec is registered as `mp3`, because that is what the files are
called. ::GAUD_Sample_Coding keeps the layers apart -
`GAUD_CODING_MPEG_LAYER1`, `_LAYER2` and `_LAYER3` - since a Layer II
frame and a Layer III frame of the same length at the same rate have four
bytes in common and nothing after them.

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

**Decoding, in integer arithmetic.** Layer III with every window type,
both joint stereo modes, the bit reservoir and all 30 usable Huffman
tables; Layer II with all five allocation tables and its intensity
stereo; Layer I with every quantiser width. **Every sampling frequency
the format has**, which is to say MPEG-1's three, MPEG-2's three and
MPEG-2.5's three - see below for where the last three's scalefactor band
tables come from, since no standard contains them. The output is signed
16-bit, and **there is no floating point anywhere in the decoder** -
which is what makes the next paragraph possible.

**Saturating arithmetic, not wrapping.** `global_gain` is eight bits and
every one of its 256 values is legal, so an ordinary frame can ask for a
spectrum near the end of what a Q28 int32 holds - and then ask for two
such values to be added. Mid/side, the alias-reduction butterflies, the
overlap-add between granules, the frequency inversion and both
filterbank accumulators all clamp rather than wrap. A wrap turns a loud
passage into a click; a saturation turns it into a loud passage, and
signed overflow is undefined besides. The clamping is counted, so a loud
file and a clipping decoder are different facts from outside.

**The same bytes on every architecture.** `make check-golden`
cross-compiles this library for s390x and powerpc64, decodes the whole
corpus under qemu, and compares hashes with this machine. That is a
promise about this library rather than about a reference, and no
comparison against a reference could establish it: both references are
floating point and are entitled to differ from themselves between builds.

## MPEG-2.5, and where a table with no standard comes from

MPEG-2.5 halves MPEG-2's sampling frequencies again, to 8, 11.025 and
12 kHz. **It is not in any standard**: neither ISO/IEC 11172-3 nor
ISO/IEC 13818-3 defines that version, the frame header spells it with a
field value 11172-3 marks reserved, and its Layer III scalefactor band
tables appear in no document. Every other table in this library is
extracted from one of those two texts by `tools/tables/gen_mp3_tables.py`,
so these three sampling frequencies needed a different answer.

**Two of the three needed no new data at all.** The band tables for
11.025 and 12 kHz are not similar to MPEG-2's 16 kHz tables, they are the
same numbers, so those two rates index the row this library already
generates from 13818-3 and their support rests on that document. The
generator asserts the equality when it builds the tables and a unit test
asserts that the mapping in the C actually lands on that row, because the
two are different claims and a decode depends on both.

**Only 8 kHz is new**, one long row of 22 bands and one short row of 13.
Those come from minimp3, whose CC0-1.0 dedication puts them in the public
domain, and what makes taking them sound is not that implementation's
reputation but the overlap: minimp3 carries all nine sampling
frequencies, six of them are in the two standards, and **all twelve of
those long and short rows are compared against what was extracted from
the documents before any of the remaining rows is used.** Twelve of
twelve are identical, including the padding band the generator computes
rather than reads. A single disagreement fails the run and nothing from
minimp3 is used.

Two further readings were taken and both agree on all nine rates:
ffmpeg's, which indexes its tables per rate where minimp3 collapses two
of them, and **LAME's own encoder tables** - which is the one that
settles it. For a format with no standard the correctness criterion is
not conformance to a text but agreement with the encoders that write the
files, and LAME is the encoder that wrote every MPEG-2.5 fixture in this
corpus. An encoder and a decoder that disagree about the band map produce
wrong audio, so the encoder's table is the authority there is.

## What it is scored against, and how closely

Two reference decoders, and for MPEG audio they really are two
implementations: ffmpeg's native decoder in libavcodec, and libsndfile's,
which is minimp3. Neither is the other's front end - unlike FLAC, where
four of this library's references are libFLAC wearing different hats.
**sox is not a reference here**: the build in the oracle image has no
MPEG handler at all, which was measured rather than assumed.

There is also a gate that consults **no other decoder at all**.
`make check-mpeg-input` compares our decode against the signal the
encoder was given, which is possible because the corpus is synthesised
from closed-form expressions and so the input can be regenerated rather
than stored. It exists for MPEG-2.5 specifically: both references carry
byte-identical copies of the same band tables, so a differential against
them is blind to one class of error - a table every implementation agrees
on and that is wrong about the actual spectrum. Measured, every band the
encoder kept is within **1.96 dB** of the input across every fixture -
0.71 dB at 8 kHz, 0.65 dB on the MPEG-1 calibration, and 1.96 dB at
12 kHz where the encoder has the least room. With the 8 kHz row
deliberately replaced by the 16 kHz one the same measurement reads 5.24
and 6.33 dB, so the 3 dB threshold sits about a decibel above the worst
correct answer and two below the wrong one.

`make check-mpeg` scores every fixture against both, and the agreement is
**one least significant bit of 16** - the difference is 88 to 106 dB
below the signal, which is the whole of what an integer decoder and a
floating-point one can differ by. The gate does not stop at the
difference, because planning/audio.md section 12 names four failures that
a difference-only comparison cannot see: the frame count is asserted
exactly before anything is compared, the absolute level of our own output
is checked, the DC offset is checked, all of it per channel, and the
whole gate is run against eight deliberately wrong versions of our own
answer, every one of which it must reject.

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

## Where the references disagree, and with whom

The corpus is written by four encoders: LAME and libshine for Layer III,
TwoLAME and libavcodec's own for Layer II, plus one Layer I file built by
hand because **nothing in the oracle image writes Layer I at all**. Four
writers matter more for a perceptual codec than they did for FLAC,
because an encoder's output is a set of *choices* - window switching,
stereo mode, how much of the bit reservoir to use - and a decoder that
has only ever read LAME has met one encoder's taste rather than the
format.

Two fixtures turn up a reference disagreement, and both are resolved
against ffmpeg with a measurement rather than an argument:

- **An ID3v1 trailer defeats ffmpeg's end-of-stream trim.** The tagged
  fixture decodes to 2,351 frames in ffmpeg and 2,003 in libsndfile, and
  2,351 is 3,456 minus the 1,105-frame start trim: the start applied and
  the end not. The same file written with `-write_id3v1 0` decodes to
  2,003 in ffmpeg too, and 2,003 is the number of samples the encoder was
  given.
- **A channel mode that changes between frames makes ffmpeg drop the
  first one.** TwoLAME's joint stereo output is joint stereo, then plain
  stereo, then joint stereo with a different bound - all legal, since the
  mode is a per-frame field. ffmpeg's demuxer requires it to be stable
  and skips the leading frame; this library compares the channel *count*
  and decodes all of them, as libsndfile does. Set ffmpeg's own cold-start
  aside and its samples agree with ours to half a least significant bit.

## How much of the decoder the corpus reaches

`make mpeg-coverage` is not a gate. It counts which named arms of the
decoder a corpus actually executes, because a differential that passes
says nothing about how much of the decoder ran to produce it - which is
the lesson phase 4 learned when a third of the FLAC decoder turned out
never to have executed while every reference agreed byte for byte.

As of this writing it reaches 63 of 72 arms, and **nothing reachable is
unreached.** Of the nine it does not: two are unreachable by construction
(the standard marks Huffman tables 4 and 14 unused and this library
refuses a frame that selects one); five need an encoder nothing in the
image is, including intensity stereo, which LAME has never implemented,
and the mixed block, which nothing emits; and two - a granule with no
reservoir, and a sample that saturated - are reached by unit tests on
hand-built frames instead, which is the right answer, since nothing in
the corpus is that loud and every fixture's first frame is grounded.

Three fixtures were added because this instrument named the arm they
reach. **The last five Huffman tables fell to something else entirely**:
giving the stereo noise and transient fixtures channels that actually
differ. They had been listed for two phases as "reachable, and LAME
simply never chose them", and the reason it never chose them was that
those fixtures' two channels were bit-identical, so every region of
every granule saw the same maximum value. The fix was to the corpus's
signal generator and not to any fixture written for the purpose.

## Known gaps

Three entries left this list when MPEG-2.5 was implemented, and one of
them is worth naming as a near miss: the short-block Huffman region
boundary is "the first 36 lines" in the standard's prose, and that
sentence is true for eight of the nine sampling frequencies. It is three
short bands counted once per window, and MPEG-1's first three short bands
are four lines each; at 8 kHz they are eight lines each and the region is
72. Written as the band count it always was, it would have been right
everywhere. Written as 36 it was wrong at one rate, and the shape of the
failure is the argument for the corpus having both an 8 kHz mono file and
an 8 kHz stereo one: LAME chose short blocks for the mono file only, so
stereo at 8 kHz agreed with both references to one bit while mono was
2.1 dB out.

- **Mixed blocks have never been exercised**, and at 8 kHz the two
  reference decoders disagree about them. The code is there; no encoder
  in existence emits one. For the MPEG-1 and MPEG-2 rates the reading
  used is 36 lines of long block, and minimp3's own mixed-block table
  agrees with that at all six - 8 bands of 4 or 6 at MPEG-1 rates, 6
  bands of 6 at the MPEG-2 ones, 36 lines either way - which retires the
  part of this gap that was about the standards disagreeing. At 8 kHz
  minimp3's table gives 3 bands of 12, again 36 lines, while ffmpeg
  handles "the 72 first exponents as long blocks" and refuses the case
  outright with a request-sample diagnostic. Nothing can write the input
  that would settle it, so neither reading is implemented in preference:
  8 kHz mixed blocks take the same 36 lines as everything else and the
  disagreement is recorded here.
- **No encoder.** Phase 8, with the two-gate harness perceptual output
  needs.
- **The free format**, which states no bitrate, so a frame's length is
  the distance to the next sync word. Refused by name.
- **A non-seekable stream.** The length comes from the file's size and
  from trailers at the end; see above.
- **VBRI's seek table is read as a length and not as a table.** Seeking
  counts frames from the start instead, which is exact and costs one
  four-byte read per frame.
- **APEv2 is excluded, not read.** It is tier 3 metadata; the diagnostic
  says it was found.
