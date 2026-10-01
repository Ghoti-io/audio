# Ghoti.io Audio

Sound in C, as a container of tracks with metadata, read and written.

**Phase 1 of ten.** WAV and AIFF read and write, losslessly, with the base
object, the pull decoder and the public codec SDK behind them. No lossy codec
exists yet. [Status](#status) says precisely what that means, and
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

Each has a page saying what it covers and where it differs: \ref format_wav
"formats/wav.md", \ref format_aiff "formats/aiff.md", \ref format_coding
"formats/coding.md" and \ref format_flac "formats/flac.md".

WAV and AIFF are in phase 1 together deliberately. **WAV is little-endian and
AIFF is big-endian**, and their 8-bit samples disagree about sign, so each
codec exercises exactly the paths the other does not - and `make
check-golden` runs the corpus on big-endian targets, where the two swap
round. From phase 4 that gate also *encodes* on those targets and compares
the files byte for byte, which is why the FLAC encoder is integer-only.

What comes next, in order: MP3; Vorbis and Opus in Ogg; ISO BMFF with ALAC,
and AAC-LC as a separate library. Every format this library reads, it
writes.

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

Phase 3 of ten, complete. What works:

- **WAV and AIFF, read and written**, across every PCM width both can carry,
  including RF64/BW64 and `WAVE_FORMAT_EXTENSIBLE` with its channel mask.
- **G.711 and two ADPCM families inside them**, read and written, with
  `GAUD_Sample_Coding` reporting what the file used separately from what the
  buffer holds. Blocks are self-contained, so seeking into a coded track is
  still exact.
- **Tags, raw carriage and cover art.** ID3v1 and ID3v2.2/2.3/2.4, RIFF
  `LIST`/`INFO`, BWF `bext` and AIFF's four text chunks, over a common
  multi-valued vocabulary with everything unmapped kept under its own
  spelling. Every string crossing the API is UTF-8, including from a frame
  that claims UTF-8 and is not. A file this library writes carries its tags
  in both its container's native scheme and an ID3 chunk, so a reader that
  knows only one still finds them.
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
| `make check-corpus` | Four independent references - ffmpeg, sox, libsndfile and Python's `wave` - decode the corpus exactly as this does. PCM is lossless, so this is a byte comparison and not a tolerance |
| `make check-writer` | Those references read what this library wrote, and the samples survived the round trip: 68 lossless round trips across both containers and both directions, plus 95 coded ones scored two ways - every reference must decode the file at all, and the SNR must clear a floor stated per coding |
| `make check-tags` | Three questions: does the ID3v1 genre table match mutagen's row by row, can ffmpeg and mutagen read the tags this library writes, and can it read theirs. The third is the one a library whose reader and writer share a misunderstanding fails |
| `make check-golden` | The corpus decodes to the same sample values on two big-endian targets, which is where each codec's byte-swapping actually runs |
| `make check-outoftree` | A codec in another repository works |
| `make fuzz` | Per-container harnesses asserting the caller-facing invariants, not merely the absence of a crash - plus `coded`, which drives the block layer below any container, because a container fuzzer must synthesise a valid header before it reaches a nibble and almost never does. It found a real defect in its first minute |

The corpus is generated **by** the references and never by this library: one
grown from our own writer would agree with our own reader by construction.
That argument has a second edge, which is why the image gained a fifth
reference in phase 3 - ffmpeg *writes* the tag fixtures, so scoring them
with ffmpeg alone would ask one implementation whether it agrees with
itself. `mutagen` shares no code with it.
`make oracle-build` builds the pinned image; `make corpus` regenerates it.

141 tests, clean under ASan, UBSan and Valgrind, from an empty build tree
serially and under `-j`, in both `?image` arms - and the arms genuinely
differ from phase 3 on, because cover-art verification is the one thing
`image` is linked for.

Two figures the gates print rather than assume: the corpus exercises **all 89
IMA step-table entries with a nibble magnitude that makes a wrong value
visible** - counting bare loads said 89 of 89 while a deliberately wrong
entry still decoded identically, and the honest figure was 54 - and the MS
ADPCM coefficient table this library writes is byte-identical to ffmpeg's,
which is the only thing that can check it, since our decoder uses the table
in the file and our encoder only ever names pair 0.

What is deliberately absent:

- **Any perceptual codec.** The four codings above are sample quantisers, not
  psychoacoustic ones, and FLAC is lossless. MP3 is phase 5.
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
