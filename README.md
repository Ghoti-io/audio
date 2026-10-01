# Ghoti.io Audio

Sound in C, as a container of tracks with metadata, read and written.

**Phases 1 through 5 of ten.** WAV, AIFF and FLAC read and write,
losslessly, with tags and cover art; **MPEG audio - MP3 and its two
sibling layers - reads**, in integer arithmetic, to the same bytes on
every architecture. Over the base object, the pull decoder and the public
codec SDK. [Status](#status) says precisely what that means, and
`planning/audio.md` in the workspace is the design it is being built to.

## Formats

Containers and codecs are two lists, because they are two lists: a `.m4a` is a
box tree that may hold AAC, ALAC, Opus or FLAC, and Opus is the same bitstream
in Ogg, Matroska, MP4 and CAF.

This is what is implemented:

- **WAV** - RIFF/WAVE, plus **RF64 and BW64**. Integer PCM at 8, 16, 24 and 32
  bits, IEEE float at 32 and 64, and `WAVE_FORMAT_EXTENSIBLE` wrapping either,
  including the channel mask. Read and written.
- **AIFF and AIFF-C** - signed 8, 16, 24 and 32-bit, and AIFF-C's `NONE`,
  `twos`, `sowt` (little-endian) and `fl32`/`fl64`. Read and written.
- **Coded samples inside both** - G.711 µ-law and A-law, IMA/DVI ADPCM in
  WAV's block framing and in QuickTime's `ima4` packets, and Microsoft
  ADPCM. Read and written. They decode to signed 16-bit and are reported
  separately from the sample format, because what is in the file and what is
  in the buffer are different questions.
- **FLAC** - RFC 9639, in its native container and in **Ogg**. Every
  subframe type, both Rice methods, escaped partitions, wasted bits and all
  four channel assignments; 8, 16, 24 and 32 bits. Read and written, with a
  seek table, a STREAMINFO MD5 that `flac -t` verifies, Vorbis comments,
  cover art and CUESHEET carriage. The encoder uses the format's fixed
  predictors and no LPC, which costs a few percent of compression and buys
  output that is byte-identical on every architecture - see \ref format_flac
  "formats/flac.md".

- **MPEG audio** - MPEG-1, MPEG-2 and MPEG-2.5, Layers I, II and III,
  which is to say MP3 and its relatives. **Read.** Layer III with every
  window type, both joint stereo modes, the bit reservoir and all thirty
  usable Huffman tables; Layer II with all five allocation tables; Layer I
  with every quantiser width. The frame header, the ID3v2 tag at the front
  and the ID3v1 trailer at the back, and the Xing, Info, VBRI and LAME
  tags that are the only places such a stream ever states its own length
  or its encoder delay. How the length was arrived at is part of the
  answer: stated by a tag, counted, estimated from the bitrate, or
  unknown, with a diagnostic for the last three. The decoder is integer
  throughout and produces **the same bytes on every architecture**, which
  `make check-golden` checks on two big-endian targets - a promise no
  comparison against a floating-point reference could establish. **Every
  sampling frequency the format has**, MPEG-2.5's three included, whose
  Layer III scalefactor band tables are in no standard: two of those
  three rates turn out to use MPEG-2's 16 kHz tables exactly, and 8 kHz's
  own row is extracted from a CC0 implementation and calibrated on the
  twelve rows two standards also define. See \ref format_mpeg
  "formats/mpeg.md".

- **Vorbis** - Vorbis I in Ogg. **Identified, not yet decoded.** The three
  header packets, the channel count, the sample rate, the two block sizes,
  the tags out of the comment header, and the length. The length is the
  part worth naming: a Vorbis stream states it *nowhere* - not in a
  header, not in a trailer - so the only answer is the granule position
  on its last page, and reading it is what `make check-vorbis` scores
  against two independent readers. The codec declares
  `GAUD_CAP_METADATA_READ` and not `GAUD_CAP_DECODE`, so a program that
  asks for a decoder is told ::GAUD_ERR_UNSUPPORTED rather than finding
  out from this paragraph. **A format that is identified and not decoded
  is a reasonable commit and an unreasonable release**, and this sentence
  is here to be deleted.

Each has a page saying what it covers and where it differs: \ref format_wav
"formats/wav.md", \ref format_aiff "formats/aiff.md", \ref format_coding
"formats/coding.md", \ref format_flac "formats/flac.md", \ref
format_mpeg "formats/mpeg.md" and \ref format_vorbis "formats/vorbis.md".

WAV and AIFF were in phase 1 together deliberately. **WAV is little-endian and
AIFF is big-endian**, and their 8-bit samples disagree about sign, so each
codec exercises exactly the paths the other does not - and `make
check-golden` runs the corpus on big-endian targets, where the two swap
round. From phase 4 that gate also *encodes* on those targets and compares
the files byte for byte, which is why the FLAC encoder is integer-only.

What comes next, in order: the Vorbis decoder, then Opus in Ogg; ISO BMFF
with ALAC, and AAC-LC as a separate library; then the perceptual
encoders. Every format this library reads, it will write.

## Before you call it

- **Loading a file decodes no samples.** An hour of 96 kHz stereo is about
  2 GB of PCM, so `gaud_doc_load()` parses the container and the track list
  and stops. Samples come from a pull decoder, a block at a time, into a
  buffer you sized. A short read means the end and is not an error.
- **A seek says where it landed.** For PCM that is always the frame you asked
  for; it is a parameter rather than a promise because it will not be, for a
  codec whose frames depend on the ones before them.
- **Samples are in the host's byte order.** That is why WAV and AIFF produce
  the same buffer, and it means the raw bytes of a correct decode differ
  between a little-endian and a big-endian machine. The *values* do not, and
  `make check-golden` proves it.
- **Writing needs a seekable sink**, and says so when the encoder is created
  rather than when it is finished. A RIFF or IFF header states a length that
  is not known until the data has been written.
- **`gaud_encoder_finish()` must be called and its result checked.** That is
  where the header is patched; an encoder merely destroyed leaves a file
  describing a size it does not have.
- **Nothing is converted unless you ask.** Sample rate, sample format, channel
  layout and loudness are carried and reported; resampling, requantising,
  remixing and gain are `ops.h`, explicitly. This is `image`'s rule about
  colour, applied to the things audio has instead.
- **A memory stream borrows its bytes.** They stay alive for the life of the
  stream.
- `NULL` for an allocator is cutil's default. `NULL` options are the defaults.

## Writing a codec

The codec interface is **public**, and deliberately so: a codec can live in
another repository and register itself. `<ghoti.io/audio/codec.h>` is the
whole interface - a vtable carrying `abi_version` and `size`, and a registry
to hand it to. Every codec in this repository registers the same way, because
a public path the shipped code bypasses is one nothing exercises.

```c
static const unsigned char flac_magic[] = {'f', 'L', 'a', 'C'};
static const GAUD_Codec_Magic flac_magics[] = {{0, flac_magic, 4}};

static const GAUD_Codec flac_codec = {
    .abi_version = GAUD_CODEC_ABI_VERSION,
    .size = sizeof(GAUD_Codec),
    .name = "flac",
    .capabilities = GAUD_CAP_DECODE,
    .encoder_tier = GAUD_ENCODER_NONE,
    .magics = flac_magics,
    .magic_count = 1,
};

gaud_registry_register(NULL, &flac_codec); // NULL is the default registry
```

The registry borrows the codec rather than copying it, so it must outlive the
registry - a file-scope constant does, a stack temporary does not.

## Examples

```c
#include <ghoti.io/audio/audio.h>
#include <stdio.h>

int identify(const void * bytes, size_t length) {
  GAUD_Stream * stream = NULL;
  if (gaud_stream_create_memory(bytes, length, &stream) != GAUD_OK) {
    return 1;
  }

  GAUD_Probe_Result result = {0};
  if (gaud_probe(NULL, stream, &result) == GAUD_OK) {
    printf("%s\n", result.codec_name ? result.codec_name : "unrecognised");
  }

  gaud_stream_destroy(stream);
  return 0;
}
```

The format is recognised from the bytes. The extension is not consulted and is
not available here, which is the point: a `.wav` holding MP3 frames exists.

## Compile and link

```bash
cc -o show show.c $(pkg-config --cflags --libs ghoti.io-audio-0)
```

The module name ends in the major version, `-0` for this release, so two
majors can be installed side by side. A build made with `make BRANCH=-dev`
installs `ghoti.io-audio-dev` instead.

## Building the library

[cutil](https://github.com/Ghoti-io/cutil) and
[security](https://github.com/Ghoti-io/security) must already be installed
where pkg-config can see them. A dependency it cannot find is a hard error
naming the fix. Google Test builds the unit tests.

```bash
make
make test
sudo make install
```

From the parent of a suite checkout, which installs the dependencies first:

```bash
./suite/install.sh
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C libs/audio test PREFIX="$PWD/.local"
```

`make test` is the suite. `make help` lists the rest.

| Target | What it does |
| --- | --- |
| `make test-asan` | The suite under ASan and UBSan |
| `make test-valgrind` | The suite under Valgrind (Linux) |
| `make check-symbols` | Fail if an exported symbol lacks the version namespace |
| `make coverage` | Line coverage, per file |
| `make docs` | The Doxygen manual, into `./docs` |

### The optional dependency

[image](https://github.com/Ghoti-io/image) is optional, and used for one
thing: checking an embedded picture against the dimensions the file claimed
for it. FLAC's `PICTURE` block states width, height, depth and palette size
and any of them can be wrong; ID3v2's `APIC` and MP4's `covr` state none.

The two builds differ **additively** - what a file stated is always reported,
what the bytes are is reported only when `image` is present - so nothing
changes meaning between them. `gaud_have_image_validation()` says which build
you have.

```bash
make WITHOUT_IMAGE=1 test    # the other arm, on a machine that has image
```

Both arms are expected to be built and tested. The arm that is not compiled is
the arm that rots.

## The API

Everything is prefixed `gaud_` / `GAUD_`, under `<ghoti.io/audio/...>`.
`<ghoti.io/audio/audio.h>` is the umbrella.

- **`core.h`** — the result enum, diagnostics, strictness, limits, and
  `gaud_have_image_validation()`.
- **`stream.h`** — a byte stream over memory. A stream may not be seekable,
  and says so before you try.
- **`codec.h`** — the codec vtable, the registry, and `gaud_probe()`.
- **`allocator.h`** — `GAUD_Allocator`, which is cutil's `GCU_Allocator`.
- **`version.h`** — what this build calls itself.

## Dependencies

Found through pkg-config, and the installed `.pc` names them.

- [ghoti.io-cutil](https://github.com/Ghoti-io/cutil) — the allocator and the
  overflow-checked size arithmetic.
- [ghoti.io-security](https://github.com/Ghoti-io/security) — MD5, for the
  digest of the unencoded audio that FLAC's `STREAMINFO` carries. One function,
  and it is what lets a decode be checked against a number a different
  implementation wrote.
- [ghoti.io-image](https://github.com/Ghoti-io/image) — optional; see above.
  Named in the installed `.pc`'s `Requires:` only when built against.
- [ghoti.io-compress](https://github.com/Ghoti-io/compress) — optional, and
  **not used yet**, so it is neither linked nor named in `Requires:`. zlib is
  wanted for two things this library does not do today: inflating ID3v2
  compressed frames, which are currently declined and kept raw, and Matroska
  header compression, which is phase 9.

## Status

Phases 1 through 5 of ten, complete. What works:

- **MPEG audio read, in all three layers.** A bare stream of MPEG-1,
  MPEG-2 or MPEG-2.5 frames loads into a document that states its rate,
  its channel count, its layer, its length and its encoder delay, with
  ID3v2 and ID3v1 read off the ends - and decodes, for **every
  combination the frame header can spell**, all three versions and all
  three layers at all nine sampling frequencies. Measured against the two
  reference decoders the agreement is **one least significant bit of
  sixteen**, per channel, in level and in offset as well as in
  difference. MPEG-2.5 is in no standard, so it is also measured against
  something neither reference can be: the signal its encoder was given,
  which every band the encoder kept tracks to 0.67 dB. **Nothing about the length is assumed:** a Xing,
  Info or VBRI frame is believed only if the bytes could hold what it
  claims, a stream short enough to walk is counted exactly, a
  constant-rate stream is estimated and says so, and a variable-rate
  stream with no tag reports an unknown length rather than a number that
  would be wrong.
- **The decoder has no floating point in it**, and that is the whole of
  how `planning/audio.md` 11.1's promise is kept: every coefficient is a
  28-bit fixed-point integer, and the tables they come from are generated
  from ISO/IEC 11172-3 and 13818-3 by a script in `tools/tables/` that
  validates every one of them - that each Huffman table is a complete
  prefix code over its whole grid, that each synthesis window coefficient
  is an exact multiple of 2^-16, and a dozen more. The documents are
  fetched and not kept; the generated C is committed.

- **WAV and AIFF, read and written**, across every PCM width both can carry,
  including RF64/BW64 and `WAVE_FORMAT_EXTENSIBLE` with its channel mask.
- **G.711 and two ADPCM families inside them**, read and written, with
  `GAUD_Sample_Coding` reporting what the file used separately from what the
  buffer holds. Blocks are self-contained, so seeking into a coded track is
  still exact.
- **FLAC, read and written, in two containers.** RFC 9639 in its native
  container and in Ogg, at 8, 16, 24 and 32 bits: every subframe type, both
  Rice methods, escaped partitions, wasted bits and all four channel
  assignments. A file this library writes carries a seek table and a
  STREAMINFO MD5 that `flac -t` verifies against libFLAC's own decode, and
  seeking uses the seek table and then decodes forward, so it lands on the
  sample asked for. CUESHEET is checked and carried rather than modelled -
  it is the first of four spellings of one idea, and a model waits for the
  second caller. The Ogg page layer underneath it is its own module, for
  Vorbis and Opus to reuse.
- **Tags, raw carriage and cover art.** ID3v1 and ID3v2.2/2.3/2.4, Vorbis
  comment, RIFF `LIST`/`INFO`, BWF `bext` and AIFF's four text chunks, over
  a common multi-valued vocabulary with everything unmapped kept under its
  own spelling. Every string crossing the API is UTF-8, including from a frame
  that claims UTF-8 and is not. A file this library writes carries its tags
  in both its container's native scheme and an ID3 chunk, so a reader that
  knows only one still finds them. Cover art is reported additively, with
  four states that keep "this build cannot verify" apart from
  "verification failed" - see the `?image` note above.
- The base object: `GAUD_Buffer` with interleaved and planar layouts, sample
  formats including the non-PCM cases DSD and opaque, and an explicit
  `GAUD_Channel_Layout` that keeps "the file did not say" distinct from mono.
- The pull decoder, seeking that reports where it landed, and an encoder that
  patches its own header.
- `ops.h`: sample-format conversion with selectable dither, layout
  conversion, and the peak/RMS/DC measurements the decode gate rests on.
- Streams over memory, over files, growable for writing, and a wrapper that
  makes one non-seekable so that path can be *tested*.
- **The codec SDK proved from outside**: `make check-outoftree` installs into
  a throwaway prefix and builds a codec against nothing but the installed
  headers. It found two functions that were declared `GAUD_API` and silently
  not exported, which `check-symbols` structurally could not see - that gap
  is now a sixth sub-check of `check-symbols`.

How it is judged:

| Gate | What it settles |
| --- | --- |
| `make check-corpus` | Independent references decode the corpus exactly as this does: 57 fixtures, 163 fixture-reference comparisons. Lossless means a byte comparison and not a tolerance. The gate prints the count of *implementations* beside the count of references, because they are not the same number - of the 20 FLAC fixtures, 4 references read them and they are 2 implementations |
| `make check-writer` | Those references read what this library wrote, and the samples survived: 217 lossless round trips and 109 coded ones, each read back by every reference that can. 106 of the FLAC files are additionally verified by `flac -t --warnings-as-errors` against the STREAMINFO MD5 we wrote - with a control that zeroes a digest and requires the refusal, because that tool exits zero on a file it has complained about |
| `make check-tags` | 134 comparisons across four containers and two references. Does the ID3v1 genre table match mutagen's row by row, can ffmpeg and mutagen read the tags this library writes, and can it read theirs - the third is the one a library whose reader and writer share a misunderstanding fails. The two FLAC containers also go three generations through our own reader and writer, which is what caught a vendor string being collected as a tag |
| `make check-mpeg` | The MPEG decode against **two** reference decoders that are two implementations - ffmpeg's native one and libsndfile's minimp3 - and not a byte comparison, because the format defines a transform rather than sample values. It checks what a difference alone cannot see: the frame count exactly, the absolute level of our own output, the DC offset, all of it per channel, and eight deliberately wrong versions of our own answer that it must reject. The measured agreement is one least significant bit of sixteen, 88 to 106 dB down |
| `make check-mpeg-input` | The MPEG decode against **the signal the encoder was given**, which is the only decode gate here that consults no other decoder - and it exists because MPEG-2.5's band tables are in no standard and both references carry the same copy of them, so a differential against those two cannot see a table they agree on and that is wrong. The corpus is synthesised from closed-form expressions, so the input is regenerated rather than stored. Every band the encoder kept is within 1.96 dB of it - 0.71 dB at 8 kHz, 0.65 on the MPEG-1 calibration, 1.96 at 12 kHz where the encoder has least room; with the 8 kHz row deliberately replaced by the 16 kHz one the same measurement reads 5.24 and 6.33 dB, so the 3 dB threshold sits a decibel above the worst correct answer and two below the wrong one. Four wrong answers, a spectral tilt and a low-pass among them, must be rejected |
| `make check-vorbis` | What a Vorbis stream *is*, against two independent readings of it - and the gate exists because Vorbis states its length nowhere in the file: the only answer is the granule position on its last page, so reading it wrong is a whole-file error that no amount of correct decoding would fix. 10 fixtures, 60 comparisons against `ffprobe`'s `duration_ts` and libsndfile, 10 more against the frame count the generator fed each encoder - which involves no decoder at all - and 5 wrong answers put through the same comparison function. **ffmpeg's decoded sample count is excluded by name**: its two Vorbis decoders are two implementations for sample values and one reading for the length, because the trim is in the demuxer they share, and that reading is 128 frames short on three fixtures and 191 over on a fourth. The gate prints in its own output that it compares no samples, because a decoder that produced silence would pass all of it |
| `make check-golden` | 81 fixtures decode to the same sample values on two big-endian targets, which is where each codec's byte-swapping actually runs - and 5 of them *re-encode* to identical bytes there, which is the same promise applied to the writer and why the FLAC encoder is integer-only. **Phase 5 is what this gate was built for**: an MPEG decoder is a filterbank and an inverse transform, and byte-identical output everywhere is a promise about this library that no floating-point reference can be asked to confirm |
| `make check-outoftree` | A codec in another repository works |
| `make fuzz` | Six harnesses asserting the caller-facing invariants, not merely the absence of a crash - `wav`, `aiff` and `flac` at the container boundary, `tags`, `coded`, which drives the block layer below any container because a container fuzzer must synthesise a valid header before it reaches a nibble and almost never does, and `mpeg`, where the opposite is true: MPEG audio has no container to synthesise, so nearly every input reaches the frame search. `coded` found a real defect in its first minute; `mpeg` found two in its first ten, and neither was a crash - a header struct whose padding made two parses of one header compare unequal, and a document whose audio began past the end of its own file. **It then found eight more on a corpus that had grown since**, all one defect: every addition in the Q28 pipeline was signed overflow on a frame that states a legal global_gain near the top of its eight-bit range. The corpus is tracked in the repository for exactly this - it is the population that walks the arithmetic, and a run against a bigger one is a different experiment |
| `make mpeg-coverage` | The same instrument for MPEG audio, and the same reason: 63 of 72 named arms are reached by the corpus, and **no reachable arm is now unreached**. Two of the remaining nine are unreachable by construction (the standard marks those Huffman tables unused), five need an encoder nothing in the image is - LAME has never implemented intensity stereo, and nothing emits a mixed block - and two are covered by unit tests on hand-built frames instead. The last five Huffman tables fell to one change: giving the stereo noise and transient fixtures channels that actually differ |
| `make flac-coverage` | Not pass/fail: it counts which named arms of the FLAC frame decoder the corpus actually reaches. It exists because `check-corpus` agreed byte for byte with every reference on every fixture while a third of the subframe decoder had never run - a codec's branches are selected by the *encoder*, so a corpus samples encoders rather than the format |

The corpus is generated **by** the references and never by this library: one
grown from our own writer would agree with our own reader by construction.
The one exception is named as one: **nothing in the image writes Layer I**
- ffmpeg has two Layer II encoders and two Layer III encoders and no Layer
I encoder at all - so that fixture is constructed by the generator, and
what makes it a differential rather than a self-comparison is that its
expected output still comes from the references, both of which decode it.
That argument has a second edge, which is why the image holds `mutagen` as
well - ffmpeg *writes* the tag fixtures, so scoring them with ffmpeg alone
would ask one implementation whether it agrees with itself, and a
misunderstanding held in both its writer and its reader would be invisible.
`mutagen` shares no code with it. Six references are in the image as of
phase 4 - ffmpeg, sox, libsndfile, Python's `wave`, mutagen and `flac` -
and **for FLAC that is two implementations, not six**: sox, libsndfile and
the `flac` tool all answer through libFLAC, and only ffmpeg is separate.
The trap is that `ldd` on the ffmpeg binary *does* list libFLAC, by a path
its demuxer never enters, so the obvious check gives the wrong answer.
`make oracle-build` builds the pinned image; `make corpus` regenerates it.

229 tests, clean under ASan, UBSan and Valgrind, from an empty build tree
serially and under `-j`, in both `?image` arms - and the arms genuinely
differ from phase 3 on, because cover-art verification is the one thing
`image` is linked for. 80.7% line coverage from the unit tests alone, which
is the figure that matters for a contributor without the oracle container:
the gates above cover a great deal that those tests do not.

Three figures the gates print rather than assume. The corpus exercises **all
89 IMA step-table entries with a nibble magnitude that makes a wrong value
visible** - counting bare loads said 89 of 89 while a deliberately wrong
entry still decoded identically, and the honest figure was 54. The MS ADPCM
coefficient table this library writes is byte-identical to ffmpeg's, which
is the only thing that can check it, since our decoder uses the table in the
file and our encoder only ever names pair 0. And the two coverage
instruments report what the corpus actually executes: `make mpeg-coverage`
**63 of 72 named arms** of the MPEG decoder, with the nine it does not
reach listed and triaged and none of them reachable, and `make flac-coverage` **22 of 24 named arms**
of the FLAC frame decoder, over two populations it keeps apart - the reference corpus reaches 19 and our own
re-encoding of it reaches 17, and neither subsumes the other. The two left
are a variable-blocksize stream and a frame that defers its bit depth to
STREAMINFO: legal spellings no encoder in the image will produce an input
for, so the unit tests build those frames by hand instead.

What is deliberately absent:

- **Any perceptual encoder.** MPEG audio is read and not written; phase 8
  brings the encoders, with the two-gate harness perceptual output needs -
  bitstream validity against pinned decoders, and quality against a
  metric. A stub encoder that produced a conformant bitstream nobody would
  want to listen to would be worse than none, because it would look like
  support.
- **ADPCM above two channels, on write.** The formats have no defined
  interleave for it and ffmpeg refuses both directions, so writing one would
  produce a file the most widely deployed reader cannot open. Reading stays
  liberal.
- **MP4 `ilst`, APEv2 and Matroska tags.** Vorbis comment arrived with FLAC
  in phase 4; the rest arrive with the containers that hold them.
- **LPC subframes on the writing side.** They are read, at every order the
  format allows. Writing them means an autocorrelation and a Levinson-Durbin
  recursion in floating point, and that would end the promise that this
  library's output is byte-identical across architectures.
- **A model for chapters and cues.** WAV's `cue `/`adtl`, FLAC's CUESHEET and
  the other two spellings of one idea. FLAC's is checked and round-tripped
  byte for byte today, and a `LIST` that is not an `INFO` is kept raw, so
  nothing is lost while they wait for a second caller to design against.
- **`GAUD_SAMPLE_DSD1` and `GAUD_SAMPLE_OPAQUE` exist and nothing produces
  them.** They are in the base object because a registered codec cannot add a
  case to it later; DSD is phase 9 and remux needs the opaque one.
- **Resampling, remixing and loudness.** `ops.h` converts formats and
  measures; it does not yet change a rate or a channel count.

## Documentation

| Page | What it settles |
| --- | --- |
| \ref format_wav "formats/wav.md" | RIFF/WAVE, RF64 and BW64: what is covered, and the five things a reader gets wrong |
| \ref format_aiff "formats/aiff.md" | AIFF and AIFF-C, including the 80-bit float its sample rate is stored as |
| \ref format_coding "formats/coding.md" | G.711 and the two ADPCM families: one algorithm in two framings, and where the references disagree |
| \ref metadata "metadata.md" | Tags, raw carriage and cover art: the schemes, the encodings, and where they disagree |
| \ref writing_a_codec "writing-a-codec.md" | The compatibility contract for a codec in another repository |
| \ref development "development.md" | Building, the gates, and why a clean tree is a different test |

`make docs` builds the manual those pages feed.

## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) for the license, and
[COPYING](COPYING) for the GPL text it is written as additional permissions on
top of.

Contributions are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md) for what is useful instead.
