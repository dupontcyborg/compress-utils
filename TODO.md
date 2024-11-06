# Compression Utils TODO

## Fixes

- [X] Fix Windows build issues and re-add `windows-latest` to Github Actions workflows
    - [X] Build `compress-utils` and `compress-utils-static`
    - [X] Build `unit-tests` and `unit-tests-static`
    - [X] Fix `ctest`
    - [X] Build `compress-utils-c` and `compress-utils-c-static`
    - [X] Build `unit-tests-c` and `unit-tests-c-static`
    - [X] Build `xz`
- [ ] Merge all static lib dependencies into `compression-utils-static*` libraries
    - [ ] Disable `ZSTD-LEGACY` & `ZSTD-MULTITHREADED`
    - [ ] Set up `whole-archive` for all platforms
- [ ] Rename `compression-utils` to `compress-utils`

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
- [ ] `python` (3.6 - 3.13)
- [ ] `rust`
- [ ] `swift`
- [ ] `cli` (standalone command-line tool)

## Algorithms

- [X] `brotli`
- [ ] `bzip2`
- [ ] `lz4`
- [X] `xz/lzma`
- [X] `zlib`
- [ ] `zstd`

## Package Managers

- [ ] `c` -> `conan`
- [ ] `c++` -> `conan`
- [ ] `go` -> `pkg.go`
- [ ] `java` -> `maven`
- [ ] `js/ts` -> `npm`
- [ ] `python` -> `pypl`
- [ ] `rust` -> `cargo`
- [ ] `swift` -> ?
- [ ] `cli-macos` -> `homebrew`
- [ ] `cli-linux` -> `apt`/`rpm`