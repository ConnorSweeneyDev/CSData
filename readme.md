# CSData
A single-header library defining the data formats of [CSEngine](https://github.com/ConnorSweeneyDev/CSEngine) resources:
how textures, fonts, sounds and music are authored, parsed, validated and laid out for packing.

## Formats
### Textures and Fonts (Aseprite)
Resources are authored directly as `.aseprite` files (32-bit RGBA) following the engine's conventions:
- Every top-level layer must be one of the groups `image`, `hitbox` or `pivot`; the `image` group holds the visible
  artwork, and every file needs at least one tag.
- Tags become animations, carrying per-frame durations from the file.
- Each layer of the flat `hitbox` group becomes a named collision shape per frame: its opaque pixels are decomposed into
  an optimal set of rectangles.
- The flat `pivot` group holds exactly one layer with exactly one opaque pixel per frame, the frame's anchor point.
  Textures require a pivot; fonts must not have one (nor hitboxes).
- For fonts, slices define the glyphs: the slice name is the character (one UTF-8 code point), the slice is the exact
  glyph box, and the uniform slice height is the line height.

Parsing composites the cels into a single horizontal sprite sheet plus animation, hitbox and glyph metadata.

### Sounds and Music (Opus/WAV renders of Reaper projects)
Audio is authored in Reaper and rendered to `.opus` or `.wav`, and the rendered file is the single source of truth: the
`.rpp` project rides inside the audio as metadata — an `RPP_SOURCE` comment tag in Opus files, an `rpp ` chunk in WAV
files — where every decoder ignores it. CSData provides the machinery to extract the embedded project, embed an updated
one (rewriting Ogg page checksums and sequence numbers as needed), or strip it for shipping.

Each file's playing time is also measured at parse time, from its headers rather than by decoding: Opus streams report
the granule position of their final Ogg page less the pre-skip declared in their `OpusHead` packet (Opus granules are
always in 48kHz units, whatever the source rate), and WAV files divide the size of their `data` chunk by the byte rate
in their `fmt ` chunk. It is measured after the embedded project is stripped, so the metadata never counts toward it.
Audio whose playing time cannot be measured is rejected rather than packed as zero — CSEngine times gameplay against
this value, so a silently wrong duration is worse than a failed build.

### Packed Layout and Accessors
Parsed resources are serialized into binary blobs read back by CSEngine's runtime loaders. Each blob is a sequence of
one of the shared record structs — every field is 8 bytes wide so the shapes are padding-free (pinned by static
assertions), and readers reinterpret the mapped bytes as arrays of them directly:
- `hitboxes`: `hitbox_record` — labelled rectangles; labels are string-table references in debug and (FNV-1a) hashes in
  release.
- `frames`: `frame_record` — UV bounds, duration, pivot and a span into the hitbox blob.
- `glyphs`: `glyph_record` — code point, UV bounds and pixel size.
- `strings`: the hitbox label pool (debug builds only).

Alongside the blobs, CSData generates the C++ accessor header and source that declare every resource (`cse::image`,
`cse::font`, `cse::sound`, `cse::music`, plus per-texture animation and hitbox structs) and bind them to regions of the
pack files they were placed in. Sound and music definitions carry their measured playing time as a second compile-time
constant.

## Features
- Single header, depending only on the C++20 standard library (a public-domain zlib decoder from stb_image is vendored
  internally for Aseprite cel decompression).
- One definition of every resource format, shared by the build system and the engine's runtime readers: the packed
  record shapes and the label hash are single definitions consumed by both sides, so they cannot drift.
- Strict validation with precise errors: malformed files, convention violations, unmeasurable audio and duplicate names
  fail the build instead of misbehaving at runtime.
- Container-agnostic packing: CSData computes layouts and generates code against offsets the caller reports back, so it
  does not depend on any particular pack file format.

## Requirements
- Windows or Linux OS.
- A C++20 compiler.

## Usage
### Loading (Build-Time)
`load()` parses one resource file into its packable form, validating it against the conventions above. `space` selects
the resource kind (`"image"`, `"font"`, `"sound"` or `"music"`) and `pack` names the pack it will be placed in; the
extension predicates `packable_texture()`/`packable_audio()` identify which globbed files are resources at all.
```cpp
csd::resource item{csd::load(file, name, space, pack)};
```

### Packing and Code Generation (Build-Time)
`layouts()` computes each pack's binary blobs in sorted pack order. The caller appends every resource's `blob` and then
the layout blobs into its container, recording where each landed in a `csd::binding`, and `accessor_header()` /
`accessor_source()` generate the code that binds the accessors to those regions (`header_preamble()` /
`source_preamble()` supply the file prologues).
```cpp
std::vector<csd::layout> layouts{csd::layouts(resources, debug)};
// Append blobs to the pack containers, filling one csd::binding per pack...
std::string header{csd::accessor_header(resources, space)};
std::string source{csd::accessor_source(resources, space, layouts, bindings, debug)};
```

### Reaper Project Round-Trip (Build-Time)
`audio_extract_rpp()` returns the embedded project if the audio carries one; `audio_replace_rpp()` returns the audio
rewritten with the given project embedded, or with any embedded project stripped when given `std::nullopt`.
```cpp
std::optional<std::vector<std::byte>> project{csd::audio_extract_rpp(bytes, file)};
std::vector<std::byte> embedded{csd::audio_replace_rpp(bytes, source, file)};
std::vector<std::byte> stripped{csd::audio_replace_rpp(bytes, std::nullopt, file)};
```

### Reading (Run-Time)
Runtime loaders resolve a blob's bytes and reinterpret them as records; name types that look hitboxes up by label should
hash through `hash_identifier` — it is constexpr, so compile-time name hashing works and release lookups can never drift
from the packed data.
```cpp
const auto *frames{reinterpret_cast<const csd::frame_record *>(base + frames_offset)};
const std::size_t frame_total{frames_size / sizeof(csd::frame_record)};
constexpr std::uint64_t identifier{csd::hash_identifier("player.hurt")};
```
