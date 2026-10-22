"""
Fetched real-world corpora: Silesia, enwik8, Canterbury, Large Canterbury and
zstd's github-users sample set, plus message sets derived from them.

These are too large to vendor (Silesia ~211 MB, enwik8 100 MB), so they're
downloaded once into a gitignored cache and extracted into data/. Integrity is
pinned trust-on-first-use: the first fetch records each file's sha256 into
`fetched.lock.json` (committed), and every later run verifies against it — so a
silently-changed upstream download fails loudly instead of skewing results.

Sources:
  - enwik8  : mattmahoney.net (the canonical text-compression benchmark).
  - Silesia : per-file zips from the MiloszKrajewski/SilesiaCorpus mirror, so a
              subset can be fetched without pulling the whole corpus.
  - Canterbury and Large Canterbury : per-file from the pfalcon/canterbury-corpus
              mirror, pinned to a commit. corpus.canterbury.ac.nz sits behind a
              bot block that refuses scripted downloads.
  - github-users : zstd's dictionary-training sample set (~9K JSON records of
              ~1 KB from the GitHub API), a release asset on facebook/zstd v1.1.3.
              Stored as one message set (see msgset.py).
  - silesia-blocks-<size> : derived, no download. Each silesia-mini file cut into
              fixed-size blocks, up to BLOCK_BUDGET bytes of them per file, as
              one message set per file.
"""

from __future__ import annotations

import hashlib
import json
import tarfile
import urllib.request
import zipfile
from pathlib import Path

import msgset

CORPUS_DIR = Path(__file__).resolve().parent
DATA_DIR = CORPUS_DIR / "data"
CACHE_DIR = CORPUS_DIR / "cache"
LOCK = CORPUS_DIR / "fetched.lock.json"

SILESIA_FILES = ["dickens", "mozilla", "mr", "nci", "ooffice", "osdb",
                 "reymont", "sao", "samba", "webster", "xml", "x-ray"]
# A representative subset (~63 MB) spanning the distinct content types — English
# prose, source code, binary, database, floats, markup — for fast iteration and
# for the WASM driver, where the JS↔WASM boundary makes the full 212 MB slow.
SILESIA_MINI = ["dickens", "samba", "x-ray", "osdb", "sao", "xml"]
SILESIA_BASE = "https://github.com/MiloszKrajewski/SilesiaCorpus/raw/master"
ENWIK8_URL = "https://mattmahoney.net/dc/enwik8.zip"

CANTERBURY_BASE = ("https://raw.githubusercontent.com/pfalcon/canterbury-corpus/"
                   "c380ef9dc1dc62691757fdfeebdc2f85710e1e46")
CANTERBURY_FILES = ["alice29.txt", "asyoulik.txt", "cp.html", "fields.c", "grammar.lsp",
                    "kennedy.xls", "lcet10.txt", "plrabn12.txt", "ptt5", "sum", "xargs.1"]
CANTERBURY_LARGE_FILES = ["E.coli", "bible.txt", "world192.txt"]

GITHUB_USERS_URL = ("https://github.com/facebook/zstd/releases/download/v1.1.3/"
                    "github_users_sample_set.tar.gz")

# Message-set block sizes, and how many bytes of blocks to keep per source file:
# enough messages for stable timing (1024 at 1 KiB) while keeping the slowest
# codec settings (brotli/xz at high levels, ~0.3-0.7 ms per call) to seconds.
BLOCK_SIZES = {"1k": 1024, "4k": 4096, "16k": 16384, "64k": 65536}
BLOCK_BUDGET = 1 << 20

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

    return _locked(dataset_id, out_path, tier, desc)


def _locked(dataset_id: str, out_path: Path, tier: str, desc: str) -> dict:
    """Verify `out_path` against the trust-on-first-use lock (recording it the
    first time) and describe it as a dataset."""
    digest = _sha256(out_path)
    lock = _load_lock()
    expected = lock.get(dataset_id)
    if expected and expected != digest:
        raise SystemExit(
            f"[corpus] sha256 mismatch for '{dataset_id}': got {digest}, "
            f"locked {expected}. Delete data/{out_path.name} to re-fetch, or fix the lock."
        )
    if not expected:
        lock[dataset_id] = digest
        _save_lock(lock)
        print(f"[corpus] locked {dataset_id} sha256={digest[:12]}…")

    ds = {
        "id": dataset_id,
        "path": str(out_path.resolve()),
        "bytes": out_path.stat().st_size,
        "sha256": digest,
        "tier": tier,
        "description": desc,
        "kind": "file",
    }
    if out_path.suffix == msgset.SUFFIX:
        messages = msgset.read(out_path)
        ds.update(kind="msgset", messages=len(messages), bytes=sum(map(len, messages)))
    return ds


def resolve_enwik8() -> list[dict]:
    return [_ensure("enwik8", ENWIK8_URL, "enwik8", "Wikipedia text, 100 MB (enwik8)")]


def resolve_silesia() -> list[dict]:
    return [
        _ensure(name, f"{SILESIA_BASE}/{name}.zip", "silesia", f"Silesia corpus: {name}")
        for name in SILESIA_FILES
    ]


def resolve_silesia_mini() -> list[dict]:
    return [
        _ensure(name, f"{SILESIA_BASE}/{name}.zip", "silesia-mini", f"Silesia (mini): {name}")
        for name in SILESIA_MINI
    ]


def resolve_canterbury() -> list[dict]:
    return [
        _ensure(name, f"{CANTERBURY_BASE}/canterbury/{name}", "canterbury",
                f"Canterbury corpus: {name}")
        for name in CANTERBURY_FILES
    ]


def resolve_canterbury_large() -> list[dict]:
    return [
        _ensure(name, f"{CANTERBURY_BASE}/large/{name}", "canterbury-large",
                f"Large Canterbury corpus: {name}")
        for name in CANTERBURY_LARGE_FILES
    ]


def resolve_github_users() -> list[dict]:
    out_path = DATA_DIR / f"github-users{msgset.SUFFIX}"
    if not out_path.exists():
        DATA_DIR.mkdir(parents=True, exist_ok=True)
        cache = CACHE_DIR / Path(GITHUB_USERS_URL).name
        if not cache.exists():
            _download(GITHUB_USERS_URL, cache)
        with tarfile.open(cache) as t:
            members = sorted((m for m in t.getmembers() if m.isfile() and m.name.endswith(".json")),
                             key=lambda m: m.name)
            messages = [t.extractfile(m).read() for m in members]
        msgset.write(out_path, messages)
    return [_locked("github-users", out_path, "github-users",
                    "zstd github-users sample set: ~9K JSON records of ~1 KB")]


def resolve_silesia_blocks(label: str) -> list[dict]:
    block = BLOCK_SIZES[label]
    out = []
    for src in resolve_silesia_mini():
        dataset_id = f"{src['id']}.{label}"
        out_path = DATA_DIR / f"{dataset_id}{msgset.SUFFIX}"
        if not out_path.exists():
            data = Path(src["path"]).read_bytes()
            msgset.write(out_path, msgset.blocks(data, block, BLOCK_BUDGET))
        out.append(_locked(dataset_id, out_path, f"silesia-blocks-{label}",
                           f"Silesia {src['id']} in {block}-byte blocks"))
    return out
