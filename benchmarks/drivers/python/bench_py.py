#!/usr/bin/env python3
"""
Python benchmark driver.

Speaks the shared benchmark driver protocol (see benchmarks/README.md): reads
"<algo> <level> [<mode>] <path>" job lines from stdin, emits one NDJSON result
(or skip/error marker) per line, honours BENCH_SAMPLES / BENCH_WARMUP /
BENCH_CHUNK, and answers `--info`.

Drives the compress-utils Python binding (bindings/python) the way a consumer
would: compress/decompress for one-shot, CompressStream/DecompressStream for
streaming.
"""

from __future__ import annotations

import json
import os
import re
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / "bindings" / "python"))
import compress_utils as cu  # noqa: E402

ALGOS = {"zstd", "brotli", "zlib", "bz2", "lz4", "xz"}
SAMPLES = int(os.environ.get("BENCH_SAMPLES") or 5)
WARMUP = int(os.environ.get("BENCH_WARMUP") or 1)
CHUNK = int(os.environ.get("BENCH_CHUNK") or 64 * 1024)


def _stats(samples: list[int]) -> dict:
    s = sorted(samples)
    n = len(s)
    med = s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2
    dev = sorted(abs(x - med) for x in s)
    mad = dev[n // 2] if n % 2 else (dev[n // 2 - 1] + dev[n // 2]) / 2
    return {"median": round(med), "mad": round(mad), "min": s[0]}


def _chunks(data: bytes):
    for off in range(0, len(data), CHUNK):
        yield data[off:off + CHUNK]


def _compress(algo, data: bytes, level: int, is_stream: bool) -> bytes:
    if not is_stream:
        return cu.compress(data, algo, level)
    cs = cu.CompressStream(algo, level)
    parts = [cs.compress(c) for c in _chunks(data)]
    parts.append(cs.finish())
    return b"".join(parts)


def _decompress(algo, comp: bytes, is_stream: bool) -> bytes:
    if not is_stream:
        return cu.decompress(comp, algo)
    ds = cu.DecompressStream(algo)
    parts = [ds.decompress(c) for c in _chunks(comp)]
    parts.append(ds.finish())
    return b"".join(parts)


def _run_job(algo_name: str, level: int, is_stream: bool, path: str) -> dict:
    algo = getattr(cu.Algorithm, algo_name)
    with open(path, "rb") as f:
        data = f.read()

    comp = b""
    for _ in range(WARMUP):
        comp = _compress(algo, data, level, is_stream)
    c_t = []
    for _ in range(SAMPLES):
        t0 = time.perf_counter_ns()
        comp = _compress(algo, data, level, is_stream)
        c_t.append(time.perf_counter_ns() - t0)

    dec = b""
    for _ in range(WARMUP):
        dec = _decompress(algo, comp, is_stream)
    d_t = []
    for _ in range(SAMPLES):
        t0 = time.perf_counter_ns()
        dec = _decompress(algo, comp, is_stream)
        d_t.append(time.perf_counter_ns() - t0)

    c, d = _stats(c_t), _stats(d_t)
    return {
        "lang": "python",
        "impl": "compress-utils",
        "algo": algo_name,
        "level": level,
        "mode": "stream" if is_stream else "oneshot",
        "chunk_bytes": CHUNK if is_stream else 0,
        "input": path,
        "input_bytes": len(data),
        "output_bytes": len(comp),
        "compress_ns_median": c["median"], "compress_ns_mad": c["mad"], "compress_ns_min": c["min"],
        "decompress_ns_median": d["median"], "decompress_ns_mad": d["mad"], "decompress_ns_min": d["min"],
        "samples": SAMPLES,
        "warmup": WARMUP,
        "verified": dec == data,
    }


def _emit(obj: dict) -> None:
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


JOB_RE = re.compile(r"^(\S+)\s+(\S+)\s+(.*)$")


def main() -> None:
    if len(sys.argv) > 1 and sys.argv[1] == "--info":
        _emit({"lang": "python", "version": cu.version(), "driver": "python"})
        return

    # readline loop (not `for line in sys.stdin`) to avoid read-ahead buffering
    # that would deadlock the runner's line-synchronous protocol.
    for raw in iter(sys.stdin.readline, ""):
        line = raw.strip()
        if not line:
            continue
        m = JOB_RE.match(line)
        if not m:
            _emit({"error": True})
            continue
        algo, level_s, rest = m.group(1), m.group(2), m.group(3)
        is_stream = False
        if rest.startswith("stream "):
            is_stream, rest = True, rest[7:]
        elif rest.startswith("oneshot "):
            rest = rest[8:]
        path = rest.strip()

        if algo not in ALGOS:
            _emit({"skipped": True})
            continue
        try:
            _emit(_run_job(algo, int(level_s), is_stream, path))
        except Exception as e:  # noqa: BLE001
            sys.stderr.write(f"bench-py: {algo} L{level_s} "
                             f"{'stream' if is_stream else 'oneshot'} failed: {e}\n")
            _emit({"error": True})


if __name__ == "__main__":
    main()
