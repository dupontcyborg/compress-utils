"""
Fetched real-world corpora: Silesia and enwik8.

These are too large to vendor (Silesia ~211 MB, enwik8 100 MB), so they're
downloaded once into a gitignored cache and extracted into data/. Integrity is
pinned trust-on-first-use: the first fetch records each file's sha256 into
`fetched.lock.json` (committed), and every later run verifies against it — so a
silently-changed upstream download fails loudly instead of skewing results.

Sources:
  - enwik8  : mattmahoney.net (the canonical text-compression benchmark).
  - Silesia : per-file zips from the MiloszKrajewski/SilesiaCorpus mirror, so a
              subset can be fetched without pulling the whole corpus.
"""

from __future__ import annotations

import hashlib
import json
import urllib.request
import zipfile
from pathlib import Path

CORPUS_DIR = Path(__file__).resolve().parent
DATA_DIR = CORPUS_DIR / "data"
CACHE_DIR = CORPUS_DIR / "cache"
LOCK = CORPUS_DIR / "fetched.lock.json"

SILESIA_FILES = ["dickens", "mozilla", "mr", "nci", "ooffice", "osdb",
                 "reymont", "sao", "samba", "webster", "xml", "x-ray"]
SILESIA_BASE = "https://github.com/MiloszKrajewski/SilesiaCorpus/raw/master"
ENWIK8_URL = "https://mattmahoney.net/dc/enwik8.zip"

_CHUNK = 1 << 20


def _load_lock() -> dict:
    return json.loads(LOCK.read_text()) if LOCK.exists() else {}


def _save_lock(lock: dict) -> None:
    LOCK.write_text(json.dumps(lock, indent=2, sort_keys=True) + "\n")


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(_CHUNK), b""):
            h.update(chunk)
    return h.hexdigest()


def _download(url: str, dest: Path) -> None:
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"[corpus] downloading {url}")
    req = urllib.request.Request(url, headers={"User-Agent": "compress-utils-bench"})
    tmp = dest.with_suffix(dest.suffix + ".part")
    with urllib.request.urlopen(req) as r, open(tmp, "wb") as f:
        while True:
            chunk = r.read(_CHUNK)
            if not chunk:
                break
            f.write(chunk)
    tmp.replace(dest)


def _extract_largest(zip_path: Path, out_path: Path) -> None:
    """Extract a single-file corpus zip. Picks the largest member so a stray
    README/LICENSE in the archive never wins."""
    with zipfile.ZipFile(zip_path) as z:
        members = [n for n in z.namelist() if not n.endswith("/")]
        if not members:
            raise SystemExit(f"[corpus] {zip_path.name} has no files")
        member = max(members, key=lambda n: z.getinfo(n).file_size)
        with z.open(member) as src, open(out_path, "wb") as dst:
            while True:
                chunk = src.read(_CHUNK)
                if not chunk:
                    break
                dst.write(chunk)


def _ensure(dataset_id: str, url: str, tier: str, desc: str) -> dict:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    out_path = DATA_DIR / dataset_id
    if not out_path.exists():
        cache = CACHE_DIR / Path(url).name
        if not cache.exists():
            _download(url, cache)
        if cache.suffix == ".zip":
            _extract_largest(cache, out_path)
        else:
            out_path.write_bytes(cache.read_bytes())

    digest = _sha256(out_path)
    lock = _load_lock()
    expected = lock.get(dataset_id)
    if expected and expected != digest:
        raise SystemExit(
            f"[corpus] sha256 mismatch for '{dataset_id}': got {digest}, "
            f"locked {expected}. Delete data/{dataset_id} to re-fetch, or fix the lock."
        )
    if not expected:
        lock[dataset_id] = digest
        _save_lock(lock)
        print(f"[corpus] locked {dataset_id} sha256={digest[:12]}…")

    return {
        "id": dataset_id,
        "path": str(out_path.resolve()),
        "bytes": out_path.stat().st_size,
        "sha256": digest,
        "tier": tier,
        "description": desc,
    }


def resolve_enwik8() -> list[dict]:
    return [_ensure("enwik8", ENWIK8_URL, "enwik8", "Wikipedia text, 100 MB (enwik8)")]


def resolve_silesia() -> list[dict]:
    return [
        _ensure(name, f"{SILESIA_BASE}/{name}.zip", "silesia", f"Silesia corpus: {name}")
        for name in SILESIA_FILES
    ]
