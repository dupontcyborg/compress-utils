#!/usr/bin/env python3
"""
Alternative compression libraries for the C ecosystem comparison.

These are benchmark-only: they are fetched and built here to measure
compress-utils against what a C developer could use instead, and never ship.
Each is pinned to a release, its source archive is locked by sha256 on first
download (deps.lock.json, tracked), and it is built in release mode with its
own build system, the way a user would get it. Output goes to out/<name>/
(include/ and lib/); downloads and build trees stay in cache/ and src/.

    python3 benchmarks/deps/deps.py            # fetch + build everything
    python3 benchmarks/deps/deps.py zlib-ng    # just one
    python3 benchmarks/deps/deps.py --list

Recipes:
  cmake   configure + build + install with CMake (Release)
  cargo   build a Rust crate's C-ABI staticlib (cargo --release)
  sources compile a list of C files ourselves (-O3), for projects whose own
          build system is a Makefile we can't reuse portably
"""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
CACHE = HERE / "cache"
SRC = HERE / "src"
OUT = HERE / "out"
LOCK = HERE / "deps.lock.json"


def gh(repo: str, tag: str) -> str:
    return f"https://github.com/{repo}/archive/refs/tags/{tag}.tar.gz"


# name -> recipe. `libs` are the archives the drivers link, relative to out/<name>/lib.
# `driver: False` marks a dep used only by the binary-size comparison.
DEPS: dict[str, dict] = {
    "zlib-ng": {
        "url": gh("zlib-ng/zlib-ng", "2.3.3"), "version": "2.3.3",
        "formats": "zlib, gzip, deflate (native zng_ API)",
        "recipe": "cmake",
        "cmake": ["-DZLIB_COMPAT=OFF", "-DBUILD_SHARED_LIBS=OFF", "-DBUILD_TESTING=OFF",
                  "-DZLIB_ENABLE_TESTS=OFF", "-DZLIBNG_ENABLE_TESTS=OFF", "-DWITH_GTEST=OFF"],
        "libs": ["libz-ng.a"],
    },
    "libdeflate": {
        "url": gh("ebiggers/libdeflate", "v1.26"), "version": "1.26",
        "formats": "zlib, gzip, deflate (one-shot only)",
        "recipe": "cmake",
        "cmake": ["-DLIBDEFLATE_BUILD_SHARED_LIB=OFF", "-DLIBDEFLATE_BUILD_GZIP=OFF"],
        "libs": ["libdeflate.a"],
    },
    "miniz": {
        "url": gh("richgel999/miniz", "3.1.2"), "version": "3.1.2",
        "formats": "zlib, deflate",
        "recipe": "cmake",
        "cmake": ["-DBUILD_SHARED_LIBS=OFF", "-DBUILD_EXAMPLES=OFF", "-DBUILD_TESTS=OFF",
                  "-DBUILD_FUZZERS=OFF"],
        "libs": ["libminiz.a"],
    },
    "isa-l": {
        "url": gh("intel/isa-l", "v2.32.1"), "version": "2.32.1",
        "formats": "zlib, gzip, deflate (igzip, levels 0-3)",
        "recipe": "cmake",
        "cmake": ["-DBUILD_SHARED_LIBS=OFF", "-DBUILD_TESTING=OFF"],
        "libs": ["libisal.a"],
    },
    "zlib-rs": {
        "url": gh("trifectatechfoundation/zlib-rs", "v0.6.8"), "version": "0.6.8",
        "formats": "zlib, gzip, deflate (Rust, zlib C ABI)",
        "recipe": "cargo", "crate": "libz-rs-sys-cdylib", "staticlib": "libz_rs.a",
        # It implements zlib's ABI and ships no header: use the zlib-compatible one we
        # vendor (zlib-ng's). Its zconf.h includes a name-mangling header that zlib-ng's
        # own build generates; zlib-rs exports the plain zlib names, so an empty one is right.
        "headers": {"zlib.h": "{repo}/third_party/zlib/zlib.h",
                    "zconf.h": "{repo}/third_party/zlib/zconf.h"},
        "empty_headers": ["zlib_name_mangling.h"],
        "libs": ["libz_rs.a"],
    },
    "libbz2-rs": {
        "url": gh("trifectatechfoundation/libbzip2-rs", "v0.2.5"), "version": "0.2.5",
        "formats": "bz2 (Rust, libbzip2 C ABI)",
        # The crate only declares a cdylib, and its stdio feature (the BZ2_bzRead/Write file
        # API, unused here) fails to link on macOS: build a staticlib without it.
        "recipe": "cargo", "crate": "libbz2-rs-sys-cdylib", "staticlib": "libbz2_rs.a",
        "cargo": ["--no-default-features"], "crate_type": "staticlib",
        "headers": {"bzlib.h": "bzlib.h"},
        "libs": ["libbz2_rs.a"],
    },
    "lzma-sdk": {
        "url": gh("ip7z/7zip", "26.05"), "version": "26.05",
        "formats": "xz (7-Zip's XzEnc/XzDec, single-threaded)",
        "recipe": "sources", "dir": "C",
        "cflags": ["-DZ7_ST"],
        "sources": ["7zCrc.c", "7zCrcOpt.c", "Alloc.c", "Bra.c", "Bra86.c", "BraIA64.c",
                    "CpuArch.c", "Delta.c", "LzFind.c", "LzFindOpt.c", "Lzma2Dec.c",
                    "Lzma2Enc.c", "LzmaDec.c", "LzmaEnc.c", "Sha256.c", "Sha256Opt.c",
                    "Xz.c", "XzCrc64.c", "XzCrc64Opt.c", "XzDec.c", "XzEnc.c", "XzIn.c"],
        "headers_glob": "*.h",
        "libs": ["liblzmasdk.a"],
    },
    "google-snappy": {
        "url": gh("google/snappy", "1.3.1"), "version": "1.3.1",
        "formats": "snappy (Google's C++ reference, via its C API)",
        "recipe": "cmake",
        "cmake": ["-DBUILD_SHARED_LIBS=OFF", "-DSNAPPY_BUILD_TESTS=OFF",
                  "-DSNAPPY_BUILD_BENCHMARKS=OFF", "-DSNAPPY_INSTALL=ON"],
        "libs": ["libsnappy.a"], "cxx": True,
    },
    "xz-embedded": {
        "url": gh("tukaani-project/xz-embedded", "v20240322"), "version": "2024-03-22",
        "formats": "xz (decoder only; for the binary-size comparison)", "driver": False,
        "recipe": "sources", "dir": "linux/lib/xz",
        "cflags": ["-I{src}/userspace", "-I{src}/linux/include/linux", "-DXZ_DEC_ANY_CHECK",
                   "-DXZ_USE_CRC64"],
        "sources": ["xz_crc32.c", "xz_crc64.c", "xz_dec_lzma2.c", "xz_dec_stream.c"],
        "headers": {"xz.h": "linux/include/linux/xz.h", "xz_config.h": "userspace/xz_config.h"},
        "libs": ["libxzembedded.a"],
    },
}


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _run(cmd: list[str], cwd: Path | None = None) -> None:
    print("[deps] $", " ".join(map(str, cmd)))
    subprocess.run(cmd, cwd=cwd, check=True)


def fetch(name: str) -> Path:
    """Download and unpack `name`'s source, verifying it against the lock."""
    dep = DEPS[name]
    archive = CACHE / f"{name}-{dep['version']}.tar.gz"
    if not archive.exists():
        CACHE.mkdir(parents=True, exist_ok=True)
        print(f"[deps] downloading {dep['url']}")
        req = urllib.request.Request(dep["url"], headers={"User-Agent": "compress-utils-bench"})
        with urllib.request.urlopen(req) as r:
            archive.write_bytes(r.read())
    digest = _sha256(archive)
    lock = json.loads(LOCK.read_text()) if LOCK.exists() else {}
    key = f"{name}@{dep['version']}"
    if key in lock and lock[key] != digest:
        sys.exit(f"[deps] sha256 mismatch for {key}: got {digest}, locked {lock[key]}. "
                 f"Delete {archive.relative_to(HERE)} to re-fetch, or fix the lock.")
    if key not in lock:
        lock[key] = digest
        LOCK.write_text(json.dumps(lock, indent=2, sort_keys=True) + "\n")
        print(f"[deps] locked {key} sha256={digest[:12]}…")

    src = SRC / f"{name}-{dep['version']}"
    if not src.exists():
        SRC.mkdir(parents=True, exist_ok=True)
        tmp = SRC / f".{name}.tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        with tarfile.open(archive) as t:
            t.extractall(tmp, filter="data")
        (top,) = tmp.iterdir()  # GitHub archives have one top-level directory
        top.rename(src)
        tmp.rmdir()
    return src


def build(name: str) -> Path:
    dep = DEPS[name]
    out = OUT / name
    if all((out / "lib" / lib).exists() for lib in dep["libs"]):
        return out
    src = fetch(name)
    shutil.rmtree(out, ignore_errors=True)
    (out / "lib").mkdir(parents=True)
    (out / "include").mkdir(parents=True)

    if dep["recipe"] == "cmake":
        bdir = src / "_build"
        _run(["cmake", "-S", src, "-B", bdir, "-DCMAKE_BUILD_TYPE=Release",
              f"-DCMAKE_INSTALL_PREFIX={out}", "-DCMAKE_INSTALL_LIBDIR=lib", *dep["cmake"]])
        _run(["cmake", "--build", bdir, "-j", "--config", "Release"])
        _run(["cmake", "--install", bdir, "--config", "Release"])
    elif dep["recipe"] == "cargo":
        manifest = ["--manifest-path", src / dep["crate"] / "Cargo.toml", "--target-dir",
                    src / "_target", *dep.get("cargo", [])]
        if dep.get("crate_type"):
            _run(["cargo", "rustc", "--release", *manifest, "--crate-type", dep["crate_type"]])
        else:
            _run(["cargo", "build", "--release", *manifest])
        shutil.copy(src / "_target" / "release" / dep["staticlib"], out / "lib" / dep["staticlib"])
    elif dep["recipe"] == "sources":
        sdir = src / dep["dir"]
        objs = []
        for f in dep["sources"]:
            obj = src / "_obj" / (Path(f).stem + ".o")
            obj.parent.mkdir(exist_ok=True)
            cflags = [c.format(src=src) for c in dep.get("cflags", [])]
            _run(["cc", "-O3", "-DNDEBUG", "-c", sdir / f, "-o", obj, f"-I{sdir}", *cflags])
            objs.append(obj)
        _run(["ar", "rcs", out / "lib" / dep["libs"][0], *objs])
        for h in sdir.glob(dep.get("headers_glob", "")) if dep.get("headers_glob") else []:
            shutil.copy(h, out / "include" / h.name)
    else:
        raise SystemExit(f"[deps] unknown recipe {dep['recipe']}")

    for dst, rel in dep.get("headers", {}).items():
        shutil.copy(src / rel.format(repo=REPO), out / "include" / dst)
    for dst in dep.get("empty_headers", []):
        (out / "include" / dst).write_text("")
    missing = [lib for lib in dep["libs"] if not (out / "lib" / lib).exists()]
    if missing:
        raise SystemExit(f"[deps] {name}: build finished but {missing} not found in {out}/lib")
    print(f"[deps] built {name} {dep['version']} -> {out.relative_to(REPO)}")
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("names", nargs="*", help=f"deps to build (default: all): {', '.join(DEPS)}")
    ap.add_argument("--list", action="store_true", help="list the deps and exit")
    args = ap.parse_args()
    if args.list:
        for name, dep in DEPS.items():
            print(f"  {name:12} {dep['version']:11} {dep['formats']}")
        return
    for name in args.names or DEPS:
        if name not in DEPS:
            sys.exit(f"[deps] unknown dep '{name}'. Known: {', '.join(DEPS)}")
        build(name)


if __name__ == "__main__":
    main()
