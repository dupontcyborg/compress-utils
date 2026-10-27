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
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import bench_common as bc  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent / "corpus"))
import corpora  # noqa: E402

sys.path.insert(0, str(Path(__file__).resolve().parent / "deps"))
import deps  # noqa: E402

ALL_ALGOS = ["zstd", "brotli", "zlib", "bz2", "lz4", "xz", "snappy", "gzip"]
DEFAULT_LEVELS = [1, 3, 6, 9]

DRIVER_DIR = bc.BENCH_ROOT / "drivers" / "c"


# --------------------------------------------------------------------------- #
# C drivers. The binding driver links the prebuilt library under dist/c; the
# baseline links the vendored codecs directly (see _baseline_flags).
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


# The baseline links the vendored codecs as built for compress-utils (same
# sources, same defines), from the per-codec archives a normal CMake build
# leaves under build/algorithms/, with their public headers from third_party/.
BASELINE_CODECS = {
    # codec: (archive name, public include dir under third_party/<codec>)
    "zstd": ("zstd_library", "lib"),
    "brotli": ("brotli_library", "c/include"),
    "bz2": ("bz2_library", "."),
    "lz4": ("lz4_library", "lib"),
    "xz": ("xz_library", "src/liblzma/api"),
    "zlib": ("zlib_library", "."),
    "snappy": ("snappy_library", "."),
}


def _baseline_flags() -> tuple[list[str], list[str]]:
    build = bc.REPO_ROOT / "build" / "algorithms"
    cflags, ldflags = ["-DLZMA_API_STATIC"], []
    for codec, (archive, inc) in BASELINE_CODECS.items():
        lib = build / codec / f"lib{archive}.a"
        if not lib.exists():
            sys.exit(f"error: {lib.relative_to(bc.REPO_ROOT)} not found.\n"
                     "       It's produced by a normal build:  ./build.sh --release\n")
        # Snappy is first-party (no third_party/snappy): its core lives next to the adapter.
        root = (bc.REPO_ROOT / "src" / "algorithms" / "snappy" / "core" if codec == "snappy"
                else bc.REPO_ROOT / "third_party" / codec / inc)
        cflags.append(f"-I{root}")
        ldflags.append(str(lib))
    return cflags, ldflags


def _compile(src: Path, out: Path, cflags: list[str], ldflags: list[str]) -> Path:
    deps = [src, DRIVER_DIR / "bench_harness.h", DRIVER_DIR / "bench_zapi.h"]
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
    cflags, ldflags = _baseline_flags()
    out = _compile(
        DRIVER_DIR / "bench_baseline.c",
        DRIVER_DIR / "bench_baseline",
        cflags=cflags,
        ldflags=ldflags,
    )
    return [str(out)]


def build_c_alt(name: str, variant: str = "") -> list[str]:
    """An alternative C library (benchmarks/deps/deps.py builds it): bench_alt.c
    compiled with -DALT_<NAME>[_<VARIANT>], linking only that library."""
    dep = deps.DEPS[name]
    out_dir = deps.build(name)
    ldflags = [str(out_dir / "lib" / lib) for lib in dep["libs"]]
    if dep.get("cxx"):
        ldflags.append("-lc++" if sys.platform == "darwin" else "-lstdc++")
    key = f"{name}-{variant}" if variant else name
    macro = "ALT_" + key.upper().replace("-", "_")
    out = _compile(
        DRIVER_DIR / "bench_alt.c",
        DRIVER_DIR / f"bench_alt_{key}",
        cflags=[f"-D{macro}", f'-DALT_VERSION="{dep["version"]}"', f"-I{out_dir / 'include'}"],
        ldflags=ldflags,
    )
    return [str(out)]


def build_c_vendored_variant(codec: str, variant: str, extra: list[str] = ()) -> list[str]:
    """A vendored codec called with different settings (bench_alt.c
    ALT_<VARIANT>), linked from the same archive as c-baseline."""
    archive, inc = BASELINE_CODECS[codec]
    lib = bc.REPO_ROOT / "build" / "algorithms" / codec / f"lib{archive}.a"
    if not lib.exists():
        sys.exit(f"error: {lib} not found. Build first:  ./build.sh --release\n")
    out = _compile(
        DRIVER_DIR / "bench_alt.c",
        DRIVER_DIR / f"bench_alt_{variant}",
        cflags=[f"-DALT_{variant.upper().replace('-', '_')}", '-DALT_VERSION="vendored"',
                f"-I{bc.REPO_ROOT / 'third_party' / codec / inc}", *extra],
        ldflags=[str(lib)],
    )
    return [str(out)]


JS_DIR = bc.BENCH_ROOT / "drivers" / "wasm"
# The portable JavaScript/WASM competitors (see bench_wasm.mjs), one driver each.
JS_IMPLS = ["compression-stream", "fflate", "pako", "brotli-wasm", "zstd-wasm",
            "fzstd", "xz-decompress", "lz4js", "snappyjs"]


def build_js(impl: str = "compress-utils") -> list[str]:
    """A JavaScript driver (drivers/wasm/bench_wasm.mjs) for one implementation:
    the built compress-utils WASM package, or a competitor from
    drivers/wasm/package.json. BENCH_JS_RUNTIME (--js-runtime) picks node or bun."""
    runtime = os.environ.get("BENCH_JS_RUNTIME", "node")
    # BENCH_NODE / BENCH_BUN pick an exact binary; otherwise whichever is first on PATH
    # (which can differ from your shell's, e.g. nvm). The version lands in every record.
    exe = os.environ.get(f"BENCH_{runtime.upper()}") or shutil.which(runtime)
    if not exe:
        sys.exit(f"error: {runtime} not found on PATH")
    dist = bc.REPO_ROOT / "bindings" / "wasm" / "dist" / "algorithms"
    if not dist.joinpath("zstd", "zstd.wasm").exists():
        sys.exit("error: WASM package not built. Build it first:\n"
                 "       (cd bindings/wasm && npm run build)\n")
    if impl != "compress-utils" and not (JS_DIR / "node_modules").exists():
        print("[runner] installing the JavaScript competitors (npm ci)")
        subprocess.run(["npm", "ci", "--no-audit", "--no-fund"], cwd=JS_DIR, check=True,
                       capture_output=True)
    return [exe, str(JS_DIR / "bench_wasm.mjs"), "--impl", impl]


def build_wasm() -> list[str]:
    return build_js("compress-utils")


PY_DIR = bc.BENCH_ROOT / "drivers" / "python"
PY_VENV = PY_DIR / ".venv"
PY_BUILD = PY_DIR / "build"
# Every Python driver (compress-utils and the competitors) runs on one
# interpreter: 3.14, the first with compression.zstd in the stdlib.
PY_VERSION = "3.14"


def python_env() -> Path:
    """The benchmark interpreter: a venv on Python 3.14 with the pinned
    competitors (drivers/python/requirements.txt) and compress-utils' binding
    built from this checkout for it (an incremental CMake build in
    drivers/python/build, writing the extension into bindings/python)."""
    py = PY_VENV / "bin" / "python"
    if not py.exists():
        base = os.environ.get("BENCH_PYTHON") or shutil.which(f"python{PY_VERSION}")
        if not base:
            sys.exit(f"error: python{PY_VERSION} not found (or set BENCH_PYTHON)")
        print(f"[runner] creating {PY_VENV.relative_to(bc.REPO_ROOT)} with {base}")
        subprocess.run([base, "-m", "venv", str(PY_VENV)], check=True)
        subprocess.run([str(py), "-m", "pip", "install", "-q", "-r",
                        str(PY_DIR / "requirements.txt")], check=True)
    if not (PY_BUILD / "CMakeCache.txt").exists():
        subprocess.run(["cmake", "-S", str(bc.REPO_ROOT), "-B", str(PY_BUILD),
                        "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_PYTHON_BINDINGS=ON",
                        "-DENABLE_TESTS=OFF", f"-DPython3_EXECUTABLE={py}"],
                       check=True, capture_output=True)
    print("[runner] building the Python binding for the benchmark interpreter")
    subprocess.run(["cmake", "--build", str(PY_BUILD), "--target", "compress_utils_py", "-j"],
                   check=True, capture_output=True)
    return py


def build_python(impl: str = "compress-utils") -> list[str]:
    """A Python driver (drivers/python/bench_py.py) for one implementation:
    compress-utils' binding, or a competitor library."""
    return [str(python_env()), str(PY_DIR / "bench_py.py"), "--impl", impl]


GO_DIR = bc.BENCH_ROOT / "drivers" / "go"
GO_IMPLS = ["stdlib", "klauspost", "brotli", "lz4", "xz", "bzip2"]


def build_go(impl: str = "compress-utils") -> list[str]:
    """The Go driver (drivers/go, its own module, so the competitors stay out of the
    root go.mod) is `go build`-compiled; its replace directive builds the binding from
    this checkout, C core included, via cgo. The binary is cached; `go build` does
    its own staleness tracking, so this always shells out."""
    out = GO_DIR / "bench_go"
    print("[runner] go build in drivers/go (first run is slow: cgo compiles the C core)")
    subprocess.run(["go", "build", "-o", str(out), "."], cwd=str(GO_DIR), check=True)
    return [str(out), "--impl", impl]


RUST_DIR = bc.BENCH_ROOT / "drivers" / "rust"
RUST_IMPLS = ["flate2", "flate2-zlib-rs", "zstd", "brotli", "lz4_flex", "snap", "bzip2", "xz2"]


def build_rust(impl: str = "compress-utils") -> list[str]:
    """The Rust driver is a sibling crate (benchmarks/drivers/rust) with a path
    dependency on the binding, so it never touches the published manifest, and the
    competitor crates are its own dependencies. Like Go it compiles the C core from
    source; only cargo and a C toolchain are needed. Cargo does its own staleness
    tracking, so this always shells out."""
    print(f"[runner] cargo build --release in {RUST_DIR.name} (first run is slow)")
    # flate2's backend is a build-time choice, so zlib-rs is its own binary.
    target, extra = ("target-zlib-rs", ["--features", "zlib-rs"]) if impl == "flate2-zlib-rs" \
        else ("target", [])
    subprocess.run(["cargo", "build", "--release", "--target-dir", target, *extra],
                   cwd=str(RUST_DIR), check=True)
    return [str(RUST_DIR / target / "release" / "bench_rust"), "--impl", impl]


# The alternative C libraries, one driver each: "c-zlib-ng", "c-libdeflate", ...
ALT_DRIVERS = {f"c-{name}": (lambda name=name: build_c_alt(name))
               for name, dep in deps.DEPS.items() if dep.get("driver", True)}
# Python competitors, one driver each: "python-cramjam", "python-stdlib", ...
PY_DRIVERS = {f"python-{impl}": (lambda impl=impl: build_python(impl))
              for impl in ("cramjam", "stdlib", "zstandard", "brotli", "lz4")}

# Go and Rust competitors: "go-klauspost", "rust-flate2", ...
GO_DRIVERS = {f"go-{impl}": (lambda impl=impl: build_go(impl)) for impl in GO_IMPLS}
RUST_DRIVERS = {f"rust-{impl}": (lambda impl=impl: build_rust(impl)) for impl in RUST_IMPLS}

# JavaScript competitors: "js-fflate", "js-pako", ...
JS_DRIVERS = {f"js-{impl}": (lambda impl=impl: build_js(impl)) for impl in JS_IMPLS}

# Setup-cost variants: the same library with its setup moved in or out of each
# call, to separate codec speed from what reusing a context saves.
ALT_DRIVERS["c-libdeflate-percall"] = lambda: build_c_alt("libdeflate", "percall")
ALT_DRIVERS["c-libzstd-ctx"] = lambda: build_c_vendored_variant("zstd", "libzstd-ctx")
ALT_DRIVERS["c-liblzma-fitdict"] = lambda: build_c_vendored_variant(
    "xz", "liblzma-fitdict", ["-DLZMA_API_STATIC"])

# Drivers for native libraries take native levels: the runner translates each
# job's compress-utils level through levels.json before sending it.
NATIVE_LEVEL_DRIVERS = {"c-baseline", *ALT_DRIVERS, *PY_DRIVERS, *JS_DRIVERS, *GO_DRIVERS,
                        *RUST_DRIVERS}

DRIVERS = {
    "c": build_c,
    "c-baseline": build_c_baseline,
    "wasm": build_wasm,
    "python": build_python,
    "go": build_go,
    "rust": build_rust,
    **ALT_DRIVERS,
    **PY_DRIVERS,
    **JS_DRIVERS,
    **GO_DRIVERS,
    **RUST_DRIVERS,
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
        # A message set runs as one one-shot call per message ("msgset" mode),
        # with the one-shot runs; it has no streaming form.
        ds_modes = modes
        if ds.get("kind") == "msgset":
            ds_modes = ["msgset"] if "oneshot" in modes else []
        for algo in algos:
            for level in levels:
                for mode in ds_modes:
                    jobs.append((algo, level, mode, ds["path"], ds["id"]))
    return jobs


def _native_range(info: dict, algo: str) -> list[int]:
    """The native levels a driver supports for `algo`: the range it declares in
    --info, else levels.json's, else [0] (a format without levels)."""
    if algo in (info.get("levelless") or []):
        return [0]
    lo_hi = (info.get("levels") or {}).get(algo) or bc.level_range(algo)
    return list(range(lo_hi[0], lo_hi[1] + 1)) if lo_hi else [0]


def plan_steps(jobs: list[tuple], built: list[tuple], sweep: bool) -> list[tuple]:
    """Turn jobs into interleaved steps: (algo, mode, path, ds_id, sends), where
    sends maps a driver key to (level sent, compress-utils level or None, native
    level or None); a driver missing from sends sits that step out.

    Default: every driver runs each job's compress-utils level; a native-level
    driver gets it translated through levels.json, and sits it out if the result
    is outside its own range. With sweep, compress-utils drivers run the job
    levels and each native-level driver runs its whole native range instead,
    so every implementation gets a full ratio/speed curve; steps interleave the
    drivers level by level."""
    native_keys = [(k, info) for k, info, _ in built if k in NATIVE_LEVEL_DRIVERS]
    cu_keys = [k for k, _, _ in built if k not in NATIVE_LEVEL_DRIVERS]
    steps = []
    if not sweep:
        first = {}  # (algo, mode, path) -> the lowest requested level
        for a, lvl, mode, path, ds_id in jobs:
            first[(a, mode, path)] = min(lvl, first.get((a, mode, path), lvl))
        for a, lvl, mode, path, ds_id in jobs:
            native = bc.native_level(a, lvl)
            sends = {k: (lvl, lvl, native) for k in cu_keys}
            for k, info in native_keys:
                if a in (info.get("levelless") or []):
                    # A format without levels runs once per input, not once per level.
                    if lvl == first[(a, mode, path)]:
                        sends[k] = (0, None, 0)
                    continue
                n = native if native is not None else 0
                if n in _native_range(info, a):
                    sends[k] = (n, lvl, native)
            steps.append((a, mode, path, ds_id, sends))
        return steps

    groups: dict[tuple, list[int]] = {}
    for a, lvl, mode, path, ds_id in jobs:
        groups.setdefault((a, mode, path, ds_id), []).append(lvl)
    for (a, mode, path, ds_id), levels in groups.items():
        plans = {k: [(lvl, lvl, bc.native_level(a, lvl)) for lvl in levels] for k in cu_keys}
        for k, info in native_keys:
            plans[k] = [(n, None, n) for n in _native_range(info, a)]
        for i in range(max(map(len, plans.values()), default=0)):
            sends = {k: plan[i] for k, plan in plans.items() if i < len(plan)}
            steps.append((a, mode, path, ds_id, sends))
    return steps


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


def run_interleaved(built: list[tuple], steps: list[tuple], samples: int, warmup: int,
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
        for i, (a, mode, path, ds_id, sends) in enumerate(steps):
            for entry in procs:
                key, p, dead = entry
                if key not in sends:
                    continue
                sent, lvl, native = sends[key]
                line = f"{a} {sent} {mode} {path}\n"
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
                    print(f"[runner] {key}: job timed out ({a} L{sent} {mode}); "
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
                rec["level"] = lvl  # the compress-utils level; None in a native sweep
                rec["native_level"] = native
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
    ap.add_argument("--levels", default=None,
                    help="comma-separated compress-utils levels, 1..10 (default "
                         f"{','.join(map(str, DEFAULT_LEVELS))}; all ten with --sweep)")
    ap.add_argument("--sweep", action="store_true",
                    help="run each native-library driver over its own whole level range "
                         "(compress-utils drivers run --levels), for full ratio/speed curves")
    ap.add_argument("--modes", default="oneshot",
                    help="comma-separated modes: oneshot, stream")
    ap.add_argument("--chunk", type=int, default=64 * 1024,
                    help="streaming chunk size in bytes (stream mode only)")
    ap.add_argument("--js-runtime", default="node", choices=["node", "bun"],
                    help="runtime for the JavaScript drivers (wasm, js-*)")
    ap.add_argument("--samples", type=int, default=5)
    ap.add_argument("--warmup", type=int, default=1)
    args = ap.parse_args()

    driver_keys = [d.strip() for d in args.drivers.split(",") if d.strip()]
    for k in driver_keys:
        if k not in DRIVERS:
            sys.exit(f"error: unknown driver '{k}'. Known: {', '.join(DRIVERS)}")
    algos = [a.strip() for a in args.algos.split(",") if a.strip()]
    if args.levels is None:
        levels = list(range(1, 11)) if args.sweep else DEFAULT_LEVELS
    else:
        levels = [int(x) for x in args.levels.split(",") if x.strip()]
    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    for m in modes:
        if m not in ("oneshot", "stream"):
            sys.exit(f"error: unknown mode '{m}'. Known: oneshot, stream")

    os.environ["BENCH_JS_RUNTIME"] = args.js_runtime
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

    steps = plan_steps(jobs, built, args.sweep)
    meta = bc.RunMeta(drivers=driver_meta, corpus=args.corpus, chunk=args.chunk,
                      samples=args.samples, warmup=args.warmup, sweep=args.sweep)
    stamp = meta.timestamp.replace(":", "").replace("-", "")[:15]
    drivers_tag = "+".join(driver_keys)
    if any(k == "wasm" or k.startswith("js-") for k in driver_keys):
        drivers_tag += f"@{args.js_runtime}"
    corpus_tag = args.corpus.replace(",", "+")
    if len(drivers_tag) > 60:
        drivers_tag = f"{len(driver_keys)}drivers"
    if len(corpus_tag) > 60:
        corpus_tag = f"{len(datasets)}inputs"
    fname = f"{stamp}-{drivers_tag}-{corpus_tag}-{meta.git_sha}.json"
    path = bc.RESULTS_DIR / fname

    n_runs = sum(len(sends) for *_, sends in steps)
    print(f"[runner] drivers={','.join(driver_keys)}  {len(jobs)} specs "
          f"({len(datasets)} inputs × {len(algos)} algos × {len(levels)} levels × "
          f"{len(modes)} modes){', native sweep' if args.sweep else ''}: {n_runs} driver runs, "
          f"{args.samples} samples + {args.warmup} warmup")

    keep_awake()
    # Checkpoint progressively so a long run is never all-or-nothing.
    checkpoint = lambda recs: bc.save_results(meta, recs, path)  # noqa: E731
    all_records = run_interleaved(built, steps, args.samples, args.warmup, args.chunk, checkpoint)
    path = bc.save_results(meta, all_records, path)

    n_bad = sum(1 for r in all_records if not r.get("verified", False))
    print(f"[runner] {len(all_records)} records → {path}")
    if n_bad:
        print(f"[runner] WARNING: {n_bad} records failed round-trip verification", file=sys.stderr)
    print(f"[runner] report:  python3 benchmarks/report.py {path}")


if __name__ == "__main__":
    main()
