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

## zlib-ng backend (2026-10-25)

The deflate codecs (zlib, gzip, raw deflate share one module per wrapper) moved from madler zlib to zlib-ng 2.3.3. On wasm32 zlib-ng runs its generic C paths: no SIMD and no runtime CPU detection (the dispatch table is still there, with one entry per function). Sizes, post `wasm-strip` + `wasm-opt -O3`, before and after:

| Module | madler zlib | zlib-ng | Δ |
|--------|------------:|--------:|--:|
| zlib (both directions) | 86.9 KB | 104.4 KB | +17.5 KB |
| zlib decompress-only | 54.5 KB | 67.8 KB | +13.3 KB |
| zlib compress-only | 62 KB | 80 KB | +18 KB |
| gzip (both directions) | 84.9 KB | 103.4 KB | +18.5 KB |
| gzip decompress-only | 53.1 KB | 67.0 KB | +13.9 KB |
| gzip compress-only | 61 KB | 79.7 KB | +18.7 KB |

Direction-only builds still drop the other direction's code, even though zlib-ng's dispatch table (`functable`) names functions from both sides. The budgets in `bindings/wasm/tests/bundle.test.ts` were raised to match (zlib 96 → 115 KB, gzip 93 → 114 KB, and the four direction caps).

## lz4 64-bit registers (2026-10)

The WASM build compiles lz4 with a 64-bit `reg_t` (`third_party/patches/lz4`, see `third_party/VENDOR.md`), the register width native x86-64 and arm64 builds already use. Sizes, post `wasm-strip` + `wasm-opt -Oz`, from the same pipeline built before and after (gzip is `gzip -9`):

| Module | Before | After | Δ | gzip before | gzip after |
|--------|-------:|------:|--:|------------:|-----------:|
| lz4 (both directions) | 123,853 B | 126,963 B | +3,110 B (+2.5%) | 44,168 B | 43,870 B |
| lz4 decompress-only | 48,626 B | 48,626 B | 0 | 20,226 B | 20,226 B |
| lz4 compress-only | 99,073 B | 102,183 B | +3,110 B (+3.1%) | 35,338 B | 35,066 B |

The decoder never uses `reg_t`, so the decompress-only module is byte-identical to the one before. The full and compress-only modules stay inside the budgets in `bindings/wasm/tests/bundle.test.ts`, which are unchanged. An earlier variant that also turned on lz4's fast decode loop (`-DLZ4_FAST_DEC_LOOP=1`) grew the decompress-only module by 11.5% (48,626 to 54,230 B, past its 52 KB cap) and decoded slower on large files under Bun on x86-64, so it is not part of the change.

## zstd 64-bit registers (2026-10)

The WASM build compiles zstd with 64-bit registers for its bit containers, reload decisions, encoder flush decisions and the row-hash mask (`third_party/patches/zstd`, see `third_party/VENDOR.md`). Sizes, post `wasm-strip` + `wasm-opt -Oz`, from the same pipeline built before and after (gzip is `gzip -9`):

| Module | Before | After | Δ | gzip before | gzip after |
|--------|-------:|------:|--:|------------:|-----------:|
| zstd (both directions) | 414,254 B | 412,086 B | −2,168 B (−0.5%) | 127,357 B | 126,679 B |
| zstd decompress-only | 97,011 B | 98,277 B | +1,266 B (+1.3%) | 38,605 B | 38,643 B |
| zstd compress-only | 347,704 B | 343,954 B | −3,750 B (−1.1%) | 103,162 B | 102,576 B |

The module does not grow, so the budgets in `bindings/wasm/tests/bundle.test.ts` are unchanged. An earlier version of the change that also enabled zstd's fast 4-stream Huffman loop and widened `ZSTD_count` grew the full module by 3.2% (414,254 to 427,694 B) and the decompress-only module by 6.5%; the ablation showed neither part pays for itself (the fast loop is a gain on JavaScriptCore/Graviton, a loss on V8/Graviton, and an upstream-unsupported configuration with 32-bit pointers; `ZSTD_count` made no measurable difference), so they are not part of the patch.

## brotli 64-bit registers (2026-10)

The WASM build compiles brotli with `BROTLI_BUILD_64_BIT` plus two source patches (`third_party/patches/brotli`, see `third_party/VENDOR.md`). Sizes, post `wasm-strip` + `wasm-opt -Oz`, from the same pipeline built before and after (gzip is `gzip -9`):

| Module | Before | After | Δ | gzip before | gzip after |
|--------|-------:|------:|--:|------------:|-----------:|
| brotli (both directions) | 503,756 B | 507,004 B | +3,248 B (+0.6%) | 175,227 B | 172,193 B |
| brotli decompress-only | 192,278 B | 191,097 B | −1,181 B (−0.6%) | 87,083 B | 86,486 B |
| brotli compress-only | 457,453 B | 461,882 B | +4,429 B (+1.0%) | 157,712 B | 155,762 B |

The budgets in `bindings/wasm/tests/bundle.test.ts` are unchanged. The flag alone, without the second patch, enables upstream's "SIMD" hashers (H58/H68) for qualities up to 6 and adds 26.5 to 30 KB to the compressor (full module 533,458 B); the second patch keeps the scalar hashers, which is what recovers the size.
