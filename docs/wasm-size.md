# WASM module size — findings & reduction plan

_Investigation 2026-06-09. Measured against the shipped (post `wasm-strip` +
`wasm-opt -O3`) artifacts in `bindings/wasm/tests-browser/.serve/`._

## Shipped sizes — pre-optimization (2026-06-09 baseline)

_These are the numbers that motivated the plan. For post-optimization sizes see
**Results** at the end._

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

> **Correction (measured 2026-06-12).** The original analysis below conflated
> *dead-rooted exports* with *dead code*. Removing an export root lets wasm-ld
> GC the unused API **wrapper** and any code reachable only through it — but the
> shared compression/decompression core (`ZSTD_compress`/`decompress` and the
> FSE/Huffman/match-finder machinery they call) stays referenced by our `cu_*`
> entry points and does **not** go away. So the export count is a weak predictor
> of the recoverable bytes. Actuals: zstd **−24%** (not the ~50–65% projected
> below), lz4 −20%, the rest <1%. See **Results** at the end.

- **zstd (531 KB): export-bloat-bound.** 649 dead-rooted symbols. Pruning
  exports is the dominant lever here. _(Actual: −24%. The exports were real
  dead weight, but most of zstd's bytes are the live compression core, which
  pruning can't touch. zstd.wasm is ~96% code; after pruning it's code-bound,
  not export-bound.)_
- **xz (132 KB), lz4 (133 KB): meaningfully export-bound** (196 / 247 extra
  exports relative to small modules). _(Actual: lz4 −20%, xz <1%.)_
- **zlib (78 KB), bz2 (92 KB): modestly export-bound**, already small.
  _(Actual: <1% each.)_
- **brotli (713 KB): dictionary-bound, _not_ export-bound.** Only 108 extra
  exports. _(Correction: brotli is **data-bound** — 403 KB data vs 325 KB code
  — but the dictionary is only ~123 KB of it. The other ~192 KB is a **new
  v1.2.0 encoder-only static-dictionary LUT** (`kStaticDictionaryWords` /
  `Buckets`) that brotli embeds as const data by default
  (`BROTLI_STATIC_INIT=NONE`). That LUT — not the dictionary — is the dominant
  removable chunk, and it's recoverable by computing it at init instead. The
  ~123 KB dictionary is wire-format and stays. Export pruning barely moves
  brotli; static-init does. See #6 in the plan.)_

## Reduction plan (ranked by win ÷ risk)

| # | Change | Expected effect | Perf risk | Status / actual |
|---|--------|-----------------|-----------|-----------------|
| 1 | **Stop using `--export-dynamic`.** Export only the loader-facing symbols (`cu_*` ABI + `cu_alloc`/`cu_free` + `_initialize`) via an explicit `-Wl,--export=` list (in `bindings/wasm/CMakeLists.txt`). Lets `wasm-ld` GC + `wasm-opt` DCE strip the unreferenced upstream API. | Projected large for zstd (~50–65%) | **None** — identical code paths | ✅ **Done.** zstd exports 691→21. Drove most of zstd −24% / lz4 −20%. Projection was too high (see correction above). |
| 2 | zstd: `ZSTD_LEGACY_SUPPORT=OFF` + `ZSTD_BUILD_DICTBUILDER=OFF` (no dict API exposed). | Meaningful on zstd | None — unused features | ✅ **Done.** Marginal on top of #1's DCE (the legacy/dict code was already unreferenced and GC'd), but correct to set. |
| 3 | Final `wasm-opt -Oz` pass (after #1, DCE actually bites). | Small–moderate | None | ✅ **Done** (was `-O3`). |
| 4 | Compile the codec libs with `-Oz`/`-Os`; or zstd `ZSTD_LIB_MINIFY`. | Small | **Real** — can cost throughput; gate behind the benchmark suite | ❌ **Measured, rejected (zstd).** `ZSTD_LIB_MINIFY` = byte-identical no-op (subsumed by #1+#3 DCE). `-Oz` on the zstd lib = −11% size (416→370 KB) but a clean same-session A/B showed decode **−3.8% median / −8.8% worst** and high-level compress down to **−17%** — fails the gate. Not worth 45 KB on a module already under budget. |
| 5 | Target `wasm32-freestanding` + a tiny `malloc` instead of `wasm32-wasi` — removes libc startup and the WASI import shim entirely. | Moderate fixed per-module saving | Medium effort/risk; defer | ⬜ Deferred. |
| 6 | **brotli `BROTLI_STATIC_INIT=EARLY`** (wasm-only, `algorithms/brotli/CMakeLists.txt`). Compute the v1.2.0 encoder LUT at `_initialize` instead of embedding it. | — (found during the brotli deep-dive) | **None** measured — one-time init at load, ratio unchanged | ✅ **Done. brotli −34% (714→471 KB)** — the single biggest lever. |

**Sequencing (as it played out).** #1 was the unlock. #1+#2+#3 landed zstd −24%,
lz4 −20%, total ~1.69→1.49 MB — short of the projected "~half", because the
projection over-counted (exports ≠ code). The decisive win came from **#6**, a
brotli-specific lever not in the original plan: total ~1.49→**1.26 MB (−25%
overall)**. #4 (`-Oz` on codec libs) and #5 remain optional, benchmark-gated.

## Validation gate

#1–#3 are perf-neutral by construction, but #4–#5 are not, and future codec
version bumps need a standing guard. See `benchmarks/` for the suite:

- **Module size** is deterministic → hard CI gate with committed baselines
  (extends `bindings/wasm/tests/bundle.test.ts`).
- **Compression ratio** is algorithm-determined → cross-language invariant.
- **Throughput** is noisy → trend + large-regression threshold on dedicated
  hardware only, never gated on shared CI runners.

## Results (2026-06-12)

Applied #1–#3 + #6. Validated on silesia-mini (AppleM4Max) vs `baseline-wasm`:
compression **ratio identical** on all 216 specs, no throughput regression
(the few flags were thermal-tail noise, gone on a fresh re-measure), 29/29 wasm
tests pass. Budgets in `bundle.test.ts` tightened to the new sizes.

| Module | Before | After | Δ | What moved it |
|--------|-------:|------:|---:|---------------|
| zlib   |  78 KB |  78 KB |   — | — |
| bz2    |  92 KB |  92 KB |   — | — |
| xz     | 133 KB | 132 KB |  −1% | #1 |
| lz4    | 134 KB | 108 KB | −20% | #1 (export pruning) |
| zstd   | 535 KB | 406 KB | −24% | #1 + #3 |
| brotli | 714 KB | 471 KB | −34% | #6 (EARLY static-init) |
| **total** | **1.69 MB** | **1.26 MB** | **−25%** | |

**Two lessons for next time.** (1) Export count is a poor size predictor —
measure the *code* that's reachable only through dead exports, not the export
list. (2) The biggest single win (#6) wasn't in the original plan; it surfaced
only from a per-module section breakdown (`wasm-objdump -h`). Lead with that
breakdown on the next codec bump rather than reasoning from the export table.
