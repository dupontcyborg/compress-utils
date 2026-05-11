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
- [X] **Extract magic numbers to constants.hpp**
  - [X] `xz.cpp:36` - `64 * 1024` buffer size → `internal::DEFAULT_BUFFER_SIZE`
  - [X] `xz.cpp:100` - `16384` minimum buffer → `internal::MIN_DECOMP_BUFFER_SIZE`
  - [X] `zlib.cpp:53` - `data.size() * 4` initial buffer multiplier → `internal::DECOMP_BUFFER_MULTIPLIER_ZLIB`
  - [X] `zlib.cpp:61` - `10` retry count → `internal::MAX_DECOMP_RETRIES`
  - [X] Algorithm max levels (ZSTD_MAX_LEVEL, BROTLI_MAX_LEVEL, etc.)
  - [X] Helper functions MapLevel() and MapLevelZeroBased()
- [ ] **Deduplicate buffer resizing logic** - Each algorithm implements its own buffer-doubling loop; extract to shared utility
- [X] **Remove `using namespace` from compress_utils_py.cpp:7** - Fixed during streaming bindings implementation
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
- [ ] **Add CHANGELOG.md** for tracking releases

## New Features

- [X] Github Workflow for artifact publishing
- [X] **Streaming compression/decompression support** (Complete)
  - [X] Design streaming API (CompressStream/DecompressStream classes)
  - [X] Implement for each algorithm using native streaming APIs (ZSTD, Brotli, zlib, XZ)
  - [X] Add Python bindings for streaming (CompressStream/DecompressStream)
  - [X] Add C bindings for streaming (compress_stream_*/decompress_stream_* functions)
  - [X] Add streaming unit tests (C++, C, and Python)
  - [X] Fix move semantics tests for streaming API (was a test bug, not implementation bug)
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
- [X] `python` (3.10 - 3.14)
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

## Core Language Migration: C++ → C (decided 2026-05-10)

### Rationale

The underlying algorithm libraries (zstd, brotli, zlib, bz2, lz4, xz) are all C. The current C++ "core" is a thin veneer over C that adds an unstable ABI, libstdc++/libc++ runtime baggage, exception-translation cost at every binding boundary, and a double-hop (C-binding → C++ → C-algo-lib) for every non-C++ consumer. The binding roadmap (`go`, `java`, `rust`, `swift`, `js/ts`, `python`, `cli`) is a polyglot substrate — polyglot substrates ship as C. C++ gets demoted to a peer binding alongside the others.

### Build tooling decision

- **Keep CMake** as the canonical build. After C-ification it gets simpler, not more complex.
- **Adopt `zig cc` as the cross-compiler** for aarch64, musl, universal2, and (optionally) WASM. Drop-in clang frontend, no system deps, unlocks the "missing architectures" TODO.
- **WASM via Emscripten first**; revisit Zig→WASM only if Emscripten bloat becomes a real problem. Cost of switching is low once the core is C.
- **Do not migrate to Meson / Zig build / Bazel**. The win doesn't justify churning the ecosystem integration (cibuildwheel, vcpkg, find_package, MSVC).

### Migration plan (do in order — each phase ships independently)

#### Phase 0 — Lock the ABI contract

**Locked decisions (2026-05-10):**

- **Allocation model: caller-allocates with `cu_*_bound()` helpers.** No `cu_free` in the public ABI. The library never owns memory it hands back to the caller. `cu_decompress` returns `CU_ERR_BUF_TOO_SMALL` with `*out_len` set to the required size when the buffer is too small — caller resizes and retries. For unknown-size decompression that can't be probed, the API directs users to `cu_stream_t`. One way to do each thing.
- **Error model:** `int` return codes; thread-local last-error retrievable via `cu_last_error()` returning a struct or via `cu_strerror(code)`. No `errno`-style globals. No out-params for error info.
- **Streaming handle:** opaque `cu_stream_t*`. **Two separate types** — `cu_compress_stream_t*` and `cu_decompress_stream_t*` — so the type system prevents calling decompress operations on a compress stream. Both follow the same `create/write/finish/destroy` pattern.
- **No `Compressor` / `cu_compressor_t` in v1.0.** The current `Compressor` class is dropped; it's a wrapper around free functions with no cached state. A real context-caching `cu_compressor_t*` is deferred to v1.x and added only if benchmarks show context-creation cost matters for real workloads. Non-breaking addition later.
- **C++ binding** is just free functions in `namespace cu` plus `CompressStream`/`DecompressStream` RAII wrappers. No `Compressor` class.
- **Symbol prefix:** `cu_` for all functions, `CU_` for all macros/enums. Audit existing exports during Phase 1.
- **Versioning:** `CU_VERSION_MAJOR`/`MINOR`/`PATCH` macros + runtime `cu_version()`. ABI is unstable until v1.0.0 is tagged.

**Deliverables:**

- [ ] **Draft `include/compress_utils.h`** as the canonical core header. This is the spec — Phase 1 code conforms to it. Get sign-off on signatures before writing any implementation code.
- [ ] **Audit existing `bindings/c/compress_utils.h`** to catalog what's worth preserving (error codes, names) vs. what changes (allocation model, Compressor removal).
- [ ] **Add `set(CMAKE_EXPORT_COMPILE_COMMANDS ON)`** to `CMakeLists.txt` so clangd/IDEs/fuzzers can find symbols.

#### Phase 1 — Convert algorithm implementations to C ✅

Completed 2026-05-10. All six algorithms (zstd, zlib, brotli, bz2, lz4, xz) ported to C as `src/algorithms/<algo>/<algo>.c`, each exporting a `cu_<algo>_vtbl` of type `cu_algorithm_vtbl_t`. Smoke test (`tests/smoke_test.c`) exercises one-shot round-trip, size-hint probe, BUF_TOO_SMALL behavior, and streaming round-trip through a 256-byte buffer for every available algorithm.

Bugs fixed in the process:
- LZ4 wire-format unified to LZ4 frame format (both one-shot and streaming) — old C++ used incompatible raw-block one-shot vs LZ4F streaming.
- Streaming "buffer fills mid-write" data-loss bug fixed across all six algorithms via the BUF_TOO_SMALL + drain protocol.
- ZSTD `pledgedSrcSize` now set on compression so the size-hint probe works on this library's own output.
- LZ4 streaming uses internal in/out buffers to compose with arbitrary caller buffer sizes (LZ4F's `compressBound` is pessimistic).

Known limitations (deferred):
- XZ `cu_decompress_size_hint` returns `CU_ERR_SIZE_UNKNOWN`; proper implementation needs footer/index parsing. Tracked here.

#### Phase 2 — Unify dispatch behind a C `IAlgorithm` ✅

Landed alongside Phase 1: `src/algorithm_registry.h` defines `cu_algorithm_vtbl_t`, `src/registry.c` provides `cu_registry_lookup` with `#ifdef INCLUDE_<ALGO>`-gated cases, and `src/compress_utils.c` dispatches all public-ABI calls via the vtable. The old `algorithms_router.hpp` header-defined-function landmine and the 934-line streaming mega-switch in `compress_utils_stream.cpp` are dead code now (deletion happens in Phase 4 cleanup).

Adding a new algorithm = drop in `src/algorithms/<algo>/<algo>.c` exporting `cu_<algo>_vtbl`, add a `case CU_ALGO_X` to `registry.c`, add to CMakeLists. Three places, mechanical.

(Original detailed spec retained below for reference.)

- [ ] ~~Define `cu_algorithm_t` in `src/utils/algorithms.h`~~ — done as `cu_algorithm_vtbl_t` in `src/algorithm_registry.h`.
  ```c
  typedef struct {
      const char* name;
      int (*compress)(const uint8_t* in, size_t in_len, uint8_t* out, size_t* out_len, int level);
      int (*decompress)(const uint8_t* in, size_t in_len, uint8_t* out, size_t* out_len);
      cu_stream_t* (*create_compress_stream)(int level);
      cu_stream_t* (*create_decompress_stream)(void);
      int (*stream_write)(cu_stream_t* s, const uint8_t* in, size_t in_len, uint8_t* out, size_t* out_len);
      int (*stream_finish)(cu_stream_t* s, uint8_t* out, size_t* out_len);
      void (*stream_destroy)(cu_stream_t* s);
  } cu_algorithm_t;
  ```
- [ ] One `cu_algorithm_t` instance per algorithm, registered via `#ifdef INCLUDE_<ALGO>` in a single registry array.
- [ ] **Delete `src/utils/algorithms_router.hpp`** (the header-defined non-inline function landmine) and the 934-line streaming mega-switch in `src/compress_utils_stream.cpp`. Replaced by registry lookup.
- [ ] Adding a new algorithm becomes: one new `src/algorithms/<algo>/` directory, one registry entry. That's it.

#### Phase 3 — Fix wire-format and level-mapping bugs in C

(Moved here from the "Pre-WASM Polish" section below — natural to fix during the rewrite.)

- [ ] **LZ4 wire format**: standardize on LZ4 frame format (`LZ4F_*`) for both one-shot and streaming. Drop the homegrown `[4-byte size][raw block]` format. **This is a breaking change for any existing one-shot LZ4 consumer.** Bump major version, document in CHANGELOG.
- [ ] **ZSTD streaming-aware decompress**: one-shot `cu_zstd_decompress` falls back to `ZSTD_decompressStream` with a grow-loop when frame content size is unknown. Removes the spurious throw on streaming-produced ZSTD frames.
- [ ] **Centralized level mapping**: `cu_map_level(user_level, algo_max)` and `cu_map_level_zero_based` in `src/utils/levels.h`. Every algorithm calls the helpers — no hand-rolled formulas anywhere.
- [ ] **Decompression size caps**: configurable `cu_set_max_decompressed_size(bytes)` (default 1 GB) applied uniformly. Closes the ZSTD/bz2/xz allocation-bomb gap.
- [ ] **Add cross-API round-trip tests**: for every algorithm, `compress → stream_decompress` and `stream_compress+finish → decompress` must both round-trip.

#### Phase 4 — Reshape bindings

- [x] **C++ binding** (`bindings/cpp/`): header-only INTERFACE library at `bindings/cpp/include/compress_utils.hpp`. Free `cu::compress`/`cu::decompress`, RAII `CompressStream`/`DecompressStream`, `cu::Algorithm` enum, `cu::Error` exception with `.code()`. ~250 lines. Old C++ core (`src/compress_utils.{cpp,hpp}`, `src/compress_utils_func.{cpp,hpp}`, `src/compress_utils_stream.{cpp,hpp}`, all `.cpp/.hpp` under `src/algorithms/*/`, `src/utils/*.hpp`, `src/algorithms.hpp{,.in}`, `src/version.hpp.in`, `src/symbol_exports*.hpp`) deleted.
- [x] **C binding** (`bindings/c/`): deleted — the new `include/compress_utils.h` IS the C ABI; there's no separate shim layer.
- [x] **Python binding**: retargeted. `bindings/python/compress_utils_py.cpp` is a thin pybind11 wrapper over the C++ binding (which is itself a wrapper over the C ABI). `compressor()` factory dropped (it was a smell). `version()`, `is_available()`, `set_max_decompressed_size()` added. `CompressError` exception exported. Tests rewritten as a unittest module covering free fn / streaming / cross-API / garbage-rejection / introspection.
- [ ] **WASM binding**: discard the `claude/wasm-support-tree-shakeable` branch's per-algorithm C++ reimplementations. New WASM binding is a thin TS wrapper over the C ABI, with one shared dispatch — not six copy-pasted ones. Tree-shake by separate `.wasm` artifacts per algorithm, all built from the same C source. **Deferred — out of scope for this session.**

#### Phase 5 — Build & distribution

- [ ] **Update CMakeLists.txt** for C-as-primary: drop `set(CMAKE_CXX_STANDARD 20)` from the core (C++ standard now only needed for the C++ binding and tests). Set `CMAKE_C_STANDARD 11`.
- [ ] **Add `zig cc` toolchain file** at `cmake/toolchains/zig-cc.cmake` for cross-builds. Document usage in `README.md`.
- [ ] **CI: add aarch64-linux, universal2-macOS** to the matrix via `zig cc`. Closes the existing "missing architectures" TODO.
- [ ] **CI: build WASM once on Linux**, test on all OS runners. Closes the WASM-matrix-waste item.
- [ ] **Single static library**: collapse all algorithm static libs + their static deps into one `libcompress_utils.a` via `whole-archive`. (Already on the existing TODO — easier post-migration.)
- [ ] **CMake package config**: add `compress_utils-config.cmake` for `find_package(compress_utils)` support. (Already on the existing TODO.)

#### Phase 6 — Post-migration (unlocks the binding roadmap)

- [ ] **Fuzz harnesses** per algorithm, libFuzzer + sanitizers, integrated into CI. Trivial to wire up over a C ABI.
- [ ] **Go binding** via cgo. Direct consumer of the C ABI.
- [ ] **Rust binding** via `bindgen` over the C header.
- [ ] **Swift, Java, Zig** bindings — all consume the same C ABI.
- [ ] **Tag `v1.0.0`** once the C ABI has been stable through at least one minor-version cycle.

### What we are NOT doing

- Rewriting in Zig (overkill; smaller contributor pool; the wins come from `zig cc` as a tool, not Zig as a source language).
- Migrating off CMake (Meson/Bazel/Zig-build don't justify the ecosystem-integration loss).
- Maintaining the C++ core API as a public surface during/after migration. The C++ binding is the public C++ surface from v1.0 onward.

---

## Pre-WASM Polish (2026-05 code review)

### Shape change (do first — unblocks everything else)

- [ ] **Unify one-shot and streaming dispatch behind a single `IAlgorithm` interface**
  - Today: `src/utils/algorithms_router.hpp` holds a function-pointer vtable for one-shot; `src/compress_utils_stream.cpp` holds a 6-arm `#ifdef`-gated mega-switch repeated across ctor/Compress/Finish for both CompressStream and DecompressStream (~12 sites).
  - Target: one interface per algorithm exposing `Compress(span, level)`, `Decompress(span)`, `MakeCompressStream(level)`, `MakeDecompressStream()`. Register once. Adding a new algorithm = one new `src/algorithms/<algo>/` directory.
  - Forces a single answer to wire-format and level-mapping questions per algorithm (fixes several bugs below "for free").

### Correctness bugs (wire-format compatibility)

- [ ] **LZ4 one-shot and streaming produce incompatible wire formats**
  - `src/algorithms/lz4/lz4.cpp:57-61` writes `[4-byte LE original_size][raw LZ4 block]`.
  - `src/compress_utils_stream.cpp:316+` uses `LZ4F_*` (standard LZ4 frame format).
  - Decision needed: standardize on LZ4 frame format everywhere (recommended — interoperable with `lz4` CLI and other libs). Breaking change for any existing one-shot LZ4 consumers.
- [ ] **ZSTD streaming output cannot be decompressed via the one-shot API**
  - `compress_utils_stream.cpp:145` calls `ZSTD_initCStream` without `pledgedSrcSize`, so frames carry `ZSTD_CONTENTSIZE_UNKNOWN`.
  - `src/algorithms/zstd/zstd.cpp:54-56` throws on `ZSTD_CONTENTSIZE_UNKNOWN`.
  - Fix: one-shot `Decompress` should fall back to `ZSTD_decompressStream` with a grow-loop when content size is unknown. (Also fixes external ZSTD producers that don't embed content size.)
- [ ] **Add cross-API round-trip tests**: for every algorithm, `Compress → DecompressStream` and `CompressStream+Finish → Decompress` must both round-trip. Currently only stream↔stream and one-shot↔one-shot are tested (`tests/test_compress_utils_stream.cpp`).

### Correctness bugs (level mapping)

- [ ] **Streaming and one-shot use different LZ4 HC level mappings**
  - One-shot (`lz4.cpp:39`): `MapLevel(level-3, 12)` → for user level 10, HC level 9.
  - Streaming (`compress_utils_stream.cpp:312`): `(level-3)*12/7` → for user level 10, HC level 12.
  - Fix: single mapping function in `src/utils/constants.hpp`, called from both paths.
- [ ] **Streaming bz2/zlib/xz reimplement level math by hand** instead of calling `internal::MapLevel`. Same formula by coincidence; centralize via the helper. (`compress_utils_stream.cpp:171, 198, 211`.)

### Security / robustness

- [ ] **No upper bound on ZSTD decompressed allocation**
  - `src/algorithms/zstd/zstd.cpp:59` allocates a buffer sized to the frame's claimed content size with no cap. A crafted frame can request multi-GB. LZ4 has a 512 MB cap (`lz4.cpp:105`) — apply equivalent caps to ZSTD/bz2/xz, configurable via a build option or an API parameter.
- [ ] **Add a fuzz harness** for `Decompress(bytes, algorithm)` per algorithm. A compression library that parses untrusted input ships with zero fuzzing today. libFuzzer or AFL++ integrated into CI catches things sanitizers won't.

### Code quality

- [ ] **`GetCompressionFunctions` is a non-inline function defined in a header** (`src/utils/algorithms_router.hpp:34`). Currently saved by exactly one TU including it; a second includer will multi-define. Fix when refactoring for the `IAlgorithm` change above, or mark `inline` in the meantime.
- [ ] **Misleading closing comment** at `src/compress_utils_func.cpp:42` — `}  // namespace compress_utils` actually closes a function body, not the namespace.
- [ ] **`ValidateLevel` runs after `Impl` allocation** in `compress_utils_stream.cpp:134`. Move to first line so invalid input doesn't allocate a 64KB buffer.
- [ ] **Deduplicate per-algorithm grow-loops** (already on the TODO above) — easier after `IAlgorithm` lands since the buffer-resizing utility has one obvious home.

### Build / CI

- [ ] **`fetch-depth: 0` on all CI checkouts** so `cmake/GitVersion.cmake` doesn't silently fall back to `0.1.0`. Verify Python/C workflows match the WASM workflow on this.
- [ ] **WASM matrix waste**: the WASM build is platform-independent; CI rebuilds it on Linux/macOS/Windows. Build once on Linux, test the JS package on all three. (Applies to the WASM branch when merged.)

### WASM direction (gating for the next branch)

- [ ] **Build WASM on top of the C ABI in `bindings/c/`**, not the C++ core. The C surface is already stable and includes streaming (per TODO above). Whether Emscripten or Zig→WASM, the WASM layer should be a thin shim — not a reimplementation of the streaming layer per algorithm (see what the `claude/wasm-support-tree-shakeable` branch did wrong).
- [ ] **Decide Emscripten vs Zig→WASM**:
  - Emscripten: lower-friction (libc, malloc, exception ABI handled), reuses existing toolchain.
  - Zig→WASM: smaller output, no Emscripten runtime, but requires building C deps under Zig's clang. Worth it if you also want a Zig binding for non-WASM consumers; otherwise overkill.
- [ ] **Tree-shake budget as a test**, not just a smaller-than-multi assertion. Cap per-algorithm bundle size in CI so regressions actually fail.

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
