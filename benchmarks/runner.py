#!/usr/bin/env python3
"""
Benchmark runner — runs one or more drivers over the corpus × algo × level
matrix and writes a single merged, enriched results file.

A run can combine drivers so the report overlays them, e.g. the compress-utils
binding against its native-library baseline:

    python3 benchmarks/runner.py --drivers c,c-baseline

Adding a language is: implement the driver protocol (see README), then register
a builder in DRIVERS below. The matrix, corpus, schema, and reporting are all
language-agnostic.

Usage:
    python3 benchmarks/runner.py                          # c driver, default matrix
    python3 benchmarks/runner.py --drivers c,c-baseline     # binding + C baseline
    python3 benchmarks/runner.py --algos zstd,brotli --levels 1,9 --samples 9
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import bench_common as bc  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent / "corpus"))
import corpora  # noqa: E402

ALL_ALGOS = ["zstd", "brotli", "zlib", "bz2", "lz4", "xz", "snappy"]
DEFAULT_LEVELS = [1, 3, 6, 9]

DRIVER_DIR = bc.BENCH_ROOT / "drivers" / "c"


# --------------------------------------------------------------------------- #
# C drivers. Both link the prebuilt artifacts under dist/c (the binding) and
# algorithms/dist (the upstream static libs the binding was built from).
# --------------------------------------------------------------------------- #


def _cu_paths() -> tuple[Path, Path]:
    lib_dir = bc.REPO_ROOT / "dist" / "c" / "lib"
    inc_dir = bc.REPO_ROOT / "dist" / "c" / "include"
    if inc_dir.joinpath("compress_utils.h").exists():
        for name in ("libcompress_utils.dylib", "libcompress_utils.so"):
            if lib_dir.joinpath(name).exists():
                return lib_dir, inc_dir
    sys.exit("error: prebuilt C library not found under dist/c/.\n"
             "       Build it first:  ./build.sh --release\n")


def _baseline_paths() -> tuple[Path, Path]:
    lib_dir = bc.REPO_ROOT / "algorithms" / "dist" / "lib"
    inc_dir = bc.REPO_ROOT / "algorithms" / "dist" / "include"
    needed = ["libzstd.a", "libbrotlienc.a", "libbz2_static.a", "liblz4.a",
              "liblzma.a", "libz.a"]
    if all(lib_dir.joinpath(n).exists() for n in needed) and inc_dir.is_dir():
        return lib_dir, inc_dir
    sys.exit("error: upstream static libs not found under algorithms/dist/.\n"
             "       They are produced by a normal build:  ./build.sh --release\n")


def _compile(src: Path, out: Path, cflags: list[str], ldflags: list[str]) -> Path:
    deps = [src, DRIVER_DIR / "bench_harness.h"]
    if out.exists() and all(out.stat().st_mtime >= d.stat().st_mtime for d in deps):
        return out
    cmd = ["cc", "-O2", "-std=c11", f"-I{DRIVER_DIR}", *cflags, str(src), "-o", str(out), *ldflags]
    print(f"[runner] compiling {out.name}: {' '.join(cmd)}")
    subprocess.run(cmd, check=True)
    return out


# A driver builder returns the argv used to launch it (so C drivers are a
# compiled binary, the WASM driver is `node <script>`, future drivers whatever).


def build_c() -> list[str]:
    lib, inc = _cu_paths()
    out = _compile(
        DRIVER_DIR / "bench.c",
        DRIVER_DIR / "bench",
        cflags=[f"-I{inc}"],
        ldflags=[f"-L{lib}", "-lcompress_utils", f"-Wl,-rpath,{lib}"],
    )
    return [str(out)]


def build_c_baseline() -> list[str]:
    lib, inc = _baseline_paths()
    # brotli enc/dec depend on common → list common last for picky linkers.
    libs = ["-lzstd", "-lbrotlienc", "-lbrotlidec", "-lbrotlicommon",
            "-lbz2_static", "-llz4", "-llzma", "-lz"]
    out = _compile(
        DRIVER_DIR / "bench_baseline.c",
        DRIVER_DIR / "bench_baseline",
        cflags=[f"-I{inc}"],
        ldflags=[f"-L{lib}", *libs],
    )
    return [str(out)]


def build_wasm() -> list[str]:
    """The WASM driver runs the built compress-utils WASM package via Node."""
    script = bc.BENCH_ROOT / "drivers" / "wasm" / "bench_wasm.mjs"
    dist = bc.REPO_ROOT / "bindings" / "wasm" / "dist" / "algorithms"
    if not dist.joinpath("zstd", "zstd.wasm").exists():
        sys.exit("error: WASM package not built. Build it first:\n"
                 "       (cd bindings/wasm && npm run build)\n")
    return ["node", str(script)]


def build_python() -> list[str]:
    """The Python driver runs the compress-utils Python binding."""
    script = bc.BENCH_ROOT / "drivers" / "python" / "bench_py.py"
    pkg = bc.REPO_ROOT / "bindings" / "python"
    check = subprocess.run(
        [sys.executable, "-c", "import compress_utils"],
        env={**os.environ, "PYTHONPATH": str(pkg)}, capture_output=True,
    )
    if check.returncode != 0:
        sys.exit("error: Python binding not importable. Build it first:  ./build.sh\n")
    return [sys.executable, str(script)]


DRIVERS = {
    "c": build_c,
    "c-baseline": build_c_baseline,
    "wasm": build_wasm,
    "python": build_python,
}


def driver_info(argv: list[str]) -> dict:
    out = subprocess.run([*argv, "--info"], capture_output=True, text=True, check=True)
    return json.loads(out.stdout)


# --------------------------------------------------------------------------- #
# Corpus + matrix
# --------------------------------------------------------------------------- #


def build_jobs(datasets: list[dict], algos: list[str], levels: list[int],
               modes: list[str]) -> list[tuple]:
    jobs = []
    for ds in datasets:
        for algo in algos:
            for level in levels:
                for mode in modes:
                    jobs.append((algo, level, mode, ds["path"], ds["id"]))
    return jobs


# --------------------------------------------------------------------------- #
# Run
# --------------------------------------------------------------------------- #


# A single stuck job (pathological codec, runaway drain) must not wedge the
# whole run. Generous because xz/bz2 at high levels on 100 MB inputs over
# (warmup+samples) iterations is legitimately slow.
JOB_TIMEOUT_S = 1800


def keep_awake() -> None:
    """On macOS, prevent sleep for the lifetime of this process. A sleeping
    laptop mid-run is what made driver phases incomparable (see docs)."""
    if sys.platform != "darwin":
        return
    try:
        subprocess.Popen(["caffeinate", "-i", "-w", str(os.getpid())])
        print("[runner] caffeinate: holding off sleep for this run")
    except FileNotFoundError:
        pass


def _read_line(proc: subprocess.Popen, timeout: float) -> str | None:
    """Read one line from proc.stdout, or None on timeout/EOF."""
    import select

    ready, _, _ = select.select([proc.stdout], [], [], timeout)
    if not ready:
        return None
    return proc.stdout.readline()


def run_interleaved(built: list[tuple], jobs: list[tuple], samples: int, warmup: int,
                    chunk: int, checkpoint=None) -> list[dict]:
    """Run every driver on each job spec back-to-back, so all impls are measured
    in the same thermal window. Drivers are persistent processes; the protocol
    is line-synchronous (one job line in → exactly one result/marker line out),
    which is why the drivers emit skip/error markers.

    `built` is a list of (key, info, binary). `checkpoint(records)` is called
    periodically so a long run is never all-or-nothing.
    """
    env = {**os.environ, "BENCH_SAMPLES": str(samples), "BENCH_WARMUP": str(warmup),
           "BENCH_CHUNK": str(chunk)}
    procs = []
    for key, _info, argv in built:
        p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             text=True, bufsize=1, env=env)
        procs.append([key, p, False])  # [key, proc, dead]

    records: list[dict] = []
    try:
        for i, (a, lvl, mode, path, ds_id) in enumerate(jobs):
            line = f"{a} {lvl} {mode} {path}\n"
            for entry in procs:
                key, p, dead = entry
                if dead or p.poll() is not None:
                    entry[2] = True
                    continue
                try:
                    p.stdin.write(line)
                    p.stdin.flush()
                except BrokenPipeError:
                    entry[2] = True
                    continue
                out = _read_line(p, JOB_TIMEOUT_S)
                if out is None:
                    print(f"[runner] {key}: job timed out ({a} L{lvl} {mode}); "
                          f"killing driver", file=sys.stderr)
                    p.kill()
                    entry[2] = True
                    continue
                out = out.strip()
                if not out:
                    continue
                rec = json.loads(out)
                if rec.get("skipped") or rec.get("error"):
                    continue
                rec["input_id"] = ds_id
                records.append(rec)
            if checkpoint and (i + 1) % 64 == 0:
                checkpoint(records)
    finally:
        for key, p, _dead in procs:
            try:
                if p.stdin and not p.stdin.closed:
                    p.stdin.close()
            except Exception:
                pass
            try:
                p.wait(timeout=10)
            except Exception:
                p.kill()
    return records


def main() -> None:
    ap = argparse.ArgumentParser(description="compress-utils benchmark runner")
    ap.add_argument("--drivers", default="c",
                    help=f"comma-separated drivers to run+merge ({', '.join(DRIVERS)})")
    ap.add_argument("--corpus", default="smoke",
                    help=f"comma-separated corpus tiers ({', '.join(corpora.TIERS)}, all)")
    ap.add_argument("--algos", default=",".join(ALL_ALGOS), help="comma-separated algorithms")
    ap.add_argument("--levels", default=",".join(map(str, DEFAULT_LEVELS)),
                    help="comma-separated levels (1..10)")
    ap.add_argument("--modes", default="oneshot",
                    help="comma-separated modes: oneshot, stream")
    ap.add_argument("--chunk", type=int, default=64 * 1024,
                    help="streaming chunk size in bytes (stream mode only)")
    ap.add_argument("--samples", type=int, default=5)
    ap.add_argument("--warmup", type=int, default=1)
    args = ap.parse_args()

    driver_keys = [d.strip() for d in args.drivers.split(",") if d.strip()]
    for k in driver_keys:
        if k not in DRIVERS:
            sys.exit(f"error: unknown driver '{k}'. Known: {', '.join(DRIVERS)}")
    algos = [a.strip() for a in args.algos.split(",") if a.strip()]
    levels = [int(x) for x in args.levels.split(",") if x.strip()]
    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    for m in modes:
        if m not in ("oneshot", "stream"):
            sys.exit(f"error: unknown mode '{m}'. Known: oneshot, stream")

    datasets = corpora.resolve(args.corpus)
    jobs = build_jobs(datasets, algos, levels, modes)

    # Build every driver up front so they run interleaved per spec.
    built: list[tuple] = []
    driver_meta: list[dict] = []
    for key in driver_keys:
        argv = DRIVERS[key]()
        info = driver_info(argv)
        built.append((key, info, argv))
        driver_meta.append({"key": key, "lang": info["lang"], "version": info["version"]})

    meta = bc.RunMeta(drivers=driver_meta, corpus=args.corpus, chunk=args.chunk,
                      samples=args.samples, warmup=args.warmup)
    stamp = meta.timestamp.replace(":", "").replace("-", "")[:15]
    corpus_tag = args.corpus.replace(",", "+")
    fname = f"{stamp}-{'+'.join(driver_keys)}-{corpus_tag}-{meta.git_sha}.json"
    path = bc.RESULTS_DIR / fname

    print(f"[runner] drivers={','.join(driver_keys)}  {len(jobs)} specs × {len(built)} drivers "
          f"({len(datasets)} inputs × {len(algos)} algos × {len(levels)} levels × "
          f"{len(modes)} modes), {args.samples} samples + {args.warmup} warmup")

    keep_awake()
    # Checkpoint progressively so a long run is never all-or-nothing.
    checkpoint = lambda recs: bc.save_results(meta, recs, path)  # noqa: E731
    all_records = run_interleaved(built, jobs, args.samples, args.warmup, args.chunk, checkpoint)
    path = bc.save_results(meta, all_records, path)

    n_bad = sum(1 for r in all_records if not r.get("verified", False))
    print(f"[runner] {len(all_records)} records → {path}")
    if n_bad:
        print(f"[runner] WARNING: {n_bad} records failed round-trip verification", file=sys.stderr)
    print(f"[runner] report:  python3 benchmarks/report.py {path}")


if __name__ == "__main__":
    main()
