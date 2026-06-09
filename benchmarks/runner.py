#!/usr/bin/env python3
"""
Benchmark runner — orchestrates one driver over the corpus × algo × level
matrix and writes an enriched results file.

Today only the C driver is wired up. Adding a language is: implement the
driver protocol (see README), then register it in DRIVERS below. The matrix,
corpus, result schema, and reporting are all language-agnostic.

Usage:
    python3 benchmarks/runner.py                      # C driver, default matrix
    python3 benchmarks/runner.py --algos zstd,brotli --levels 1,9
    python3 benchmarks/runner.py --samples 9 --warmup 2
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import bench_common as bc  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent / "corpus"))
import generate as corpus  # noqa: E402

ALL_ALGOS = ["zstd", "brotli", "zlib", "bz2", "lz4", "xz"]
DEFAULT_LEVELS = [1, 3, 6, 9]


# --------------------------------------------------------------------------- #
# C driver: locate the prebuilt shared library, compile the driver against it.
# --------------------------------------------------------------------------- #


def find_c_lib() -> tuple[Path, Path]:
    """Return (lib_dir, include_dir) for the prebuilt C library, or exit with
    a build hint if it isn't there."""
    lib_dir = bc.REPO_ROOT / "dist" / "c" / "lib"
    inc_dir = bc.REPO_ROOT / "dist" / "c" / "include"
    if inc_dir.joinpath("compress_utils.h").exists():
        for name in ("libcompress_utils.dylib", "libcompress_utils.so"):
            if lib_dir.joinpath(name).exists():
                return lib_dir, inc_dir
    sys.exit(
        "error: prebuilt C library not found under dist/c/.\n"
        "       Build it first:  ./build.sh --release\n"
    )


def build_c_driver() -> Path:
    lib_dir, inc_dir = find_c_lib()
    src = bc.BENCH_ROOT / "drivers" / "c" / "bench.c"
    out = bc.BENCH_ROOT / "drivers" / "c" / "bench"

    # Rebuild only when stale.
    if out.exists() and out.stat().st_mtime >= src.stat().st_mtime:
        return out

    cc = "cc"
    cmd = [
        cc,
        "-O2",
        "-std=c11",
        f"-I{inc_dir}",
        str(src),
        "-o",
        str(out),
        f"-L{lib_dir}",
        "-lcompress_utils",
        f"-Wl,-rpath,{lib_dir}",
    ]
    print(f"[runner] compiling C driver: {' '.join(cmd)}")
    subprocess.run(cmd, check=True)
    return out


def driver_info(binary: Path) -> dict:
    out = subprocess.run([str(binary), "--info"], capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


# --------------------------------------------------------------------------- #
# Corpus + matrix
# --------------------------------------------------------------------------- #


def ensure_corpus() -> list[dict]:
    if not corpus.verify():
        print("[runner] generating corpus…")
        corpus.generate(force=True)
    m = corpus.load_manifest()
    return m["datasets"]


def build_jobs(datasets: list[dict], algos: list[str], levels: list[int]) -> list[tuple]:
    jobs = []
    for ds in datasets:
        path = (corpus.CORPUS_DIR / ds["file"]).resolve()
        for algo in algos:
            for level in levels:
                jobs.append((algo, level, str(path), ds["id"]))
    return jobs


# --------------------------------------------------------------------------- #
# Run
# --------------------------------------------------------------------------- #


def run_driver(binary: Path, jobs: list[tuple], samples: int, warmup: int) -> list[dict]:
    stdin = "".join(f"{a} {lvl} {p}\n" for (a, lvl, p, _id) in jobs)
    env = {"BENCH_SAMPLES": str(samples), "BENCH_WARMUP": str(warmup)}
    import os

    proc = subprocess.run(
        [str(binary)],
        input=stdin,
        capture_output=True,
        text=True,
        env={**os.environ, **env},
    )
    if proc.stderr:
        print(proc.stderr, end="", file=sys.stderr)
    if proc.returncode != 0:
        print(f"[runner] driver exited {proc.returncode} (partial results kept)", file=sys.stderr)

    # Map absolute input path -> corpus id so records carry the stable id.
    path_to_id = {p: _id for (_a, _l, p, _id) in jobs}
    records = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        rec = json.loads(line)
        rec["input_id"] = path_to_id.get(rec.get("input", ""), rec.get("input", ""))
        records.append(rec)
    return records


def main() -> None:
    ap = argparse.ArgumentParser(description="compress-utils benchmark runner")
    ap.add_argument("--driver", default="c", choices=["c"], help="language driver to run")
    ap.add_argument("--algos", default=",".join(ALL_ALGOS), help="comma-separated algorithms")
    ap.add_argument(
        "--levels",
        default=",".join(map(str, DEFAULT_LEVELS)),
        help="comma-separated levels (1..10)",
    )
    ap.add_argument("--samples", type=int, default=5)
    ap.add_argument("--warmup", type=int, default=1)
    args = ap.parse_args()

    algos = [a.strip() for a in args.algos.split(",") if a.strip()]
    levels = [int(x) for x in args.levels.split(",") if x.strip()]

    binary = build_c_driver()
    info = driver_info(binary)

    datasets = ensure_corpus()
    jobs = build_jobs(datasets, algos, levels)
    print(
        f"[runner] {info['lang']} driver v{info['version']}: "
        f"{len(jobs)} jobs ({len(datasets)} inputs × {len(algos)} algos × {len(levels)} levels), "
        f"{args.samples} samples + {args.warmup} warmup"
    )

    records = run_driver(binary, jobs, args.samples, args.warmup)

    meta = bc.RunMeta(
        driver_lang=info["lang"],
        driver_version=info["version"],
        samples=args.samples,
        warmup=args.warmup,
    )
    path = bc.save_results(meta, records)

    n_bad = sum(1 for r in records if not r.get("verified", False))
    print(f"[runner] {len(records)} records → {path}")
    if n_bad:
        print(f"[runner] WARNING: {n_bad} records failed round-trip verification", file=sys.stderr)
    print(f"[runner] report:  python3 benchmarks/report.py {path}")


if __name__ == "__main__":
    main()
