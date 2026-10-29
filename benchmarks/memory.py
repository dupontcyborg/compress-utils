#!/usr/bin/env python3
"""
Memory benchmark for the C drivers: how much memory one compress and one decompress need.

Per (driver, algo, level) three jobs, each in a FRESH driver process (so an earlier job
cannot hide a peak through heap reuse): `mem` on dickens (10 MB, one-shot), `memstream` on
dickens (64 KiB chunks and window), and `mem` on its first 4 KiB (the fixed cost of one
call on a small message). The driver (bench_run_mem_job in bench_harness.h) measures the
peak resident set size of the call: VmRSS before, VmHWM reset through
/proc/self/clear_refs, one call, VmHWM after. The overhead columns subtract what the call
had to write (the compressed bytes; the decompressed bytes one-shot, one window streaming).
RSS counts touched pages only: a large dictionary the codec never fills is invisible.
Linux only (the EC2 machines); elsewhere this prints a note and exits 0.

Cost of a full default run: about 2.2 one-shot passes over what the ecosystem run measured
(compress + decompress of dickens per driver x algo x level), so roughly 5 minutes on a
c8i.xlarge and 6 on a c8g.xlarge. That is an ESTIMATE from the speed numbers in
benchmarks/results/ec2 (levels 1, 6, 10), not a measured run. Narrow with --drivers,
--algos, --levels.

    python3 benchmarks/memory.py
    python3 benchmarks/memory.py --drivers c,c-baseline --algos zstd,xz --levels 1,6
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import asdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "lib"))
sys.path.insert(0, str(HERE / "corpus"))
sys.path.insert(0, str(HERE / "deps"))
import bench_common as bc  # noqa: E402
import corpora  # noqa: E402
import runner  # noqa: E402

DEFAULT_DRIVERS = ["c", "c-baseline", "c-libdeflate", "c-zlib-ng", "c-zlib-rs", "c-isa-l",
                   "c-miniz", "c-libbz2-rs", "c-lzma-sdk", "c-liblzma-fitdict",
                   "c-libzstd-ctx", "c-google-snappy"]
DEFAULT_ALGOS = ["zstd", "brotli", "zlib", "gzip", "deflate", "bz2", "lz4", "xz", "snappy"]
SMALL_BYTES = 4096
CHUNK = 64 * 1024
JOB_TIMEOUT_S = 600
MIB = 1024  # the drivers report kB (KiB)


def small_input(big: Path) -> Path:
    """The first 4 KiB of dickens, written once next to it (gitignored like the rest of data/)."""
    small = big.with_name("dickens.first4k")
    if not small.exists() or small.stat().st_size != SMALL_BYTES:
        small.write_bytes(big.read_bytes()[:SMALL_BYTES])
    return small


def native_level(info: dict, algo: str, level: int, first: bool) -> tuple[int, int | None] | None:
    """The level to send a driver for compress-utils `level`, as the runner does: ours as is,
    a native-library driver's through levels.json. None when the driver sits it out (level
    outside its range; a levelless format runs once, at the first level)."""
    native = bc.native_level(algo, level)
    if info.get("_cu"):
        return level, native
    if algo in (info.get("levelless") or []):
        return (0, None) if first else None
    n = native if native is not None else 0
    return (n, native) if n in runner._native_range(info, algo) else None


def run_job(argv: list[str], line: str) -> dict | None:
    try:
        p = subprocess.run(argv, input=line + "\n", capture_output=True, text=True,
                           timeout=JOB_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        print(f"[memory] timed out: {' '.join(argv)} {line}", file=sys.stderr)
        return None
    out = p.stdout.strip().splitlines()
    rec = json.loads(out[0]) if out else {}
    if "mem_compress_kb" not in rec:
        if not rec.get("skipped"):
            print(f"[memory] {Path(argv[0]).name}: {line}: {p.stderr.strip() or rec}", file=sys.stderr)
        return None
    return rec


def mib(rec: dict | None, key: str) -> str:
    return f"{rec[key] / MIB:7.1f}" if rec else "      -"


def print_table(records: list[dict], drivers: list[str], algos: list[str], levels: list[int]) -> None:
    by = {(r["driver"], r["algo"], r["level"], r["input_id"], r["mode"]): r for r in records}
    print("\nOverhead in MiB (peak RSS above the RSS before the call, minus the output written);"
          " 'one' = dickens one-shot, 'strm' = dickens streaming, '4KB' = a 4 KiB message")
    for algo in algos:
        for level in levels:
            rows = []
            for d in drivers:
                cells = [by.get((d, algo, level, i, m)) for i, m in
                         (("dickens", "mem"), ("dickens", "memstream"), ("dickens.first4k", "mem"))]
                if any(cells):
                    rows.append((d, cells))
            if not rows:
                continue
            print(f"\n{algo} level {level}\n{'':20s}  decompress: one    strm     4KB"
                  f"   compress: one    strm     4KB")
            for d, cells in rows:
                print(f"{d:20s}  {'':11s}" + " ".join(mib(c, "mem_decompress_overhead_kb") for c in cells)
                      + f"   {'':9s}" + " ".join(mib(c, "mem_compress_overhead_kb") for c in cells))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--drivers", default=",".join(DEFAULT_DRIVERS),
                    help="comma-separated C drivers (a missing one is skipped, with the command that builds it)")
    ap.add_argument("--algos", default=",".join(DEFAULT_ALGOS), help="comma-separated algorithms")
    ap.add_argument("--levels", default="1,6,10", help="compress-utils levels, 1..10")
    ap.add_argument("--out", default=str(bc.RESULTS_DIR), help="results directory")
    args = ap.parse_args()

    if sys.platform != "linux":
        print("memory benchmark needs Linux /proc (clear_refs); run it on the EC2 machines")
        return

    keys = [k.strip() for k in args.drivers.split(",") if k.strip()]
    for k in keys:
        if k not in runner.DRIVERS or not k.startswith("c"):
            sys.exit(f"error: '{k}' is not a C driver. Known: {', '.join(DEFAULT_DRIVERS)}")
    algos = [a.strip() for a in args.algos.split(",") if a.strip()]
    levels = [int(x) for x in args.levels.split(",") if x.strip()]

    big = Path(corpora.resolve("ecosystem-large")[0]["path"])
    inputs = [("dickens", "mem", big), ("dickens", "memstream", big),
              ("dickens.first4k", "mem", small_input(big))]

    built = []
    for k in keys:
        try:
            argv = runner.DRIVERS[k]()
            info = runner.driver_info(argv)
        except SystemExit as e:  # the runner's own "build it first" message
            print(f"[memory] skipping {k}: {e}", file=sys.stderr)
            continue
        info["_cu"] = k not in runner.NATIVE_LEVEL_DRIVERS
        built.append((k, info, argv))
    if not built:
        sys.exit("error: no driver could be built")

    meta = bc.RunMeta(drivers=[{"key": k, "lang": i["lang"], "version": i["version"]}
                               for k, i, _ in built],
                      corpus="ecosystem-large", chunk=CHUNK, samples=1, warmup=0)
    records = []
    for k, info, argv in built:
        for algo in algos:
            for n, level in enumerate(levels):
                sent = native_level(info, algo, level, first=n == 0)
                if sent is None:
                    continue
                for input_id, mode, path in inputs:
                    rec = run_job(argv, f"{algo} {sent[0]} {mode} {path}")
                    if rec:
                        rec.update(driver=k, input_id=input_id, level=level, native_level=sent[1])
                        records.append(rec)
        print(f"[memory] {k}: done", flush=True)

    stamp = meta.timestamp.replace(":", "").replace("-", "")[:15]
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    path = out / f"{stamp}-memory-{'+'.join(k for k, _, _ in built)[:60]}-{meta.git_sha}.json"
    path.write_text(json.dumps({"meta": asdict(meta), "records": records}, indent=2) + "\n")

    print_table(records, [k for k, _, _ in built], algos, levels)
    bad = sum(1 for r in records if not r.get("verified"))
    print(f"\n[memory] {len(records)} records -> {path}")
    if bad:
        print(f"[memory] WARNING: {bad} records failed round-trip verification", file=sys.stderr)


if __name__ == "__main__":
    main()
