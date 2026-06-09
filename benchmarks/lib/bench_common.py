"""
Shared benchmark plumbing: run metadata, result schema, throughput math.

Kept dependency-free (stdlib only) so any driver's runner can import it without
a virtualenv. Plotting (report.py) is the only place matplotlib is needed.
"""

from __future__ import annotations

import json
import platform
import subprocess
from dataclasses import dataclass, field, asdict
from datetime import datetime, timezone
from pathlib import Path

SCHEMA_VERSION = 1

BENCH_ROOT = Path(__file__).resolve().parent.parent
REPO_ROOT = BENCH_ROOT.parent
RESULTS_DIR = BENCH_ROOT / "results"


# --------------------------------------------------------------------------- #
# Run metadata — pinned to every result file so cross-run comparison only ever
# happens between like machines. Throughput is meaningless across hardware;
# the machine fingerprint is the guard against accidentally comparing apples to
# oranges in regression mode.
# --------------------------------------------------------------------------- #


def git_sha() -> str:
    try:
        return subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    except Exception:
        return "unknown"


def git_dirty() -> bool:
    try:
        out = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=True,
        ).stdout
        return bool(out.strip())
    except Exception:
        return False


def cpu_brand() -> str:
    try:
        if platform.system() == "Darwin":
            return subprocess.run(
                ["sysctl", "-n", "machdep.cpu.brand_string"],
                capture_output=True,
                text=True,
                check=True,
            ).stdout.strip()
        if platform.system() == "Linux":
            for line in Path("/proc/cpuinfo").read_text().splitlines():
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except Exception:
        pass
    return platform.processor() or "unknown"


def machine_fingerprint() -> dict:
    """A stable-ish id for "the same machine". Used to gate regression
    comparisons — two runs are only comparable if these match."""
    return {
        "os": platform.system(),
        "os_release": platform.release(),
        "arch": platform.machine(),
        "cpu": cpu_brand(),
        "python": platform.python_version(),
    }


@dataclass
class RunMeta:
    schema_version: int = SCHEMA_VERSION
    timestamp: str = field(default_factory=lambda: datetime.now(timezone.utc).isoformat())
    git_sha: str = field(default_factory=git_sha)
    git_dirty: bool = field(default_factory=git_dirty)
    # A run may merge several drivers (e.g. the binding + its native baseline)
    # into one result set so the report can overlay them. Each entry:
    # {"key","lang","version"}.
    drivers: list = field(default_factory=list)
    samples: int = 5
    warmup: int = 1
    machine: dict = field(default_factory=machine_fingerprint)


# --------------------------------------------------------------------------- #
# Derived metrics. Drivers emit raw bytes + timing; throughput is computed here
# so every language reports MB/s identically. Convention (matches lzbench): MB =
# 1e6 bytes, and BOTH directions are normalized to *uncompressed* bytes ÷ time.
# --------------------------------------------------------------------------- #

MB = 1_000_000


def ratio(rec: dict) -> float:
    out = rec["output_bytes"]
    return rec["input_bytes"] / out if out else 0.0


def compress_mbps(rec: dict) -> float:
    ns = rec["compress_ns_median"]
    return (rec["input_bytes"] / MB) / (ns / 1e9) if ns else 0.0


def decompress_mbps(rec: dict) -> float:
    ns = rec["decompress_ns_median"]
    return (rec["input_bytes"] / MB) / (ns / 1e9) if ns else 0.0


def enrich(rec: dict) -> dict:
    """Attach derived fields to a raw driver record (non-destructive).

    `impl` distinguishes our binding from an ecosystem baseline for the same
    (lang, algo) — e.g. "compress-utils" vs "node:zlib" vs "zstandard". A
    driver that benchmarks a native alternative sets it; ours defaults here so
    existing drivers need no change."""
    r = dict(rec)
    r.setdefault("impl", "compress-utils")
    r["ratio"] = ratio(rec)
    r["compress_mbps"] = compress_mbps(rec)
    r["decompress_mbps"] = decompress_mbps(rec)
    return r


# --------------------------------------------------------------------------- #
# Result file IO
# --------------------------------------------------------------------------- #


def save_results(meta: RunMeta, records: list[dict], path: Path | None = None) -> Path:
    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    enriched = [enrich(r) for r in records]
    payload = {"meta": asdict(meta), "records": enriched}
    if path is None:
        stamp = meta.timestamp.replace(":", "").replace("-", "")[:15]
        path = RESULTS_DIR / f"{stamp}-{meta.driver_lang}-{meta.git_sha}.json"
    path.write_text(json.dumps(payload, indent=2) + "\n")
    # Convenience pointer for `report.py` with no args.
    (RESULTS_DIR / "latest.json").write_text(json.dumps(payload, indent=2) + "\n")
    return path


def load_results(path: Path) -> dict:
    return json.loads(path.read_text())
