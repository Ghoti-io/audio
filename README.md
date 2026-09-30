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

Each has a page saying what it covers and where it differs: \ref format_wav
"formats/wav.md" and \ref format_aiff "formats/aiff.md".

The two are in phase 1 together deliberately. **WAV is little-endian and AIFF
is big-endian**, and their 8-bit samples disagree about sign, so each codec
exercises exactly the paths the other does not - and `make check-golden` runs
the corpus on big-endian targets, where the two swap round.

What comes next, in order: µ-law, A-law and ADPCM inside these containers;
FLAC; MP3; Ogg with Vorbis and Opus; ISO BMFF with ALAC, and AAC-LC as a
separate library. Every format this library reads, it writes.

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
[compress](https://github.com/Ghoti-io/compress) must already be installed
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
- [ghoti.io-compress](https://github.com/Ghoti-io/compress) — zlib, for ID3v2
  compressed frames and Matroska header compression. No codec needs it.
- [ghoti.io-image](https://github.com/Ghoti-io/image) — optional; see above.
  Named in the installed `.pc`'s `Requires:` only when built against.

## Status

Phase 1 of ten, complete. What works:

- **WAV and AIFF, read and written**, across every PCM width both can carry,
  including RF64/BW64 and `WAVE_FORMAT_EXTENSIBLE` with its channel mask.
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
| `make check-writer` | Those references read what this library wrote, and the samples survived the round trip. 26 round trips, both containers, both directions |
| `make check-golden` | The corpus decodes to the same sample values on two big-endian targets, which is where each codec's byte-swapping actually runs |
| `make check-outoftree` | A codec in another repository works |
| `make fuzz` | Per-container harnesses asserting the caller-facing invariants, not merely the absence of a crash |

The corpus is generated **by** the references and never by this library: one
grown from our own writer would agree with our own reader by construction.
`make oracle-build` builds the pinned image; `make corpus` regenerates it.

121 tests, clean under ASan, UBSan and Valgrind, from an empty build tree
serially and under `-j`, in both `?image` arms.

What is deliberately absent:

- **Any lossy codec.** Phase 2 brings the compressed formats that live inside
  these two containers; FLAC is phase 4 and MP3 phase 5.
- **Metadata.** ID3, `LIST`/`INFO`, `bext` and the rest are phase 3. They are
  skipped by length today, so they never stop a file loading.
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
| \ref writing_a_codec "writing-a-codec.md" | The compatibility contract for a codec in another repository |
| \ref development "development.md" | Building, the gates, and why a clean tree is a different test |

`make docs` builds the manual those pages feed.

## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) for the license, and
[COPYING](COPYING) for the GPL text it is written as additional permissions on
top of.

Contributions are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md) for what is useful instead.
