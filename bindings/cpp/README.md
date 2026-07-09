# compress-utils — C++ API

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

Unified C++ interface for eight compression algorithms — Zstandard, Brotli, zlib, bzip2, LZ4, XZ/LZMA, Snappy, and gzip — over a single high-performance C core. Same API for every algorithm.

The binding is a **header-only** RAII wrapper (`bindings/cpp/include/compress_utils.hpp`) over the C ABI in [`include/compress_utils.h`](../../include/compress_utils.h). There is no separate C++ library to build — include the header and link the C library. It requires **C++20** (for `std::span`).

## Table of contents

- [Installation](#installation)
- [Quick start](#quick-start)
- [Supported algorithms](#supported-algorithms)
- [Streaming API](#streaming-api)
- [Introspection & limits](#introspection--limits)
- [Error handling](#error-handling)

## Installation

Build the C library and consume this header. With CMake, link the interface target:

```cmake
add_subdirectory(compress-utils)          # provides compress_utils + compress_utils_cpp
target_link_libraries(your_app PRIVATE compress_utils_cpp)
```

`compress_utils_cpp` is an `INTERFACE` target: it adds the include path, links the
`compress_utils` C library, and requires C++20. If you build outside CMake, add
`bindings/cpp/include` and `include/` to your include path and link
`-lcompress_utils`.

## Quick start

Everything lives in namespace `cu` and is reached through a single header:

```cpp
#include <compress_utils.hpp>
#include <span>
#include <vector>

std::vector<std::uint8_t> data = /* ... */;

// One-shot. level is 1 (fastest) .. 10 (smallest); default 5.
std::vector<std::uint8_t> compressed = cu::compress(cu::Algorithm::Zstd, data, 5);
std::vector<std::uint8_t> restored   = cu::decompress(cu::Algorithm::Zstd, compressed);
```

`compress` / `decompress` take a `std::span<const std::uint8_t>`, so they bind to a
`std::vector`, a C array, or a raw pointer + size without copying:

```cpp
std::span<const std::uint8_t> view(ptr, size);
auto compressed = cu::compress(cu::Algorithm::Zstd, view);
```

## Supported algorithms

`cu::Algorithm` selects the codec. All expose the same 1–10 level scale (mapped to
each codec's native range; codecs with no levels ignore it).

| Algorithm | Enum                    | Wire format produced                                   |
|-----------|-------------------------|--------------------------------------------------------|
| Zstandard | `cu::Algorithm::Zstd`   | ZSTD frame with content size                            |
| Brotli    | `cu::Algorithm::Brotli` | Raw Brotli stream                                       |
| zlib      | `cu::Algorithm::Zlib`   | zlib wrapper (RFC 1950)                                 |
| bzip2     | `cu::Algorithm::Bz2`    | bzip2 stream                                            |
| LZ4       | `cu::Algorithm::Lz4`    | LZ4 frame (compatible with the `lz4` CLI / `.lz4`)      |
| XZ/LZMA   | `cu::Algorithm::Xz`     | XZ stream with CRC64 (`cu::Algorithm::Lzma` is an alias)|
| Snappy    | `cu::Algorithm::Snappy` | Raw Snappy block (interoperable with reference snappy)  |
| gzip      | `cu::Algorithm::Gzip`   | gzip stream (RFC 1952)                                  |

Use `cu::is_available(algo)` to check whether a codec was compiled into the build.

## Streaming API

For data that arrives incrementally or doesn't fit in memory, use the RAII stream
classes. `write()` and `finish()` each return a `std::vector<std::uint8_t>` — the
binding runs the C ABI's "fill buffer, drain" loop internally, so you never see
`BUF_TOO_SMALL`.

```cpp
// Compression
cu::CompressStream cs(cu::Algorithm::Zstd, 5);   // level optional (default 5)
std::vector<std::uint8_t> out;
for (std::span<const std::uint8_t> chunk : chunks) {
    auto piece = cs.write(chunk);
    out.insert(out.end(), piece.begin(), piece.end());
}
auto tail = cs.finish();                          // flush — required
out.insert(out.end(), tail.begin(), tail.end());

// Decompression
cu::DecompressStream ds(cu::Algorithm::Zstd);
auto body = ds.write(out);
auto rest = ds.finish();
body.insert(body.end(), rest.begin(), rest.end());
```

Notes:
- Always call `finish()` to flush trailing data; `DecompressStream::finish()` also
  verifies the input ended on a frame boundary (throws `cu::Error` with
  `CU_ERR_TRUNCATED` otherwise).
- Streams are movable but not copyable; they free their C handle on destruction.
- Snappy's raw block format isn't incrementally codable, so its streams buffer the
  whole input and encode on `finish()` — output stays byte-identical to the
  one-shot path, but memory scales with input size.

## Introspection & limits

```cpp
std::string v   = cu::version();                    // e.g. "0.7.1"
bool        ok  = cu::is_available(cu::Algorithm::Zstd);
std::string nm  = cu::algorithm_name(cu::Algorithm::Zstd);   // "zstd"

// Bound one-shot decompression output (default 1 GiB; 0 = unbounded).
cu::set_max_decompressed_size(256 * 1024 * 1024);
```

## Error handling

Every failure throws `cu::Error` (derived from `std::runtime_error`). It carries the
underlying C status code via `.code()`:

```cpp
try {
    auto restored = cu::decompress(cu::Algorithm::Zstd, maybe_corrupt);
} catch (const cu::Error& e) {
    std::fprintf(stderr, "decompress failed (%d): %s\n", e.code(), e.what());
}
```
