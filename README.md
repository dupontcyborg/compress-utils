# compress-utils

<p align="center">
  <img src="https://img.shields.io/github/actions/workflow/status/dupontcyborg/compress-utils/build_and_test_c_cpp.yml" alt="GitHub Actions Workflow Status"/>
  <img src="https://img.shields.io/github/v/release/dupontcyborg/compress-utils" alt="GitHub Release"/>
</p>
<p align="center">
  <img src="https://img.shields.io/badge/algorithms-4-darkgreen?style=flat" alt="Algorithms"/>
  <img src="https://img.shields.io/badge/languages-3-darkgreen?style=flat" alt="Languages"/>
  <img src="https://img.shields.io/github/languages/code-size/dupontcyborg/compress-utils" alt="Code Size"/>
  <img src="https://img.shields.io/github/license/dupontcyborg/compress-utils" alt="License"/>
</p>

`compress-utils` aims to simplify data compression by offering a unified interface for various algorithms and languages, while maintaining best-in-class performance. 

## Features

- [4 built-in data compression algorithms](#built-in-compression-algorithms)
- [3 languages supported](#supported-languages)
- [Standardized API](#usage) across all algorithms & languages
- Portable & cross-platform (Linux, macOS, Windows)
- Prebuilt binaries available on [major package managers](#supported-languages) or can be [built from source](#build-from-source)
- Native or near-native [compression & decompression performance](#benchmarks)
- Lightweight binary (30 kB with single algorithm, 4 MB with all)

## Built-in Compression Algorithms

| Algorithm | Description | Benchmarks |
|:---:|---|:---:|
| [brotli](https://github.com/google/brotli.git) | General-purpose with high-to-very-high compression rates | [Benchmarks](#benchmarks) |
| [zlib](https://github.com/madler/zlib) | General-purpose, widely-used (compatible with `gzip`) | [Benchmarks](#benchmarks) |
| [zstd](https://github.com/facebook/zstd) | High-speed, high-ratio compression algorithm | [Benchmarks](#benchmarks) |
| [xz/lzma](https://github.com/tukaani-project/xz.git) | Very-high compression ratio algorithm | [Benchmarks](#benchmarks) |

## Supported Languages

| Language | Package | Code Examples & Docs |
|:---:|:---:|:---:|
| C++ | _TBD_ | [C++ API](bindings/cpp/README.md) |
| C | _TBD_ | [C API](bindings/c/README.md)
| Python | _TBD_ | [Python API](bindings/python/README.md) |

## Usage

This project aims to bring a unified interface across all algorithms & all languages (within reason). To make this possible across all targeted languages, the `compress-utils` API is made available in two flavors:

- Object-Oriented (OOP)
- Functional

Both of these APIs are made dead simple. Here's an OOP example in Python:

```py
from compress_utils import compressor

# Create a 'zstd' compressor object
comp = compressor('zstd')

# Compress data
compressed_data = comp.compress(data)

# Compress data with a compression level (1-10)
compressed_data = comp.compress(data, 5)

# Decompress data
decompressed_data = comp.decompress(compressed_data)
```

Functional usage is similarly simple:

```py
from compress_utils import compress, decompress

# Compress data using `zstd`
compressed_data = compress(data, 'zstd')

# Compress data with a compression level (1-10)
compressed_data = compress(data, 'zstd', 5)

# Decompress data
decompressed_data = decompress(compressed_data, 'zstd')
```

## Language-Specific Examples

You can find language-specific code examples below:

- [C++ API Docs >](bindings/cpp/README.md)
- [C API Docs >](bindings/c/README.md)
- [Python API Docs >](bindings/python/README.md)

## Setup

### Install From Package Manager

_To be added_

### Build From Source

1. Install pre-requisites

- CMake
- Conda (if building Python binding)

2. Clone repo

```sh
git clone https://github.com/dupontcyborg/compress-utils.git
cd compress-utils
```

3. Activate Conda environment (if building Python binding)

```sh
# If using Conda
conda env create -f environment.yml
conda activate compress-utils

# If using Mamba
mamba env create -f environment.yml
mamba activate compress-utils
```

4. Run build script

For Linux/macOS:

```sh
build.sh
```

For Windows:

```cmd
powershell.exe -file build.ps1
```

The built library/libraries will be in `dist/<language>`

A number of configuration parameters are available for `build.sh`:

- `--clean` - performs a clean rebuild of `compress-utils`
- `--algorithms=` - set which algorithms to include in the build, if not all (e.g., `build.sh --algorithms=brotli,zlib,zstd`)
- `--languages=` - set which language bindings to build, if not all (e.g., `build.sh --languages=python,js`)
- `--release` - build release version (higher optimization level)
- `--skip-tests` - skip building & running unit tests

## Benchmarks

_To be added_

## License

This project is distributed under the MIT License. [Read more >](LICENSE)

## Third-Party Code

This project utilizes several open-source compression algorithms. [Read more >](ACKNOWLEDGMENTS.md)