#!/usr/bin/env python3
"""
Check levels.json against the library.

Runs the compress-utils C driver and the native-library baseline (c-baseline)
at every level 1..10. The runner sends the baseline the native level from
levels.json, so the two must produce byte-identical output: any difference
means levels.json (or the baseline's frame settings) has drifted from the
wrappers in src/algorithms/.

Needs a normal build first (./build.sh). Exits non-zero on any mismatch.

Usage:  python3 benchmarks/check_levels.py
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import runner  # noqa: E402

# The codecs c-baseline implements (snappy has no levels, so only one setting).
ALGOS = ["zstd", "brotli", "zlib", "gzip", "deflate", "bz2", "lz4", "xz", "snappy"]


def main() -> int:
    datasets = [ds for ds in runner.corpora.resolve("smoke") if ds["id"] in ("text", "json")]
    jobs = runner.build_jobs(datasets, ALGOS, list(range(1, 11)), ["oneshot"])
    built = [(key, runner.driver_info(argv), argv)
             for key, argv in ((k, runner.DRIVERS[k]()) for k in ("c", "c-baseline"))]
    steps = runner.plan_steps(jobs, built, sweep=False)
    records = runner.run_interleaved(built, steps, samples=1, warmup=1, chunk=64 * 1024)

    sizes: dict[tuple, dict[str, int]] = {}
    for r in records:
        if not r.get("verified"):
            print(f"FAIL {r['impl']} {r['algo']} L{r['level']} {r['input_id']}: not verified")
            return 1
        sizes.setdefault((r["algo"], r["level"], r["input_id"]), {})[r["impl"]] = r["output_bytes"]

    bad = [(k, v) for k, v in sorted(sizes.items()) if len(v) != 2 or len(set(v.values())) != 1]
    for (algo, level, ds), v in bad:
        print(f"MISMATCH {algo} L{level} (native {runner.bc.native_level(algo, level)}) {ds}: {v}")
    print(f"{len(sizes)} settings checked, {len(bad)} mismatches")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
