#!/usr/bin/env python3
"""
vendor-codecs.py — regenerate the vendored, pre-configured upstream codec
source tree under third_party/ from the tags pinned in codec-versions.json.

This is the "configure once" step. It runs on a version bump or in CI (with
--check); it is NOT part of a normal build. After vendoring, every language
binding compiles third_party/<codec>/ directly with its own toolchain (cgo,
cmake, cc-rs, SwiftPM, zig) — no per-codec ExternalProject fetch or configure.

What it does, per codec:
  1. Obtain the upstream source at the pinned tag — either from a local
     checkout (--from-checkout, used to bootstrap without network) or by
     downloading the tag tarball (default).
  2. Copy the *curated* set of sources + headers (only what the static lib
     compiles) into third_party/<codec>/, preserving relative layout so
     internal #includes resolve.
  3. Write third_party/manifest.json: per codec { sources, include_dirs,
     defines, cxx, tag, tree_sha256 }. This is the single source of truth
     that CMake and every binding read to know how to build the codec.

Hand-authored config headers (see CONFIG_FILES) are portable, target-agnostic,
and committed by hand — this tool never fetches or overwrites them. They are
what replaces the upstream configure step for zlib/snappy.

Usage:
  tools/vendor-codecs.py                 # re-vendor all codecs (download tags)
  tools/vendor-codecs.py --from-checkout # bootstrap from algorithms/*/build
  tools/vendor-codecs.py --check         # verify committed tree matches manifest
  tools/vendor-codecs.py --codec zstd    # limit to one codec
"""

from __future__ import annotations

import argparse
import fnmatch
import hashlib
import io
import json
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
THIRD_PARTY = REPO / "third_party"
MANIFEST = THIRD_PARTY / "manifest.json"
VERSIONS = REPO / "codec-versions.json"

# ---------------------------------------------------------------------------
# Per-codec curation specs.
#
#   root            : path within the upstream checkout that becomes the codec
#                     root under third_party/<codec>/ (usually "").
#   source_dirs     : dirs (relative to root) to scan for *.c/*.cc sources.
#   source_files    : explicit sources (relative to root) — used when a dir
#                     mixes library and CLI/test sources.
#   source_excludes : basename globs dropped from source_dirs (arch asm, the
#                     HAVE_SMALL variants, thread-only files, codegen tools).
#   header_dirs     : dirs whose *.h/*.inc are copied wholesale (headers are
#                     cheap and internal cross-dir #includes need them).
#   include_dirs    : -I paths (relative to third_party/<codec>) for consumers.
#   defines         : uniform, target-agnostic -D list (see plan; xz's platform
#                     macros are intentionally dropped for the portable path).
#   cxx             : true if the codec has C++ translation units (snappy).
# ---------------------------------------------------------------------------

SPECS: dict[str, dict] = {
    "zstd": {
        "root": "",
        # Drop ZSTD_MULTITHREAD (we never expose MT compression) so there is no
        # pthread dependency and one build serves native + wasm. ZSTD_DISABLE_ASM
        # drops the .S path (portable across msvc/wasm).
        "source_dirs": ["lib/common", "lib/compress", "lib/decompress"],
        "source_excludes": [],
        "header_dirs": ["lib", "lib/common", "lib/compress", "lib/decompress"],
        "include_dirs": ["lib"],
        "defines": ["XXH_NAMESPACE=ZSTD_", "ZSTD_LEGACY_SUPPORT=0", "ZSTD_DISABLE_ASM"],
        "cxx": False,
    },
    "brotli": {
        "root": "",
        "source_dirs": ["c/common", "c/enc", "c/dec"],
        "source_excludes": [],
        "header_dirs": ["c/common", "c/enc", "c/dec"],
        "header_dirs_rec": ["c/include"],
        "include_dirs": ["c/include"],
        # BROTLI_STATIC_INIT_EARLY (=1): compute the large lookup/dictionary
        # tables at load time via a constructor instead of embedding them. Cuts
        # the binary ~240 KB (critical for the wasm size budget) and is portable
        # — the wasm reactor's _initialize runs the constructor. Uniform across
        # targets; excludes the lazy .cc variant (see source glob rules).
        "defines": ["BROTLI_STATIC_INIT=1"],
        "cxx": False,
    },
    "zlib": {
        "root": "",
        "source_dirs": ["."],
        # zlib ships CLI/example sources under test/, never at the root, so the
        # top-level *.c glob is exactly the library set.
        "source_excludes": [],
        "header_dirs": ["."],
        "include_dirs": ["."],
        "defines": [],
        "cxx": False,
    },
    "bz2": {
        "root": "",
        "source_files": [
            "blocksort.c", "huffman.c", "crctable.c", "randtable.c",
            "compress.c", "decompress.c", "bzlib.c",
        ],
        "header_dirs": ["."],
        "include_dirs": ["."],
        "defines": [],
        "cxx": False,
    },
    "lz4": {
        "root": "",
        "source_dirs": ["lib"],
        "source_excludes": [],
        "header_dirs": ["lib"],
        "include_dirs": ["lib"],
        "defines": ["XXH_NAMESPACE=LZ4_"],
        "cxx": False,
    },
    "xz": {
        "root": "",
        "source_dirs": [
            "src/liblzma/common", "src/liblzma/check", "src/liblzma/lz",
            "src/liblzma/rangecoder", "src/liblzma/lzma", "src/liblzma/delta",
            "src/liblzma/simple",
        ],
        "source_files": ["src/common/tuklib_physmem.c"],
        # Thread-only (_mt, outqueue, hardware_cputhreads), the HAVE_SMALL
        # variants, table generators, and hand-written asm are not part of the
        # portable single-threaded library.
        "source_excludes": [
            "*_small.c", "*_tablegen.c", "*_mt.c",
            "hardware_cputhreads.c", "outqueue.c",
        ],
        "header_dirs": [
            "src/liblzma/common", "src/liblzma/check",
            "src/liblzma/lz", "src/liblzma/rangecoder", "src/liblzma/lzma",
            "src/liblzma/delta", "src/liblzma/simple", "src/common",
        ],
        "header_dirs_rec": ["src/liblzma/api"],
        "include_dirs": [
            "src/liblzma/api", "src/liblzma/common", "src/liblzma/check",
            "src/liblzma/lz", "src/liblzma/rangecoder", "src/liblzma/lzma",
            "src/liblzma/delta", "src/liblzma/simple", "src/common",
        ],
        # Uniform, target-agnostic define set. Deterministic feature toggles
        # (encoders/decoders/checks/match-finders) plus C11-universal macros and
        # clang/gcc builtins present on every target we compile for. Platform
        # macros (HAVE_ARM64_CRC32, HAVE_CLOCK_*, *_SYSCTL/SYSCONF, _GNU_SOURCE)
        # are dropped: liblzma falls back to portable generic paths.
        "defines": [
            "HAVE_STDBOOL_H", "HAVE__BOOL", "HAVE_STDINT_H", "HAVE_INTTYPES_H",
            "HAVE___BUILTIN_BSWAPXX", "HAVE___BUILTIN_ASSUME_ALIGNED",
            "HAVE_FUNC_ATTRIBUTE_CONSTRUCTOR",
            "TUKLIB_FAST_UNALIGNED_ACCESS", "TUKLIB_SYMBOL_PREFIX=lzma_",
            "HAVE_ENCODERS", "HAVE_DECODERS",
            "HAVE_ENCODER_LZMA1", "HAVE_ENCODER_LZMA2", "HAVE_ENCODER_DELTA",
            "HAVE_ENCODER_ARM", "HAVE_ENCODER_ARM64", "HAVE_ENCODER_ARMTHUMB",
            "HAVE_ENCODER_IA64", "HAVE_ENCODER_POWERPC", "HAVE_ENCODER_RISCV",
            "HAVE_ENCODER_SPARC", "HAVE_ENCODER_X86",
            "HAVE_DECODER_LZMA1", "HAVE_DECODER_LZMA2", "HAVE_DECODER_DELTA",
            "HAVE_DECODER_ARM", "HAVE_DECODER_ARM64", "HAVE_DECODER_ARMTHUMB",
            "HAVE_DECODER_IA64", "HAVE_DECODER_POWERPC", "HAVE_DECODER_RISCV",
            "HAVE_DECODER_SPARC", "HAVE_DECODER_X86",
            "HAVE_CHECK_CRC32", "HAVE_CHECK_CRC64", "HAVE_CHECK_SHA256",
            "HAVE_MF_HC3", "HAVE_MF_HC4", "HAVE_MF_BT2", "HAVE_MF_BT3", "HAVE_MF_BT4",
            "HAVE_LZIP_DECODER",
            'PACKAGE_NAME="XZ Utils"',
            'PACKAGE_BUGREPORT="xz@tukaani.org"',
            'PACKAGE_URL="https://tukaani.org/xz/"',
        ],
        "cxx": False,
    },
    "snappy": {
        "root": "",
        "source_files": [
            "snappy.cc", "snappy-c.cc", "snappy-sinksource.cc",
            "snappy-stubs-internal.cc",
        ],
        "header_files": [
            "snappy.h", "snappy-c.h", "snappy-internal.h",
            "snappy-sinksource.h", "snappy-stubs-internal.h",
        ],
        "include_dirs": ["."],
        "defines": ["HAVE_CONFIG_H"],
        "cxx": True,
    },
}

# Hand-authored, portable config headers committed by hand. Never fetched or
# overwritten by this tool; they replace the upstream configure step.
CONFIG_FILES = {
    "zlib": ["zconf.h"],
    "snappy": ["config.h", "snappy-stubs-public.h"],
    # bz2 (1.0.8) needs no generated config — its version string is inline in
    # bzlib.c, so there is nothing to hand-author.
}

# gzip is zlib in a different wire format; it has no upstream of its own and no
# manifest entry — the CMake/bindings reuse zlib's vendored tree.

CHECKOUT_TMPL = "algorithms/{codec}/build/src/{codec}_external"

# GitHub/GitLab release tarball URL patterns, derived from codec-versions.json.
def tarball_url(url: str, tag: str) -> str:
    u = url.removesuffix(".git")
    if "github.com" in u:
        return f"{u}/archive/refs/tags/{tag}.tar.gz"
    if "gitlab.com" in u:
        name = u.rsplit("/", 1)[-1]
        return f"{u}/-/archive/{tag}/{name}-{tag}.tar.gz"
    raise SystemExit(f"unknown forge for {url}; add a tarball pattern")


def load_versions() -> dict:
    return {k: v for k, v in json.loads(VERSIONS.read_text()).items()
            if not k.startswith("_")}


def fetch_to_temp(codec: str, meta: dict, tmp: Path) -> Path:
    """Download + extract the tag tarball; return the extracted source root."""
    url = tarball_url(meta["url"], meta["tag"])
    print(f"  fetching {url}")
    with urllib.request.urlopen(url) as resp:  # noqa: S310 (trusted forge URLs)
        data = resp.read()
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tf:
        tf.extractall(tmp)  # noqa: S202 (trusted upstreams)
    entries = [p for p in tmp.iterdir() if p.is_dir()]
    if len(entries) != 1:
        raise SystemExit(f"{codec}: expected one top dir in tarball, got {entries}")
    return entries[0]


def iter_curated(src_root: Path, spec: dict):
    """Yield (abs_path, rel_path) for every curated source + header file."""
    root = src_root / spec.get("root", "")
    excludes = spec.get("source_excludes", [])

    def excluded(name: str) -> bool:
        return any(fnmatch.fnmatch(name, pat) for pat in excludes)

    # sources from directories. Only glob C++ (.cc) for C++ codecs — otherwise a
    # C codec's stray .cc variant (e.g. brotli's static_init_lazy.cc) gets pulled
    # in and fails to compile.
    src_globs = ["*.c"] + (["*.cc"] if spec.get("cxx") else [])
    for d in spec.get("source_dirs", []):
        base = root / d
        for pat in src_globs:
            for p in sorted(base.glob(pat)):
                if not excluded(p.name):
                    yield p, p.relative_to(root)
    # explicit sources
    for f in spec.get("source_files", []):
        p = root / f
        if not p.exists():
            raise SystemExit(f"missing source {f} under {root}")
        yield p, Path(f)
    # headers from directories (non-recursive: avoids pulling sibling dirs like
    # zstd/legacy or zlib/contrib that share a parent with the curated sources)
    for d in spec.get("header_dirs", []):
        base = root / d
        for pat in ("*.h", "*.inc"):
            for p in sorted(base.glob(pat)):
                yield p, p.relative_to(root)
    # headers from directories, recursive (for public include trees whose every
    # child is wanted, e.g. brotli c/include, xz src/liblzma/api)
    for d in spec.get("header_dirs_rec", []):
        base = root / d
        for pat in ("*.h", "*.inc"):
            for p in sorted(base.rglob(pat)):
                yield p, p.relative_to(root)
    # explicit headers
    for f in spec.get("header_files", []):
        p = root / f
        if p.exists():
            yield p, Path(f)


def is_source(rel: Path) -> bool:
    return rel.suffix in (".c", ".cc")


def sha256_tree(dest: Path, files: list[Path]) -> str:
    h = hashlib.sha256()
    for rel in sorted(files, key=str):
        h.update(str(rel).encode())
        h.update((dest / rel).read_bytes())
    return h.hexdigest()


def vendor_codec(codec: str, meta: dict, spec: dict, src_root: Path) -> dict:
    dest = THIRD_PARTY / codec
    config = set(CONFIG_FILES.get(codec, []))

    # Wipe everything except the hand-authored config headers, then prune the
    # empty directory skeletons left behind (e.g. zstd/legacy, zlib/contrib).
    if dest.exists():
        for p in sorted(dest.rglob("*")):
            if p.is_file() and p.relative_to(dest).as_posix() not in config:
                p.unlink()
        for p in sorted(dest.rglob("*"), key=lambda x: len(x.parts), reverse=True):
            if p.is_dir() and not any(p.iterdir()):
                p.rmdir()
    dest.mkdir(parents=True, exist_ok=True)

    copied: list[Path] = []
    sources: list[str] = []
    for abs_path, rel in iter_curated(src_root, spec):
        if rel.as_posix() in config:
            continue  # never overwrite hand-authored config
        out = dest / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(abs_path, out)
        copied.append(rel)
        if is_source(rel):
            sources.append(rel.as_posix())

    # Config headers count toward the tree hash + are shipped.
    for c in sorted(config):
        if (dest / c).exists():
            copied.append(Path(c))

    return {
        "tag": meta["tag"],
        "url": meta["url"],
        "cxx": spec["cxx"],
        "include_dirs": spec["include_dirs"],
        "defines": spec["defines"],
        "config_headers": sorted(config),
        "sources": sorted(sources),
        "tree_sha256": sha256_tree(dest, copied),
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--from-checkout", action="store_true",
                    help="curate from algorithms/*/build checkouts (no network)")
    ap.add_argument("--check", action="store_true",
                    help="verify committed tree matches manifest; nonzero on drift")
    ap.add_argument("--codec", help="limit to a single codec")
    args = ap.parse_args()

    versions = load_versions()
    codecs = [args.codec] if args.codec else list(SPECS)

    if args.check:
        manifest = json.loads(MANIFEST.read_text())
        bad = False
        for codec in codecs:
            entry = manifest["codecs"].get(codec)
            if not entry:
                print(f"FAIL {codec}: not in manifest"); bad = True; continue
            files = [Path(s) for s in entry["sources"]] + \
                    [Path(c) for c in entry.get("config_headers", [])]
            # include headers present on disk in the hash by re-listing them
            dest = THIRD_PARTY / codec
            files = [p.relative_to(dest) for p in sorted(dest.rglob("*")) if p.is_file()]
            got = sha256_tree(dest, files)
            if got != entry["tree_sha256"]:
                print(f"FAIL {codec}: tree hash drift (re-run vendor-codecs.py)")
                bad = True
            else:
                print(f"ok   {codec} @ {entry['tag']}")
        return 1 if bad else 0

    THIRD_PARTY.mkdir(exist_ok=True)
    manifest = {"codecs": {}}
    if MANIFEST.exists():
        manifest = json.loads(MANIFEST.read_text())

    for codec in codecs:
        meta = versions[codec]
        spec = SPECS[codec]
        print(f"vendoring {codec} @ {meta['tag']}")
        if args.from_checkout:
            src_root = REPO / CHECKOUT_TMPL.format(codec=codec)
            if not src_root.exists():
                raise SystemExit(f"{codec}: checkout {src_root} not found; "
                                 f"build once or drop --from-checkout")
            entry = vendor_codec(codec, meta, spec, src_root)
        else:
            with tempfile.TemporaryDirectory() as td:
                src_root = fetch_to_temp(codec, meta, Path(td))
                entry = vendor_codec(codec, meta, spec, src_root)
        manifest["codecs"][codec] = entry
        print(f"  {len(entry['sources'])} sources, "
              f"{len(entry['include_dirs'])} include dirs")

    # Recompute hashes for config headers now that they're on disk.
    MANIFEST.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"wrote {MANIFEST.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
