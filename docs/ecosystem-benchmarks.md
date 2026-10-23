# Ecosystem benchmarks — plan

Status: phase 1 (harness) and the C ecosystem comparison done on branch `ecosystem-benchmarks` (2026-10-22); phase 2 (Python) next. C results below. Tracked in [`TODO.md`](../TODO.md) under "Next up".

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
- **To be decided later:** Rust and Go competitor drivers, the publish-profile run, streaming beyond what each phase needs.

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

## C results (2026-10-22)

Run on the maintainer's M-series MacBook, single-threaded, every library a release build. Tiers: silesia-mini, Canterbury, github-users, silesia-blocks (1k-64k). Raw result files are in `benchmarks/results/` (not committed).

**Method additions.** Alternatives are built by `benchmarks/deps/deps.py` and run as one driver each (`bench_alt.c`). `--sweep` runs every library over its own level range. Comparisons are frontier against frontier: at each ratio our ten levels reach, each side's fastest setting that reaches it; identical code scores 1.00× on every stable tier, which validates the metric. Setup-cost variants (`c-libzstd-ctx`, `c-libdeflate-percall`, `c-liblzma-fitdict`) separate codec speed from per-call setup.

| Format | Result |
|--------|--------|
| zlib / gzip / deflate | We ship madler zlib, the slowest. libdeflate 2.7× compress / 2.45× decompress on large files, 1.8× / 1.7× on 1 KB messages (one-shot only). zlib-ng 1.4× / 1.6×, zlib-rs similar. ISA-L is a faster, lower-ratio tier (only reaches our level-1 ratio). miniz ≈ zlib. |
| snappy | Google's C++ snappy decompresses 1.5-2.0× faster than the C port we ship, compresses ~1.3-1.5× faster at 0.6% lower ratio. |
| xz | LZMA SDK ~1.15× on large files, ~1.1× decompression. Its small-input lead (up to ~8×) is dictionary sizing: liblzma with the dictionary capped at the input size matches it, at identical ratio. |
| bz2 | libbz2-rs within ±4%. Nothing to gain. |
| zstd, lz4, brotli | No alternative. Our wrapper matches the raw library within 1-2%, byte-identical output; small fixed costs on lz4 decompression of 1 KB messages (5-12%) and brotli decompression (2-4%). |
| setup cost | Reusing a context: zstd 0-8% (it sizes tables to the pledged size), libdeflate 1.2-1.5× on ≤ 4 KB messages. |

**Binary size** (KiB added to a stripped static binary, compress+decompress / decompress only): zlib 51/24 for us vs madler zlib 43/17, libdeflate 46/7, miniz 25/8, zlib-ng 85/60, ISA-L 165/144, zlib-rs 351/293; xz 117/61 vs liblzma 105/56, LZMA SDK 72/28, xz-embedded -/10; bz2 67/30 vs 60/26; snappy 11/8 vs snappy-c 4/2, Google 11/6 (+ C++ runtime); zstd 431/74 vs 401/62; lz4 110/29 vs 91/17; brotli 811/161 vs 806/157. Our default all-codec build: 1.6 MiB.

**Not yet publishable:** the github-users tier is noisy at 3-5 samples (identical code varies 0.6-1.9× level to level); rerun with ~9. The proposed actions are in `TODO.md` under "Findings from the C ecosystem benchmarks".

## References

- Silesia corpus: <https://sun.aei.polsl.pl/~sdeor/index.php?page=silesia> (fetched from the MiloszKrajewski/SilesiaCorpus mirror)
- Canterbury and Large Canterbury corpora: <https://corpus.canterbury.ac.nz/>
- enwik8: <https://mattmahoney.net/dc/textdata.html>
- zstd github-users sample set: <https://github.com/facebook/zstd/releases/tag/v1.1.3>
- `CompressionStream` format support: MDN browser-compat-data, `api/CompressionStream.json`
- lzbench block-size convention: <https://github.com/inikulin/lzbench>
