# compress-utils (Go)

Unified Go interface for eight compression algorithms — Zstandard, Brotli,
zlib, gzip, bzip2, LZ4, XZ/LZMA, Snappy — over a single high-performance C core.
Same API for every algorithm.

## Installation

```sh
go get github.com/dupontcyborg/compress-utils/bindings/go
```

This binding uses **cgo** and compiles the bundled C sources from source — no
prebuilt library and no network beyond the module fetch. You need only a **C
compiler** (`gcc`/`clang`), which cgo requires anyway; set `CGO_ENABLED=1` (the
default on native builds). The first build compiles the codecs and is then
cached by the Go build cache.

## Quick start

```go
import cu "github.com/dupontcyborg/compress-utils/bindings/go"

data := []byte("the quick brown fox ...")

compressed, err := cu.Compress(cu.Zstd, data, 5)   // level 1 (fast) .. 10 (small)
restored,   err := cu.Decompress(cu.Zstd, compressed)
```

`Decompress` recovers the size from the wire format when the codec records it
(zstd, xz, lz4, snappy) and transparently falls back to streaming otherwise, so
you don't need to know which is which.

### Streaming (io.Reader / io.Writer)

```go
// Compress into any io.Writer. Close() flushes and finalizes the frame — it is
// required; an unclosed Writer produces a truncated stream.
var buf bytes.Buffer
w, _ := cu.NewWriter(&buf, cu.Gzip, 6)
io.Copy(w, src)
w.Close()

// Decompress from any io.Reader.
r, _ := cu.NewReader(&buf, cu.Gzip)
defer r.Close()
io.Copy(dst, r)
```

## Algorithms

`Zstd`, `Brotli`, `Zlib`, `Gzip`, `Bz2`, `Lz4`, `Xz` (alias `Lzma`), `Snappy`.
Wire formats match the other compress-utils bindings and the reference tools
(e.g. `Gzip` is RFC 1952, interoperable with the `gzip` CLI). `cu.Available(a)`
reports whether an algorithm was compiled in.

## Other API

```go
cu.Version()                              // "0.1.0"
cu.SetMaxDecompressedSize(256 << 20)      // cap one-shot Decompress (0 = unbounded)

var e *cu.Error                           // errors carry the C status code
if errors.As(err, &e) { _ = e.Code }
```

## Levels

Every algorithm takes a level from **1 (fastest) to 10 (smallest)**; the C core
maps it to each codec's native range. `DefaultLevel` is 5. Codecs without levels
(Snappy) ignore it.

## Notes

- A `Writer`/`Reader` is not safe for concurrent use; the one-shot `Compress`/
  `Decompress` functions are. Use one stream per goroutine.
- Windows is supported by the C core but the Go binding's cgo build there needs
  a mingw toolchain; CI currently covers Linux and macOS.
