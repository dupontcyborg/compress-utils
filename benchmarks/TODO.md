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
- [ ] **#4** (gated) `ZSTD_LIB_MINIFY` and/or `-Oz`/`-Os` on codec libs — only if throughput holds.
- [ ] **#5** (deferred) `wasm32-freestanding` + tiny malloc (drops WASI/libc startup).
- [ ] Tighten `bindings/wasm/tests/bundle.test.ts` size budgets to the new sizes (headroom today: zstd 406/700, lz4 108/200, brotli 712/850).
- [ ] Correct `docs/wasm-size.md`: its zstd projection (~50–65%) over-counted — dead *exports* ≠ dead *code*; the shared codec core stays referenced. Actual zstd −24%.

Validation (silesia-mini, AppleM4Max, vs `baseline-wasm`): ratio identical on all
216 specs; no throughput regression (the few flags were thermal-tail noise on the
fastest codecs, gone on a fresh lz4 re-measure); 29/29 wasm tests pass. #1–#3 are
perf-neutral as predicted.

Current (after #1–#3 — was ~1.69 MB):

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
│ brotli │  714 KB │  712 KB │
├────────┼─────────┼─────────┤
│ total  │ 1.69 MB │ 1.49 MB │  −9%
└────────┴─────────┴─────────┘