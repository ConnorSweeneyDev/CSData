# CSPack
A single-header library defining the CSP file format: a packed, memory-mappable container of blobs.

## Format
```
[ 32-byte header ][ 16-byte-aligned content region ][ directory: count x entry ]
```
- `header` holds the magic, version, blob `count`, the FNV-1a `signature`, and the content `size`.
- The content region holds the blobs, each padded to a 16-byte boundary.
- The directory holds one `entry` per blob — its `offset`, `size`, and CRC32C `fingerprint` — appended in blob order, so
  it stays sorted by offset.

## Features
- Single header, depending only on the C++20 standard library and the OS mapping API.
- One definition of the CSP file format, shared by the writer and the mapper so they cannot drift.
- Memory-mapping; reading pages in on demand stays zero-copy, so mapping is cheap regardless of file size.
- Lazy, per-blob CRC32C verification: only the blobs you touch are checked, and only their pages are faulted in, so a
  multi-gigabyte pack mounts instantly. Verification is computed with a slicing-by-8 software CRC and a
  three-way-parallel hardware CRC (SSE 4.2) where available.
- Multiple packs can be mounted at once, each registered by name. Verification dispatches to the owning pack by pointer,
  so consumers never track which pack a blob came from.
- An FNV-1a signature ties a file to the build that expects it.

## Requirements
- Windows or Linux OS.
- A C++20 compiler.

## Usage
### Writer (Build-Time)
Append blobs in order, then serialise. `table` records the `(offset, size)` each blob landed at, and `signature()` ties
the file to this build; the build emits both so the consumer can resolve its regions and validate the file. `write()`
computes each blob's CRC32C and emits the directory automatically.
```cpp
csp::pack pack;
pack.append(first_bytes);
pack.append(second_bytes);
csp::write(pack, "Data.csp");
```

### Mapper (Run-Time)
`mount()` maps a file from `directory`, validates its header against the build-time signature and the directory layout,
and registers it by name in an internal table. It does **not** read the content so mounting is cheap regardless of size.
Several packs may be mounted at once; `mount()` returns the mapping so the caller can resolve its own spans against
`base()`. Mappings are read-only and stay valid until `unmount()` or exit.
```cpp
// At startup, before anything reads the file.
csp::mapping &pack{csp::mount(directory, "Data.csp", 6125984697962060194ull)};
```
Resolve a blob's bytes against `pack.base()`, then call `csp::verify()` the first time you actually read them. The free
`verify()` locates the pack whose mapped region contains the pointer, checks that blob's CRC32C once (caching the
result), faults in only that blob's pages, and returns the pointer; it throws if the blob is corrupt. After the first
call it is a cheap, lock-free flag check, so it is safe to call on every access and from multiple threads.
```cpp
const std::span<const unsigned char> first{pack.base() + 32, 2036};
csp::verify(first.data(), first.size());
// Now use first's bytes.
```
Pointers that fall outside every mapping pass through `verify()` unchanged; use `base()` directly only for bytes you do
not need verified. A mounted pack can also be retrieved by name with `csp::mounted("Data.csp")` and released with
`csp::unmount("Data.csp")` (e.g. when unloading a level). Each `mapping` additionally exposes its own `verify()` taking
either a pointer or an `(offset, size)` pair, for when you already hold the handle.
