# compression-utils

![Static Badge](https://img.shields.io/badge/Status-Under_Construction-red?style=flat)

`compression-utils` aims to simplify data compression by offering a unified interface for various algorithms and languages, while maintaining best-in-class performance. 

## Features

- [2+ built-in data compression algorithms](#built-in-compression-algorithms)
- [2+ languages supported](#supported-languages)
- [Standardized API](#usage) across all algorithms & languages
- Portable & cross-platform (Linux, macOS, Windows, WASM)
- Prebuilt binaries available on [major package managers](#supported-languages) or can be [built from source](#build-from-source)
- Native or near-native [compression & decompression performance](#Benchmarks)
- Lightweight binary (30 kB with single algorithm, 4 MB with all)

## Built-in Compression Algorithms

| Algorithm | Description | Benchmarks |
|:---:|---|:---:|
| [zlib](https://github.com/madler/zlib) | General-purpose, widely-used (compatible with `gzip`) | [Benchmarks](#benchmarks) |
| [zstd](https://github.com/facebook/zstd) | High-speed, high-ratio compression algorithm | [Benchmarks](#benchmarks) |

## Supported Languages

| Language | Package | Code Examples |
|:---:|:---:|:---:|
| C++ | _TBD_ | [C++ Code](#c-usage) |

## Usage

### C++ Usage

_To be added_

```
// C++ code example
```

## Setup

### Install From Package Manager

_To be added_

### Build From Source

1. Make sure you have `CMake` installed
2. Clone & build

```
git clone https://github.com/dupontcyborg/compression-utils.git
cd compression-utils
build.sh
```

A number of configuration parameters are available for `build.sh`:

- `--clean` - performs a clean rebuild of `compression-utils`
- `--algorithms=` - set which algorithms to include in the build (e.g., `build.sh --algorithms=zlib,zstd`)
- `--languages=` - set which language bindings to build (e.g., `build.sh --languages=python,js`)
- `--release` - build release version (higher optimization level)
- `--skip-tests` - skip building & running unit tests

## Benchmarks

_To be added_

## License

This project is distributed under the MIT License. [Read more >](LICENSE)

## Third-Party Code

This project utilizes several open-source compression algorithms. [Read more >](ACKNOWLEDGMENTS.md)