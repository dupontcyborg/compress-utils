# Ecosystem benchmarks — plan

Status: phase 1 (harness), C and Python (phase 2) done, phase 3 (JavaScript) done, on branch `ecosystem-benchmarks` (2026-10-24). Results below. Tracked in [`TODO.md`](../TODO.md) under "Next up".

## Why

Today's benchmarks compare our own bindings to each other and to raw upstream C. That measures wrapper overhead, not whether anyone should pick this library. This plan adds the comparison a prospective user actually makes: compress-utils against the library they would otherwise use in their language.

The stakes are real. In Python, cramjam covers the same nine formats and gets ~30M PyPI downloads a month, against ~2k for us; python-snappy now depends on cramjam. In JS, fflate and pako are each in the hundreds of millions of downloads a month. The results decide where to invest: if a binding loses everywhere it competes, it gets maintenance only.

**We publish the losses.** Every chart and table shows every competitor we ran, including the cases where we lose. A difference within measurement noise is reported as a tie, not a win.

## Questions the results must answer

For each language, per codec and per input size:

1. **Throughput and ratio.** Where do we sit on the ratio-vs-speed curve relative to each competitor, for compression and decompression?
2. **Small-message cost.** What does one call on a 1–64 KB message cost? This is where wrapper overhead, allocation strategy and the size probe dominate, and where same-C-underneath libraries can still differ a lot.
3. **JS only: bundle size and startup.** Bytes shipped to the browser for one codec (minified and compressed), and the cost of the first call, including WASM instantiation.
4. **Coverage.** Which formats each competitor supports at all. A format nobody else ships in a language (bz2 in JS, for example) is uncontested and gets reported as such, not benchmarked against nothing.

## Corpora

All from standard, citable corpora. Fetched corpora follow the existing model: downloaded on demand into `benchmarks/corpus/cache/`, extracted to `corpus/data/`, pinned trust-on-first-use in `fetched.lock.json`. Nothing is committed.

### Large (≥ 1 MB per file)

| Tier | Contents | Size | Status |
|------|----------|------|--------|
| `silesia` | The 12-file Silesia corpus (text, XML, database, binaries, images, medical data) | ~211 MB | exists |
| `silesia-mini` | 6 Silesia files: `dickens`, `samba`, `x-ray`, `osdb`, `sao`, `xml` | ~70 MB | exists |
| `enwik8` | First 100 MB of an English Wikipedia dump | 100 MB | exists |
| `canterbury-large` | Large Canterbury corpus: `E.coli`, `bible.txt`, `world192.txt` | ~11 MB | done |

### Medium and small whole files

| Tier | Contents | Size | Status |
|------|----------|------|--------|
| `canterbury` | Canterbury corpus, 11 files from 3.7 KB (`grammar.lsp`) to 1 MB (`kennedy.xls`) | 2.8 MB | done |

Canterbury's files are real documents of different types at sizes from a few KB up, so they cover the space between messages and bulk data. Calgary is skipped: Canterbury superseded it in 1997. corpus.canterbury.ac.nz refuses scripted downloads (a Cloudflare bot block), so both Canterbury tiers come from the pfalcon/canterbury-corpus mirror, pinned to a commit; the files match the mirror's SHA1SUMs and the official total size.

### Small messages

Two sources, because they answer different questions:

| Tier | Contents | What it measures | Status |
|------|----------|------------------|--------|
| `github-users` | zstd's `github_users_sample_set` (release asset on facebook/zstd `v1.1.3`): 9,114 JSON records, ~820 bytes on average, from the GitHub public API | Realistic small structured records, the kind sent over a network or stored per row. Also the corpus for zstd dictionary work later. | done |
| `silesia-blocks-{1k,4k,16k,64k}` | Each `silesia-mini` file cut into fixed-size blocks, each block compressed independently (lzbench's `-b` convention). 1 MiB of evenly spaced blocks per file, so the slowest settings (brotli and xz at high levels cost 0.3-0.7 ms per call) stay at seconds per sample. | Controlled message sizes over the same data, so per-call cost can be plotted against size | done |

The block tiers are derived from `silesia-mini` deterministically (no new download), and their sha256 goes in the corpus manifest like the synthetic tier.

### Optional

The google/snappy `testdata` set (HTML, URL lists, a protobuf, a JPEG, a PDF) is the conventional suite for fast codecs. It overlaps with Canterbury, so add it only if Snappy and LZ4 results look thin.

## Competitors

The rule for inclusion: what a developer in that language would reasonably reach for today, maintained, in-process. CLIs are out (see benchmarks/README: they measure process spawn, not the codec).

### Python

| Library | Formats | Notes |
|---------|---------|-------|
| cramjam | all 9 | Most important comparison. Rust-backed, one package, same breadth as us. |
| stdlib `zlib`, `gzip`, `bz2`, `lzma` | zlib, deflate, gzip, bz2, xz | The zero-dependency default. |
| stdlib `compression.zstd` (3.14+) | zstd | New in 3.14 (PEP 784); the future default for zstd. |
| `zstandard` | zstd | The established third-party zstd binding. |
| `brotli` (Google) | brotli | |
| `lz4` | lz4 (frame) | |

python-snappy is skipped because it now wraps cramjam.

### JavaScript: Node

| Library | Formats | Notes |
|---------|---------|-------|
| `node:zlib` | gzip, deflate, zlib, brotli, zstd | Built in. zstd since v23.8.0 / v22.15.0, still marked experimental. Use the `*Sync` one-shot functions for the one-shot comparison. |
| fflate | gzip, deflate, zlib | Pure JS, the most popular small-bundle choice. |
| pako | gzip, deflate, zlib | Pure JS, the long-standing default. |
| fzstd | zstd (decompress only) | Pure JS. |
| `@bokuweb/zstd-wasm` | zstd | WASM. |
| `@mongodb-js/zstd` | zstd | Native addon (Node only), the most downloaded zstd package. |
| brotli-wasm | brotli | WASM. |
| lz4js | lz4 | Pure JS. |
| snappyjs | snappy | Pure JS. |
| xz-decompress | xz (decompress only) | WASM. |

No maintained JS bz2 library is worth including. bz2 is reported as uncontested.

### JavaScript: browser

The same pure-JS and WASM libraries as Node (no native addons), plus the built-in **`CompressionStream` / `DecompressionStream`**. Per MDN's compatibility data: `gzip`, `deflate` and `deflate-raw` everywhere; `brotli` in Firefox 147+ and Safari 18.4+, not Chrome; `zstd` in Firefox 138+ only. The browser runner checks support at runtime and records unsupported pairs as absent rather than failed.

`CompressionStream` is stream-only and async, so it is compared in the streaming category, not against one-shot calls.

### Rust

| Crate | Formats | Notes |
|-------|---------|-------|
| `zstd` | zstd | C (zstd-sys). |
| `flate2` | gzip, deflate, zlib | Default backend is pure Rust (miniz_oxide); also run the `zlib-rs` backend. |
| `brotli` | brotli | Pure Rust. |
| `lz4_flex` | lz4 | Pure Rust, the popular choice. |
| `lz4` | lz4 | C. |
| `snap` | snappy | Pure Rust. |
| `liblzma` / `xz2` | xz | C. |
| `bzip2` | bz2 | 0.6 defaults to the pure-Rust `libbz2-rs-sys`. |

The baseline driver is a separate crate that does not depend on compress-utils, which sidesteps the duplicate-symbol clash with the C-backed `-sys` crates.

### Go

| Package | Formats | Notes |
|---------|---------|-------|
| stdlib `compress/gzip`, `zlib`, `flate`, `bzip2` | gzip, zlib, deflate, bz2 (decompress only) | The default. |
| `klauspost/compress` | zstd, gzip, zlib, deflate, snappy | Pure Go, the de facto standard. Most important comparison. |
| `pierrec/lz4/v4` | lz4 | Pure Go. |
| `andybalholm/brotli` | brotli | Pure Go (transpiled from C). |
| `ulikunitz/xz` | xz | Pure Go. |
| `dsnet/compress/bzip2` | bz2 (compress) | Fills the stdlib gap. |

### C

Covered by the `c-baseline` driver (raw upstream libraries). It had been broken since the July vendoring change (it linked the old `algorithms/dist/` archives); it now links the vendored codecs from `build/algorithms/`.

## Fairness rules

1. **Compare curves, not single levels.** Each library sweeps its own level range and the report plots ratio against throughput. Our level 5 and someone else's level 5 are different settings. Where a competitor wraps the same C library (zstandard, the `zstd` crate, `node:zlib`'s zlib), also run it at the exact native level ours maps to, so the overhead comparison is like for like.
2. **Same wire format.** zstd frames carry the content size; lz4 uses the frame format with the content size; xz uses CRC64; gzip has a standard header; Snappy is a raw block. Checksums match ours, or the difference is recorded. Each driver states its settings in its source.
3. **Cross-decode once.** On the first job, our decoder decodes each competitor's output and vice versa. That proves the formats really match. A failure is a harness bug, not a result.
4. **Single-threaded everywhere.** Disable worker threads in zstd and xz encoders that offer them.
5. **Each library's fastest documented API** for the case being measured: buffer reuse where its API supports it, a known output size for decompression where its API accepts one (ours: `expected_size` / capacity). For each case, record whether the decompressed size was passed in.
6. **Interleaved, same machine, same window.** This is already how the runner works; baseline drivers just join the interleave.
7. **Versions pinned and recorded.** Every driver reports each library's version through `--info`. Lockfiles (`requirements.txt` with hashes, `package-lock.json`, `Cargo.lock`, `go.sum`) are committed under `benchmarks/drivers/`.

## Harness changes

### 1. Level map as data

Done. `benchmarks/levels.json` maps cu level → native level. The runner translates each job's level for drivers in `NATIVE_LEVEL_DRIVERS`, so baseline drivers never carry a mapping of their own (`bench_baseline.c`'s hand-copied mapping is gone), and every record carries both `level` and `native_level`. `check_levels.py` verifies the file: c-baseline at the mapped levels must match compress-utils byte for byte (120 settings). Still to do in phase 2: a native-level sweep option for the curve runs, using each codec's `range`.

### 2. Message-set jobs

Done in all six drivers. Timing one 1 KB call is mostly timer noise, so small-message tiers run as **message sets**: a file of length-prefixed records (`u32 length` + bytes, little-endian). The job line gets a `msgset` flag. Each sample compresses every record independently, with a fresh one-shot call per record, and times the whole loop. Decompression likewise. The record carries `messages` and `total_bytes`, and the report derives ns per message, messages per second and aggregate MB/s. Every existing driver (C, c-baseline, Python, WASM, Go, Rust) needs this too, so our own numbers come from the same loop.

### 3. JS size and startup

A separate script, not a driver job:
- **Bundle size:** for each library and codec, bundle a minimal one-call entry point with esbuild (minified), then report raw, gzip and brotli bytes, with any `.wasm` counted.
- **Cold start:** time from `import` to the first call returning, in a fresh process (Node) or a fresh page (browser), with the median over N.

### 4. Browser runner

Reuse the Playwright setup in `bindings/wasm/tests-browser` (`serve.mjs` already serves static files). A runner page loads the job list and corpus from the local server, runs every implementation on each job back to back in the same page (interleaving, as the native runner does), and posts NDJSON back. Run it in Chromium, Firefox and WebKit. Records carry `runtime: "chromium" | "firefox" | "webkit" | "node"`.

### 5. Report

- Pareto plots per (language, codec, corpus), colored by implementation, with ours highlighted.
- A **win/loss/tie summary table** per language: for each codec and size bucket, our compress and decompress throughput as a multiple of the best competitor at matched ratio, with ties marked.
- Per-message cost against message size (1k → 64k) for the block tiers.
- The JS size and startup table.

## Phases

| # | Work | Done when |
|---|------|-----------|
| 0 | **Prerequisite: the decompressed-size cap fix** (see TODO). It changes the unknown-size decode path in Python, Go and WASM, so numbers taken before it would be stale. | Fix merged |
| 1 | Harness: `levels.json`, message sets in all six existing drivers, new corpus tiers (`canterbury`, `canterbury-large`, `github-users`, `silesia-blocks-*`), report additions | Existing drivers produce message-set results on all tiers |
| 2 | **Python** baseline driver | First win/loss table, vs cramjam especially |
| 3 | **Node** baseline driver + JS size and startup script | Node table and bundle-size table |
| 4 | **Browser** runner (Chromium, Firefox, WebKit) | Browser tables, including `CompressionStream` |
| 5 | **Rust** and **Go** baseline drivers | Their tables |
| 6 | Publish | See below |

Every language phase covers one-shot, message sets and streaming (`--modes oneshot,stream`).

**Scope decisions (2026-10-23).** Keep the benchmarks lean and fast; these hold until revisited:

- **C is done.** The conclusions are in "C results" below; no reruns for publication-grade sample counts. The speedups it found (xz dictionary cap, zlib-ng, snappy, level map) happen on a separate branch, from the list in `TODO.md`.
- **Other languages: full level sweep (`--sweep`) and the competitors listed above.** These are the point of the exercise.
- **Browser stays** (phase 4): it's the only place the WASM build is measured against native alternatives like `CompressionStream`.
- **Rust and Go competitor drivers (decided 2026-10-25):** same matrix as Python (levels 1, 4, 6, 10, one-shot and streaming, 1 sample on silesia-mini and 3 elsewhere), plus a zlib-rs run for flate2. Done; see "Go run" and "Rust run" below.
- **To be decided later:** the publish-profile run, streaming beyond what each phase needs.

Python goes first because cramjam is the most direct competitor and the answer decides whether the Python binding is worth investing in. JS comes next because it's where we most likely win.

## Run profiles

The full matrix (every tier × 9 codecs × each library's level sweep × implementations) is too slow to run casually: xz and bz2 at high levels on enwik8 dominate. Two profiles:

- **dev:** `silesia-mini`, `canterbury`, `github-users` and `silesia-blocks-4k`; levels 1, 6, 9; 3 samples. For iterating on drivers.
- **publish:** every tier, the full level sweep, 9 samples, run on a quiet machine with nothing else open. enwik8 is limited to 3 levels per codec. Run once per published release.

Hardware: the maintainer's Apple Silicon MacBook for now (decided 2026-10-22). Publish the hardware alongside every result. An x86-64 Linux machine (a dedicated instance, not a shared CI runner) can be added later, since SIMD paths differ by architecture. GitHub-hosted runners are too noisy for throughput (benchmarks/README already says so).

## Publishing

- `benchmarks/README.md` gets an "Against the ecosystem" section with all the tables and plots, the methodology above, library versions, hardware, and the exact command to reproduce.
- Each binding README's **Performance** section gets a short table against its ecosystem alternatives. `docs/readme-skeleton.md` already allows this "only once a baseline driver for that language exists ... and only with numbers we are willing to stand behind."
- The root README gets a one-paragraph summary linking to the full results. It says where we lose, not only where we win.
- Result JSON for every published chart is committed, so anyone can check the numbers.

## Open questions

- **zstd dictionaries.** `github-users` is the standard dictionary demo corpus. Once the dictionary API is designed (path to 1.0), add a dictionary scenario. Until then, competitors run without dictionaries too.
- **Streaming (decided 2026-10-23): every language gets both one-shot and streaming results.** Each competitor driver implements its library's streaming API where it has one (fed in 64 KiB chunks, as our drivers are), and runs on the file tiers with `--modes stream`. Message sets have no streaming form. Libraries without a streaming API (libdeflate, raw snappy) appear in the one-shot results only, and the tables say so.
- **Concurrency.** Python GIL release and Go goroutine scaling are real differences between libraries, but they are a separate benchmark. Defer it.

## JavaScript (2026-10-24)

**Scope (decided 2026-10-24).** Our JS package is WASM and runs everywhere (browsers, edge runtimes, Deno, Bun, Node), so competitors are the libraries that do too: pure JavaScript and WASM only, plus `node:zlib` (one reference row, labelled Node-only and native) and `CompressionStream`. Native Node addons (`@mongodb-js/zstd`, `zstd-napi`) are out. Run on Node and on Bun (V8 and JavaScriptCore). **No Playwright:** the browser-specific pieces it would add (SpiderMonkey, browser `CompressionStream`) aren't worth the machinery; Node implements `CompressionStream` too. Competitors and drivers are in `benchmarks/README.md` ("JavaScript: competitors").

**Bundle size and cold start** (`size/js_size.py`; minified browser bundle; KiB):

| Format | Library | JS | WASM | Total | gzip |
|--------|---------|----|------|-------|------|
| zstd | compress-utils (this format only) | 10.1 | 404.5 | 414.7 | 127.6 |
| | compress-utils, decode only | 10.0 | 94.7 | 104.8 | 40.9 |
| | zstd-wasm | 6.3 | 245.9 | 252.2 | 80.4 |
| | fzstd (pure JS, decode only) | 6.9 | - | 6.9 | 3.3 |
| brotli | compress-utils | 10.1 | 491.8 | 502.0 | 174.2 |
| | compress-utils, decode only | 10.0 | 187.7 | 197.7 | 88.2 |
| | brotli-wasm | 6.1 | 1032.1 | 1038.2 | 487.8 |
| gzip | compress-utils | 10.1 | 86.7 | 96.8 | 39.2 |
| | compress-utils, decode only | 10.0 | 54.5 | 64.5 | 29.3 |
| | fflate | 9.0 | - | 9.0 | 4.5 |
| | pako | 41.2 | - | 41.2 | 12.5 |
| lz4 | compress-utils | 10.1 | 121.0 | 131.1 | 46.4 |
| | lz4js | 5.4 | - | 5.4 | 2.3 |
| snappy | compress-utils | 10.1 | 36.3 | 46.4 | 19.0 |
| | snappyjs | 4.8 | - | 4.8 | 2.1 |
| xz | compress-utils | 10.1 | 142.9 | 153.0 | 66.9 |
| | compress-utils, decode only | 10.0 | 87.1 | 97.2 | 48.1 |
| | xz-decompress (WASM embedded in JS, decode only) | 20.5 | - | 20.5 | 9.5 |
| bz2 | compress-utils | 10.1 | 95.5 | 105.6 | 38.3 |
| | compress-utils, decode only | 10.0 | 55.0 | 65.1 | 25.7 |

Per format we are 7-20× bigger than the pure-JS libraries (fflate 9 KiB, lz4js 5 KiB, snappyjs 5 KiB against 97, 131 and 46 KiB), which is what WASM costs; 40% bigger than zstd-wasm, 2× smaller than brotli-wasm (which ships a 1 MiB binary), and decode-only builds are 2-4× smaller than full ones. Cold start (import plus one 1 KB round trip, minus an empty process) is 4-26 ms for compress-utils on Node and 7-16 ms on Bun, against 0-10 ms for the others; it is a few milliseconds and noisy, so read it as an order of magnitude.

**Performance run (2026-10-24).** M-series MacBook, Node 24.12 and Bun 1.3.10. Node: levels 1, 4, 6, 10, one-shot, streaming and message sets, silesia-mini at 1 sample and everything else at 3 (5,571 records). Bun: level 6 only, silesia-mini and 4 KB blocks (459 records). All records round-trip; compress-utils decodes every competitor's output except lz4js on 3 inputs (below). Took about 2 h 20 min, against an estimate of 75 min; the slowest libraries (brotli at top quality in three implementations) dominate. Each cell is **compress / decompress in MB/s** (uncompressed bytes per second), at level 6 for compress-utils and at the native level that maps to for the others (zlib 6, xz 5, zstd 13, brotli 6, ...), summed over the files of a tier. `ratio` is the compression ratio on silesia-mini, so you can see the libraries are doing comparable work. Libraries with no levels (lz4js, `CompressionStream`) are compared with compress-utils at the matching level, shown as an extra row. Decode-only libraries (fzstd, xz-decompress) show decompression only.

**Node (V8)**

**zlib**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 2.51 | 36.5 / 325 | 37.4 / 341 | 53.2 / 228 | 34.3 / 148 |
| node:zlib | 2.49 | 64.2 / 560 | 47.7 / 171 | 76.0 / 330 | 38.5 / 161 |
| fflate | 2.48 | 53.9 / 225 | - | 45.9 / 133 | 25.7 / 86.8 |
| pako | 2.49 | 33.4 / 171 | 33.4 / 165 | 31.5 / 87.9 | 14.4 / 58.2 |
| CompressionStream (no levels) | 2.49 | - | 49.4 / 162 | - | - |

**zstd**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 2.92 | 13.7 / 785 | 12.2 / 890 | 20.0 / 549 | 19.9 / 272 |
| zstd-wasm | 2.92 | 12.4 / 1,085 | - | 15.6 / 474 | 12.6 / 252 |
| node:zlib | 2.92 | 17.2 / 1,203 | 17.3 / 266 | 26.9 / 572 | 22.9 / 257 |
| fzstd | - | - / 253 | - / 193 | - / 60.9 | - / 47.6 |

**brotli**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 2.86 | 38.4 / 402 | 37.2 / 386 | 42.4 / 207 | 29.2 / 121 |
| compress-utils (level 10) | 3.28 | 0.8 / 322 | 0.8 / 317 | 1.0 / 165 | 0.7 / 106 |
| brotli-wasm | 2.67 | 16.6 / 236 | 16.5 / 238 | 12.4 / 108 | 5.1 / 65.8 |
| node:zlib | 2.86 | 43.3 / 466 | 37.6 / 200 | 52.3 / 229 | 28.1 / 148 |
| CompressionStream (no levels) | 3.28 | - | 0.9 / 175 | - | - |

**lz4**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 2.22 | 51.0 / 2,457 | 44.4 / 1,564 | 148 / 2,037 | 136 / 1,021 |
| compress-utils (level 1) | 1.77 | 541 / 2,135 | 489 / 1,584 | 687 / 1,957 | 567 / 901 |
| lz4js (no levels) | 1.86 | 160 / 581 | - | 106 / 772 | 46.1 / 739 |

**snappy**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 1.77 | 656 / 1,554 | 439 / 1,039 | 773 / 1,623 | 571 / 921 |
| snappyjs (no levels) | 1.77 | 269 / 558 | - | 213 / 573 | 246 / 571 |

**xz**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 3.32 | 4.6 / 78.7 | 4.7 / 82.3 | 0.8 / 45.5 | 0.2 / 38.7 |
| xz-decompress | - | - | - / 57.0 | - | - |

**Bun (JavaScriptCore)**, level 6

**zlib**

| Implementation | ratio | large files | large, streaming | 4 KB messages |
|---|---|---|---|---|
| compress-utils | 2.51 | 39.4 / 352 | 38.5 / 325 | 55.8 / 224 |
| node:zlib | 2.49 | 83.2 / 574 | 50.2 / 179 | 84.4 / 312 |
| fflate | 2.48 | 50.7 / 66.8 | - | 41.9 / 52.7 |
| pako | 2.49 | 46.9 / 286 | 46.0 / 335 | 45.5 / 193 |
| CompressionStream (no levels) | 2.49 | - | 51.6 / 179 | - |

**zstd**

| Implementation | ratio | large files | large, streaming | 4 KB messages |
|---|---|---|---|---|
| compress-utils | 2.92 | 17.1 / 911 | 16.3 / 924 | 23.6 / 471 |
| zstd-wasm | 2.92 | 16.2 / 781 | - | 16.8 / 395 |
| node:zlib | 2.92 | 19.9 / 1,308 | 16.5 / 304 | 28.4 / 498 |
| fzstd | - | - / 348 | - / 266 | - / 30.4 |

**brotli**

| Implementation | ratio | large files | large, streaming | 4 KB messages |
|---|---|---|---|---|
| compress-utils | 2.86 | 22.1 / 415 | 21.5 / 439 | 32.9 / 184 |
| brotli-wasm | 2.67 | 19.4 / 276 | 19.2 / 272 | 13.8 / 112 |
| node:zlib | 2.86 | 48.2 / 542 | 36.4 / 200 | 57.0 / 235 |
| CompressionStream (no levels) | 3.28 | - | 0.9 / 165 | - |

- **Native code beats WASM, as expected.** `node:zlib` does 64 / 560 MB/s on zlib against our 37 / 325 (and 83 / 574 on Bun), and 17 / 1,203 on zstd against 14 / 785. The WASM package's advantage is that it runs identically everywhere, not that it out-runs native code.
- **Against the portable libraries we win nearly everywhere.**
  - *deflate family:* pako does 33 / 171 and fflate 54 / 225 against our 37 / 325 on large files. fflate compresses faster than we do on large files and slower on small messages (26 vs 34 MB/s at 1 KB); pako is 2× slower at 1 KB (14 vs 34). Both decompress at roughly half to 70% of our speed.
  - *brotli:* brotli-wasm does 17 / 236 against our 38 / 402, at a 7% worse ratio (2.67 vs 2.86).
  - *snappy:* snappyjs does 269 / 558 against our 656 / 1,554.
  - *lz4:* lz4js does 160 / 581 at ratio 1.86 against our fast mode's 541 / 2,135 at 1.77. Against our level 6 (lz4's high-compression mode, ratio 2.22) it compresses faster (160 vs 51) at a worse ratio.
  - *zstd:* zstd-wasm is the closest competitor: 12 / 1,085 against our 14 / 785 on large files (it decompresses 38% faster, we compress 10% faster), and we are ahead on small messages (4 KB: 20 / 549 vs 16 / 474). fzstd, decode only, decompresses at 253 MB/s against our 785 (60 against 549 at 4 KB).
- **Streaming:** compress-utils decompresses streams at 341 MB/s against `node:zlib`'s 171 and `CompressionStream`'s 162, and compresses at 37 against 48 and 49.
- **`CompressionStream` brotli** is fixed at brotli's top quality: 0.9 MB/s at a ratio of 3.28, exactly what compress-utils does at its top level (0.8 MB/s, 3.28). It is not slow, it just can't be told to be fast.
- **Pure-JS performance depends on the engine; WASM barely moves.** fflate decompresses at 225 MB/s on V8 but 67 on JavaScriptCore (against our 325 and 352); pako is steadier (171 and 286).
- **xz in WASM is slow on small inputs:** compress-utils compresses 4 KB messages at 0.8 MB/s and 1 KB messages at 0.2 MB/s, against 10.9 / 5.2 in C and 11.1 / 7.0 in Python. The dictionary-sizing cost (`TODO.md`, xz item) is far worse under WASM, probably from allocating and clearing the preset's full dictionary on every call.

**Library bugs found (not ours):** fflate 0.8.3's streaming `Deflate` produces invalid DEFLATE streams at level 4 and up on some inputs (silesia `sao`, at every chunk size; its one-shot path is fine), so its streaming results are left out above. lz4js 0.2.0 writes invalid LZ4 frames for some inputs (`samba`, `dickens`, `x-ray`; the reference python-lz4 and our decoder reject them), so its numbers cover files where its own output is not valid and should be read with that in mind.

Caveats: single machine; large files at 1 sample (their spread was a few percent); Bun covers level 6 only; `CompressionStream` has no level; cold-start and bundle-size numbers are above.

## Python run (2026-10-23)

compress-utils and the competitors (cramjam, stdlib, zstandard, brotli, lz4) on Python 3.14, at compress-utils levels 1, 4, 6 and 10, each competitor at the native level ours maps to (`levels.json`), one-shot and streaming. Large files at 1 sample (their spread is a few percent), everything else at 3. About 55 minutes on an idle machine.

```sh
D=python,python-stdlib,python-cramjam,python-zstandard,python-brotli,python-lz4
A=zstd,brotli,zlib,gzip,deflate,bz2,lz4,xz,snappy
python3 benchmarks/runner.py --drivers $D --algos $A --levels 1,4,6,10 --modes oneshot,stream \
    --corpus silesia-mini --samples 1
python3 benchmarks/runner.py --drivers $D --algos $A --levels 1,4,6,10 --modes oneshot,stream \
    --corpus canterbury,github-users,silesia-blocks --samples 3
```

cramjam can't decode chunked input (its `Decompressor` needs a whole stream per call), so it has no streaming results.

**Results** (M-series MacBook; 5,760 records, all verified; compress-utils decodes every competitor's output). Each cell is **compress / decompress in MB/s** (uncompressed bytes per second), at level 6 for compress-utils and at the native level that maps to for the others (zlib 6, xz 5, zstd 13, brotli 6, ...), summed over the files of a tier. `ratio` is the compression ratio on silesia-mini, so you can see the libraries are doing comparable work. Python 3.14; the stdlib's zlib here is macOS's system zlib 1.2.12. cramjam can't decode chunked input, so it has no streaming results.

**zlib**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 2.51 | 46.4 / 416 | 44.4 / 404 | 60.2 / 255 | 39.9 / 146 |
| stdlib | 2.51 | 45.9 / 962 | 45.5 / 873 | 65.3 / 421 | 39.9 / 201 |
| cramjam | 2.52 | 54.7 / 439 | - | 84.4 / 205 | 59.6 / 110 |

**zstd**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 2.92 | 15.4 / 1,724 | 17.1 / 1,436 | 26.7 / 797 | 26.1 / 414 |
| zstandard | 2.92 | 17.9 / 1,646 | 16.9 / 1,330 | 27.7 / 745 | 26.7 / 449 |
| stdlib | 2.92 | 18.9 / 1,464 | 16.9 / 1,626 | 27.0 / 786 | 25.0 / 415 |
| cramjam | 2.92 | 18.8 / 1,373 | - | 26.3 / 700 | 26.0 / 401 |

**brotli**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 2.86 | 75.5 / 560 | 66.6 / 541 | 51.8 / 243 | 34.7 / 159 |
| brotli | 2.86 | 73.4 / 530 | 66.8 / 526 | 53.5 / 233 | 33.3 / 175 |
| cramjam | 2.67 | 40.8 / 400 | - | 44.9 / 168 | 23.9 / 88.5 |

**lz4**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 2.22 | 68.1 / 2,102 | 58.8 / 2,009 | 226 / 2,031 | 158 / 1,066 |
| lz4 | 2.22 | 60.1 / 2,186 | 56.4 / 2,096 | 207 / 2,187 | 132 / 1,345 |
| cramjam | 2.22 | 68.0 / 1,829 | - | 210 / 964 | 144 / 406 |

**bz2**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 3.17 | 19.7 / 47.8 | 18.3 / 50.5 | 9.6 / 39.2 | 7.4 / 31.2 |
| stdlib | 3.17 | 19.8 / 49.1 | 21.6 / 53.8 | 9.8 / 42.5 | 7.2 / 31.1 |
| cramjam | 3.17 | 18.3 / 49.2 | - | 9.7 / 43.7 | 6.8 / 32.3 |

**xz**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 3.32 | 5.1 / 92.1 | 5.1 / 97.3 | 11.1 / 52.6 | 7.0 / 42.0 |
| stdlib | 3.32 | 5.3 / 97.9 | 5.1 / 97.1 | 11.5 / 53.9 | 6.8 / 45.0 |
| cramjam | 3.32 | 4.4 / 92.2 | - | 10.0 / 50.3 | 6.3 / 43.6 |

**snappy**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 1.77 | 787 / 1,735 | 644 / 1,493 | 987 / 2,049 | 1,016 / 1,612 |
| cramjam | 1.75 | 825 / 2,072 | - | 968 / 2,523 | 1,177 / 2,430 |

- **zstd, brotli, lz4, bz2, xz: parity** with the dedicated library and the stdlib, within a few percent at every size: zstd 15-19 MB/s compress and 1.4-1.7 GB/s decompress for all four; brotli 75 / 560 for us and 73 / 530 for Google's package. cramjam's brotli is slower (41 / 400, and a ratio of 2.67 against 2.86), and its lz4 decompresses 1 KB messages at 406 MB/s against our 1,066.
- **Deflate: we lose to the stdlib on decompression.** 416 MB/s against 962 on large files (421 vs 255 at 4 KB, 201 vs 146 at 1 KB); compress is level (46 vs 46 MB/s). cramjam compresses faster (55 / 84 / 60 MB/s on large / 4 KB / 1 KB against our 46 / 60 / 40) but decompresses 1 KB messages slower (110 vs 146). On Linux the stdlib links the system zlib, usually plain madler zlib, so the stdlib's lead there is likely much smaller than on macOS; this machine can't confirm it.
- **Snappy: cramjam is faster at decompression:** 2,072 MB/s against our 1,735 on large files and 2,430 against 1,612 at 1 KB, consistent with the C finding that the port is the slow part.
- **No per-call penalty for our binding:** on 1 KB messages we match the native-extension competitors.

## Go run (2026-10-25)

compress-utils' Go binding (cgo) against the stdlib, klauspost/compress, andybalholm/brotli, pierrec/lz4, ulikunitz/xz and dsnet/compress, at compress-utils levels 1, 4, 6 and 10, each competitor at the native level ours maps to (`levels.json`), one-shot and streaming. Large files at 1 sample, everything else at 3. About 33 minutes on a quiet machine, against an estimate of 2-3 hours. 4,054 records, all verified, and compress-utils decodes every competitor's output. The Go and Rust bindings have no `deflate` format, so the matrix has eight formats.

```sh
D=go,go-stdlib,go-klauspost,go-brotli,go-lz4,go-xz,go-bzip2
A=zstd,brotli,zlib,gzip,bz2,lz4,xz,snappy
python3 benchmarks/runner.py --drivers $D --algos $A --levels 1,4,6,10 --modes oneshot,stream \
    --corpus silesia-mini --samples 1
python3 benchmarks/runner.py --drivers $D --algos $A --levels 1,4,6,10 --modes oneshot,stream \
    --corpus canterbury,github-users,silesia-blocks --samples 3
```

**Results** (M-series MacBook). Each cell is **compress / decompress in MB/s** (uncompressed bytes per second), at level 6 for compress-utils and at the native level that maps to for the others (zlib 6, zstd 13, brotli 6, bz2 6, lz4 6), summed over the files of a tier. `ratio` is the compression ratio on silesia-mini, so you can see how comparable the work is: klauspost's levels are its own match finders, not zlib's or zstd's, so its ratios are 3% lower at the "same" level. Libraries with no levels (ulikunitz/xz, klauspost's snappy) are compared at compress-utils' level 6 and are not like for like on ratio. lz4 also has a row for compress-utils at level 1, the fast mode. Go's stdlib bzip2 only decompresses, so its compress cell is empty and its input comes from compress-utils, untimed.

**zlib**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 2.51 | 52.8 / 439 | 51.8 / 445 | 70.2 / 302 | 45.6 / 193 |
| stdlib | 2.47 | 63.7 / 262 | 63.4 / 261 | 40.1 / 154 | 16.1 / 86.9 |
| klauspost/compress | 2.43 | 151 / 373 | 150 / 373 | 102 / 244 | 45.0 / 145 |

**zstd**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 2.92 | 20.8 / 2,131 | 20.7 / 1,537 | 29.9 / 891 | 27.5 / 516 |
| klauspost/compress | 2.83 | 46.5 / 1,895 | 43.9 / 1,636 | 46.7 / 767 | 30.6 / 365 |

**brotli**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 2.86 | 81.4 / 575 | 72.2 / 567 | 55.5 / 273 | 36.5 / 197 |
| andybalholm/brotli | 2.86 | 37.7 / 279 | 37.1 / 272 | 25.1 / 124 | 9.0 / 75.8 |

**lz4**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 2.22 | 75.7 / 2,680 | 69.4 / 2,210 | 248 / 2,551 | 178 / 1,780 |
| compress-utils (level 1) | 1.76 | 878 / 3,084 | 774 / 2,363 | 867 / 2,534 | 827 / 1,728 |
| pierrec/lz4 | 2.08 | 63.4 / 2,731 | 64.6 / 3,162 | 312 / 2,691 | 185 / 1,893 |

**bz2**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 3.17 | 24.8 / 64.0 | 24.9 / 64.2 | 11.0 / 46.7 | 8.1 / 35.3 |
| dsnet/compress | 2.86 | 17.2 / 64.8 | 17.3 / 64.7 | 10.3 / 52.6 | 6.7 / 35.9 |
| stdlib (decode only) | - | - / 40.0 | - / 39.3 | - / 15.7 | - / 7.0 |

**xz**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 3.32 | 5.4 / 107 | 5.4 / 107 | 12.3 / 60.5 | 7.7 / 50.7 |
| ulikunitz/xz (no levels) | 2.82 | 14.4 / 39.9 | 12.5 / 40.7 | 6.4 / 14.4 | 2.3 / 5.4 |

**snappy**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 1.77 | 637 / 2,153 | 613 / 1,617 | 814 / 2,516 | 863 / 2,817 |
| klauspost/compress (no levels) | 2.04 | 456 / 2,435 | - | 598 / 3,099 | 764 / 4,796 |

- **We win decompression of the C codecs and brotli, bzip2 and xz outright.** zlib decompress is 439 MB/s against klauspost's 373 and the stdlib's 262 (large files), 193 against 145 and 87 at 1 KB; zstd 2,131 against 1,895 (large) and 516 against 365 (1 KB). brotli is 2.1× faster both ways (81 / 575 MB/s against 38 / 279) at the same ratio. bzip2: we compress 1.4× faster than dsnet (25 against 17 MB/s) at a higher ratio (3.17 against 2.86) and decompress at the same speed; the stdlib decoder is 1.6× slower on large files and 5× slower on 1 KB messages (7 against 35 MB/s).
- **klauspost/compress compresses 2-3× faster than us at zlib and zstd, at a ratio about 3% lower:** zlib 151 against 53 MB/s on large files (102 against 70 at 4 KB; level with us at 1 KB, 45 against 46), zstd 46.5 against 20.8 (47 against 30 at 4 KB, 31 against 28 at 1 KB). This is pure Go beating the C library on compression speed by trading a little ratio; it is the thing a Go user would notice.
- **Snappy: klauspost is faster at decompression and better at ratio, we are faster at compression.** Its decoder does 2,435 MB/s on large files against our 2,153, 3,099 against 2,516 at 4 KB and 4,796 against 2,817 at 1 KB (1.7×); we compress at 637 against 456 (814 against 598 at 4 KB). Its ratio is 2.04 against 1.77 (the library's snappy encoder is s2-derived, still decodable by any snappy decoder; compress-utils decodes it). Same direction as the C and Python findings: the C port's decompressor is the slow part.
- **lz4 is a trade.** At level 6 we compress 76 MB/s against pierrec's 63 at a better ratio (2.22 against 2.08), and decompress at the same speed on large one-shot (2,680 / 2,731); pierrec is faster streaming (3,162 against 2,210), a quarter faster to compress 4 KB messages (312 against 248, at a lower ratio) and 6% faster to decompress 1 KB messages (1,893 against 1,780).
- **xz: not a like-for-like comparison** since ulikunitz/xz has no levels and its default settings give a ratio of 2.82 against our 3.32 at level 6. It compresses 2.7× faster on large files (14.4 against 5.4 MB/s) but decompresses 2.7× slower (40 against 107), and on small messages we are 3× faster at 1 KB to compress (7.7 against 2.3) and 9× to decompress (51 against 5.4).
- **cgo costs us little per call:** on 1 KB messages the Go binding is within 2-6% of the Rust binding on every format but snappy (863 / 2,817 MB/s against 1,032 / 3,343), where it is 16-19% slower.

## Rust run (2026-10-25)

compress-utils' Rust binding against flate2 (with its default miniz_oxide backend and with zlib-rs), the zstd, brotli, lz4_flex, snap, bzip2 and xz2 crates, at compress-utils levels 1, 4, 6 and 10, each competitor at the native level ours maps to, one-shot and streaming. Large files at 1 sample, everything else at 3. About 29 minutes for the main run and 3 for the zlib-rs run, on a quiet machine, against an estimate of 2-3 hours. The main run is 3,405 records and the zlib-rs run 1,416 (zlib and gzip only, with our binding and flate2's default backend interleaved, which is where the zlib rows come from); all are verified and compress-utils decodes every competitor's output. flate2 picks its backend at build time, so zlib-rs is a second binary (`--features zlib-rs`, driver `rust-flate2-zlib-rs`).

```sh
D=rust,rust-flate2,rust-zstd,rust-brotli,rust-lz4_flex,rust-snap,rust-bzip2,rust-xz2
A=zstd,brotli,zlib,gzip,bz2,lz4,xz,snappy
python3 benchmarks/runner.py --drivers $D --algos $A --levels 1,4,6,10 --modes oneshot,stream \
    --corpus silesia-mini --samples 1
python3 benchmarks/runner.py --drivers $D --algos $A --levels 1,4,6,10 --modes oneshot,stream \
    --corpus canterbury,github-users,silesia-blocks --samples 3
# zlib-rs: our binding, flate2's default backend and flate2 with zlib-rs, interleaved
D=rust,rust-flate2,rust-flate2-zlib-rs
python3 benchmarks/runner.py --drivers $D --algos zlib,gzip --levels 1,4,6,10 --modes oneshot,stream \
    --corpus silesia-mini --samples 1
python3 benchmarks/runner.py --drivers $D --algos zlib,gzip --levels 1,4,6,10 --modes oneshot,stream \
    --corpus canterbury,github-users,silesia-blocks --samples 3
```

**Results** (M-series MacBook). Same cell format as the Go and Python runs: **compress / decompress in MB/s** at level 6 for compress-utils and at the native level that maps to for the others, summed over a tier's files, with the ratio on silesia-mini. lz4_flex and snap have no levels and are compared with compress-utils at the matching level, shown as an extra row for lz4 (level 1, the fast mode).

**zlib**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 2.51 | 52.4 / 447 | 52.3 / 445 | 72.0 / 310 | 46.5 / 205 |
| flate2 (miniz_oxide) | 2.52 | 61.1 / 502 | 61.1 / 511 | 96.9 / 313 | 65.5 / 171 |
| flate2 (zlib-rs) | 2.47 | 112 / 757 | 111 / 746 | 104 / 410 | 52.5 / 224 |

**zstd**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 2.92 | 21.3 / 2,157 | 21.6 / 1,560 | 32.0 / 911 | 29.9 / 521 |
| zstd crate | 2.92 | 22.3 / 1,779 | 22.5 / 1,667 | 31.6 / 916 | 30.0 / 537 |

**brotli**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 2.86 | 81.9 / 573 | 73.2 / 573 | 59.6 / 282 | 38.6 / 202 |
| brotli crate | 2.86 | 45.5 / 574 | 44.0 / 538 | 47.4 / 221 | 26.6 / 121 |

**lz4**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 2.22 | 75.9 / 2,902 | 69.6 / 2,183 | 252 / 2,843 | 187 / 1,944 |
| compress-utils (level 1) | 1.76 | 847 / 3,197 | 763 / 2,254 | 931 / 2,682 | 921 / 1,937 |
| lz4_flex (no levels) | 1.70 | 696 / 2,185 | 692 / 2,180 | 1,002 / 2,034 | 792 / 1,331 |

**bz2**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 3.17 | 25.0 / 63.4 | 24.9 / 62.8 | 11.2 / 48.0 | 8.2 / 36.9 |
| bzip2 crate | 3.17 | 25.3 / 61.0 | 25.5 / 60.8 | 10.1 / 40.7 | 6.7 / 25.9 |

**xz**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 3.32 | 5.4 / 108 | 5.3 / 107 | 12.8 / 62.1 | 7.8 / 51.4 |
| xz2 crate | 3.32 | 5.6 / 107 | 5.3 / 108 | 12.8 / 63.4 | 7.8 / 51.8 |

**snappy**

| Implementation | ratio | large files | large, streaming | 4 KB messages | 1 KB messages |
|---|---|---|---|---|---|
| compress-utils | 1.77 | 627 / 2,157 | 588 / 1,588 | 887 / 2,843 | 1,032 / 3,343 |
| snap crate (no levels) | 1.75 | 970 / 2,250 | - | 1,207 / 3,270 | 1,538 / 4,101 |

- **zstd, bzip2 and xz are parity:** the crates are bindings to (or ports of) the same codecs. zstd 21 / 2,157 for us against 22 / 1,779 for the crate (ours decompresses large files 21% faster, small messages are equal), xz 5.4 / 108 against 5.6 / 107, bzip2 25.0 / 63 against 25.3 / 61, with our decompression 18% faster on 4 KB messages and 42% on 1 KB (37 against 26 MB/s), probably per-call setup in the pure-Rust port.
- **Deflate: we lose to both flate2 backends on compression and to zlib-rs on decompression.** miniz_oxide compresses at 61 MB/s against our 52 (65 against 46 at 1 KB) and decompresses at 502 against 447, at the same ratio. zlib-rs is 2.1× faster than us to compress (112 against 52) and 1.7× to decompress (757 against 447) on large files, at a ratio 2% lower; on 1 KB messages it compresses slower than miniz_oxide (52 against 65) and its decompression lead over us is 9% (224 against 205). This is the Rust counterpart of the C finding (zlib-ng, zlib-rs and libdeflate are all faster than the zlib we ship) and the same switch fixes it.
- **brotli: we compress 1.8× faster** (82 against 45 MB/s) at the same ratio and decompress at the same speed on large files (573 against 574), and 1.3× / 1.7× faster on 4 KB / 1 KB messages (282 against 221, 202 against 121).
- **lz4: we decompress 1.5× faster, compression is close.** lz4_flex (frame format, no levels, ratio 1.70) compresses 696 MB/s against our fast mode's 847 on large files (ratio 1.76); it is 8% faster on 4 KB messages (1,002 against 931) and 14% slower at 1 KB (792 against 921). We decompress 1.5× faster on large files (3,197 against 2,185) and on 1 KB messages (1,937 against 1,331).
- **Snappy: the pure-Rust crate is faster at everything.** snap compresses 970 MB/s against our 627 on large files (1.5×, and again at 4 KB and 1 KB: 1,207 against 887, 1,538 against 1,032), and decompresses 4% / 15% / 23% faster (large / 4 KB / 1 KB). Ratios are equal (1.75 against 1.77). Same cause as the C and Python findings, and the clearest argument for Google's snappy or a rewrite of the C port.

## C results (2026-10-22)

Run on the maintainer's M-series MacBook, single-threaded, every library a release build. Tiers: silesia-mini, Canterbury, github-users, silesia-blocks (1k-64k). Raw result files are in `benchmarks/results/` (not committed).

**Method.** Alternatives are built by `benchmarks/deps/deps.py` and run as one driver each (`bench_alt.c`); `--sweep` runs every library over its own level range. Each cell is **compress / decompress in MB/s** (uncompressed bytes per second), at level 6 for compress-utils and at the native level that maps to for the others (zlib 6, xz 5, zstd 13, brotli 6, ...), summed over the files of a tier. `ratio` is the compression ratio on silesia-mini, so you can see the libraries are doing comparable work. Libraries number levels differently, so read the ratio column: at level 6 every row is within 2% of the others except ISA-L, which tops out at level 3 (ratio 2.31). (An earlier version of this section averaged speed ratios across all ten levels at equal compression ratio, which gave smaller multiples: zlib-ng 1.4× against 2.4× at level 6 alone.) Setup-cost variants (`c-libzstd-ctx`, `c-libdeflate-percall`, `c-liblzma-fitdict`) separate codec speed from per-call setup.

**zlib**

| Implementation | ratio | large files | 4 KB messages | 1 KB messages |
|---|---|---|---|---|
| compress-utils | 2.51 | 50.3 / 470 | 69.3 / 291 | 44.7 / 186 |
| zlib | 2.51 | 50.1 / 470 | 68.8 / 291 | 44.4 / 189 |
| zlib-ng | 2.47 | 120 / 778 | 112 / 402 | 54.0 / 225 |
| zlib-rs | 2.47 | 108 / 817 | 100 / 407 | 50.5 / 226 |
| libdeflate | 2.53 | 131 / 1,164 | 140 / 573 | 81.6 / 318 |
| miniz | 2.52 | 47.9 / 549 | 81.9 / 317 | 67.7 / 179 |
| isa-l (level 3, its nearest) | 2.31 | 213 / 514 | 155 / 338 | 96.5 / 247 |

**bz2**

| Implementation | ratio | large files | 4 KB messages | 1 KB messages |
|---|---|---|---|---|
| compress-utils | 3.17 | 23.5 / 57.9 | 10.7 / 42.7 | 8.0 / 34.2 |
| libbz2 | 3.17 | 23.7 / 58.3 | 10.7 / 42.5 | 8.0 / 34.2 |
| libbz2-rs | 3.17 | 24.5 / 58.5 | 9.5 / 37.9 | 6.5 / 25.1 |

**xz**

| Implementation | ratio | large files | 4 KB messages | 1 KB messages |
|---|---|---|---|---|
| compress-utils | 3.32 | 5.1 / 107 | 10.9 / 59.3 | 5.2 / 50.5 |
| liblzma | 3.32 | 5.1 / 107 | 10.3 / 59.9 | 5.1 / 50.1 |
| lzma-sdk | 3.32 | 5.0 / 116 | 18.0 / 62.2 | 18.3 / 52.0 |

**snappy**

| Implementation | ratio | large files | 4 KB messages | 1 KB messages |
|---|---|---|---|---|
| compress-utils | 1.77 | 811 / 2,106 | 1,160 / 2,824 | 1,349 / 3,451 |
| snappy-c (no levels) | 1.77 | 827 / 2,036 | 1,121 / 2,672 | 1,345 / 3,280 |
| google-snappy (no levels) | 1.76 | 1,163 / 4,258 | 1,541 / 5,274 | 1,855 / 5,274 |

- **zlib / gzip / deflate: we ship the slowest.** On large files madler zlib does 50 MB/s compress and 470 decompress; libdeflate does 131 / 1,164 (one-shot only), zlib-ng 120 / 778 and zlib-rs 108 / 817, all at the same ratio. At 1 KB messages libdeflate does 82 / 318 against our 45 / 186. ISA-L reaches 213 / 514 at its top level, at a lower ratio. miniz is level with us on compress and 17% faster at decompress.
- **snappy:** Google's C++ snappy does 1,163 / 4,258 on large files against our C port's 811 / 2,106, and 1,855 / 5,274 against 1,349 / 3,451 on 1 KB messages, at a ratio 0.6% lower.
- **xz:** on large files the LZMA SDK is level with liblzma at compress (5.0 vs 5.1) and 8% faster at decompress (116 vs 107). On small messages it compresses 1.7× (4 KB: 18.0 vs 10.9) to 3.5× (1 KB: 18.3 vs 5.2) faster; the cause is dictionary sizing, not the codec: separately, liblzma with its dictionary capped at the input size reaches the SDK's speed at identical ratio (`c-liblzma-fitdict`).
- **bz2:** libbz2-rs is within 5% on large files and 10-27% slower on small messages. Nothing to gain.
- **zstd, lz4, brotli:** no alternative. Our wrapper matches the raw library within 1-2% with byte-identical output; the visible costs are lz4 decompression of 1 KB messages (5-12% slower) and brotli decompression (2-4%).
- **Reusing a context** saves zstd 0-8% even on 1 KB messages (it sizes its tables to the pledged input), and libdeflate 10-50% on messages of 4 KB and under, nothing from 16 KB up.

**Binary size** (KiB added to a stripped static binary, compress+decompress / decompress only): zlib 51/24 for us vs madler zlib 43/17, libdeflate 46/7, miniz 25/8, zlib-ng 85/60, ISA-L 165/144, zlib-rs 351/293; xz 117/61 vs liblzma 105/56, LZMA SDK 72/28, xz-embedded -/10; bz2 67/30 vs 60/26; snappy 11/8 vs snappy-c 4/2, Google 11/6 (+ C++ runtime); zstd 431/74 vs 401/62; lz4 110/29 vs 91/17; brotli 811/161 vs 806/157. Our default all-codec build: 1.6 MiB.

**Caveat:** the github-users tier is noisy at 3-5 samples (identical code varies 0.6-1.9× level to level) and is left out of the tables above. The proposed actions are in `TODO.md` under "Findings from the C ecosystem benchmarks".

## References

- Silesia corpus: <https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia> (fetched from the MiloszKrajewski/SilesiaCorpus mirror)
- Canterbury and Large Canterbury corpora: <https://corpus.canterbury.ac.nz/>
- enwik8: <https://mattmahoney.net/dc/textdata.html>
- zstd github-users sample set: <https://github.com/facebook/zstd/releases/tag/v1.1.3>
- `CompressionStream` format support: MDN browser-compat-data, `api/CompressionStream.json`
- lzbench block-size convention: <https://github.com/inikulin/lzbench>
