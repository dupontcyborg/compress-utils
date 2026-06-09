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

Status: **C driver + C baseline implemented.** Other languages follow
the same protocol.

## Quick start

```sh
./build.sh --release                 # produces dist/c/ + algorithms/dist/ (drivers link them)
python3 benchmarks/runner.py         # runs the matrix, writes results/<…>.json
python3 benchmarks/report.py         # prints a table + writes results/plots/*.png
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

Regression diff between two runs (same machine):

```sh
python3 benchmarks/report.py new.json --baseline results/baseline-c.json
```

## Layout

```
benchmarks/
  corpus/
    generate.py      deterministic corpus (fixed seed → reproducible)
    manifest.json    per-file sha256; runner verifies + regenerates on drift
    data/            generated inputs (gitignored)
  drivers/
    c/bench_harness.h  shared C harness: timing, stats, NDJSON, job loop
    c/bench.c          compress-utils driver (wraps the cu_* ABI)
    c/bench_baseline.c  baseline: raw libzstd/libbrotli/… linked directly
  lib/
    bench_common.py  run metadata, result schema, throughput math
  runner.py          builds a driver, runs the matrix, writes results
  report.py          tables, plots, regression diff
  results/           output JSON + plots (gitignored; commit baselines explicitly)
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

## Driver protocol

Every language driver is a process that:

- reads **one job per line** from stdin: `<algo> <level> <abs_input_path>`
- writes **one NDJSON object per job** to stdout, in input order
- honors env `BENCH_SAMPLES` (default 5) and `BENCH_WARMUP` (default 1)
- prints `{"lang","version"}` and exits when invoked with a single `--info` arg

Each result object carries raw measurements; the runner enriches with derived
`ratio` / `*_mbps` and attaches run metadata:

```json
{
  "lang": "c", "algo": "zstd", "level": 6,
  "input": "/abs/path/text.bin", "input_bytes": 1500000, "output_bytes": 412345,
  "compress_ns_median": 1234567, "compress_ns_mad": 1234, "compress_ns_min": 1200000,
  "decompress_ns_median": 234567, "decompress_ns_mad": 234, "decompress_ns_min": 230000,
  "samples": 5, "warmup": 1, "verified": true
}
```

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
