# TODO — Benchmarks & WASM size opt

## Benchmarks

Done (internal benchmarks — mergeable): C driver + C-native baseline, WASM
(Node), and Python drivers — all one-shot + streaming; interleaved runner
(caffeinate, checkpoint, skip/error markers); corpus tiers (smoke / silesia /
silesia-mini / enwik8, fetch + sha lock); report (tables, pareto/throughput
plots, regression diff; impl- & mode-aware); baked baselines.

Todo (later PRs — ecosystem comparisons, not needed to merge):
- [ ] Decide default level set (`1,3,6,9` vs `1,3,5,7,9`) + per-codec edge mapping question.
- [ ] Python ecosystem baseline (stdlib `zlib/bz2/lzma`, pip `zstandard/brotli/lz4`).
- [ ] JS ecosystem baseline for WASM (`node:zlib`, `CompressionStream`, `fzstd`).
- [ ] CI: size budgets as hard gate; throughput trend on dedicated HW only (never shared runners).
- [ ] Rust/Go drivers as those bindings land.

## WASM size opt  (plan + measurements: `docs/wasm-size.md`; guard: `baseline-wasm`)

Workflow per step: change → rebuild wasm → re-run wasm driver on `silesia-mini`
→ regression-diff size+throughput vs `baseline-wasm` (want smaller, not slower).

- [x] **#1** Drop `-Wl,--export-dynamic` in `cmake/toolchains/zig-wasm.cmake`; export only the 19-symbol `cu_*` ABI allow-list (`bindings/wasm/CMakeLists.txt`). zstd exports 691→21.
- [x] **#2** zstd build flags: `ZSTD_LEGACY_SUPPORT=OFF` + `ZSTD_BUILD_DICTBUILDER=OFF` (`algorithms/zstd/CMakeLists.txt`). **`ZSTD_LIB_MINIFY` held back** — it trades decode speed for size, so it moves to the #4 (perf-gated) bucket.
- [x] **#3** Final `wasm-opt -Oz` pass (was `-O3`; DCE bites once #1 lands).
- [x] **#6** brotli EARLY static-init (`-DBROTLI_STATIC_INIT=1`, wasm-only gate in `algorithms/brotli/CMakeLists.txt`). v1.2.0 embeds a ~192 KB encoder-only dictionary LUT as const data by default; EARLY computes it at `_initialize` instead. **brotli 712→471 KB (−34%)** — the single biggest lever, bigger than #1–#3 combined. Native artifact unchanged.
- [x] Tighten `bindings/wasm/tests/bundle.test.ts` size budgets to the new sizes (zlib 88, bz2 102, xz 145, lz4 120, zstd 440, brotli 520 KB — ~8–12% headroom; trips on a real regression).
- [x] **#4** (gated) zstd levers — **measured, rejected.** `ZSTD_LIB_MINIFY` is a byte-identical no-op (already subsumed by #1+#3 DCE). `-Oz` on the zstd lib saves 11% (416→370 KB) but a same-session A/B showed decode −3.8% median / −8.8% worst and high-level compress to −17% — fails the >8% gate. zstd stays at 406 KB. `-Oz`/`-Os` on the *other* codec libs untested (lower payoff; same risk profile).
- [x] **#5** `wasm32-freestanding` + tiny malloc — **tested, rejected.** Premise was "drops the WASI shim + libc startup," but the real modules already import **0** WASI functions (the shim is fully DCE'd; the loader's `wasiImports` are dead JS), and the wasi-libc allocator (`malloc`/`calloc`/`realloc`/`free` + `mem*`) is only **~1.7 KB** under `-Oz`. A hand-written allocator might not even beat that and would risk correctness against codecs that hammer alloc/free. No meaningful prize.
- [x] **#7** Split each module into compress-only / decompress-only `.wasm`. **Done + shipped.** `CU_WASM_DIR` (both|compress|decompress) in `bindings/wasm/CMakeLists.txt` + `host.cmake`; the missing lever was that each codec's vtable statically references both directions, so the export list alone shed nothing (~0%). Gating the vtable slots with `CU_OMIT_COMPRESS`/`CU_OMIT_DECOMPRESS` (all six `src/algorithms/*/*.c`) lets LTO+GC drop the unused direction's codec closure. Measured decode-only, round-trip-verified:

  ┌────────┬─────────┬───────────┬──────┐
  │ algo   │  both   │ decode-only│  Δ   │
  ├────────┼─────────┼───────────┼──────┤
  │ zstd   │ 416 KB  │   90 KB   │ −78% │
  │ brotli │ 482 KB  │  183 KB   │ −61% │  (keeps the 123 KB wire dict)
  │ lz4    │ 110 KB  │   35 KB   │ −67% │
  │ bz2    │  92 KB  │   54 KB   │ −42% │
  │ xz     │ 135 KB  │   81 KB   │ −39% │
  │ zlib   │  78 KB  │   49 KB   │ −37% │
  └────────┴─────────┴───────────┴──────┘

  **Shipped:** the build emits all three variants per algo (`dist/algorithms/<algo>/{,decompress/,compress/}<algo>.wasm`), exposed as `compress-utils/<algo>` (both, default), `/<algo>/decompress`, and `/<algo>/compress` subpath exports with direction-split surfaces. 41 wasm tests pass (incl. 12 per-variant size budgets + a subpath round-trip). README documents the import paths + raw/gzip size table. Pairs with #6 (decode-only brotli never builds the encoder LUT).
- [x] Correct `docs/wasm-size.md`: (a) its zstd projection (~50–65%) over-counted — dead *exports* ≠ dead *code*, the shared codec core stays referenced (actual zstd −24%); (b) added the brotli static-init LUT finding — brotli is data-bound, and the v1.2.0 LUT, not just the dictionary, drives its size.

Validation (silesia-mini, AppleM4Max, vs `baseline-wasm`): ratio identical on all
216 specs; no throughput regression (#1–#3 flags were thermal-tail noise on the
fastest codecs, gone on a fresh re-measure; brotli EARLY measured +6.8% C / +7.9% D
median, ratio unchanged); 29/29 wasm tests pass. All changes perf-neutral.

Current (after #1–#3 + #6 — was ~1.69 MB):

┌────────┬─────────┬─────────┐
│ module │  before │  after  │
├────────┼─────────┼─────────┤
│ zlib   │   78 KB │   78 KB │
├────────┼─────────┼─────────┤
│ bz2    │   92 KB │   92 KB │
├────────┼─────────┼─────────┤
│ xz     │  133 KB │  132 KB │
├────────┼─────────┼─────────┤
│ lz4    │  134 KB │  108 KB │  −20%
├────────┼─────────┼─────────┤
│ zstd   │  535 KB │  406 KB │  −24%
├────────┼─────────┼─────────┤
│ brotli │  714 KB │  471 KB │  −34%
├────────┼─────────┼─────────┤
│ total  │ 1.69 MB │ 1.26 MB │  −25%
└────────┴─────────┴─────────┘