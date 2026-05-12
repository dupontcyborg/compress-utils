# compress-utils

[![PyPI version](https://badge.fury.io/py/compress-utils.svg)](https://badge.fury.io/py/compress-utils)
[![PyPI Downloads](https://static.pepy.tech/badge/compress-utils)](https://pepy.tech/projects/compress-utils)
[![Python Versions](https://img.shields.io/pypi/pyversions/compress-utils.svg)](https://pypi.org/project/compress-utils/)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

Unified Python interface for six compression algorithms — Brotli, bzip2, LZ4, XZ/LZMA, zlib, and Zstandard — backed by a single high-performance C library. Same API for every algorithm.

## Installation

```bash
pip install compress-utils
```

The wheel is self-contained: no system codec libraries required at runtime. Type stubs (`.pyi`) and a `py.typed` marker ship with the wheel, so `mypy`, `pyright`, and IDE autocomplete work out of the box.

## Quick start

### Functional API (most common)

```python
import compress_utils as cu

data = b"the quick brown fox jumps over the lazy dog" * 100

compressed = cu.compress(data, "zstd", level=5)
restored   = cu.decompress(compressed, "zstd")
assert restored == data
```

Algorithm names accept both lowercase strings (`"zstd"`, `"brotli"`, …) and the typed enum:

```python
cu.compress(data, cu.Algorithm.zstd, level=5)
```

### Streaming API

For inputs that don't fit in memory or arrive incrementally:

```python
from compress_utils import CompressStream, DecompressStream

cs = CompressStream("zstd", level=5)
chunks = [cs.compress(chunk) for chunk in iter_chunks(some_file)]
chunks.append(cs.finish())
compressed = b"".join(chunks)

ds = DecompressStream("zstd")
restored = ds.decompress(compressed) + ds.finish()
```

## Supported algorithms

| Algorithm | Spelling                | Notes                                          |
|-----------|-------------------------|------------------------------------------------|
| Zstandard | `"zstd"`                | Wire format: ZSTD frame with content size.     |
| Brotli    | `"brotli"`              | Wire format: raw Brotli stream.                |
| zlib      | `"zlib"` (also `"gzip"`) | Wire format: zlib wrapper (RFC 1950).         |
| bzip2     | `"bz2"` (also `"bzip2"`) | bzip2 stream.                                  |
| LZ4       | `"lz4"`                 | LZ4 **frame** format (compatible with `lz4` CLI / `.lz4` files). |
| XZ/LZMA   | `"xz"` (also `"lzma"`)  | XZ stream with CRC64.                          |

`cu.is_available(name_or_enum)` returns `True` for algorithms compiled into the installed wheel.

## Other helpful APIs

```python
cu.version()                                  # "0.1.0"
cu.set_max_decompressed_size(256 * 1024**2)   # bound one-shot decompression
                                              # (default: 1 GiB; 0 = unbounded)

try:
    cu.decompress(garbage, "zstd")
except cu.CompressError as e:
    print(e)
```

## Compression levels

Every algorithm accepts a `level` from **1 (fastest) to 10 (smallest)**. The Python binding maps this to each algorithm's native range automatically — you don't need to know that ZSTD goes 1–22 or zlib goes 1–9. Defaults to `5` if omitted.

## Type checking

The package ships PEP 561 type information:

```python
import compress_utils as cu
reveal_type(cu.compress)           # → (data: Buffer, algorithm: object, level: int = 5) -> bytes
reveal_type(cu.Algorithm.zstd)     # → Algorithm
```

Stubs are regenerated from the compiled module at build time (via `pybind11-stubgen`), so they cannot drift from the binding signatures.

## Performance notes

The Python binding is a thin pybind11 wrapper over the C library. Streaming uses chunked buffers with an internal drain protocol — output is yielded in 64 KB pages, so streaming a multi-GB file does not hold the whole compressed result in memory.

For applications that round-trip many small payloads with the same algorithm, prefer the functional API over creating a new `CompressStream` per payload — internal codec state is short-lived and reused.

## Project layout

This is one of three official bindings to the [compress-utils](https://github.com/dupontcyborg/compress-utils) C library:

- **C** — the canonical ABI in `include/compress_utils.h`.
- **C++** — header-only `cu::` namespace, `bindings/cpp/`.
- **Python** — this package.

WASM/JS, Go, Rust, Swift, and Java bindings are tracked in [TODO.md](https://github.com/dupontcyborg/compress-utils/blob/main/TODO.md).

## License

MIT. See [LICENSE](https://github.com/dupontcyborg/compress-utils/blob/main/LICENSE).
