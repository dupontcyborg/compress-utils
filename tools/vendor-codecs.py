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
what replaces the upstream configure step for zlib.

Source patches for the WASM build live in third_party/patches/<codec>/*.patch.
They are never applied to third_party/<codec>/ (which stays verbatim upstream);
cmake/Vendor.cmake applies them to a build-directory copy. This tool only
verifies that they still apply cleanly (re-vendoring and --check), and a patch
that no longer applies is an error that needs a manual fix.

Source patches for the WASM build live in third_party/patches/<codec>/*.patch.
They are never applied to third_party/<codec>/ (which stays verbatim upstream);
cmake/Vendor.cmake applies them to a build-directory copy. This tool only
verifies that they still apply cleanly (re-vendoring and --check), and a patch
that no longer applies is an error that needs a manual fix.

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
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.parse
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
THIRD_PARTY = REPO / "third_party"
MANIFEST = THIRD_PARTY / "manifest.json"
VERSIONS = REPO / "codec-versions.json"
PATCHES = THIRD_PARTY / "patches"

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
        # No uniform defines. BROTLI_STATIC_INIT_EARLY (compute tables at load
        # instead of embedding them, ~240 KB smaller) is needed only for the
        # wasm size budget and relies on a constructor MSVC can't parse — so it
        # is applied to the wasm build alone (see bindings/wasm/CMakeLists.txt),
        # not here. Native keeps brotli's default (embedded tables), which every
        # compiler accepts.
        "defines": [],
        "cxx": False,
    },
    "zlib": {
        # zlib-ng, built in zlib-compat mode (ZLIB_COMPAT) so the deflate
        # backend keeps calling the classic zlib API. Upstream needs CMake to
        # generate its headers, pick per-arch sources, and pass per-file SIMD
        # flags; zlib_ng_finish() replaces that (see its docstring). Every
        # source the manifest lists is a generated wrapper under cu/.
        "root": "",
        "source_dirs": [".", "arch/generic", "arch/x86", "arch/arm"],
        # gz* (file API) and infback are unused by compress-utils;
        # slide_hash_armv6 is 32-bit ARM only (we enable SIMD on AArch64 only).
        "source_excludes": ["gz*.c", "gz*.h", "infback.c", "slide_hash_armv6.c"],
        "header_dirs": [".", "arch/generic", "arch/x86", "arch/arm"],
        "include_dirs": ["."],
        "defines": [],
        "cxx": False,
        "zlib_ng": True,
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
        # variants, table/constant generators, and hand-written asm are not part
        # of the portable single-threaded library. `*_gen.c` covers every
        # generator: they are standalone programs with their own main(), so one
        # slipping in drags a second entry point into the static lib. 5.8 named
        # the CLMUL one crc_clmul_consts_gen.c, but `*_gen.c` does NOT match
        # `*_tablegen.c` (no underscore before "gen") — keep both patterns.
        "source_excludes": [
            "*_small.c", "*_gen.c", "*_tablegen.c", "*_mt.c",
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
        # Uniform, target-agnostic define set: deterministic feature toggles
        # (encoders/decoders/checks/match-finders) plus C11-universal header
        # macros. Compiler-specific knobs are intentionally NOT set so liblzma
        # picks its own portable fallback per compiler:
        #   - HAVE___BUILTIN_BSWAPXX / HAVE___BUILTIN_ASSUME_ALIGNED /
        #     HAVE_FUNC_ATTRIBUTE_CONSTRUCTOR are gcc/clang-only. MSVC has no
        #     __builtin_bswap* (link error) or __attribute__((constructor)), so
        #     without them tuklib falls to its _MSC_VER byteswap path and skips
        #     the ctor; gcc/clang use a portable manual byteswap (byte-swapping
        #     isn't a hot path for us).
        #   - Platform macros (HAVE_ARM64_CRC32, HAVE_CLOCK_*, *_SYSCTL/SYSCONF,
        #     _GNU_SOURCE) are dropped for the portable generic paths.
        "defines": [
            "HAVE_STDBOOL_H", "HAVE__BOOL", "HAVE_STDINT_H", "HAVE_INTTYPES_H",
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
}

# Hand-authored, portable config headers committed by hand. Never fetched or
# overwritten by this tool; they replace the upstream configure step.
CONFIG_FILES = {
    "zlib": ["cu_zng_config.h"],
    # bz2 (1.0.8) needs no generated config — its version string is inline in
    # bzlib.c, so there is nothing to hand-author.
}

# gzip is zlib in a different wire format; it has no upstream of its own and no
# manifest entry — the CMake/bindings reuse zlib's vendored tree.

CHECKOUT_TMPL = "algorithms/{codec}/build/src/{codec}_external"

# GitHub/GitLab release tarball URL patterns, derived from codec-versions.json.
def tarball_url(url: str, tag: str) -> str:
    u = url.removesuffix(".git")
    host = (urllib.parse.urlparse(u).hostname or "").lower()
    if host == "github.com":
        return f"{u}/archive/refs/tags/{tag}.tar.gz"
    if host == "gitlab.com":
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
                if not excluded(p.name):
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


# ---------------------------------------------------------------------------
# zlib-ng: replaces upstream's CMake/configure step.
#
#   * zlib.h / zconf.h / zlib_name_mangling.h are upstream's templates with the
#     (empty) symbol-prefix substituted — what CMake's configure_file() does for
#     a ZLIB_COMPAT build with no ZLIB_SYMBOL_PREFIX — plus one portability
#     patch in zconf.h (compile-time <unistd.h> detection, as before).
#   * Every source is compiled through a generated wrapper cu/<flat>.c whose
#     first line includes the hand-authored cu_zng_config.h (so upstream's
#     zbuild.h sees its config macros) and, for x86 SIMD units, opens a
#     compiler target pragma (upstream enables the ISA with per-file -m flags,
#     which cgo / cc-rs / the manifest cannot express). The wrappers are
#     arch-independent: on a target that lacks the ISA the upstream file is
#     already #ifdef'd to nothing, so one source list serves every binding.
# ---------------------------------------------------------------------------

# x86 unit -> target string for the GCC/Clang pragma; None = baseline x86-64.
# Mirrors the per-group flags in upstream's CMakeLists.txt / detect-intrinsics.
_AVX512 = "avx512f,avx512dq,avx512bw,avx512vl,bmi2"
ZNG_X86_TARGETS = {
    "x86_features.c": "xsave",
    "chunkset_sse2.c": None, "chorba_sse2.c": None,
    "compare256_sse2.c": None, "slide_hash_sse2.c": None,
    "adler32_ssse3.c": "ssse3", "chunkset_ssse3.c": "ssse3",
    "chorba_sse41.c": "sse4.1",
    "adler32_sse42.c": "sse4.2",
    "crc32_pclmulqdq.c": "sse4.2,pclmul",
    "slide_hash_avx2.c": "avx2,bmi2", "chunkset_avx2.c": "avx2,bmi2",
    "compare256_avx2.c": "avx2,bmi2", "adler32_avx2.c": "avx2,bmi2",
    "adler32_avx512.c": _AVX512, "chunkset_avx512.c": _AVX512,
    "compare256_avx512.c": _AVX512,
    "adler32_avx512_vnni.c": _AVX512 + ",avx512vnni",
    "crc32_vpclmulqdq.c": "pclmul,vpclmulqdq," + _AVX512,
}

ZNG_UNISTD_PATCH = (
    "#if defined(__has_include)\n"
    "#  if __has_include(<unistd.h>)\n"
    "#    define HAVE_UNISTD_H 1\n"
    "#  endif\n"
    "#elif !defined(_WIN32)\n"
    "#  define HAVE_UNISTD_H 1\n"
    "#endif\n"
    "/* ^ compress-utils: compile-time <unistd.h> detection (replaces upstream's\n"
    " * configure-time #define) so one header is correct on MSVC and POSIX. */\n"
)


def zng_wrapper(rel: Path) -> str:
    """Wrapper TU for one upstream source (rel is relative to the codec root).
    Arch units are included only when cu_zng_config.h enabled that arch: some
    upstream files #include <immintrin.h> before their own #ifdef."""
    head = ('/* Code generated by tools/vendor-codecs.py. DO NOT EDIT. */\n'
            '#include "../cu_zng_config.h"\n')
    inc = f'#include "../{rel.as_posix()}"\n'
    if rel.parts[:2] == ("arch", "arm"):
        return head + f"#if defined(ARM_FEATURES)\n{inc}#endif\n"
    if rel.parts[:2] != ("arch", "x86"):
        return head + inc
    if rel.name not in ZNG_X86_TARGETS:
        raise SystemExit(
            f"zlib-ng: new x86 unit {rel} has no entry in ZNG_X86_TARGETS "
            f"(tools/vendor-codecs.py); add its ISA target string")
    target = ZNG_X86_TARGETS[rel.name]
    if not target:
        return head + f"#if defined(X86_FEATURES)\n{inc}#endif\n"
    return (head
            + "#if defined(X86_FEATURES)\n"
            + "#  if defined(__clang__)\n"
            + f'#    pragma clang attribute push (__attribute__((target("{target}"))), apply_to=function)\n'
            + "#  elif defined(__GNUC__)\n"
            + "#    pragma GCC push_options\n"
            + f'#    pragma GCC target("{target}")\n'
            + "#  endif\n"
            + inc
            + "#  if defined(__clang__)\n"
            + "#    pragma clang attribute pop\n"
            + "#  elif defined(__GNUC__)\n"
            + "#    pragma GCC pop_options\n"
            + "#  endif\n"
            + "#endif\n")


def zng_finish(src_root: Path, dest: Path, spec: dict,
               curated: list[Path]) -> tuple[list[Path], list[str]]:
    """Generate zlib-ng's configured headers and wrapper sources into dest.
    Returns (generated files relative to dest, manifest source list)."""
    root = src_root / spec.get("root", "")
    out: list[Path] = []

    def put(rel: str, text: str):
        (dest / rel).parent.mkdir(parents=True, exist_ok=True)
        (dest / rel).write_text(text)
        out.append(Path(rel))

    zlib_h = (root / "zlib.h.in").read_text().replace("@ZLIB_SYMBOL_PREFIX@", "")
    left = sorted(set(re.findall(r"@[A-Z][A-Z0-9_]*@", zlib_h)))
    if left:
        raise SystemExit(f"zlib-ng: unsubstituted tokens in zlib.h.in: {left}")
    put("zlib.h", zlib_h)

    zconf = (root / "zconf.h.in").read_text()
    marker = "#ifdef HAVE_UNISTD_H"
    if zconf.count(marker) != 1:
        raise SystemExit("zlib-ng: zconf.h.in no longer has the expected "
                         "HAVE_UNISTD_H block; update tools/vendor-codecs.py")
    put("zconf.h", zconf.replace(marker, ZNG_UNISTD_PATCH + marker))
    put("zlib_name_mangling.h", (root / "zlib_name_mangling.h.empty").read_text())

    sources: list[str] = []
    for rel in sorted((p for p in curated if is_source(p)), key=str):
        flat = rel.as_posix().replace("/", "_")
        put(f"cu/{flat}", zng_wrapper(rel))
        sources.append(f"cu/{flat}")
    return out, sources


# ---------------------------------------------------------------------------
# WASM source patches: third_party/patches/<codec>/*.patch, applied in sorted
# order to a copy of third_party/<codec>/ by cmake/Vendor.cmake (WASM build
# only). Here we only prove they still apply to the vendored tree.
# ---------------------------------------------------------------------------

def codec_patches(codec: str) -> list[Path]:
    return sorted((PATCHES / codec).glob("*.patch"))


def check_patches(codec: str, tag: str) -> str | None:
    """Apply every patch of `codec`, in order, to a scratch copy of its vendored
    tree. Returns None if all apply, else a message naming codec, tag, patch."""
    patches = codec_patches(codec)
    if not patches:
        return None
    with tempfile.TemporaryDirectory() as td:
        work = Path(td) / codec
        shutil.copytree(THIRD_PARTY / codec, work)
        # GIT_DIR pointing nowhere makes `git apply` behave as plain `patch -p1`
        # in `work`; inside a repository it would silently skip paths that do
        # not exist relative to the repository root.
        env = {**os.environ, "GIT_DIR": str(Path(td) / "no-git")}
        for patch in patches:
            try:
                r = subprocess.run(["git", "apply", "-p1", str(patch)], cwd=work,
                                   env=env, capture_output=True, text=True)
            except FileNotFoundError:
                return "git is required to verify the WASM patches but was not found on PATH"
            if r.returncode != 0:
                return (f"patch {patch.relative_to(REPO)} does not apply to "
                        f"{codec} {tag}:\n{r.stderr.rstrip()}\n"
                        f"  This needs a manual fix: re-apply the patch's intent to the new "
                        f"sources, regenerate the patch, and re-run the codec's upstream test "
                        f"suites (see third_party/VENDOR.md, \"Patches for the WASM build\").")
    return None


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

    if spec.get("zlib_ng"):
        generated, sources = zng_finish(src_root, dest, spec, copied)
        copied.extend(generated)

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
                err = check_patches(codec, entry["tag"])
                if err:
                    print(f"FAIL {codec}: {err}")
                    bad = True
                else:
                    n = len(codec_patches(codec))
                    print(f"ok   {codec} @ {entry['tag']}"
                          + (f" (+{n} wasm patch{'es' if n != 1 else ''} apply cleanly)" if n else ""))
        if not args.codec:
            for d in sorted(p for p in PATCHES.glob("*") if p.is_dir()):
                if d.name not in SPECS:
                    print(f"FAIL third_party/patches/{d.name}: no such codec "
                          f"(known: {', '.join(SPECS)})")
                    bad = True
        return 1 if bad else 0

    THIRD_PARTY.mkdir(exist_ok=True)
    manifest = {"codecs": {}}
    if MANIFEST.exists():
        manifest = json.loads(MANIFEST.read_text())

    patch_failed: list[str] = []
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
        err = check_patches(codec, meta["tag"])
        if err:
            # Keep the old manifest entry: its tree hash no longer matches the
            # rewritten tree, so --check stays red until the patch is fixed.
            print(f"\nERROR {codec}: {err}", file=sys.stderr)
            print(f"third_party/{codec}/ now holds {meta['tag']} but its manifest.json "
                  f"entry was NOT updated; fix the patch, then re-run this tool.",
                  file=sys.stderr)
            patch_failed.append(codec)
            continue
        manifest["codecs"][codec] = entry
        print(f"  {len(entry['sources'])} sources, "
              f"{len(entry['include_dirs'])} include dirs")
        if codec_patches(codec):
            print(f"  {len(codec_patches(codec))} wasm patch(es) apply cleanly")

    # Recompute hashes for config headers now that they're on disk.
    MANIFEST.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"wrote {MANIFEST.relative_to(REPO)}")

    # Hand-authored config headers are preserved across re-vendors, so a version
    # bump does NOT refresh them — they can silently drift from upstream. Remind
    # the maintainer to diff them against the new upstream template.
    preserved = [(c, f) for c in codecs for f in CONFIG_FILES.get(c, [])]
    if preserved:
        print("\nNOTE: these hand-authored config headers were preserved, NOT "
              "refreshed from upstream — if you just bumped a version, review "
              "each against the new upstream template:")
        for c, f in preserved:
            print(f"  third_party/{c}/{f}")
    if patch_failed:
        print(f"\nFAILED: WASM patch no longer applies for: {', '.join(patch_failed)}",
              file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
