# Metadata {#metadata}

Tags, the bytes underneath them, and cover art. `meta.h` is the API;
this page is what the schemes do and where they disagree.

## Two layers, and why the common one is a list

A **common** vocabulary that every scheme maps onto, so a caller can ask
for the title without knowing what the file is, plus **raw** carriage of
the bytes beneath it, so a round trip puts back what this library did not
interpret. That is `image`'s arrangement.

Audio departs from it in one way. `image`'s common metadata is a small
fixed struct - orientation, DPI, description - because image metadata
largely *is* a fixed set. Audio's is not: Vorbis comment has no registry
at all, ID3v2 has some eighty frame identifiers, and a tag may
legitimately occur more than once. So the common layer here is an
**enumerable, multi-valued key/value list** over ::GAUD_Tag, with
unmapped keys kept under their own spelling.

## What is implemented

| Scheme | Where | Read | Write |
| --- | --- | --- | --- |
| ID3v2.2 | `id3 `/`ID3 ` chunk | yes | as 2.4 |
| ID3v2.3 | `id3 `/`ID3 ` chunk | yes | as 2.4 |
| ID3v2.4 | `id3 `/`ID3 ` chunk | yes | yes |
| ID3v1 | trailer | yes | no |
| RIFF `LIST`/`INFO` | WAV | yes | yes |
| BWF `bext` | WAV | yes, and kept raw | raw only |
| AIFF `NAME` `AUTH` `(c) ` `ANNO` | AIFF | yes | yes |

A WAV this library writes carries its tags **twice**: once in
`LIST`/`INFO` and once in `id3 `. An AIFF carries them in its four text
chunks and again in `ID3 `. That is deliberate - a reader that knows only
one of the two still finds a title - and it is why the readers merge
rather than append. A file legitimately says the same thing in two
places, and reporting it twice would be this library's defect, not the
file's.

**ID3v2.4 is what gets written**, whatever was read. 2.3 has no UTF-8
text encoding, so writing it would mean every non-Latin-1 string going
out as UTF-16 with a byte-order mark - larger, and the encoding every
reader has a bug in.

## Text encodings

Everything crossing the API is UTF-8, without exception.

ID3v2 states its encoding per frame: `0` ISO-8859-1, `1` UTF-16 with a
byte-order mark, `2` UTF-16BE, `3` UTF-8. All four are decoded.
planning/audio.md 8 settled that this needs no new dependency: `unicode`
has UTF-8 and codepoints but neither Latin-1 nor UTF-16, and depending on
`text` for two short loops would drag in `chron`, `unicode` and `regex`.

**RIFF `INFO` and AIFF's text chunks state no encoding at all.** The
specifications predate Unicode and say "the file's code page". Measured:
ffmpeg 7.1.5 writes UTF-8 and reads it back, and round-trips Japanese
through an `INFO` chunk, which Latin-1 cannot represent. So the bytes
decide - valid UTF-8 is taken as UTF-8 and anything else as Latin-1.
Reading those chunks as Latin-1 unconditionally, which the first draft
did, hands back doubled-up mojibake for this library's own output.

A frame that **claims** UTF-8 and is not gets read as Latin-1 too. A
promise that holds only for well-formed input is not a promise, and a
caller's validator rejecting a string this library handed it would be
this library's defect.

## Cover art, and the four states

planning/audio.md 11.4. `GAUD_Picture` reports what the container
**stated** in every build, and what the bytes **are** only where `image`
is linked. The two arms differ additively: a caller ignoring the verified
fields gets correct answers either way.

::GAUD_Picture_Status has four values and not three, because "the
container said nothing" and "this build cannot check" are different
facts - the difference between *your build is minimal* and *this file is
corrupt*.

**An ID3v2 picture is always ::GAUD_PICTURE_NOT_STATED**, because `APIC`
states no dimensions. FLAC's `PICTURE` block does state them, so a
picture from a FLAC file is ::GAUD_PICTURE_UNVERIFIED in a build without
`image` and ::GAUD_PICTURE_VERIFIED or ::GAUD_PICTURE_MISMATCH in one
with it.

## Limits

`GAUD_Limits::max_metadata_entries` counts **values, not frames**. One
ID3v2.4 text frame may carry any number of NUL-separated values and
`TRCK` alone yields two tags, so counting frames lets a cap of four
produce a hundred tags. The check also sits where no path can skip it:
the three branches that keep a frame raw used to `continue` straight over
it.

## Things that are easy to get wrong, and what this does

**2.3's frame sizes are plain and 2.4's are syncsafe.** The tag header's
size is syncsafe in every version. Reading 2.3's frame sizes as syncsafe
drops every frame after the first one longer than 127 bytes; reading
2.4's as plain overruns. This is the single most common ID3 bug.

**A frame kept raw must keep its flags, normalised.** The bit meanings
differ between 2.3 and 2.4 - 2.3's compression bit is `0x0080` and 2.4's
is `0x0008`, 2.3's grouping is `0x0020` and 2.4's is `0x0040` - and this
library always writes 2.4. Copying the flags across relabels the frame,
and the next reader then tries to parse a frame that is still compressed.
2.3's four-byte uncompressed-size prefix is 2.4's data-length indicator,
so that mapping sets two bits where one was set.

**Unsynchronisation is undone before anything is parsed**, into a scratch
copy, because doing it as the parser walks makes every offset inside a
frame wrong. It is **not** re-applied on write: these tags live in a
chunk with a stated length, where nothing scans for a sync word.

**A frame identifier is `A-Z0-9` and nothing else.** It reaches a caller
as a custom key and as a raw block's id, and every string this API
returns is UTF-8 - so a frame id with a high byte would break that
promise. A frame whose identifier is not one also means the walk has lost
its place, so parsing stops there rather than inventing frames out of
sample data.

**A UTF-16 string ends with two zero bytes.** A reader that searches for
a single zero finds the high byte of the first ASCII character and
truncates every such field to nothing.

**`COMM` and `USLT` carry a language and a description before the text.**
A reader that takes the first string gets the description - which is
empty in everything iTunes ever wrote.

**A known frame too short for its own type is kept raw, not dropped.**
The rule that an uninterpretable frame must not be lost applies to a
malformed `COMM` exactly as it does to an unrecognised identifier.

**`TRCK` is `"3/12"` and Vorbis comment uses two keys.** The split on
read and the join on write live next to each other, so a change to one is
made in front of the other.

**ID3v1's genre is a byte index into a list no specification contains** -
80 entries from ID3v1 and 112 more from successive Winamp releases. The
table is generated from mutagen's rather than transcribed: the typed
version had 148 entries and three wrong names, and reading it back over
would not have found either, because one was an absence.

## How it is checked

`make check-tags` asks three questions, and they are different questions:
does the genre table match mutagen's row by row; can ffmpeg and mutagen
read what this library writes; and can this library read what ffmpeg
writes. The third is the one a library whose reader and writer share a
misunderstanding fails - it passes the second perfectly.

**mutagen is in the oracle image for that reason.** ffmpeg generates the
tag fixtures, so scoring them with ffmpeg alone would be asking one
implementation whether it agrees with itself.

`make fuzz-run-tags` drives the parsers below any container, because a
container fuzzer must synthesise a valid chunk before it reaches a frame.
It asserts more than the absence of a crash: every string handed back is
valid UTF-8, the limits are kept, and **build, re-parse, re-build
produces the same bytes**. That last one found five defects that a single
pass cannot see, each of which lost or grew something once per
generation.
