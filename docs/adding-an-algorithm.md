# Adding a new compression algorithm

This is the end-to-end checklist for adding a codec to compress-utils. The C
ABI in [`include/compress_utils.h`](../include/compress_utils.h) is the
canonical surface; every binding is a thin shim over it, so **most of the work
is in the C core and the build system** — the bindings are a handful of
one-line registrations each.

Snappy (added 2026-07) is used as the worked example throughout. Grep the tree
for `snappy` / `SNAPPY` to see every touch point in one commit.

## Mental model

```
include/compress_utils.h     cu_algorithm_t enum + public ABI (the contract)
src/algorithms/<algo>/<algo>.c   the vtable: one-shot + streaming, one file
src/registry.c               maps the enum value → the vtable
codec-versions.json          pins the upstream git URL + tag
tools/vendor-codecs.py       curation spec: which sources/headers/defines to vendor
third_party/<algo>/          the vendored, pre-configured upstream sources (committed)
third_party/manifest.json    generated: per-codec sources + include dirs + defines
algorithms/<algo>/CMakeLists.txt  one line: cu_add_vendored_codec(<algo>)
CMakeLists.txt (root)        INCLUDE_<ALGO> option + wiring
```

**Upstream sources are vendored, not fetched.** Instead of an
`ExternalProject_Add` that git-clones and configures each codec at build time,
the curated upstream sources live in `third_party/<algo>/` with a portable,
compile-time config (no configure step). `tools/vendor-codecs.py` produces that
tree from the pinned tags and `cmake/Vendor.cmake` builds `<algo>_library` from
the manifest. See [`third_party/VENDOR.md`](../third_party/VENDOR.md). This is
what lets the Go/Rust/Zig/Swift bindings compile the codecs directly with their
own toolchains — no CMake, no network.

A codec is a `cu_algorithm_vtbl_t` (see
[`src/algorithm_registry.h`](../src/algorithm_registry.h)): four one-shot
function pointers (`compress_bound`, `compress`, `decompress`,
`decompress_size_hint`) plus two streaming quartets
(`create`/`write`/`finish`/`destroy` for each direction). `registry.c` routes
the enum value to it; `compress_utils.c` dispatches every public call through
the vtable. Nothing else in the core knows the codec exists.

## Before you write code: three design questions

1. **Wire format.** Decide exactly what bytes you emit and pick the *one*
   format both the one-shot and streaming paths produce. The cross-API tests
   require stream output to decode via one-shot and vice versa, so they must be
   byte-compatible. Document it in the file header (as
   [`lz4.c`](../src/algorithms/lz4/lz4.c) and
   [`snappy.c`](../src/algorithms/snappy/snappy.c) do).

2. **Streaming shape.** Codecs with a native incremental API (zstd, zlib,
   brotli, xz, lz4) stream with bounded memory. Block-only codecs (Snappy's raw
   format) can't stream incrementally — buffer all input in `write`, run the
   one-shot codec in `finish`, and drain the result through the caller's buffer
   (see the `sn_out_drain` pattern in `snappy.c`). This holds the whole
   input/output in memory; note the caveat in the file header.

3. **Size hint.** If the wire format carries the decompressed size (zstd, lz4
   frames, xz, snappy's varint prefix), implement `decompress_size_hint` to
   return it — this lets one-shot `decompress` size the buffer in one pass.
   Otherwise return `CU_ERR_SIZE_UNKNOWN` (as zlib/bz2/brotli do) and the
   bindings fall back to streaming.

Also note: `level` is 1..10 at the ABI; map it to the codec's native range in a
`<algo>_native_level()` helper. Codecs with no levels (Snappy) accept and
ignore it.

## Step 1 — the C core

- [ ] **`codec-versions.json`** — add `"<algo>": { "url": ..., "tag": ... }`.
      Single source of truth for the upstream version.
- [ ] **`include/compress_utils.h`** — add `CU_ALGO_<ALGO> = <n>` to
      `cu_algorithm_t`. Append; never renumber existing values (ABI stability).
- [ ] **`src/algorithms/<algo>/<algo>.c`** — implement the vtable. Copy the
      closest existing codec (a native-streaming one like `zlib.c`, or the
      buffer-all `snappy.c` for block-only formats). Include the upstream header
      in the natural style (`#include <zstd.h>` — the vendored include dir is on
      the path). Export `const cu_algorithm_vtbl_t cu_<algo>_vtbl`. Guard the two
      direction halves with `#ifndef CU_OMIT_COMPRESS` / `#ifndef
      CU_OMIT_DECOMPRESS` — the WASM direction-split builds define these to drop
      a direction's code.
- [ ] **`src/registry.c`** — add an `extern` decl and a
      `case CU_ALGO_<ALGO>: return &cu_<algo>_vtbl;`, both under
      `#ifdef INCLUDE_<ALGO>`.
- [ ] **`tools/vendor-codecs.py`** — add a `SPECS["<algo>"]` entry: the source
      dirs/files to curate, header dirs, `include_dirs`, the portable `defines`,
      and `cxx`. Derive the exact compiled source set from upstream's build
      (`ar t` on the static archive is ground truth). If the upstream needs a
      generated config header (like zlib/snappy), hand-author a portable one and
      list it in `CONFIG_FILES` so the tool preserves it. See
      [`third_party/VENDOR.md`](../third_party/VENDOR.md).
- [ ] **Vendor it** — run `tools/vendor-codecs.py` (or `--from-checkout` to
      curate from an existing build). This populates `third_party/<algo>/` and
      `third_party/manifest.json`. Commit both.
- [ ] **`algorithms/<algo>/CMakeLists.txt`** — one line:
      `cu_add_vendored_codec(<algo>)` (add `PUBLIC_DEFINES <X>` for any
      header-visible define the vtable also needs, as xz does with
      `LZMA_API_STATIC`). Builds `<algo>_library` from the manifest.
- [ ] **`CMakeLists.txt` (root)** — add the `INCLUDE_<ALGO>` option, include it
      in the "no algorithms" guard, and add the `if(INCLUDE_<ALGO>)` block that
      does `add_subdirectory`, appends the `.c` to `CU_CORE_SOURCES`, appends
      `<algo>_library` to `CU_TARGET_LIBS`, and appends `INCLUDE_<ALGO>` to
      `CU_TARGET_DEFINITIONS`. (No `add_dependencies` — the codec lib is an
      ordinary in-tree target now.)

### If the upstream is C++ (like Snappy)

The static archive references the C++ runtime, but our library/tests link with
the C driver, which won't pull in libstdc++/libc++ automatically:

- In the root `CMakeLists.txt` `INCLUDE_<ALGO>` block, append the C++ stdlib to
  `CU_TARGET_LIBS` (`c++` on Apple, `stdc++` elsewhere; MSVC needs nothing).
  This propagates to the shared lib, tests, and fuzz targets via the OBJECT
  library's interface.
- In `bindings/wasm/CMakeLists.txt`, enable `CXX` on the `project()` and set
  `LINKER_LANGUAGE CXX` on the target for this algo so the zig toolchain links
  libc++ for `wasm32-wasi`.

### If the codec is a variant of an existing one (like gzip / zlib)

Some "codecs" are the same underlying library in a different wire format — gzip
is zlib's DEFLATE with the RFC 1952 wrapper (windowBits 31 vs 15). Don't fetch a
second copy of the upstream or duplicate the implementation:

- **Share the code.** Factor the common machinery into a header the two `.c`
  files include, parameterized by the differing knob. gzip/zlib do this via
  `src/algorithms/zlib/deflate_backend.h`: every streaming/drain function is
  shared `static`, and each codec's `<algo>.c` supplies a few one-line wrappers
  that bake in its windowBits plus a vtable. `gzip.c` includes it with
  `#include "../zlib/deflate_backend.h"`.
- **Share the vendored sources (native).** In the root `CMakeLists.txt`, build
  the base library once if *either* codec is enabled
  (`if(INCLUDE_ZLIB OR INCLUDE_GZIP)` → `add_subdirectory(algorithms/zlib)` →
  `zlib_library`) and link it from both. The variant contributes only its `.c`
  source + `INCLUDE_<ALGO>` define — no second `add_subdirectory`, and it needs
  no `third_party/` tree or manifest entry of its own.
- **WASM still needs its own `algorithms/<algo>/CMakeLists.txt`.** The per-algo
  wasm build configures one codec in isolation and links `${ALGO}_library`, so
  the variant needs a CMakeLists that produces that target. It builds from the
  base codec's vendored sources via the `MANIFEST_KEY` argument — see
  `algorithms/gzip/CMakeLists.txt`, which is just
  `cu_add_vendored_codec(gzip MANIFEST_KEY zlib)` — so it stays out of both
  `codec-versions.json` and `third_party/manifest.json`.
- **Don't double-count upstreams in docs.** A variant adds an *algorithm* but
  not an *upstream library* — bump algorithm counts, leave
  "N upstream libraries" / ACKNOWLEDGMENTS untouched.

## Step 2 — tests (C)

- [ ] **`tests/test_compress_utils.c`** — add `CU_ALGO_<ALGO>` to `ALL_ALGOS`.
      The suite then exercises one-shot round-trip, tight-buffer streaming, and
      cross-API round-trips for it automatically. `size_hint` assertions accept
      both `CU_OK` and `CU_ERR_SIZE_UNKNOWN`, so either choice is fine.

Build and run before touching bindings:

```bash
cmake -S . -B build -DBUILD_PYTHON_BINDINGS=OFF -DBUILD_CPP_BINDINGS=OFF
cmake --build build --target test_compress_utils -j && ctest --test-dir build
```

## Step 3 — bindings

- [ ] **C++** (`bindings/cpp/include/compress_utils.hpp`) — add
      `<Algo> = CU_ALGO_<ALGO>` to `enum class Algorithm`. Add it to the `ALL[]`
      array in `bindings/cpp/test/test_compress_utils.cpp`.
- [ ] **Python** (`bindings/python/compress_utils_py.cpp`) — add the string
      case in `parse_algorithm` and the `.value("<algo>", ...)` in the enum.
      The `.pyi` stub is auto-regenerated by `pybind11-stubgen` at build time.
      Update `bindings/python/API.md` and the `__init__.py` header comment.
- [ ] **WASM** (`bindings/wasm/`):
  - `host.cmake`: add `<algo>` to `CU_WASM_ALGOS_ALL`.
  - `CMakeLists.txt`: add `<algo>` to `_CU_VALID_ALGOS` (+ any per-codec flags).
  - `src/core/types.ts`: add to the `Algorithm` enum and the `AlgorithmName`
    union.
  - `src/algorithms/<algo>/{index,compress/index,decompress/index}.ts`: copy an
    existing algo's trio, swapping the enum, name, and `.wasm` URL.
  - `package.json`: add the three subpath exports (`./<algo>`,
    `./<algo>/compress`, `./<algo>/decompress`) and the keyword.
  - Tests: add to `tests/all.test.ts`, `tests/bundle.test.ts`
    (`ALL_ALGOS`, alias map, `BUDGET_KB`, `VARIANT_KB`),
    `tests-runtime/deno-smoke.ts`, and `tests-browser/{serve.mjs,browser.spec.ts}`.

## Step 4 — interop tests

- [ ] **`bindings/python/tests/test_interop.py`** — add a `_<algo>_ref()` that
      returns an *independent* reference implementation's
      `(compress, decompress)` and register it in `REFERENCES`. This is the
      test that proves wire-format compatibility with the rest of the ecosystem
      (it's what would have caught the legacy LZ4 framing bug). Add the
      dependency to the `test` extra in `pyproject.toml`. If the reference
      library ships wheels for every cibuildwheel target, also add it to
      `test-requires`; otherwise leave it out so the wheel matrix self-skips.
- [ ] **`tests/interop/cli_crosscheck.py`** — add an entry to `TOOLS` if a
      ubiquitous CLI reads the format. If not (raw zlib, raw Snappy block),
      leave a documented note explaining the exclusion.

## Step 5 — benchmarks

Add `<algo>` to each hardcoded list:

- [ ] `benchmarks/runner.py` (`ALL_ALGOS`)
- [ ] `benchmarks/drivers/c/bench.c` (`CODECS[]`)
- [ ] `benchmarks/drivers/python/bench_py.py` (`ALGOS`)
- [ ] `benchmarks/drivers/wasm/bench_wasm.mjs` (`ALGOS`)
- [ ] `benchmarks/plot_langs.py` (`ALGOS`)

The C driver compiles against the *installed* header, so
`cmake --install build` after building, then:

```bash
python3 benchmarks/runner.py --drivers c,python,wasm --algos <algo> \
    --levels 6 --samples 2 --warmup 1 --corpus smoke
```

## Step 6 — docs

- [ ] `README.md` (root): bump the `algorithms-N` badge, the "N algorithms"
      prose, the ASCII diagram's fan-out row, and add a row to the
      **Supported algorithms** table.
- [ ] `bindings/wasm/README.md`, `bindings/python/README.md`,
      `bindings/cpp/README.md`: description line + algorithm table (+ the wasm
      bundle-size table).
- [ ] `bindings/wasm/DEVELOPMENT.md`: status line, artifact-size table, and a
      per-codec build-quirks entry if the codec needed special handling.
- [ ] `TODO.md`: tick the algorithm off the Algorithms list.

## Verifying everything

```bash
# C + C++ + Python
cmake -S . -B build && cmake --build build -j && ctest --test-dir build

# WASM (needs zig, wasm-strip, wasm-opt on PATH)
cd bindings/wasm && npm run build && npx vitest run
```

All of the above pass on a clean checkout with the new codec added. If a codec
needs a system dependency at test time (a reference library, a CLI), it must
self-skip when absent so a bare checkout stays green.
