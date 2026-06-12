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
- [ ] **#4** (gated) `ZSTD_LIB_MINIFY` and/or `-Oz`/`-Os` on codec libs — only if throughput holds.
- [ ] **#5** (deferred) `wasm32-freestanding` + tiny malloc (drops WASI/libc startup).
- [ ] Correct `docs/wasm-size.md`: (a) its zstd projection (~50–65%) over-counted — dead *exports* ≠ dead *code*, the shared codec core stays referenced (actual zstd −24%); (b) add the brotli static-init LUT finding — brotli is data-bound, and the v1.2.0 LUT, not just the dictionary, drives its size.

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