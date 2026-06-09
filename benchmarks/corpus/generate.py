#!/usr/bin/env python3
"""
Deterministic benchmark corpus generator.

We generate the corpus rather than vendoring binaries so the repo stays light
and every run is byte-reproducible (fixed seed → identical files → comparable
results across machines and over time). Each dataset targets a distinct
compressibility regime, because codec rankings flip wildly between, say,
natural-language text and already-compressed bytes.

`generate(force=False)` writes the data files plus a `manifest.json` recording
each file's sha256. The runner verifies those hashes and regenerates on
mismatch, so a corpus change is always explicit and visible in git.

Run directly to (re)generate:  python3 benchmarks/corpus/generate.py
"""

from __future__ import annotations

import hashlib
import json
import random
import struct
from pathlib import Path

CORPUS_DIR = Path(__file__).resolve().parent
DATA_DIR = CORPUS_DIR / "data"
MANIFEST = CORPUS_DIR / "manifest.json"

SEED = 0xC0FFEE  # fixed; do not change without a baseline rebuild
TARGET = 1_500_000  # ~1.5 MB per dataset: stable timing without making xz/bz2 L9 crawl

# A small vocabulary produces realistic-ish, highly-compressible prose.
_WORDS = (
    "the of and a to in is be that it for not on with as you do at this but his "
    "by from they we say her she or an will my one all would there their what so "
    "compress stream buffer ratio level codec window dictionary entropy symbol "
    "block frame header footer checksum throughput latency baseline regression"
).split()


def _text(n: int, rng: random.Random) -> bytes:
    """Natural-language-like prose. High redundancy; favors the strong coders."""
    out = []
    size = 0
    while size < n:
        words = rng.choices(_WORDS, k=rng.randint(8, 16))
        line = " ".join(words) + ".\n"
        b = line.encode()
        out.append(b)
        size += len(b)
    return b"".join(out)[:n]


def _json(n: int, rng: random.Random) -> bytes:
    """Structured records. Repetitive keys, mixed value types — log/API shaped."""
    out = [b"[\n"]
    size = 2
    i = 0
    while size < n:
        rec = (
            '  {"id": %d, "ts": %d, "level": %d, "name": "%s", '
            '"ok": %s, "ratio": %.3f},\n'
            % (
                i,
                1_700_000_000 + i,
                rng.randint(1, 10),
                rng.choice(_WORDS),
                "true" if rng.random() > 0.3 else "false",
                rng.uniform(1.0, 8.0),
            )
        )
        b = rec.encode()
        out.append(b)
        size += len(b)
        i += 1
    return b"".join(out)[:n]


def _binary(n: int, rng: random.Random) -> bytes:
    """Structured binary: columnar-ish records with smooth + noisy fields.
    Compressible but not trivially so — exercises the LZ + entropy stages."""
    out = bytearray()
    counter = 0.0
    while len(out) < n:
        counter += rng.uniform(-1.0, 1.0)
        out += struct.pack(
            "<IfHH",
            len(out),  # monotonic — very predictable
            counter,  # random walk — locally smooth
            rng.randint(0, 1023),  # bounded noise
            rng.randint(0, 65535),  # full noise
        )
    return bytes(out[:n])


def _random(n: int, rng: random.Random) -> bytes:
    """Incompressible bytes — proxy for already-compressed/encrypted data.
    The worst case: good codecs should detect it and barely expand."""
    return rng.randbytes(n)


DATASETS = {
    "text": (_text, "Natural-language-like prose (highly compressible)"),
    "json": (_json, "Structured JSON records (log/API shaped)"),
    "binary": (_binary, "Columnar binary: smooth + noisy numeric fields"),
    "random": (_random, "Incompressible random bytes (already-compressed proxy)"),
}


def generate(force: bool = False) -> dict:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    entries = []
    for name, (fn, desc) in DATASETS.items():
        path = DATA_DIR / f"{name}.bin"
        # Each dataset gets its own deterministic stream seeded off the global
        # seed + name, so adding a dataset doesn't perturb the others.
        rng = random.Random(SEED ^ (hash(name) & 0xFFFFFFFF))
        if force or not path.exists():
            data = fn(TARGET, rng)
            path.write_bytes(data)
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        entries.append(
            {
                "id": name,
                "file": f"data/{name}.bin",
                "bytes": path.stat().st_size,
                "sha256": digest,
                "description": desc,
            }
        )
    manifest = {"seed": SEED, "target_bytes": TARGET, "datasets": entries}
    MANIFEST.write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def load_manifest() -> dict | None:
    if not MANIFEST.exists():
        return None
    return json.loads(MANIFEST.read_text())


def verify() -> bool:
    """True if every manifest file exists with the recorded sha256."""
    m = load_manifest()
    if not m:
        return False
    for d in m["datasets"]:
        p = CORPUS_DIR / d["file"]
        if not p.exists():
            return False
        if hashlib.sha256(p.read_bytes()).hexdigest() != d["sha256"]:
            return False
    return True


if __name__ == "__main__":
    m = generate(force=True)
    print(f"Generated {len(m['datasets'])} datasets into {DATA_DIR}")
    for d in m["datasets"]:
        print(f"  {d['id']:8} {d['bytes']:>9,} B  {d['sha256'][:12]}…")
