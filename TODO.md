# Compress Utils TODO

## Next up (2026-10)

Direction: stop adding breadth and find out where the library wins. Likely positioning is "every format, including the browser, hardened against untrusted input", to be confirmed by the ecosystem benchmarks.

- [x] **Fix the decompressed-size cap bypass** (see Bugs; fixed 2026-10-22).
- [x] **README: untrusted-input section** (2026-10-22): root README "Untrusted input" section (output cap for every format, stream limits, bounded decoder memory, final errors, `.xz`-only decoding, fuzzing in CI with a regression corpus, pinned offline builds), linked from each binding's "Errors and limits".
- [ ] **Ecosystem benchmark baselines** — plan in [docs/ecosystem-benchmarks.md](docs/ecosystem-benchmarks.md). The harness supports them (`impl` field, baseline overlays) but no driver exists. Python: cramjam, zstandard, stdlib zlib/bz2/lzma/`compression.zstd` (3.14). JS: fflate, pako, a zstd WASM package. Rust: the native crates. Until this lands, "high-performance" in the README is a claim, not a result.
- [ ] **Path to 1.0:** settle the ABI questions that are expensive to change later, then freeze the C ABI.
  - [ ] An escape hatch past the 1–10 level (parameters struct: window size, zstd long mode, ...).
  - [ ] zstd dictionaries: decide the API shape, even if it ships after 1.0.
  - [ ] Fix the open stream bugs below.
  - [ ] Add CHANGELOG.md.

## Findings from the C ecosystem benchmarks (2026-10-22)

Measured on branch `ecosystem-benchmarks` (M-series MacBook, single-threaded, release builds; details in `docs/ecosystem-benchmarks.md`). Proposals, not yet decided:

- [ ] **xz: cap the dictionary at the input size** (rounded up to a power of two, at least 4 KiB), as 7-Zip does. liblzma otherwise sets up the preset's full dictionary (8 MiB at preset 6, 64 MiB at 9) on every call. Measured as `c-liblzma-fitdict`: compression up to 7.5× faster on 1 KB-4 KB messages at high levels (0.13× the time at level 9), ratio unchanged, neutral on large files; matches the LZMA SDK's small-input lead. Output headers then record the smaller dictionary, so bytes differ from `xz -N` (still standard `.xz`).
- [ ] **Deflate backend (zlib, gzip, deflate): we ship the slowest mainstream implementation** (madler zlib). At equal or better ratio, on silesia-mini: libdeflate 2.7× compress / 2.45× decompress (one-shot only, no streaming); zlib-ng 1.4× / 1.6× (one-shot and streaming, ~+40 KiB binary); zlib-rs similar to zlib-ng. Recommendation: zlib-ng. Lifts every binding.
- [ ] **Snappy: Google's C++ snappy decompresses 1.5-2.0× faster** than the andikleen C port we switched to in 2026-07, and compresses ~1.3-1.5× faster at 0.6% lower ratio. Trade-off against taking back the C++ runtime dependency.
- [ ] **Level map on small inputs:** our xz level 9 maps to preset 8, which is slower than preset 9 at the same ratio on small inputs; revisit together with the lz4 levels 1/2 item below.
- [ ] **Small fixed per-call costs:** one-shot lz4 decompression is 5-12% slower than raw liblz4 on 1 KB messages, brotli decompression 2-4% slower everywhere. Everything else is within 1-2% of the raw library, with byte-identical output.
- [ ] **Reusable compressor/decompressor objects: post-1.0.** Measured gain is modest: zstd 0-8% even on 1 KB messages (it already sizes its tables to the pledged input size); libdeflate 1.2-1.5× on messages of 4 KB and under, nothing from 16 KB up.
- [ ] **Document single-codec builds.** A stripped static binary with the default all-codec archive carries 1.6 MiB; with one codec (`INCLUDE_<CODEC>`) it's 11-811 KiB, and decode-only (`-DCU_OMIT_COMPRESS`) less. Neither is documented as a supported native configuration.
- [x] ~~Benchmarks: rerun github-users with ~9 samples before publishing~~ — dropped 2026-10-23 (C is done; keep the benchmarks lean). Caveat for anyone quoting it: at 3-5 samples, identical code varies 0.6-1.9× level to level on that tier (per-call allocation noise); the block tiers and large files are stable.

## Bugs & Critical Issues

- [x] **The decompressed-size cap was bypassed for formats without a recorded size (fixed 2026-10-22).** `set_max_decompressed_size` (default 1 GiB) was only enforced when the size probe succeeded (zstd, lz4, Snappy). For zlib, gzip, deflate, brotli, bz2 and xz, one-shot `decompress()` fell back to an uncapped streaming decode in C++, Python, Go and WASM: with a 1 MiB cap, a 102-byte brotli and a 110-byte bz2 input each decompressed to 64 MiB. Separately, `cu_decompress_size_hint` returned the declared size without checking the cap, so bindings allocated an attacker-chosen size before `cu_decompress` rejected it. Fix: a per-stream output limit in the C core (`cu_decompress_stream_set_max_output`, exposed in every binding as `set_max_output` / `max_output` / `SetMaxOutput` / `maxOutput`), which every binding's one-shot fallback sets to the global cap (`cu_get_max_decompressed_size`, now public); and the size hint returns `CU_ERR_SIZE_LIMIT` above the cap. Rust's binding-side cap mirror was removed, so every binding enforces the cap the same way. Tests per codec in C, C++, Python, Go, Rust and WASM.
- [ ] **lz4 levels 1 and 2 produce identical output.** They map to native 0 and 1 (`lz4_native_level` in `src/algorithms/lz4/lz4.c`), which LZ4F both treats as its default fast mode, so the 1..10 scale loses a step at the fast end. Found while building `benchmarks/levels.json` (2026-10-22). A fix changes what level 2 produces, so it's a behavior change; consider spreading 1..3 over LZ4's acceleration settings instead.
- [ ] **xz `size_hint` contradicts the header docs.** `include/compress_utils.h` says XZ size is available via the stream footer/index, but `xz_decompress_size_hint` returns `CU_ERR_SIZE_UNKNOWN` (TODO in `src/algorithms/xz/xz.c`). Either parse the index or fix the docs.
- [X] **XZ decompress hang / bomb on malformed input (fixed 2026-07).** Feeding a
  non-xz-magic byte (e.g. a lone `0x00`) to the xz decompressor never terminated.
  Two independent causes, both fixed in `src/algorithms/xz/xz.c`:
  1. `xz_dstream_create`/`xz_decompress` used `lzma_auto_decoder`, which falls
     back to the legacy `.lzma` (LZMA_ALONE) format on a non-`0xFD` first byte;
     that format can declare "unknown size", so liblzma expanded garbage without
     bound → **switched to `lzma_stream_decoder`** (`.xz`-only, no `.lzma`).
  2. `stream_pump` mapped `LZMA_BUF_ERROR` (no forward progress on a truncated
     stream) to `CU_ERR_BUF_TOO_SMALL` — a *drain-and-retry* signal — so the
     caller looped forever with zero output. Now `LZMA_BUF_ERROR`/no-progress on
     FINISH is **terminal → `CU_ERR_TRUNCATED`**; the one-shot path likewise stops
     treating `LZMA_OK`-without-`STREAM_END` as success.
  Affected every source binding (Rust already had a binding-layer guard; Go was
  vulnerable — now fixed at the core). Regression test:
  `tests/test_compress_utils.c::test_reject_garbage` drains `finish()` in a
  bounded loop for every codec and fails if it doesn't terminate.
- [X] **Default-mode Snappy stream decoder allocated the declared size uncapped.** `snappy_dstream_finish` malloc'd the uncompressed length read from the block header (up to ~4 GiB from a few header bytes) before the caller saw any output, so the caller couldn't enforce its own cap. Every Snappy stream decoder now enforces the decompressed-size cap captured at creation, as single-stream mode and the raw LZ4/LZO decoders do; a stream over the cap fails with `CU_ERR_SIZE_LIMIT`.
- [ ] **Misleading bz2 error after trailing bytes (default mode).** After a complete bz2 stream followed by extra bytes in the same write, `decompress` returns the full output but `finish()` fails with "bz2: sequence error" (`BZ2_bzDecompress` called again after `BZ_STREAM_END`). It should report data after the end of the stream, as the other codecs do. Single-stream mode is unaffected.
- [ ] **Default-mode stream errors drop the output decoded before them in the bindings.** The C core reports that output (`*out_len` alongside the error status). The Go `Reader` and Rust `Decompressor` return it before the error, but the C++ wrapper's `drain_loop` (and so Python) and the WASM streams throw it away when they raise. Returning it would change what the default mode raises, so it needs its own design (e.g. attaching the partial output to the exception).
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
- [X] **Extract magic numbers to constants.hpp**
  - [X] `xz.cpp:36` - `64 * 1024` buffer size → `internal::DEFAULT_BUFFER_SIZE`
  - [X] `xz.cpp:100` - `16384` minimum buffer → `internal::MIN_DECOMP_BUFFER_SIZE`
  - [X] `zlib.cpp:53` - `data.size() * 4` initial buffer multiplier → `internal::DECOMP_BUFFER_MULTIPLIER_ZLIB`
  - [X] `zlib.cpp:61` - `10` retry count → `internal::MAX_DECOMP_RETRIES`
  - [X] Algorithm max levels (ZSTD_MAX_LEVEL, BROTLI_MAX_LEVEL, etc.)
  - [X] Helper functions MapLevel() and MapLevelZeroBased()
- [x] **Deduplicate buffer resizing logic** — every algorithm's streaming impl now shares the same `pending_append` / drain pattern, and the C++/Python bindings have a single drain loop in their stream classes.
- [X] **Remove `using namespace` from compress_utils_py.cpp** — file rewritten in Phase 4; no `using namespace` anywhere.
- [x] ~~Add allocation failure checks in C tests~~ — N/A; C tests rewritten in `tests/smoke_test.c` using `CHECK()` macros and use the library API not raw malloc.
- [x] ~~Replace hardcoded test values with named constants in `tests/smoke_test.c`~~ — N/A; `smoke_test.c` no longer exists (C tests are now split across `tests/test_*.c`).

## Build System Fixes

- [x] **Vendor upstream codec sources** (2026-07). Replaced the per-codec
  `ExternalProject_Add` (git fetch + configure at build time) with a curated,
  pre-configured tree under `third_party/`, driven by `third_party/manifest.json`
  (generated by `tools/vendor-codecs.py`) and built via `cmake/Vendor.cmake`.
  No network, no per-codec configure; builds are offline + reproducible and much
  faster to configure. C/C++/Python/WASM all migrated. Config portability solved
  with hand-authored headers (zlib `zconf.h`, snappy `config.h` +
  `snappy-stubs-public.h`) and a uniform `-D` set for liblzma. This is the shared
  substrate the Go/Rust/Zig/Swift bindings compile directly. See
  `third_party/VENDOR.md`.
- [X] Fix Windows build issues and re-add `windows-latest` to Github Actions workflows
  - [X] Build `compress-utils` and `compress-utils-static`
  - [X] Build `unit-tests` and `unit-tests-static`
  - [X] Fix `ctest`
  - [X] Build `compress-utils-c` and `compress-utils-c-static`
  - [X] Build `unit-tests-c` and `unit-tests-c-static`
  - [X] Build `xz`
- [X] Rename `compress-utils` to `compress-utils`
- [x] Ship a bundled self-contained static `.a`
- [x] Disable `ZSTD-LEGACY` & `ZSTD-MULTITHREADED` to slim the shared library — done via the vendored zstd defines (`ZSTD_LEGACY_SUPPORT=0`, `ZSTD_MULTITHREAD` dropped, `ZSTD_DISABLE_ASM`); also removes the pthread dependency so one build serves native + wasm.
- [ ] **Make the CMake build consumable** (`FetchContent` / `add_subdirectory` / `find_package`). This is the prerequisite for the Conan, vcpkg and Homebrew entries under Package Managers: each recipe is essentially `cmake --install` plus a Config file, so do this first.
  - [ ] Install to `GNUInstallDirs` (`CMAKE_INSTALL_LIBDIR` etc.) instead of the hardcoded `${CMAKE_SOURCE_DIR}/dist/c`, so a package manager's prefix / `DESTDIR` is honored. Keep `dist/c` as an opt-in layout for the release workflows that rely on it.
  - [ ] `install(EXPORT)` plus a `compress_utilsConfig.cmake` / `ConfigVersion.cmake` (`CMakePackageConfigHelpers`) for `find_package(compress_utils)`, and a pkg-config file.
  - [ ] Namespaced alias targets (`compress_utils::compress_utils` for shared, `compress_utils::static`) with the include directory on the interface.
  - [ ] Gate developer-only defaults on `PROJECT_IS_TOP_LEVEL`: tests, benchmarks, C++/Python/WASM bindings, the forced Debug build type and global `-flto` should not apply when consumed via `FetchContent`.
  - [ ] Make the static archive an easy path: today `compress_utils_static` only exists with `BUILD_STATIC_LIB`, and most C/C++ consumers want static.
  - [ ] Version without a `.git` directory: `FetchContent` and release tarballs fall back to the hardcoded `DEFAULT_VERSION` (0.11.1), which drifts. Add a `CU_VERSION` override or a version file written at release time.
  - [ ] Option to link system codecs (`find_package` for zlib, zstd, ...) instead of the vendored `third_party/` copies, so Conan/vcpkg recipes don't duplicate libraries consumers already have. Needed for distros too; same duplicate-symbol concern as the Rust static-lib limitation.
  - [ ] Document `FetchContent` and `find_package` usage in the root README.

## Optimizations

- [X] Support iterative builds in `cibuildwheel` (via separated Python binding project & shared core lib project)
- [X] Add source wheel distribution for unsupported Python wheel configurations
- [X] Split CI/CD pipelines hierarchically
- [X] Support free-threaded Python

## Documentation

- [X] **Document thread-safety guarantees** (documented in compress_utils.hpp and compress_utils_stream.hpp)
  - [X] Functional API is thread-safe (stateless)
  - [X] Compressor class requires external synchronization for shared instances
  - [X] Streaming classes are NOT thread-safe (documented in header)
- [X] **Document compression level mappings per algorithm** (documented in constants.hpp)
  - [X] ZSTD: 1-10 → 2-22 (via MapLevel with ZSTD_MAX_LEVEL=22)
  - [X] Brotli: 1-10 → 0-11 (via MapLevel with BROTLI_MAX_LEVEL=11)
  - [X] zlib: 1-10 → 1-9 (capped at ZLIB_MAX_LEVEL=9)
  - [X] XZ: 1-10 → 0-9 (via MapLevelZeroBased)
- [ ] **Add API reference documentation** (Doxygen or similar)
- [ ] **Add CHANGELOG.md** for tracking releases (part of the path to 1.0 above)

## New Features

- [X] Github Workflow for artifact publishing
- [X] **Streaming compression/decompression support** (Complete)
  - [X] Design streaming API (CompressStream/DecompressStream classes)
  - [X] Implement for each algorithm using native streaming APIs (ZSTD, Brotli, zlib, XZ)
  - [X] Add Python bindings for streaming (CompressStream/DecompressStream)
  - [X] Add C bindings for streaming (compress_stream_*/decompress_stream_* functions)
  - [X] Add streaming unit tests (C++, C, and Python)
  - [X] Fix move semantics tests for streaming API (was a test bug, not implementation bug)
- [X] Cross-language performance testbench
- [X] **Single-stream decoding** (`CU_DSTREAM_SINGLE`): stop at the end of the first frame/member/stream and hand back the input after it (`eof` / `unused`), in C, C++, Python, Rust, Go and WASM. Lets callers like zpickle read one message and carry on with what follows.
- [ ] Standalone CLI executable
- [ ] Multi-file input/output (archiving) via `zip` and `tar.*`
- [ ] Async/multi-threaded compression support

## Bindings (implementation, tooling, tests & ci/cd updates)

- [X] `c++` (Main Lib)
- [X] `c`
- [X] `go`
- [X] `js/ts` (WebAssembly via Emscripten)
- [X] `python` (3.11 - 3.15)
- [X] `rust`
- [ ] `cli` (standalone command-line tool) — deferred; if one new surface is added, this is the cheapest and most visible, but it comes after the "Next up" items.

Planned language bindings, in priority order (each new binding multiplies the maintenance matrix, so keep each one's tests and benchmark minimal, and check what the ecosystem already offers before starting). All consume the C ABI in `include/compress_utils.h` (30 `CU_API` symbols):

- [ ] `java` / `kotlin` — largest audience and a real gap (JVM compression libraries are mostly single-codec JNI wrappers). Use the Panama FFM API (Java 22+) with `jextract` over the header rather than JNI. The real work is Maven Central packaging with per-platform native libs in the jar. Kotlin consumes the Java artifact.
- [ ] `csharp` / `.NET` — P/Invoke over the C ABI; NuGet `runtimes/<rid>/native/` layout. Best reach for the effort after Java.
- [ ] `swift` — SwiftPM can consume a C target directly over the vendored sources (`module.modulemap` plus an idiomatic wrapper). Aimed at iOS/macOS users who want Brotli/zstd/lz4 in one package.
- [ ] `zig` — cheapest to add: `@cImport` plus `build.zig.zon` over the vendored sources, and the zig toolchain is already a WASM build dependency. Small audience; check what `std` already covers first.
- [ ] `ruby` — FFI gem over the shared library, or a native extension over the vendored sources (decide).
- [ ] `php` — FFI (`ext-ffi`) versus a C extension distributed via PECL/PIE (decide; FFI is often disabled on shared hosts).
- [ ] `lua` — C module over the vendored sources, packaged on LuaRocks (LuaJIT FFI as an alternative).
- [ ] `r` — `.Call` package over the vendored sources. Check CRAN's size and bundled-source policy before committing.

Not planned: Node.js native addon (N-API). The WASM binding covers Node and the browser, and Node's built-in zlib/brotli/zstd cover the common cases.

## Algorithms

See [docs/adding-an-algorithm.md](docs/adding-an-algorithm.md) for the
end-to-end checklist (C core → build → bindings → tests → benchmarks → docs).

- [X] `brotli`
- [X] `bzip2`
- [X] `gzip`
- [X] `lz4`
- [X] `snappy` — **switched from google/snappy (C++) to the pure-C port
  andikleen/snappy-c (2026-07)** so the whole release library is pure C (no
  libc++/libstdc++ in any binding — native shared/static, Rust, Go, WASM all
  verified). Wire format identical (byte-for-byte + both-direction interop).
  google/snappy is kept at `third_party/snappy-oracle` as the C++ differential
  test oracle only (`tests/test_snappy_oracle`), never shipped.
  `third_party/snappy/compat.h` is a hand-authored portable shim (C11 `_Generic`
  + MSVC/wasm fallbacks) replacing upstream's GCC-only kernel compat.h.
- [X] `xz/lzma`
- [X] `zlib`
- [X] `zstd`
- [ ] `snappy-framed` (deferred — add only on demand). *Decision (2026-07-08): support the raw Snappy block format only; do NOT add the framing (`.sz`) format now.*

## Package Managers

C/C++ package managers all depend on "Make the CMake build consumable" under Build System Fixes. Suggested order: vcpkg, Conan, Homebrew.

- [ ] `c` -> `vcpkg` — portfile plus the CMake Config file; the registry PR process is lighter than Conan Center's, so likely first.
- [ ] `c++` -> `vcpkg` — same package; adds the `compress_utils_cpp` INTERFACE target export.
- [ ] `c` -> `conan` — write a `conanfile.py` recipe in this repo first, then upstream it to conan-center-index (review takes weeks). Expose an option to use system codecs instead of the vendored ones.
- [ ] `c++` -> `conan` — same recipe as the C package, plus the `compress_utils_cpp` INTERFACE target export.
- [ ] `c` / `c++` -> `homebrew` — formula or own tap over a tagged release tarball; a tap avoids review.
- [ ] `c` / `c++` -> Linux distros (`apt`/`rpm`) — deferred until someone asks. Debian and Fedora policy forbids bundled zlib/zstd/etc., so this needs the system-codecs option and likely unbundling of the patched Snappy C port and the LZO decompressor.
- [X] `go` -> `pkg.go`
- [X] `js/ts` -> `npm`
- [X] `python` -> `pypi`
- [x] `rust` -> `cargo` — `build_and_test_rust.yml` publishes to crates.io on a
  `v*` tag (a tag-gated `publish` job in the same workflow, matching the WASM/
  Python pattern; verifies Cargo.toml == tag first) plus a `cargo publish
  --dry-run` package gate on every run. Packaged crate is 7.1 MiB / 1.7 MiB
  compressed (well under the 10 MiB limit). Published since 2026-07 (0.11.1 is live on crates.io).
- [ ] `java` / `kotlin` -> `maven` (Maven Central)
- [ ] `csharp` -> `nuget`
- [ ] `swift` -> SwiftPM
- [ ] `zig` -> `build.zig.zon` package
- [ ] `ruby` -> `rubygems`
- [ ] `php` -> `packagist` (FFI) or `pecl`/PIE (extension)
- [ ] `lua` -> `luarocks`
- [ ] `r` -> `cran`
- [ ] `cli-macos` -> `homebrew`
- [ ] `cli-linux` -> `apt`/`rpm`

