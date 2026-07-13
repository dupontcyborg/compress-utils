# compress-utils

<p align="center">
  <img src="https://img.shields.io/badge/algorithms-8-green?style=flat" alt="Algorithms"/>
  <img src="https://img.shields.io/badge/languages-6-yellow?style=flat" alt="Languages"/>
  <img src="https://img.shields.io/github/license/dupontcyborg/compress-utils" alt="License"/>
</p>
<p align="center">
  <img src="https://img.shields.io/github/actions/workflow/status/dupontcyborg/compress-utils/pr_build_and_test.yml" alt="GitHub Actions Workflow Status"/>
  <img src="https://img.shields.io/github/v/release/dupontcyborg/compress-utils" alt="GitHub Release"/>
  <img src="https://img.shields.io/github/languages/code-size/dupontcyborg/compress-utils" alt="Code Size"/>
</p>

A unified, high-performance interface for eight compression algorithms — **Zstandard, Brotli, zlib, bzip2, LZ4, XZ/LZMA, Snappy, gzip** — exposed identically across multiple languages.

```
                      ┌─────────────────────────────┐
   Your application → │  C / C++ / Python / JS / TS │
                      └──────────────┬──────────────┘
                                     │
                      ┌──────────────▼──────────────┐
                      │   compress-utils C ABI      │
                      │  (one library, eight algos) │
                      └──────────────┬──────────────┘
                                     │
         ┌───────┬───────┬───────┬───┴───┬───────┬───────┬───────┐
       zstd   brotli   zlib    gzip     bz2     lz4     xz    snappy
```

The C library is the canonical surface. Every other binding is a thin shim — same allocation model, same error codes, same streaming protocol. Add a binding for any language that speaks C ABI; the work is mostly making the language's idioms (strings, exceptions, generators) feel natural on top of a uniform substrate.

## Supported languages

| Language | Install                                              | Docs                                          |
|----------|------------------------------------------------------|-----------------------------------------------|
| **C**    | Build from source | [include/compress_utils.h](include/compress_utils.h) |
| **C++**  | Build from source | [bindings/cpp/README.md](bindings/cpp/README.md) |
| **Python** | `pip install compress-utils` | [bindings/python/README.md](bindings/python/README.md) |
| **JavaScript (WASM)** | `npm install compress-utils` | [bindings/wasm/README.md](bindings/wasm/README.md) |
| **TypeScript (WASM)** | `npm install compress-utils` | [bindings/wasm/README.md](bindings/wasm/README.md) |
| **Go** | `go get github.com/dupontcyborg/compress-utils/bindings/go` | [bindings/go/README.md](bindings/go/README.md) |
| Rust, Swift, Java | _Planned — all consume the C ABI directly_ |  |

For now each binding's README has its own installation + quickstart. A cross-cutting `docs/` is planned for architecture, allocation model, and per-algorithm notes — tracked in [TODO.md](TODO.md#documentation-plan-planned-2026-05-11).

## Supported algorithms

| Algorithm                                          | Strength               | Wire format produced            |
|----------------------------------------------------|------------------------|---------------------------------|
| [Zstandard](https://github.com/facebook/zstd)      | High speed, high ratio | ZSTD frame with content size    |
| [Brotli](https://github.com/google/brotli)         | Web-optimized          | Raw Brotli stream               |
| [zlib](https://github.com/madler/zlib)             | Ubiquitous             | zlib wrapper (RFC 1950)         |
| [gzip](https://www.gnu.org/software/gzip/)         | Ubiquitous (.gz files) | gzip stream (RFC 1952)          |
| [bzip2](https://sourceware.org/bzip2)              | High ratio             | bzip2 stream                    |
| [LZ4](https://github.com/lz4/lz4)                  | Highest speed          | LZ4 frame (interoperable with `lz4` CLI / `.lz4` files) |
| [XZ / LZMA](https://github.com/tukaani-project/xz) | Highest ratio          | XZ stream with CRC64            |
| [Snappy](https://github.com/google/snappy)         | Very high speed, low ratio | Raw Snappy block (interoperable with reference snappy / python-snappy) |

All algorithms expose the same API surface and the same level scale (`1` fastest → `10` smallest). The library maps each user level to the algorithm's native range so you don't need to remember that ZSTD goes 1–22 and zlib goes 1–9.

## Benchmarks

![throughput by language and algorithm](benchmarks/assets/lang-comparison.png)

Full methodology, per-corpus Pareto plots, and how to reproduce are in [benchmarks/README.md](benchmarks/README.md).

## Building from source

Prerequisites, build flags, and the test suites are documented in [DEVELOPMENT.md](DEVELOPMENT.md). The short version:

```sh
git clone https://github.com/dupontcyborg/compress-utils.git
cd compress-utils
./build.sh                  # Linux / macOS  (powershell -File build.ps1 on Windows)
```

## AI disclosure

This project was built with substantial use of large language models. Specifically:

- Architecture and design: human (me, [@dupontcyborg](https://nico.codes), a senior software engineer).
- Implementation: basically entirely LLM-driven. Most of the C core, all of the C++/Python/WASM bindings, and the test suites were drafted by Mr. Claude
- Review: me again.

Bugs and typos are most likely my own.

## License

MIT — see [LICENSE](LICENSE).

## Acknowledgments

This project wraps seven battle-tested upstream compression libraries. See [ACKNOWLEDGMENTS.md](ACKNOWLEDGMENTS.md).

---

Built by [Nico Dupont](https://nico.codes).
