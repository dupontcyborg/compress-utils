# compress-utils benchmarks

A language-agnostic harness for measuring compression **ratio**, **compress /
decompress throughput**, and (for WASM) **module size** across every binding
and algorithm. Three jobs:

1. **Regression** — guard size/speed when we change build flags (e.g. the WASM
   size work in [`docs/wasm-size.md`](../docs/wasm-size.md)) or bump a codec.
2. **Per-language** — compare the same algorithm across C / Python / JS / Rust …
   on one corpus with one metric definition.
3. **Vs. ecosystem** — compare against each language's native alternatives
   (`node:zlib`, Python `zstandard`, the Rust `zstd` crate, …) on the same
   inputs. _(Baseline drivers land alongside the language drivers.)_

Status: **C, C-native baseline, WASM (Node), Python, Go, and Rust drivers
implemented** — all one-shot, streaming and message sets. Corpora: `smoke`
(synthetic), `silesia`, `silesia-mini`, `enwik8`, `canterbury`,
`canterbury-large`, `github-users` and `silesia-blocks-*`. Ecosystem-library
baselines (JS/Python native libs) are not done yet — see TODO.md.

## Language comparison

![throughput by language and algorithm](assets/lang-comparison.png)

All eight codecs across the five language bindings, measured back-to-back in
one interleaved run (Silesia-mini, level 6, one-shot). Every binding drives the
same C core, so compression **ratio is byte-identical** across languages — only
throughput differs. Relative to native C (higher = closer to the ceiling):

- **Rust ≈ native C**, 0.95–1.00× compress and 0.87–1.04× decompress. The one
  outlier is **snappy compress at 0.73×**: safe Rust cannot hand out an
  uninitialized output buffer, so the one-shot path zeroes `compress_bound`
  bytes before the call. That memset is noise next to xz, but on the fastest
  codec it is a large slice of the total.
- **Python ≈ native C**, 0.89–1.07× compress and 0.74–0.98× decompress. The
  binding is the C core via pybind11; overhead only shows on the fastest
  decoders (zlib/gzip).
- **Go pays for cgo**, 0.60–0.99× compress and 0.39–0.95× decompress. The
  per-call transition plus `runtime.LockOSThread` is a fixed cost, so it is
  invisible on xz and dominant on snappy compress and zlib/gzip decode.
- **WASM is the slowest across the board**, 0.60–0.84× compress and 0.50–1.00×
  decompress. zstd/brotli decode are the worst cases (they lean on native SIMD);
  lz4 decode is free (memory-bound, so the sandbox costs nothing).

Ratios at or slightly above 1.00× are measurement noise — those bindings run
literally the same codec code as the C driver.

Regenerate with `python3 benchmarks/plot_langs.py` after a 5-way run
(`--drivers c,wasm,python,go,rust`).

## Quick start

```sh
./build.sh --languages=c,cpp,python,wasm --cores=8   # Release by default; produces dist/ + packages the drivers link
python3 benchmarks/runner.py         # runs the matrix, writes results/<…>.json
python3 benchmarks/report.py         # prints a table + writes results/plots/*.png
```

The Go and Rust drivers need no `build.sh` step — those bindings compile the C
core themselves, so the runner just invokes `go build` / `cargo build --release`
on demand:

```sh
python3 benchmarks/runner.py --drivers c,wasm,python,go,rust
```

Overlay the binding against its native-library baseline (measures wrapper
overhead — the two should land on top of each other):

```sh
python3 benchmarks/runner.py --drivers c,c-baseline
python3 benchmarks/report.py         # plots show solid=compress-utils, dashed=baseline
```

Narrow the matrix while iterating:

```sh
python3 benchmarks/runner.py --algos zstd,brotli --levels 1,9 --samples 9 --warmup 2
```

Pick a corpus tier (default `smoke`):

```sh
python3 benchmarks/runner.py --corpus silesia          # standard real-world corpus
python3 benchmarks/runner.py --corpus smoke,enwik8     # merge tiers
```

Small messages: message-set tiers run one one-shot call per message, timed over
the whole set (see [Message sets](#message-sets)):

```sh
python3 benchmarks/runner.py --corpus github-users,silesia-blocks
```

Benchmark streaming as well as one-shot (default `oneshot`):

```sh
python3 benchmarks/runner.py --modes oneshot,stream --chunk 65536
```

Regression diff between two runs (same machine):

```sh
python3 benchmarks/report.py new.json --baseline results/baseline-c.json
```

## Layout

```
benchmarks/
  corpus/
    corpora.py       tier registry + resolve() — the runner's entry point
    generate.py      synthetic 'smoke' tier (fixed seed → reproducible)
    fetch.py         fetched tiers (download + extract + verify) and derived block tiers
    msgset.py        message-set file format (many small messages in one file)
    manifest.json    synthetic per-file sha256 (tracked)
    fetched.lock.json  sha256 lock for fetched corpora, trust-on-first-use (tracked)
    data/            generated/extracted inputs (gitignored)
    cache/           downloaded archives (gitignored)
  drivers/
    c/bench_harness.h  shared C harness: timing, stats, NDJSON, job loop
    c/bench.c          compress-utils driver (wraps the cu_* ABI)
    c/bench_baseline.c  baseline: raw libzstd/libbrotli/… linked directly (the
                        vendored codecs, from build/algorithms/)
    c/bench_alt.c       alternative C libraries, one binary per library
    c/bench_zapi.h      zlib/gzip/deflate codecs over any zlib-API library
    wasm/bench_wasm.mjs   compress-utils WASM package via Node (records module size)
    python/bench_py.py    compress-utils Python binding
    go/bench_go.go        compress-utils Go binding (cgo; `go build`)
    rust/                 compress-utils Rust binding — a sibling crate with a
                          path dependency, so the published manifest stays clean
  lib/
    bench_common.py  run metadata, result schema, throughput math
  deps/deps.py       fetch + build the alternative C libraries (benchmark-only)
  size/size.py       binary size of each library (probe.c linked statically)
  levels.json        compress-utils level → each codec's native level
  check_levels.py    verifies levels.json against the library (via c-baseline)
  runner.py          builds a driver, runs the matrix, writes results
  report.py          tables, plots, regression diff
  plot_langs.py      cross-language comparison chart for this README
  assets/            tracked chart(s) embedded above
  results/           output JSON + plots (gitignored; commit baselines explicitly)
```

## Corpora (tiers)

Selected with `--corpus` (comma-separated; `all` = every tier):

| Tier      | Contents                                            | Source | Size |
|-----------|-----------------------------------------------------|--------|------|
| `smoke`   | 4 synthetic datasets (text/json/binary/random)      | generated, fixed seed | ~6 MB |
| `silesia` | the standard 12-file real-world corpus              | fetched (per-file zips) | ~211 MB |
| `silesia-mini` | 6 Silesia files: dickens, samba, x-ray, osdb, sao, xml | fetched | ~70 MB |
| `enwik8`  | 100 MB Wikipedia text — the standard ratio benchmark | fetched | 100 MB |
| `canterbury` | the 11-file Canterbury corpus, 3.7 KB to 1 MB per file | fetched (pfalcon/canterbury-corpus mirror, pinned) | 2.8 MB |
| `canterbury-large` | E.coli, bible.txt, world192.txt | fetched (same mirror) | 11 MB |
| `github-users` | zstd's dictionary sample set: 9,114 JSON records of ~820 bytes, one message set | fetched (facebook/zstd v1.1.3 release) | 7.5 MB |
| `silesia-blocks-1k`, `-4k`, `-16k`, `-64k` | each `silesia-mini` file cut into blocks of that size, 1 MiB of evenly spaced blocks per file, one message set per file | derived from `silesia-mini` | 6 MB each |

`silesia-blocks` selects all four block sizes. The Canterbury corpora come from a mirror because corpus.canterbury.ac.nz refuses scripted downloads; the files match the mirror's SHA1SUMs.

`smoke` is the default and the **only tier suited to CI**: deterministic, no
network, fast. The fetched tiers are for on-demand local runs (slower, larger).

**Integrity.** Synthetic data is reproducible from `generate.py` and checked
against `manifest.json`. Fetched corpora are pinned **trust-on-first-use**: the
first download records each file's sha256 into `fetched.lock.json` (tracked),
and every later run verifies against it — a changed upstream download fails
loudly. Downloads cache under `corpus/cache/`; payloads extract to
`corpus/data/` (both gitignored). Delete `data/<id>` to force a re-fetch.

## Message sets

Timing one call on a 1 KB message is mostly timer noise, so small-message tiers are stored as **message sets**: one file of length-prefixed records (`u32` little-endian length, then the bytes; see `corpus/msgset.py`). The runner sends their jobs in `msgset` mode, and each sample compresses every message with its own one-shot call, then decompresses each one. The times cover the whole set, and records carry `messages` so the report can show the cost per message (`compress_ns_per_msg`, `decompress_ns_per_msg`). Decompression is given each message's size, through each binding's fastest path for a known size (`expected_size` in Python, `DecompressWithCapacity` in Go, `decompress_with_capacity` in Rust, `expectedSize` in JS); records mark this with `decompress_size_known`. The WASM driver uses the synchronous API for message sets, after `preload()`.

`report.py` adds per-message columns to the table and, for the block tiers, plots throughput and ratio against message size (`msgsize-<file>-L<level>.png`).

## Levels

`levels.json` maps compress-utils' level 1..10 to each codec's native level, as the wrappers in `src/algorithms/` do. Drivers for native libraries (listed in `NATIVE_LEVEL_DRIVERS` in `runner.py`) receive the native level instead of ours, and every record carries both `level` (compress-utils) and `native_level`. `check_levels.py` verifies the file: the native-library C baseline at the mapped levels must produce byte-identical output to compress-utils at every level.

```sh
python3 benchmarks/check_levels.py     # needs a normal build first
```

## Metric definitions

- **ratio** = uncompressed ÷ compressed bytes (higher = smaller output).
- **throughput** = uncompressed bytes ÷ median time, in MB/s (MB = 1e6),
  reported for **both** directions normalized to uncompressed size (lzbench
  convention).
- Timing wraps only the one-shot `compress` / `decompress` call. Buffer
  allocation and I/O are excluded. We report **median + MAD + min** over the
  sampled iterations after a warmup.
- Every job **round-trips and byte-compares** once; an unverified record is a
  correctness failure, not a benchmark result.

### Why three gating strategies

| metric        | determinism      | gating                                              |
|---------------|------------------|-----------------------------------------------------|
| module size   | exact            | hard CI gate, committed baselines                   |
| ratio         | ~exact per codec | cross-language invariant + strict regression (>1%)  |
| throughput    | noisy            | trend + large-move threshold (>8%), dedicated HW    |

Throughput must only be compared between runs on the **same machine** — the
runner stamps a CPU fingerprint into every result file and `report.py` warns
when a regression diff crosses machines.

**Impls are measured interleaved.** The runner runs every driver on each job
spec back-to-back (persistent processes, one job line → one result line), not
all-of-A-then-all-of-B. This keeps two impls in the same thermal/throttle
window — running them minutes or hours apart silently corrupts the comparison
(a fast codec's true overhead is ~0, so any time-skew shows up as fake
overhead). On macOS the runner also holds off sleep via `caffeinate`, and it
checkpoints the results file every 64 specs so a long run is never
all-or-nothing.

## Driver protocol

Every language driver is a process that:

- reads **one job per line** from stdin: `<algo> <level> [<mode>] <path>` where
  `mode` is `oneshot` (default if omitted), `stream` or `msgset` (`path` is a
  message set); `path` may contain spaces
- writes **one NDJSON object per job** to stdout, in input order
- honors env `BENCH_SAMPLES` (default 5), `BENCH_WARMUP` (default 1),
  `BENCH_CHUNK` (stream chunk size, default 65536)
- prints `{"lang","version","driver"}` and exits when invoked with `--info`
- for a `stream` job on an algorithm it doesn't stream, emits nothing (skip)

Each result object carries raw measurements; the runner enriches with derived
`ratio` / `*_mbps` and attaches run metadata:

```json
{
  "lang": "c", "impl": "compress-utils", "algo": "zstd", "level": 6,
  "mode": "oneshot", "chunk_bytes": 0,
  "input": "/abs/path/text.bin", "input_bytes": 1500000, "output_bytes": 412345,
  "compress_ns_median": 1234567, "compress_ns_mad": 1234, "compress_ns_min": 1200000,
  "decompress_ns_median": 234567, "decompress_ns_mad": 234, "decompress_ns_min": 230000,
  "samples": 5, "warmup": 1, "verified": true
}
```

**Modes.** One-shot times `cu_compress`/`cu_decompress`; streaming feeds the
input in `chunk`-sized pieces through the `cu_*_stream_*` drain protocol and
times the whole operation. Streaming and one-shot can produce slightly
different output for the same level (different framing), so they're tracked as
distinct series. Both the compress-utils driver and the native baseline
implement streaming (the baseline mirrors each library's native streaming API
— `ZSTD_compressStream2`, `deflate`, `BrotliEncoderCompressStream`, etc.), so
stream mode gets a full cu-vs-native overlay.

## Adding a language driver

1. Implement the protocol above (read jobs, time `samples`+`warmup`, emit
   NDJSON, support `--info`).
2. Register a builder/locator in `runner.py`'s driver dispatch.
3. The corpus, matrix, schema, tables, plots, and regression logic are reused
   as-is.

## Comparing against ecosystem baselines (goal 3)

A "baseline" is the language's native alternative for an algorithm — Python
`zstandard`, `node:zlib`, the Rust `zstd` crate, raw `libzstd` in C. These are
**in-process library calls in a sibling driver**, _not_ CLI subprocesses:

- **CLI is for interop/correctness**, which `tests/interop/cli_crosscheck.py`
  already covers — not for throughput. Shelling out measures `fork`+`exec` +
  file I/O on top of the codec; process spawn (~1–5 ms) dwarfs compressing a
  1.5 MB buffer for fast codecs, so the numbers would reflect the OS, not the
  library, and wouldn't be apples-to-apples across CLIs (differing block
  sizes, threading, buffering).
- A baseline driver runs the **same corpus, the same warmup/sample/median
  methodology, the same in-memory buffer-to-buffer timing, on the same
  machine** — the only variable is the library.

Each record carries an `impl` field distinguishing our binding from the
baseline for the same `(lang, algo)`; it defaults to `"compress-utils"` and a
baseline driver sets e.g. `"zstandard"` or `"node:zlib"`. `report.py` overlays
implementations on the Pareto plot (color = algorithm, line style = impl) and
keys regressions on it. For C specifically, the baseline is raw upstream
`libzstd`/`libbrotli`/… — it measures the binding's **wrapper overhead** and
sets the throughput ceiling.

### C: alternative libraries

Beyond `c-baseline` (the codecs we vendor, called directly), C has other implementations of the same formats. `deps/deps.py` fetches and builds them, benchmark-only and never shipped: each pinned to a release, its source archive locked by sha256 on first download (`deps/deps.lock.json`, tracked), built in release mode with its own build system.

| Driver | Library | Formats |
|--------|---------|---------|
| `c-zlib-ng` | zlib-ng 2.3.3 (native `zng_` API) | zlib, gzip, deflate |
| `c-libdeflate` | libdeflate 1.26 (one-shot only, levels 1–12) | zlib, gzip, deflate |
| `c-isa-l` | Intel ISA-L 2.32.1 igzip (levels 0–3) | zlib, gzip, deflate |
| `c-zlib-rs` | zlib-rs 0.6.8 (Rust, zlib C ABI) | zlib, gzip, deflate |
| `c-miniz` | miniz 3.1.2 (levels 1–10) | zlib, deflate |
| `c-libbz2-rs` | libbzip2-rs 0.2.5 (Rust, libbzip2 C ABI) | bz2 |
| `c-lzma-sdk` | 7-Zip LZMA SDK 26.05, single-threaded | xz |
| `c-google-snappy` | Google snappy 1.3.1 (C++, via its C API) | snappy |

All are one binary built from `drivers/c/bench_alt.c` with `-DALT_<NAME>`, linking only that library (zlib-rs and zlib export the same symbols). zstd, LZ4 and Brotli have no mainstream alternative C implementation, so for them `c-baseline` is the whole comparison. Chromium's zlib fork is left out (no standalone build outside Chromium's GN), as is Cloudflare's (archived).

```sh
python3 benchmarks/deps/deps.py --list        # what's available
python3 benchmarks/runner.py --drivers c,c-baseline,c-zlib-ng,c-libdeflate --algos zlib --sweep
```

Each driver uses its library's documented one-shot API. A library whose one-shot API takes a reusable context or work buffer (libdeflate's compressor, ISA-L's level buffer) gets one per level, reused across calls, as its documentation intends; compress-utils has no reusable context, so it pays its setup on every call, which shows on small messages.

**Sweeps.** Libraries number their levels differently (libdeflate 1–12, ISA-L 0–3, the LZMA SDK's levels pick different dictionaries than xz's presets), so comparing "level 6" across them means little. `--sweep` runs every native-library driver over its whole level range (declared in `--info`, else `levels.json`'s `range`) and compress-utils over all ten levels, and the Pareto plots then show each implementation's full ratio/speed curve. Without `--sweep`, native drivers get compress-utils' levels translated through `levels.json` and sit out levels outside their range.

### Binary size

`size/size.py` measures what each library adds to a stripped, statically linked, dead-stripped program that uses one format, compressing and decompressing or decompressing only (`size/probe.c`), against an empty program. compress-utils is built the way a size-conscious user would: a static archive with only that codec, and `-DCU_OMIT_COMPRESS` for decode-only; a separate row is the default all-codec archive. All libraries are release builds as their projects ship them (not rebuilt with `-Os`).

```sh
python3 benchmarks/size/size.py               # table + results/size-<stamp>.json
```
