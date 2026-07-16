# compress-utils (Rust)

Unified, safe Rust interface for eight compression algorithms — Zstandard,
Brotli, zlib, gzip, bzip2, LZ4, XZ/LZMA, Snappy — over a single
high-performance C core. Same API for every algorithm.

## Installation

```toml
[dependencies]
compress-utils = { git = "https://github.com/dupontcyborg/compress-utils" }
```

The crate compiles the bundled C sources from source with the `cc` crate — no
prebuilt library and no network beyond the crate fetch, so it works offline and
on docs.rs. You need only a **C/C++ compiler** (`gcc`/`clang`/MSVC). The first
build compiles the codecs and is then cached by Cargo.

## Quick start

```rust
use compress_utils::{compress, decompress, Algorithm};

let data = b"the quick brown fox ...";
let packed   = compress(Algorithm::Zstd, data, 5)?;   // level 1 (fast)..10 (small)
let restored = decompress(Algorithm::Zstd, &packed)?;
assert_eq!(&restored, data);
# Ok::<(), compress_utils::Error>(())
```

`decompress` recovers the size from the wire format when the codec records it
(zstd, xz, lz4, snappy) and transparently falls back to streaming otherwise, so
you don't need to know which is which. It is bounded by
[`set_max_decompressed_size`] (default 1 GiB) to stay safe against malformed
"decompression bomb" inputs.

### Streaming (`std::io::Read` / `Write`)

```rust
use std::io::{Read, Write};
use compress_utils::{Algorithm, Compressor, Decompressor};

// Compress into any Write. finish() flushes and finalizes the frame — call it;
// as a safety net drop() also finalizes, but that discards any I/O error.
let mut w = Compressor::new(Vec::new(), Algorithm::Gzip, 6)?;
w.write_all(b"...")?;
let packed = w.finish()?;

// Decompress from any Read.
let mut r = Decompressor::new(&packed[..], Algorithm::Gzip)?;
let mut out = Vec::new();
r.read_to_end(&mut out)?;
# Ok::<(), Box<dyn std::error::Error>>(())
```

## Algorithms

`Zstd`, `Brotli`, `Zlib`, `Gzip`, `Bz2`, `Lz4`, `Xz` (alias `Lzma`), `Snappy`.
Wire formats match the other compress-utils bindings and the reference tools
(e.g. `Gzip` is RFC 1952, interoperable with the `gzip` CLI; the test suite
round-trips every codec against independent pure-Rust implementations).
`Algorithm::Zstd.available()` reports whether an algorithm was compiled in.

## Other API

```rust
compress_utils::version();                          // "0.7.1"
compress_utils::set_max_decompressed_size(256 << 20); // cap decompress (0 = unbounded)

// Errors carry the C status code for programmatic matching.
match compress_utils::decompress(algo, bad) {
    Err(e) if e.code == compress_utils::Status::Truncated => { /* ... */ }
    _ => {}
}
```

## Levels

Every algorithm takes a level from **1 (fastest) to 10 (smallest)**; the C core
maps it to each codec's native range. `DEFAULT_LEVEL` is 5. Codecs without
levels (Snappy) ignore it.

## Notes

- A `Compressor`/`Decompressor` is not safe for concurrent use; the one-shot
  `compress`/`decompress` functions are. Use one stream per thread.
- The crate is pure C — every codec (including Snappy, which uses the C
  `andikleen/snappy-c` port) compiles to C, so there is no `libc++`/`libstdc++`
  runtime dependency.
- Windows is supported by the C core; the crate's source build works with MSVC.
  CI currently covers Linux and macOS (matching the other source bindings).

[`set_max_decompressed_size`]: https://docs.rs/compress-utils
