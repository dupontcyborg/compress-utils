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

- [ ] **#1** Drop `-Wl,--export-dynamic` in `cmake/toolchains/zig-wasm.cmake`; export only `cu_*` + `cu_alloc/cu_free` + `_initialize`. (biggest win, no perf risk)
- [ ] **#2** zstd build flags: `ZSTD_LEGACY_SUPPORT=0`, `ZSTD_BUILD_DICTBUILDER=OFF` (+`ZSTD_LIB_MINIFY`).
- [ ] **#3** Final `wasm-opt -Oz` pass (DCE bites once #1 lands).
- [ ] **#4** (gated) `-Oz`/`-Os` on codec libs — only if throughput holds.
- [ ] **#5** (deferred) `wasm32-freestanding` + tiny malloc (drops WASI/libc startup).
- [ ] Tighten `bindings/wasm/tests/bundle.test.ts` size budgets to the new sizes.
