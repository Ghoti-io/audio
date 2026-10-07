# MPEG audio {#format_mpeg}

MPEG-1, MPEG-2 and MPEG-2.5 audio, Layers I, II and III - which is to say
MP3 and the two layers nobody calls by name. **Read, in all three
layers; written, in Layer III.** See "Writing MP3" below for what the
encoder does, how it is checked, and what it does not do.

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

## Writing MP3 {#mpeg_writing}

`gaud_encoder_create("mp3", ...)` with `coding = GAUD_CODING_MPEG_LAYER3`,
signed 16-bit samples, one or two channels, and any of the nine sampling
frequencies MPEG-1, 2 and 2.5 define. **It is a full encoder and not a
stub**, which is a claim about measurement and not about intent; the
section on checking says what was measured and how weak a proxy it is.

**Settings.** ::GAUD_Encode_Params carries `rate_control`, `bitrate`,
`min_bitrate` and `quality`:

| `rate_control` | `bitrate` | What it writes |
| --- | --- | --- |
| `GAUD_RATE_CBR` (and the default) | the rate, from the version's table; 0 picks 128 kbit/s stereo or 64 mono for MPEG-1 and half that below | every frame the same size |
| `GAUD_RATE_ABR` | the average wanted; must be in `[min_bitrate, the version's top]` | frames of any size, steered so the file's average lands on it (within about 4 per cent over twelve seconds, measured) |
| `GAUD_RATE_VBR` | the ceiling, or 0 for the version's top | the smallest frame that holds what the audio needs; `quality` (1 to 100, 0 meaning 50) says how much noise may stand under what the ear masks, a point being 0.4 dB, and the default sits where LAME's `-V4` does on the same input |

**Tags.** An ID3v2.4 tag at the front from the metadata, and a tag frame
after it that says `Info` for a constant-rate file and `Xing` for any other.
The tag states the file's frames and bytes, a hundred-entry table of
contents for a variable-rate one, and - in the extension LAME added - the
encoder's delay and padding, which are 528 and whatever the last frame
leaves. The tag is rewritten at the end of the file, so the stream has to
accept a seek. **The encoder's name field says `LAME(G<major>)`**, with this library's
major version, and the reason is one reader. The delay and padding make a
file play gaplessly, and three readers were measured against it:

| Reader | Applies the tag's delay and padding |
|---|---|
| ffmpeg (libavformat) | only if the name's first four bytes are `LAME`, `Lavf` or `Lavc`; case-sensitive, nothing after them is read |
| libmpg123, and libsndfile through it | whatever the name says |
| this library | whatever the name says, if it is printable |

ffmpeg's demuxer is behind most players and browsers, so a name it does not
recognise gives a file that plays 1,057 samples late and ends with 609 of
padding nearly everywhere. The four letters are therefore **a prefix a reader
requires and not a claim to be LAME's code**: nothing parses what follows,
the digits are this library's major version, and the encoder is this
library's own. The honest alternative, `Ghoti.io`, is what shipped first, and
it was reversed for exactly that cost. A major release changes every file's
bytes (the tag carries it), and `make check-golden` compares architectures
with each other and not with stored hashes, so nothing needs recomputing.

**How a frame is made.** In the order a granule travels:

1. A 32-band polyphase analysis and an MDCT of 36 lines or three of 12,
   which are the decoder's synthesis run backwards - the decoder's window
   and matrices, scaled by 1/9 and 1/3 so that the pair is the identity.
2. A psychoacoustic model in the shape of ISO 11172-3's second one: a
   windowed FFT of 1024 samples (256 for a short block), a per-bin measure of
   how predictable the bin is from the two spectra before it, energy summed
   into partitions a third of a Bark wide, Schroeder's spreading function,
   and a threshold 18 dB under a tone and 6 under noise, never less than the
   threshold of hearing (taken as 16 dB lower than the textbook curve, because
   a listener turns a quiet passage up), and held to at least 24 dB of signal-to-noise below
   600 Hz, falling to 10 at 3 kHz and 3 above. Out of it come the noise
   each scalefactor band may carry.
3. Block switching, from that model's thresholds: a window whose energy in a
   band that matters is ten times that of the two before it, or a granule
   whose loud window is far over the noise a long block would leave in a
   band where another window is far under it, starts a transient, and the
   legal sequences (start, short, stop) follow. A granule's type waits one
   granule for its successor's, so the encoder runs one granule behind.
4. Mid/side, per frame, when it costs fewer perceptual bits than left/right;
   the thresholds of both are then the smaller of the two channels'.
5. The quantiser, with its two loops, in integers: a table of thresholds
   finds the value whose requantisation is nearest, the step is bisected to
   fit the bits or the masking, and each band's scalefactor is moved toward
   the median margin, both ways, so a surplus buys an even noise-to-mask
   ratio.
6. Every granule is first coded as coarsely as masking allows. What that
   used is what it needed; a frame that can afford all of them is that frame,
   one that cannot shares what it has in proportion to need, and one that
   has more than the reservoir should keep spends the rest on finer steps.
7. An exhaustive search of the Huffman regions and tables (prefix sums per
   table, every split the format can state), the scalefactor field widths,
   and scalefactor reuse between the two granules of an MPEG-1 frame.
8. The bit reservoir: a frame is written once nothing more can land in its
   main-data area, and what a frame saves is stuffed away only past 511
   bytes (255 for MPEG-2 and 2.5).

**The same bytes on every architecture.** Everything above is integer
arithmetic, including the model: its FFT is fixed point with growth bounded
by construction, the phase prediction is a complex product of unit vectors,
the logarithm and the power of ten are table routines, and the Bark scale,
the threshold of hearing and the spreading function are generated tables.
`make check-golden` encodes seven MP3s - constant, average and variable
rate, three MPEG versions, with block switching and mid/side - on s390x and
powerpc64 under qemu and compares bytes with this machine's. It found the
first thing that was not so: the tag's checksum summed 190 bytes of a frame
that could be 156, which on one machine was whatever followed it in memory.

**How it is checked**, in three layers, because no single one sees enough:

- *Unit tests* state what a reference cannot be asked: every Huffman code
  word decodes back through the decoder's own trees; the header writer and
  the decoder's parser agree over every version, bit rate and rate; the
  quantiser and the decoder's requantiser are inverses and the fast quantiser
  agrees with a full search on twenty million values; the model's thresholds
  sit where the constants say; a burst in silence has far less noise before
  it than a long block would leave; an impulse comes back at its own sample at
  every alignment; the file is a function of its samples and not of how they
  were cut into writes.
- `make check-mp3-encode` encodes 108 signals - full-scale noise and square
  waves, the highest frequency the format holds, silence between bursts, a
  channel silent or inverted, a recording one bit deep - at nine sampling
  frequencies in all three modes, and requires ffmpeg's decoder, mpg123's, libsndfile's
  (which is libmpg123 again) and this library's to read each to the same
  samples and the recording's exact length, the tag to be true,
  every frame's back-pointer and granule lengths to fit the bytes, and the
  recording to be in the decode. Three deliberately broken files must be
  rejected.
- `make check-mp3-quality` scores the encoder against LAME and Shine at the
  same bit rate with a neurogram similarity - the core of ViSQOL, which is
  the metric planning/audio.md 11.3 names. **It is not ViSQOL**: no pinned
  build could be had (it builds with Bazel, and the one PyPI wrapper
  downloads a binary from a model hub), so this is the same similarity index
  on a similar spectrogram, written from the published description. It
  cannot hear, and it knows nothing of masking; what it can do is compare
  three encoders on one input at one rate, where its calibration drops out.
  If `tools/corpus/fetch.sh` has been run it also scores the VBR quality
  scale on twelve freely licensed music clips from Wikimedia Commons (not
  committed; each pinned by SHA-256, licences in `tools/corpus/MANIFEST`).
  On six signals ours is on average level with LAME and ahead of it on
  three; on the sound effects of an office suite, which are real recordings,
  it is ahead of LAME on six of eight at 32 kbit/s, and on two recorded
  voices at 128 it is within 0.003 of it. Those are not in the repository.

**What the gates found.** Each of these passed the unit tests and every
comparison with a decoder, because a decoder reads a wrong stream
faithfully, and so is worth knowing about:

- *A granule's length is a twelve-bit field.* A masked impulse needs about
  5,000 bits; the length wrapped, every decoder read the truncated granule as
  written, and the click came back with a fifth of its energy.
- *A pure tone is one line and silence.* The search for the finest step that
  fits the bits picked one at which the line clamps, which costs almost
  nothing and returned the tone at a quarter of its level.
- *A sum was narrowed before it was scaled.* A full-scale low-frequency wave
  puts the same sign in all 36 inputs of a subband and its first coefficient
  is eleven times the largest input.
- *Noise spread over a quiet window.* A long block spreads quantisation noise
  evenly over 1,152 samples; a granule loud in one window and nearly silent
  in another had its noise well over the signal in the quiet one, after an
  attack as a decay.
- *Reading past the end of a frame* in the tag's checksum.

**What it does not do.** No intensity stereo and no mixed blocks (an encoder
that wants them can write them; this one does not), no `subblock_gain`,
`preflag` or `scalefac_scale` - the quantiser's scalefactors stop at what the
field widths hold - no lowpass beyond what the model's threshold of hearing
implies, no Layer I or II, no more than two channels, and no sampling
frequency but the nine. It is not fast: about five times real time for
stereo at 44.1 kHz on one core. And it was tuned against synthetic signals
and eight low-rate sound effects, because no music could be had to tune
against; a collection of real music is the first thing that would improve
it.

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
- **Layers I and II are not written.** The encoder is Layer III only.
- **The free format**, which states no bitrate, so a frame's length is
  the distance to the next sync word. Refused by name.
- **A non-seekable stream.** The length comes from the file's size and
  from trailers at the end; see above.
- **VBRI's seek table is read as a length and not as a table.** Seeking
  counts frames from the start instead, which is exact and costs one
  four-byte read per frame.
- **APEv2 is excluded, not read.** It is tier 3 metadata; the diagnostic
  says it was found.
