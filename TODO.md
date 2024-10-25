# Compression Utils TODO

## Fixes

- [ ] Fix Windows build issues and re-add `windows-latest` to Github Actions workflows
- [ ] Merge all static lib dependencies into `compression-utils-static*` libraries

## Additions

- [ ] Cross-language performance testbench
- [ ] Standalone CLI executable
- [ ] Github Workflow for artifact publishing

## Bindings (implementation, tooling, tests & ci/cd updates)

- [X] `c++` (Main Lib)
- [X] `c`
- [ ] `go`
- [ ] `java`
- [ ] `js/ts` (WebAssembly via Emscripten)
- [ ] `python` (3.6 - 3.13)
- [ ] `rust`
- [ ] `swift`

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