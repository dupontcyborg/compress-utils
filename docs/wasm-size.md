# WASM module size — findings & reduction plan

_Investigation 2026-06-09. Measured against the shipped (post `wasm-strip` +
`wasm-opt -O3`) artifacts in `bindings/wasm/tests-browser/.serve/`._

## Current shipped sizes

| Module   | Size   | Total exports | `cu_*` exports | Non-`cu_` exports |
|----------|-------:|--------------:|---------------:|------------------:|
| zlib     |  78 KB |           100 |             42 |                58 |
| bz2      |  92 KB |            96 |             42 |                54 |
| xz       | 132 KB |           238 |             42 |               196 |
| lz4      | 133 KB |           289 |             42 |               247 |
| zstd     | 531 KB |           691 |             42 |               649 |
| brotli   | 713 KB |           150 |             42 |               108 |

The loader (`bindings/wasm/src/core/loader.ts`) calls **~20** entry points. The
42 exported `cu_*` symbols are already more than we need; the **non-`cu_`
exports are upstream codec API that nothing in our JS ever calls.**

## Root cause: `--export-dynamic` defeats dead-code elimination

`cmake/toolchains/zig-wasm.cmake` links every module with:

```cmake
set(CMAKE_EXE_LINKER_FLAGS_INIT
    "-target ${CU_WASM_TARGET} -mexec-model=reactor -Wl,--export-dynamic")
```

`--export-dynamic` marks **every default-visibility symbol as an export**, and
an exported function is a **GC root** in `wasm-ld`. The upstream codecs
(`libzstd.a`, brotli, …) declare their public API with default visibility, so
all of it becomes rooted. `wasm-opt` then cannot strip an exported function
either, so the post-link optimize pass doesn't recover it.

Net effect: the entire upstream public API and its transitive closure ship in
every module, regardless of what we call. For zstd that's **649 dead-rooted
`ZSTD_*` functions** — the bulk of its 531 KB.

This is a pure-overhead defect: removing it changes **zero** executed code
paths, so it carries **no performance risk**.

## Per-codec breakdown — the win is not uniform

- **zstd (531 KB): export-bloat-bound.** 649 dead-rooted symbols. Pruning
  exports is the dominant lever here.
- **xz (132 KB), lz4 (133 KB): meaningfully export-bound** (196 / 247 extra
  exports relative to small modules).
- **zlib (78 KB), bz2 (92 KB): modestly export-bound**, already small.
- **brotli (713 KB): dictionary-bound, _not_ export-bound.** Only 108 extra
  exports. Most of brotli's size is its built-in static dictionary (~120 KB)
  plus the q10/q11 encoder hash tables. The dictionary is **part of the wire
  format and cannot be dropped without breaking interop.** Export pruning still
  helps (108 rooted functions + closure), but brotli will stay the largest
  module.

## Reduction plan (ranked by win ÷ risk)

| # | Change | Expected effect | Perf risk |
|---|--------|-----------------|-----------|
| 1 | **Stop using `--export-dynamic`.** Export only the loader-facing symbols (`cu_*` ABI + `cu_alloc`/`cu_free` + `_initialize`) via an explicit `-Wl,--export=` list, or `__attribute__((export_name(...)))` on the `CU_API` functions (the pattern `src/wasm_runtime.c` already uses for `cu_alloc`/`cu_free`). Lets `wasm-ld` GC + `wasm-opt` DCE strip the unreferenced upstream API. | Large for zstd (~50–65%); meaningful for xz/lz4; modest for zlib/bz2; smaller for brotli | **None** — identical code paths |
| 2 | zstd: `ZSTD_LEGACY_SUPPORT=0` (we never read pre-v0.8 frames) + `ZSTD_BUILD_DICTBUILDER=OFF` (no dictionary API exposed) + `ZSTD_LIB_MINIFY`. Set in `algorithms/zstd/CMakeLists.txt` (cache currently shows `LEGACY_SUPPORT=ON` level 5, `BUILD_DICTBUILDER=ON`). | Meaningful on zstd | None — unused features |
| 3 | Final `wasm-opt -Oz` pass (after #1, DCE actually bites). | Small–moderate | None |
| 4 | Compile the codec libs with `-Oz`/`-Os` instead of `-O3`. | Small | **Real** — can cost throughput; gate behind the benchmark suite |
| 5 | Target `wasm32-freestanding` + a tiny `malloc` instead of `wasm32-wasi` — removes libc startup and the WASI import shim entirely. | Moderate fixed per-module saving | Medium effort/risk; defer |

**Sequencing.** #1 is the unlock; until exports are pruned, #2–#4 are largely
masked. #1 + #2 + #3 should beat the TODO's "~half of current for zstd and
brotli" target with no performance risk, leaving #4/#5 as optional, benchmark-
gated extras.

## Validation gate

#1–#3 are perf-neutral by construction, but #4–#5 are not, and future codec
version bumps need a standing guard. See `benchmarks/` for the suite:

- **Module size** is deterministic → hard CI gate with committed baselines
  (extends `bindings/wasm/tests/bundle.test.ts`).
- **Compression ratio** is algorithm-determined → cross-language invariant.
- **Throughput** is noisy → trend + large-regression threshold on dedicated
  hardware only, never gated on shared CI runners.
