# Development

Building compress-utils from source, and the test suites.

## Prerequisites

- CMake 3.17+
- A C11 compiler (Clang, GCC, MSVC)
- A C++20 compiler (only for the C++ binding and the pybind11 module)
- Python 3.10+ and `pybind11-stubgen` (only for the Python binding)

## Building

```sh
git clone https://github.com/dupontcyborg/compress-utils.git
cd compress-utils
./build.sh                  # Linux / macOS
# or:
powershell -File build.ps1  # Windows
```

The default build produces:

- `dist/c/lib/libcompress_utils.{dylib,so,dll}` — the shared C library, self-contained (all eight algorithms baked in).
- `dist/c/include/compress_utils.h` — the public C header.
- `dist/cpp/include/compress_utils.hpp` — the header-only C++ binding.
- `bindings/python/compress_utils/` — the importable Python package, including auto-generated `.pyi` type stubs.

Builds default to Release (LTO, `-O3` / `/O2`). Useful flags:

- `--algorithms=zstd,zlib` — limit which compressors are included (smaller binary). Default: all.
- `--languages=c,cpp,python,wasm,zig` — which bindings to build. Default: `c,cpp,python` (C is the core and is always built).
- `--debug` — Debug build instead of the default Release.
- `--cores=N` — parallel build cores (default: 1).
- `--clean` — clean every build directory + `dist/` before building.
- `--skip-tests` — don't build/run the test suites.

For raw CMake usage (without `build.sh`):

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

## Testing

Each binding has its own test suite, all wired through ctest:

| Target | What it covers |
|--------|----------------|
| `test_compress_utils` (C) | One-shot, streaming with tight buffers, cross-API round-trip, error codes, edge cases |
| `test_compress_utils_cpp` (C++) | `cu::` namespace surface, RAII semantics, exception translation |
| `test_compress_utils_py` (Python) | Same surface via pybind11, plus 1MB random/repetitive cases, string-vs-enum spellings |

Plus a libFuzzer harness (`-DENABLE_FUZZ=ON`, clang only) at `tests/fuzz/fuzz_decompress.c`.

## Adding an algorithm

See [docs/adding-an-algorithm.md](docs/adding-an-algorithm.md) for the end-to-end walkthrough of wiring a new codec through the C core and every binding.
