# Writing a codec {#writing_a_codec}

This is a **compatibility contract**, not internal guidance. The codec
interface in `<ghoti.io/audio/codec.h>` is installed, and codecs are expected
to live outside this repository. What is written here is what such a codec may
rely on.

## Why the interface is public

A codec that ships separately can only exist if the vtable it fills in and the
registry it hands itself to are both installed headers. `ghoti.io-compress`
works this way. `ghoti.io-image` does not - its codec struct holds its load and
decode callbacks in a private header, so the only thing an outside party can
build against it is a probe - and that difference is the reason this was
settled before any codec was written rather than after.

Placement is a separate question from capability. `audio-aac` lives outside
this repository for licensing reasons, not because encumbered codecs belong
outside; AC-3 is intended as an ordinary in-tree codec.

## The two rules that keep it honest

1. **Every codec in this repository registers the same way an outside one
   would.** There is no privileged internal path, and there will not be one. A
   public interface the shipped code bypasses is exercised by nothing that
   matters, and it is broken the first time somebody depends on it.
2. **An out-of-tree codec is part of the test suite** - built against the
   *installed* headers, registering itself, driven by the core. Without that,
   "third parties can add codecs" is a claim that has never been run.

## What a codec provides

One `GAUD_Codec`, usually a file-scope constant, handed to
`gaud_registry_register()`.

```c
static const unsigned char magic[] = {'f', 'L', 'a', 'C'};
static const GAUD_Codec_Magic magics[] = {{0, magic, 4}};

static const GAUD_Codec codec = {
    .abi_version = GAUD_CODEC_ABI_VERSION,
    .size = sizeof(GAUD_Codec),
    .name = "flac",
    .capabilities = GAUD_CAP_DECODE,
    .encoder_tier = GAUD_ENCODER_NONE,
    .magics = magics,
    .magic_count = 1,
};
```

**The registry borrows it.** The struct is not copied, so it must outlive the
registry. A file-scope constant does; a stack temporary does not.

`abi_version` and `size` are how a codec built against a different version of
this header is detected rather than misread. Set `size` to
`sizeof(GAUD_Codec)` as *your* compiler sees it, never to a literal.

## Registering

Two paths, and a codec that wants to work everywhere ships both:

```c
GAUD_INIT_FUNCTION(register_flac) { gaud_registry_register(NULL, &codec); }

GAUD_API void gaud_flac_register(void) { gaud_registry_register(NULL, &codec); }
```

The constructor covers a shared library, which is the ordinary case.
**It does not cover a static archive**: a constructor in an object file that
nothing references by name is dropped by the linker unless the consumer passes
`--whole-archive`. An explicit function that the application calls is the
answer there, and the one a codec's own documentation should lead with for
static linking.

## Rules a probe must obey

- **Leave the stream where you found it.** The registry probes codecs in turn
  and the next one is entitled to the position the caller left. The registry
  restores it after your probe as a backstop, but a probe that relies on that
  is relying on an implementation detail.
- **Returning an error is not "no".** An error means the probe could not
  answer. To say no, return `GAUD_OK` with `GAUD_CONFIDENCE_NONE`.
- **Do not match on a prefix.** A truncated stream that happens to agree with
  the first bytes of your signature is not your format.
- A probe is optional. Declaring magics is enough for most formats; a probe
  exists for the ones where a signature cannot tell two apart.

## Declaring an encoder honestly

`encoder_tier` is checked against `GAUD_CAP_ENCODE` at registration: a codec
that declares an encoder must state a tier, and one that declares no encoder
must not. This is deliberate. A perceptual encoder may ship before it is
competitive, and that is only an acceptable bargain if a caller can *ask* -
a sentence in a README is not something a program can act on.

`GAUD_ENCODER_STUB` does not mean the output is invalid. A stub must still
produce a bitstream the reference decoders accept; that gate is exact and is
separate from the quality one. It means the rate-distortion result is not
competitive with the reference encoder.

## Stability

The interface grows by **appending to the end of `GAUD_Codec`** and bumping
`GAUD_CODEC_ABI_VERSION` when the meaning of an existing field changes. A
codec reporting a smaller `size` is one built against an older header of the
same ABI version, and the library will not read past what it declared.

Phase 0 defines identification only: name, capabilities, tier, magics and
probe. **The decode and encode entry points arrive in phase 1**, appended, and
that addition is exactly the case `size` exists to make safe.

Expect the ABI version to move. `compress` has bumped its twice. Each bump is
a recompile for every out-of-tree codec, which is the price of the interface
being public at all, and it is stated here rather than discovered.
