# Compress Utils TODO

## Fixes

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

## Optimizations

- [X] Support iterative builds in `cibuildwheel` (via separated Python binding project & shared core lib project)
- [X] Add source wheel distribution for unsupported Python wheel configurations
- [X] Split CI/CD pipelines hierarchically
- [ ] Add missing architectures to CI/CD pipelines (`aarch64` on Linux & Windows, `x86/universal2` on macOS)

## Additions

- [ ] Cross-language performance testbench
- [ ] Standalone CLI executable
- [ ] Github Workflow for artifact publishing
- [ ] Multi-file input/output (archiving) via `zip` and `tar.*`
- [ ] Streaming compression/decompression support

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
- [ ] `lz4`
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