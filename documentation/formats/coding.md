# Coded samples {#format_coding}

Four codings that live inside WAV and AIFF-C without being either container's
business: G.711 µ-law and A-law, and two ADPCM families. They are listed here
rather than on the container pages because the same coding appears in both,
and because what a caller needs to know about them is the same wherever it
is found.

`coding.h` is the API. ::GAUD_Sample_Format says what is in the buffer,
::GAUD_Sample_Coding says what was in the file, and for all four of these the
first answer is ::GAUD_SAMPLE_S16. A µ-law sample is one byte on disk
carrying about fourteen bits of range; an ::GAUD_SAMPLE_U8 sample is one byte
carrying eight. Folding the first into the second would make
gaud_frame_size() wrong and would lose the coding on a round trip, which is
why they are two fields.

## What is implemented

| Coding | WAV tag | AIFF-C type | Channels |
| --- | --- | --- | --- |
| ::GAUD_CODING_G711_ULAW | `0x0007` | `ulaw`, `ULAW` | any |
| ::GAUD_CODING_G711_ALAW | `0x0006` | `alaw`, `ALAW` | any |
| ::GAUD_CODING_ADPCM_IMA_WAV | `0x0011` | — | 1 or 2 |
| ::GAUD_CODING_ADPCM_IMA_QT | — | `ima4` | 1 or 2 |
| ::GAUD_CODING_ADPCM_MS | `0x0002` | — | 1 or 2 |

All five read and write. The upper-case AIFF-C spellings are SGI's and are
accepted on read; Apple's lower-case is what gets written, because one
spelling out is one thing to be wrong about and it is the one both ffmpeg and
libsndfile emit.

**Microsoft ADPCM has no AIFF-C spelling at all**, and WAV's IMA framing has
no AIFF-C spelling either. Asking for one is ::GAUD_ERR_UNSUPPORTED rather
than a file labelled with a framing it is not in. ffmpeg refuses the same
combination, producing a zero-byte file.

## The two IMA entries are one algorithm in two framings

Same nibble arithmetic, different packaging, and they are separate values
because a caller asking for one and getting the other would get a file no
reader accepts.

|  | WAV (`0x0011`) | QuickTime (`ima4`) |
| --- | --- | --- |
| preamble, per channel | 4 bytes | 2 bytes |
| block size | the header states it | fixed, 34 bytes |
| the preamble's predictor | **is** the first sample | is **not** a sample |
| interleave | four-byte groups per channel | a whole packet per channel |

The third row is the one that produces an off-by-one length for a whole file
if it is got wrong.

## Things that are easy to get wrong, and what this does

**Seeking is exact, and that is a property of the format.** Every block of
every coding here carries its own predictor and step state in its header, so
the block holding frame *N* can be decoded without touching any earlier one.
gaud_decoder_seek() therefore lands on the frame asked for and `out_landed`
is that frame. MP3's bit reservoir in a later phase is where that stops being
true - `out_landed` exists for then.

**Coded decoder state is per decoder, not per track.** An uncompressed track
can hang its read-only state off the track, because reading PCM changes
nothing and two decoders can share it. A coded decoder holds a decoded block,
and two decoders reading different parts of one track would evict each
other's cache and return each other's samples - silently, because every frame
they returned would be a plausible one.

**A coded file's last block is padded, and WAV's `fact` chunk is the only
record of where the signal stops.** AIFF-C has no equivalent, so an `ima4`
file is always a whole number of 64-frame packets. This library honours
`fact`; no reference decoder in the oracle image does.

**µ-law has two codes for zero.** `0x7F` and `0xFF` both decode to 0, so
encoding a decoded code returns the same code for 255 of the 256 and maps the
other to `0xFF`. That is a property of the law, not of this implementation;
A-law has no duplicate and is exactly idempotent over all 256.

**A-law's alternate bits are inverted.** Silence in a valid A-law file is
`0xD5`, and a run of zero bytes is not silence.

## Where the references disagree

These are measured, reproduced, and recorded in `check-corpus`'s exclusion
table with the numbers. In every case this library matches at least one
reference byte for byte, and the references disagree with each other by the
same amount.

- **IMA in WAV.** ffmpeg expands a nibble by multiplication,
  `((2*delta+1)*step) >> 3`; this library and libsndfile use the additive
  form the IMA reference states. Reproducing ffmpeg's arithmetic matches it
  on every sample of the corpus, which is what identifies the mechanism
  rather than guessing at it.
- **`ima4` in AIFF-C.** ffmpeg carries the predictor across packets instead
  of reloading it from each packet header. Apple's packet begins with a
  predictor precisely so packets are independent, and that independence is
  what makes the exact seek above possible.
- **MS ADPCM's predictor division.** `(sample1*coef1 + sample2*coef2) / 256`
  truncates toward zero in C where an arithmetic shift floors. Coefficient
  pairs 0, 1 and 2 all give exact multiples of 256, so **no file an encoder
  produces can tell the two apart** - every encoder picks pair 0. This
  library and ffmpeg truncate; sox and libsndfile shift. The corpus carries a
  synthetic fixture that names pairs 3 to 6 deliberately, which is the only
  thing that can see it.

## Not implemented

- ADPCM above two channels, on write. The formats have no defined interleave
  for it and ffmpeg refuses both to encode and to decode it. Reading stays
  liberal: a six-channel `ima4` someone else wrote is still decoded.
- Choosing an MS ADPCM coefficient pair per block. Every block this library
  writes names pair 0, which is what both writers in the oracle image do. The
  other six pairs are written into the header, where a reader may use them.
- G.721, G.723, GSM 6.10, and the other WAVE format tags.

## Specification

ITU-T G.711 (1988) defines both companding laws. IMA/DVI ADPCM is the IMA
Digital Audio Focus and Technical Working Groups' "Recommended Practices for
Enhancing Digital Audio Compatibility", 1992; Microsoft's WAVE framing of it
and of MS ADPCM are in the Multimedia Programming Interface specification and
in Microsoft's later `MS-ADPCM` documentation. Apple's `ima4` is in the
AIFF-C specification, 1991.

## How it is checked

`make check-corpus` scores this library's decode of ffmpeg-generated fixtures
against ffmpeg, sox and libsndfile. `make check-writer` requires every
reference to read what this writes, and measures the SNR of the round trip
against a stated floor per coding - which is
planning/audio.md 11.3's two gates, exact and
by-metric.

Two things about the corpus are worth knowing, because both were wrong first:

- The IMA step table is 89 entries, and **loading an entry is not testing
  it**. At nibble magnitude 0 the decoder computes only `step >> 3`, so most
  of a wrong value is invisible. Counting loads gave 89 of 89 while a
  deliberately wrong entry still decoded identically; counting loads *at a
  nonzero magnitude* gave 54. The corpus now carries noise fixtures at four
  amplitudes chosen to reach all 89 observably, and `check-corpus` reports
  the figure every run so it cannot quietly regress.
- MS ADPCM's seven coefficient pairs were exercised by one. Every encoder
  picks pair 0, so `wav_msadpcm_coefs_8000.wav` is generated and then has
  each block's predictor index rewritten to cycle 0..6.

`make fuzz-run-coded` drives the block layer below any container, because a
container fuzzer has to synthesise a valid header before it reaches a nibble
and almost never does.
