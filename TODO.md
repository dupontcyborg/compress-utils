# Compress Utils TODO

## Bugs & Critical Issues

- [X] **Fix comment/code mismatch in zlib.cpp:60-61** - Comment says "max 4 times" but code uses `retries = 10`
- [X] **C API discards all error context** - `catch (const std::exception& e)` returns `-1` with no way to diagnose failures
  - [X] Add error retrieval function (`compress_utils_last_error()`)
  - [X] Add thread-local error storage
- [X] **Add semantic versioning** - No `VERSION` in CMakeLists.txt or headers
  - [X] Add `project(compress-utils VERSION 0.1.0)` to CMakeLists.txt
  - [X] Add version macros to public headers (`version.hpp`)
  - [X] Add runtime version query functions

## Code Quality

- [X] **Fix span parameter const-correctness** - All algorithm headers now pass `std::span<const uint8_t>` by value (idiomatic)
  - [X] `src/algorithms/zstd/zstd.hpp`
  - [X] `src/algorithms/brotli/brotli.hpp`
  - [X] `src/algorithms/xz/xz.hpp`
  - [X] `src/algorithms/zlib/zlib.hpp`
- [X] **Fix docstring copy-paste errors** - brotli.hpp and xz.hpp incorrectly said "Zstandard"
- [ ] **Extract magic numbers to constants.hpp**
  - [ ] `xz.cpp:36` - `64 * 1024` buffer size
  - [ ] `xz.cpp:100` - `16384` minimum buffer
  - [ ] `zlib.cpp:53` - `data.size() * 4` initial buffer multiplier
  - [ ] `zlib.cpp:61` - `10` retry count
- [ ] **Deduplicate buffer resizing logic** - Each algorithm implements its own buffer-doubling loop; extract to shared utility
- [ ] **Remove `using namespace` from compress_utils_py.cpp:7** - Bad practice in implementation files
- [ ] **Add allocation failure checks in C tests** - `test_compress_utils.c` doesn't check malloc return values
- [ ] **Replace hardcoded test values with named constants** - `1024 * 1024`, `1024 * 1024 * 32`, etc.

## Build System Fixes

- [X] Fix Windows build issues and re-add `windows-latest` to Github Actions workflows
  - [X] Build `compress-utils` and `compress-utils-static`
  - [X] Build `unit-tests` and `unit-tests-static`
  - [X] Fix `ctest`
  - [X] Build `compress-utils-c` and `compress-utils-c-static`
  - [X] Build `unit-tests-c` and `unit-tests-c-static`
  - [X] Build `xz`
- [X] Rename `compress-utils` to `compress-utils`
- [ ] Merge all static lib dependencies into `compress-utils-static*` libraries
  - [ ] Disable `ZSTD-LEGACY` & `ZSTD-MULTITHREADED`
  - [ ] Set up `whole-archive` for all platforms
- [ ] Re-enable macOS Universal2 builds (CMakeLists.txt:19-22)
- [ ] Add CMake package config for `find_package(compress_utils)` support

## Optimizations

- [X] Support iterative builds in `cibuildwheel` (via separated Python binding project & shared core lib project)
- [X] Add source wheel distribution for unsupported Python wheel configurations
- [X] Split CI/CD pipelines hierarchically
- [ ] Add missing architectures to CI/CD pipelines (`aarch64` on Linux & Windows, `x86/universal2` on macOS)

## Documentation

- [ ] **Document thread-safety guarantees**
  - [ ] Functional API is thread-safe (stateless)
  - [ ] Compressor class requires external synchronization for shared instances
  - [ ] Streaming classes are NOT thread-safe (documented in header)
- [ ] **Document compression level mappings per algorithm**
  - [ ] ZSTD: 1-10 → 2-22
  - [ ] Brotli: 1-10 → 1-11
  - [ ] zlib: 1-10 → 1-9 (capped)
  - [ ] XZ: 1-10 → 0-9
- [ ] **Add API reference documentation** (Doxygen or similar)
- [ ] **Add CHANGELOG.md** for tracking releases

## New Features

- [X] Github Workflow for artifact publishing
- [X] **Streaming compression/decompression support** (C++ API complete)
  - [X] Design streaming API (CompressStream/DecompressStream classes)
  - [X] Implement for each algorithm using native streaming APIs (ZSTD, Brotli, zlib, XZ)
  - [ ] Add Python bindings for streaming
  - [ ] Add C bindings for streaming
  - [ ] Add streaming unit tests
- [ ] Cross-language performance testbench
- [ ] Standalone CLI executable
- [ ] Multi-file input/output (archiving) via `zip` and `tar.*`
- [ ] Async/multi-threaded compression support

## Bindings (implementation, tooling, tests & ci/cd updates)

- [X] `c++` (Main Lib)
- [X] `c`
- [ ] `go`
- [ ] `java`
- [ ] `js/ts` (WebAssembly via Emscripten)
- [X] `python` (3.6 - 3.13)
- [ ] `rust`
- [ ] `swift`
- [ ] `cli` (standalone command-line tool)

## Algorithms

- [X] `brotli`
- [ ] `bzip2`
- [ ] `lz4` (high priority - fast compression option)
- [X] `xz/lzma`
- [X] `zlib`
- [X] `zstd`

## Package Managers

- [ ] `c` -> `conan`
- [ ] `c++` -> `conan`
- [ ] `go` -> `pkg.go`
- [ ] `java` -> `maven`
- [ ] `js/ts` -> `npm`
- [X] `python` -> `pypi`
- [ ] `rust` -> `cargo`
- [ ] `swift` -> ?
- [ ] `cli-macos` -> `homebrew`
- [ ] `cli-linux` -> `apt`/`rpm`
