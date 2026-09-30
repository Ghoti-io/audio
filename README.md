# Ghoti.io Audio

Sound in C, as a container of tracks with metadata, read and written.

**This is a scaffold.** Nothing decodes audio yet. What exists is the base
object, the byte stream, the public codec SDK and the registry over it, with
the build, the gates and the install path all working end to end. The
[Status](#status) section says precisely what that means, and
`planning/audio.md` in the workspace is the design it is being built to.

## Formats

None yet. The plan reads containers and codecs as two lists, because they are
two lists: a `.m4a` is a box tree that may hold AAC, ALAC, Opus or FLAC, and
Opus is the same bitstream in Ogg, Matroska, MP4 and CAF. What is intended, in
the order it is intended:

- **WAV** (PCM, `WAVEFORMATEXTENSIBLE`, RF64/BW64) and **AIFF/AIFF-C**.
- **µ-law, A-law, IMA and MS ADPCM**, inside those.
- **FLAC** (RFC 9639), native and in Ogg.
- **MP3**, then **Ogg** with **Vorbis** and **Opus**.
- **ISO BMFF** with **ALAC**, and **AAC-LC** as a separate library.
- Matroska, CAF, Wave64, au, DSF/DFF.

Every format this library reads, it will write.

## Before you call it

- **Loading a file decodes no samples.** An hour of 96 kHz stereo is about
  2 GB of PCM, so `gaud_doc_load()` will parse the container, the track list
  and the metadata and stop. Samples come from a pull decoder, a block at a
  time.
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

Phase 0 of ten, complete. What works:

- The base object's result codes, diagnostics and limits.
- A memory stream: read, seek from three origins, tell, size, eof, and a
  seekable query. The file stream is phase 1.
- The public codec SDK: the vtable with `abi_version` and `size`, a registry
  that borrows rather than copies, duplicate-name and ABI refusal, and a probe
  that identifies by signature or by a codec's own function and leaves the
  stream where it found it.
- 51 tests, clean under ASan, UBSan and Valgrind, from an empty build
  directory, serially and under `-j`. Both `image` arms build and pass.
- `make install` and a consumer built against nothing but the installed
  headers.

What is deliberately absent, and when it arrives:

- **Any codec.** Phase 1 brings WAV and AIFF.
- **`GAUD_Doc`, `GAUD_Track`, `GAUD_Buffer` and the pull decoder.** Phase 1.
  The codec vtable has identification only; the decode and encode entry points
  are appended to it then, which is what `abi_version` and `size` exist for.
- **`make check-fixtures` is not in `TEST_GATES`.** It refuses to run against
  a tree with no `tests/data`, which is right - a gate measuring an empty set
  reports success. It goes back in phase 1 with the first fixtures.
- **No fuzz harnesses.** One per container, added with the container.

## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) for the license, and
[COPYING](COPYING) for the GPL text it is written as additional permissions on
top of.

Contributions are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md) for what is useful instead.
