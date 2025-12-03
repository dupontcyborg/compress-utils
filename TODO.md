# Compress Utils TODO

## Bugs & Critical Issues

- [ ] **Fix comment/code mismatch in zlib.cpp:60-61** - Comment says "max 4 times" but code uses `retries = 10`
- [ ] **C API discards all error context** - `catch (const std::exception& e)` returns `-1` with no way to diagnose failures
  - [ ] Add error retrieval function (e.g., `compress_utils_last_error()`)
  - [ ] Or add thread-local error storage
  - [ ] Or return error codes enum instead of generic `-1`
- [ ] **Add semantic versioning** - No `VERSION` in CMakeLists.txt or headers
  - [ ] Add `project(compress-utils VERSION x.y.z)` to CMakeLists.txt
  - [ ] Add version macros to public headers
  - [ ] Add runtime version query function

## Code Quality

- [ ] **Fix span parameter const-correctness** - All algorithm headers pass `std::span<const uint8_t>&` instead of `const std::span<const uint8_t>&` or by value
  - [ ] `src/algorithms/zstd/zstd.hpp`
  - [ ] `src/algorithms/brotli/brotli.hpp`
  - [ ] `src/algorithms/xz/xz.hpp`
  - [ ] `src/algorithms/zlib/zlib.hpp`
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
- [ ] **Document compression level mappings per algorithm**
  - [ ] ZSTD: 1-10 → 2-22
  - [ ] Brotli: 1-10 → 1-11
  - [ ] zlib: 1-10 → 1-9 (capped)
  - [ ] XZ: 1-10 → 0-9
- [ ] **Add API reference documentation** (Doxygen or similar)
- [ ] **Add CHANGELOG.md** for tracking releases

## New Features

- [X] Github Workflow for artifact publishing
- [ ] **Streaming compression/decompression support** (high priority - enables large file handling)
  - [ ] Design streaming API (CompressStream/DecompressStream classes)
  - [ ] Implement for each algorithm using native streaming APIs
  - [ ] Add Python bindings for streaming
  - [ ] Add C bindings for streaming
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
