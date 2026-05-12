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
- [x] **Deduplicate buffer resizing logic** — every algorithm's streaming impl now shares the same `pending_append` / drain pattern, and the C++/Python bindings have a single drain loop in their stream classes.
- [X] **Remove `using namespace` from compress_utils_py.cpp** — file rewritten in Phase 4; no `using namespace` anywhere.
- [x] ~~Add allocation failure checks in C tests~~ — N/A; C tests rewritten in `tests/smoke_test.c` using `CHECK()` macros and use the library API not raw malloc.
- [ ] **Replace hardcoded test values with named constants** — minor cleanup in `tests/smoke_test.c`.

## Build System Fixes

- [X] Fix Windows build issues and re-add `windows-latest` to Github Actions workflows
  - [X] Build `compress-utils` and `compress-utils-static`
  - [X] Build `unit-tests` and `unit-tests-static`
  - [X] Fix `ctest`
  - [X] Build `compress-utils-c` and `compress-utils-c-static`
  - [X] Build `unit-tests-c` and `unit-tests-c-static`
  - [X] Build `xz`
- [X] Rename `compress-utils` to `compress-utils`
- [x] ~~Merge all static lib dependencies into `compress-utils-static*` libraries~~ — superseded. The public `compress_utils_static` target was removed (2026-05-11); the shared library and Python wheel are the supported deliverables and both are self-contained. Internal consumers (C test, fuzz, Python module) link an OBJECT library. Decided against shipping a bundled pure-static `.a` via `whole-archive` (2026-05-11) — not enough demand to justify.
  - [ ] (deferred) Disable `ZSTD-LEGACY` & `ZSTD-MULTITHREADED` to slim the shared library
- [ ] Add CMake package config for `find_package(compress_utils)` support

## Optimizations

- [X] Support iterative builds in `cibuildwheel` (via separated Python binding project & shared core lib project)
- [X] Add source wheel distribution for unsupported Python wheel configurations
- [X] Split CI/CD pipelines hierarchically
- [ ] Add missing architectures to CI/CD pipelines (`aarch64` on Linux & Windows; macOS already arm64-native)

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
- [X] `bzip2`
- [X] `lz4`
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

- [x] **Draft `include/compress_utils.h`** as the canonical core header. ✅
- [x] **Audit existing `bindings/c/compress_utils.h`**. ✅ (bz2/lz4 missing from the stale enum confirmed; deleted whole-cloth in Phase 4.)
- [x] **Add `set(CMAKE_EXPORT_COMPILE_COMMANDS ON)`** to `CMakeLists.txt`. ✅

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


#### Phase 3 — Fix wire-format and level-mapping bugs in C ✅

- [x] **LZ4 wire format**: unified to LZ4 frame format (`LZ4F_*`) for both one-shot and streaming.
- [x] **ZSTD streaming-aware decompress**: falls back to `ZSTD_decompressStream` with grow-loop when content size is unknown.
- [x] **Decompression size caps**: `cu_set_max_decompressed_size(bytes)` (default 1 GiB) applied across zstd/zlib/brotli/bz2/lz4/xz one-shot paths.
- [x] **Cross-API round-trip tests**: `tests/smoke_test.c::test_cross_api` exercises stream→one-shot and one-shot→stream for every available algorithm.
- [x] **libFuzzer harness** added (`tests/fuzz/fuzz_decompress.c`, `-DENABLE_FUZZ=ON`). Found and fixed an XZ OOM on first run.
- [ ] **Centralized level mapping**: `cu_map_level(user_level, algo_max)` helper. Each algorithm currently has its own `<algo>_native_level()` — they're per-algorithm by necessity (different ranges), but the shared cases (clamp to [1,N]) could live in `src/utils/levels.h`. Marginal win; deferred.

#### Phase 4 — Reshape bindings

- [x] **C++ binding** (`bindings/cpp/`): header-only INTERFACE library at `bindings/cpp/include/compress_utils.hpp`. Free `cu::compress`/`cu::decompress`, RAII `CompressStream`/`DecompressStream`, `cu::Algorithm` enum, `cu::Error` exception with `.code()`. ~250 lines. Old C++ core (`src/compress_utils.{cpp,hpp}`, `src/compress_utils_func.{cpp,hpp}`, `src/compress_utils_stream.{cpp,hpp}`, all `.cpp/.hpp` under `src/algorithms/*/`, `src/utils/*.hpp`, `src/algorithms.hpp{,.in}`, `src/version.hpp.in`, `src/symbol_exports*.hpp`) deleted.
- [x] **C binding** (`bindings/c/`): deleted — the new `include/compress_utils.h` IS the C ABI; there's no separate shim layer.
- [x] **Python binding**: retargeted. `bindings/python/compress_utils_py.cpp` is a thin pybind11 wrapper over the C++ binding (which is itself a wrapper over the C ABI). `compressor()` factory dropped (it was a smell). `version()`, `is_available()`, `set_max_decompressed_size()` added. `CompressError` exception exported. Tests rewritten as a unittest module covering free fn / streaming / cross-API / garbage-rejection / introspection.
- [ ] **WASM binding**: build on top of the C ABI, not a C++ reimplementation per algorithm.
  - **Keep** the old branch's per-algorithm `.wasm` artifacts and subpath exports (`compress-utils/zstd`, `compress-utils/brotli`, …) — that's correct for tree-shaking. Combined `.wasm` would be 1.5–2 MB; per-algo is 50–250 KB each.
  - **Drop** the per-algorithm TypeScript duplication (the old branch had `compress.ts`/`stream.ts`/`module.ts` copy-pasted six times, ~2000 LOC). All algorithms now share the exact same C ABI (one drain protocol, one error model, one size-hint shape), so the TS dispatcher is generic.
  - **Shape**: `src/core/dispatch.ts` (shared, ~150 lines) + `src/algorithms/<algo>/index.ts` (~15 lines, binds the dispatcher to its `.wasm` factory) + `src/algorithms/<algo>/wasm.generated.js` (emscripten output, per-algo).
  - **Toolchain**: Emscripten first (lowest-friction; reuses existing CMake integration). Zig→WASM is a fallback if Emscripten output is too big.
  - **CI**: build the `.wasm` artifacts once on Linux, test the JS package on all OS runners. **Deferred — out of scope for this session.**

#### Phase 5 — Build & distribution

- [x] **Update CMakeLists.txt** for C-as-primary: `CMAKE_C_STANDARD 11`, hidden visibility default, `CMAKE_EXPORT_COMPILE_COMMANDS ON`. (C++20 retained for the C++ binding tests and pybind11.)
- [ ] **Add `zig cc` toolchain file** at `cmake/toolchains/zig-cc.cmake` for cross-builds. Document usage in `README.md`.
- [ ] **CI: add aarch64-linux** to the matrix via `zig cc`. (macOS runners are arm64-native; no Universal2 needed in 2026.) Closes the existing "missing architectures" TODO.
- [ ] **CI: build WASM once on Linux**, test on all OS runners. Closes the WASM-matrix-waste item.
- [ ] **CMake package config**: add `compress_utils-config.cmake` for `find_package(compress_utils)` support. (Already on the existing TODO.)

#### Phase 6 — Post-migration (unlocks the binding roadmap)

- [x] **Fuzz harness** added (`tests/fuzz/fuzz_decompress.c`, `-DENABLE_FUZZ=ON`, libFuzzer + ASan + UBSan). Found and fixed an XZ memlimit OOM on first run. Per-algorithm corpora + CI integration still TODO.
- [ ] **Go binding** via cgo. Direct consumer of the C ABI.
- [ ] **Rust binding** via `bindgen` over the C header.
- [ ] **Swift, Java, Zig** bindings — all consume the same C ABI.
- [ ] **Tag `v1.0.0`** once the C ABI has been stable through at least one minor-version cycle.

### What we are NOT doing

- Rewriting in Zig (overkill; smaller contributor pool; the wins come from `zig cc` as a tool, not Zig as a source language).
- Migrating off CMake (Meson/Bazel/Zig-build don't justify the ecosystem-integration loss).
- Maintaining the C++ core API as a public surface during/after migration. The C++ binding is the public C++ surface from v1.0 onward.

---

## Documentation Plan (planned 2026-05-11)

Pre-v1.0 with no users, the highest-ROI docs are the ones that **survive API churn**: architecture, allocation model, drain protocol, per-algorithm wire-format notes. Hand-written API reference goes stale on the same commit that ships a change — defer to auto-generated Doxygen/Sphinx output at v1.0.

### Proposed structure

```
docs/
  README.md             Index, points to everything else.
  architecture.md       Why C-core. Layout: include/ + src/ + bindings/.
                        One diagram: C ABI is the substrate, every binding
                        is a thin shim. ~100 lines.
  c-abi.md              Narrative contract: cu_status_t codes, caller-
                        allocates allocation model + cu_*_bound,
                        BUF_TOO_SMALL drain protocol, when to use
                        cu_decompress_size_hint, thread safety. ~250 lines.
                        Targets new binding authors and serious C consumers.
  algorithms.md         Per-algorithm table: wire format produced,
                        size-hint capability, level mapping, known
                        limitations (XZ size_hint TODO, brotli quality
                        special-case at user level 10, etc.). ~150 lines.
  changelog.md          CHANGELOG starting with the 2026-05 C-core
                        migration.

bindings/cpp/README.md  Install + small example + link to docs/c-abi.md.
bindings/python/README.md  pip install + example + link.
README.md (root)        Project overview, "pick your language" table,
                        pointer at docs/ and per-binding READMEs.
                        REWRITE — current root README still references
                        the old C++ core.
```

### Explicit non-goals (defer to v1.0 doc pass)

- Hand-written API reference per function. Doxygen for C/C++, Sphinx autodoc for Python will do better and stay current automatically.
- Tutorials. Treats users as the audience, of which we have zero.
- A static doc site (ReadTheDocs / Sphinx output / mkdocs). GitHub-rendered Markdown is sufficient until there's traffic.

### Tier-1 priorities (do first)

- [ ] **Rewrite root `README.md`.** The current one likely still describes the C++ core. This is the highest-bounce-rate doc — fix first.
- [ ] **Write `docs/architecture.md`.** C-core rationale, project layout, one ASCII diagram of how bindings stack on the C ABI.
- [ ] **Write `docs/algorithms.md`.** Per-algorithm table covering wire format, size-hint behavior, level mapping (user 1–10 → native), known caveats, decompression-size-cap default. Should make "should I pick zstd or brotli for X?" a 60-second question.

### Tier-2 (do when an external binding author or serious C user shows up)

- [ ] **Write `docs/c-abi.md`.** Narrative companion to the contract in `include/compress_utils.h`. Worked examples for the drain protocol, size-hint probe pattern, thread-local last-error semantics.
- [ ] **Trim per-binding READMEs** to install + 10-line example + link to docs/.
- [ ] **Stub `docs/changelog.md`** with the 2026-05 migration as v0.1.0 entry.

### Tier-3 (v1.0 or later)

- [ ] Doxygen config for C + C++ headers; generate `dist/docs/c/` and `dist/docs/cpp/` at install time.
- [ ] Sphinx config for the Python binding; `autodoc` against the pybind11 module.
- [ ] CI step that builds the docs and uploads to GitHub Pages on tag pushes.

### Two design decisions baked in

1. **`include/compress_utils.h` stays the canonical contract.** Header-local comments are what IDE tooltips show. `docs/c-abi.md` is the narrative — worked examples, diagrams, "why" — and references back to the header for the exact spec. Two sources of truth is OK when one is "what" and the other is "why."
2. **Install instructions in binding READMEs reflect reality.** Until we publish to PyPI / Conan / Maven / etc., the README says "build from source via cmake" with the exact command. Avoid writing aspirational `pip install compress-utils` instructions before the package exists.

---

## Canonical-Compressor Interop Tests (planned 2026-05-11)

The existing test suites (`test_compress_utils.c`, `test_compress_utils_cpp.cpp`, `test_compress_utils.py`) prove self-consistency: our compress output decodes with our decompress, and across our one-shot ↔ streaming APIs. But they do **not** prove that our output is a valid `.zst` / `.gz` / `.lz4` / etc. file in *anyone else's* tools, nor that we can decompress what other tools produce.

This is the gap that let the legacy LZ4 wire-format bug live for so long: it round-tripped fine against itself, but no test ever fed its output to the canonical `lz4` CLI or `python -m lz4` to confirm it was a real LZ4 frame. We should add this validation for every algorithm in every language binding.

### What "canonical" means per algorithm

The reference implementation we round-trip against:

| Algorithm | Canonical reference                         | Format we produce |
|-----------|---------------------------------------------|-------------------|
| zstd      | `python -m zstandard` (libzstd binding)     | ZSTD frame with content size |
| brotli    | `python -m brotli` / `npm:brotli`           | raw Brotli stream |
| zlib      | Python `zlib` (stdlib)                      | zlib wrapper (RFC 1950) |
| bz2       | Python `bz2` (stdlib)                       | bzip2 stream |
| lz4       | `python -m lz4.frame` / `lz4` CLI           | LZ4 frame (`.lz4`) with content checksum |
| xz / lzma | Python `lzma` (stdlib)                      | XZ stream with CRC64 |

The CLI binaries (`zstd`, `xz`, `lz4`, `brotli`, `gzip`, `bzip2`) provide a second independent validation channel — useful as a backstop and easy to run in CI.

### Test matrix per language

For each `(language, algorithm)` pair, run:

1. **Outbound interop**: `cu_compress(input) → canonical.decompress() == input`
2. **Inbound interop**: `canonical.compress(input) → cu_decompress() == input`

Both directions matter. Outbound proves we produce valid output; inbound proves we accept inputs we didn't make. (Inbound is where bugs like "ZSTD frame without `pledgedSrcSize` is rejected" show up.)

### Per-binding implementation

- **Python** (`bindings/python/tests/test_interop.py`):
  - Use stdlib `zlib` / `bz2` / `lzma` for those three (no extra deps).
  - Add `zstandard`, `brotli`, `lz4` to test extras in `pyproject.toml`.
  - One parameterized `unittest` / `pytest` test per algorithm, both directions.

- **C++** (`bindings/cpp/test/test_interop.cpp`):
  - Already links against the underlying codec libraries (zstd, brotli, etc.) via `compress_utils_static`. Call those C APIs *directly* in the test alongside `cu::compress` and compare.
  - This is essentially "do we wrap the codec correctly?" which is a more rigorous check than the Python version (no second binding layer in the middle).

- **C** (`tests/test_interop.c`):
  - Same approach as C++ — call the underlying codec libraries' C APIs directly. Lives alongside `test_compress_utils.c`. Most thorough check because there's no language indirection.

- **WASM/JS** (when that binding lands):
  - Browser `DecompressionStream` for gzip/deflate (built-in, no deps).
  - `fflate`, `@bokuweb/zstd-wasm`, `lz4js` npm packages for the others. Pick the most-downloaded canonical per algo.

### CLI cross-check (separate from per-binding tests)

A standalone shell test that:
1. Compresses a fixture file with our library (via the C smoke test or a CLI we'll eventually ship).
2. Decompresses with the system CLI binary (`zstd -d`, `xz -d`, etc.).
3. Diffs against the original.
4. Reverse direction: system CLI compresses, our library decompresses.

Gated on the CLI binaries being available in the runner; skipped on systems missing them.

### CI implications

- Python interop deps go into a `test` extra in `pyproject.toml` — only installed when running tests, not for production wheels.
- The PR workflow installs system codec binaries on Ubuntu (`apt install zstd xz-utils lz4 brotli` — all present in `ubuntu-latest` already). macOS via Homebrew. Windows via chocolatey or vcpkg.
- Expect this to add 10–30s to CI per binding. Worth it.

### What this catches that current tests don't

- Wire-format divergence (the LZ4 class of bug — different headers, different magic numbers, different optional flags).
- Off-by-one in size fields, endianness mistakes in length prefixes.
- Checksum mismatches (zstd content checksum, lz4 frame content checksum, xz CRC64).
- "We accept input only from ourselves" bugs (e.g., ZSTD without `pledgedSrcSize` once worked one direction but not the other in the legacy code).
- Compatibility with future versions of the underlying codecs (when an upstream lib updates, this catches our wrapper falling out of sync).

### Open question

Should the interop tests be **mandatory in PR CI** or **a separate nightly job**? Mandatory makes them a release gate; nightly keeps PR latency low. I'd lean mandatory — they're fast — but worth deciding when the work lands.

- [ ] **Implement interop test suites per binding** (C, C++, Python now; WASM when it exists).
- [ ] **Add the CLI cross-check** as a separate workflow step or shell script.
- [ ] **Decide PR-gate vs. nightly** placement.

---

## WASM Binding Plan (decided 2026-05-11)

The earlier `claude/wasm-support-tree-shakeable` branch got *one thing right and one thing wrong*. This section captures the corrected approach so the next person picking up WASM work doesn't relitigate the design.

### What to keep from the prior branch

- **Per-algorithm `.wasm` artifacts and subpath exports.** Combined `.wasm` is 1.5–2 MB; per-algo is 50–250 KB each. A page importing only zstd should download only zstd.
- **Package layout with subpath exports**:
  ```jsonc
  "exports": {
    "./zstd":   { "import": "./dist/algorithms/zstd/index.js"   },
    "./brotli": { "import": "./dist/algorithms/brotli/index.js" },
    "./zlib":   { "import": "./dist/algorithms/zlib/index.js"   },
    "./bz2":    { "import": "./dist/algorithms/bz2/index.js"    },
    "./lz4":    { "import": "./dist/algorithms/lz4/index.js"    },
    "./xz":     { "import": "./dist/algorithms/xz/index.js"     }
  }
  ```
- **`"sideEffects": false`** + ESM-only so bundlers can tree-shake aggressively.

### What to drop from the prior branch

- **Per-algorithm TypeScript reimplementations.** The old branch had `compress.ts`, `stream.ts`, `module.ts` copy-pasted six times (~2000 LOC of mechanical search-replace). The C ABI now provides a single uniform contract — one drain protocol, one error model, one size-hint shape — so the TS layer has nothing algorithm-specific to do.
- **Per-algorithm C++ binding code.** The prior branch reimplemented streaming, level mapping, and error translation in C++ *inside the WASM build*, divergent from the main C++ core. That's exactly the duplication the C migration was supposed to kill. The WASM `.wasm` files are now compiled directly from `src/algorithms/<algo>/<algo>.c` — same source as native builds.
- **Homegrown LZ4 wire format.** The prior branch's LZ4 WASM module shipped raw blocks; the new C core uses LZ4 frame format. No special-casing needed.

### Target directory shape

```
bindings/wasm/
  src/
    core/
      dispatch.ts            shared ~150 LOC. Wraps the cu_* ABI:
                             one-shot compress/decompress, stream classes,
                             drain loop, BUF_TOO_SMALL handling, error
                             translation to JS exceptions.
      types.ts               CompressOptions, CompressError, etc.
      loader.ts              Module factory cache; preload helper.
    algorithms/
      zstd/
        index.ts             ~15 LOC. Imports dispatch + the algo's wasm
                             factory; exports compress/decompress/streams
                             bound to that .wasm module.
        wasm.generated.js    emscripten output for zstd only.
      brotli/  …same shape, different .wasm…
      zlib/    …
      bz2/     …
      lz4/     …
      xz/      …
  scripts/
    build-wasm.sh            invokes emcc 6 times, one per algorithm,
                             linking only that algorithm's vtable + the
                             core dispatcher.
  tests/
    integration.test.ts      round-trip per algorithm
    treeshake.test.ts        bundle-size budget per subpath
  CMakeLists.txt             (or build.zig if we switch toolchains)
  package.json
```

### Build

Each algorithm's `.wasm` links:
- `src/compress_utils.c`           (dispatcher — needed for `cu_*` entry points)
- `src/registry.c`                 (with only one `INCLUDE_<ALGO>` defined)
- `src/algorithms/<algo>/<algo>.c` (that algorithm's vtable)
- the upstream algorithm library (zstd, brotli, etc.)

Six emcc invocations, each producing a self-contained `.wasm` for one algorithm.

### Toolchain

- **Emscripten first.** Lowest friction: existing CMake integration via `emcmake`, well-trodden path, handles libc/malloc/exception ABI for free.
- **Zig→WASM as a fallback** only if Emscripten output is too big. Re-evaluate after measuring first cut. Switching is cheap because the C source doesn't change.

### Tree-shaking story

- Subpath imports + `sideEffects: false` + per-algo `.wasm` = bundlers ship only what the consumer imports.
- The shared `dispatch.ts` is small (~150 LOC). It gets bundled exactly once per imported algorithm, but since it's the same module, smart bundlers dedupe across imports anyway.
- **Bundle size budget as a test.** Add a `treeshake.test.ts` that asserts per-algo bundles stay under specified KB caps (e.g., zstd subpath ≤ 280 KB including `.wasm`). Regressions fail CI, not just produce a warning.

### CI

- Build the six `.wasm` files **once on Linux** (they're platform-independent).
- Test the JS package on all OS runners (Node + Playwright for browsers).
- This closes the "WASM matrix waste" item — the prior branch rebuilt WASM on every OS for no reason.

### What this unlocks

Once shipped, the React/Vue/server-side-JS use cases all work:
- `import { compress } from 'compress-utils/zstd'` in any modern bundler
- Cloudflare Workers / Deno / Bun all use the same package
- A future Rust→WASM consumer could even reuse the same `.wasm` artifacts via WASI

---

## Pre-WASM Polish (2026-05 code review) — superseded by Phases 1–4 above

All the wire-format, level-mapping, dispatch-shape, and security items from the original code review were resolved as part of the C-migration (Phases 1–4). Surviving items that *aren't* already covered above:

- [x] **`fetch-depth: 0` on all CI checkouts** — verified: `pr_build_and_test.yml`, `build_and_test_c_cpp.yml`, `build_and_test_python.yml` all set it.
- [x] **Rewrite release CI workflows for the new layout** (2026-05-11). `build_and_test_c_cpp.yml` now produces a single per-OS `compress-utils-${OS}[-${VERSION}].tar.gz` (zip on Windows) containing both `dist/c/` and `dist/cpp/`, with a separate `release` job that downloads all OS artifacts and attaches them to the GitHub Release on tag. The `combine` cross-OS merge step is gone. `build_and_test_python.yml` was already aligned with the current layout — left as-is.
- [ ] **Decide Emscripten vs Zig→WASM** when picking up the WASM binding (Phase 4 leftover).

## Package Managers

- [ ] `c` -> `conan` (low priority — publish the C library to Conan Center for C consumers; requires writing a `conanfile.py` recipe + registering with conan-center-index)
- [ ] `c++` -> `conan` (low priority — same as above for the C++ header-only binding; shares most of the recipe with the C package, just adds the `compress_utils_cpp` INTERFACE target export)
- [ ] `go` -> `pkg.go`
- [ ] `java` -> `maven`
- [ ] `js/ts` -> `npm`
- [X] `python` -> `pypi`
- [ ] `rust` -> `cargo`
- [ ] `swift` -> ?
- [ ] `cli-macos` -> `homebrew`
- [ ] `cli-linux` -> `apt`/`rpm`
