#!/usr/bin/env python3
"""
Binary size: what each library adds to a statically linked program.

For every format, links probe.c (compress + decompress, and decompress only)
against compress-utils and against each alternative, with dead-stripping, then
strips the binary and sums its code and data sections. The figure reported is
that sum minus the same sum for an empty probe, i.e. the bytes the library adds.

compress-utils is measured the way a size-conscious user would build it: a
static archive with only that codec (INCLUDE_<CODEC>, everything else off), and
for decode-only also -DCU_OMIT_COMPRESS. A separate row links the default
all-codec archive, which is what you get without configuring the build.

Every library is a release build as its project ships it (-O3 for CMake
Release); none is rebuilt for size (-Os). Google snappy's C++ runtime links
dynamically and is not counted.

Needs a normal build first (./build.sh, for the vendored codec archives) and
benchmarks/deps/deps.py (for the alternatives).

    python3 benchmarks/size/size.py              # table + results/size-<stamp>.json
"""

from __future__ import annotations

import json
import platform
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
BENCH = HERE.parent
REPO = BENCH.parent
BUILD = HERE / "build"
VENDORED = REPO / "build" / "algorithms"
TP = REPO / "third_party"

sys.path.insert(0, str(BENCH / "deps"))
sys.path.insert(0, str(BENCH / "lib"))
import bench_common as bc  # noqa: E402
import deps  # noqa: E402

# format -> compress-utils (INCLUDE option, cu_algorithm_t)
CU = {
    "zlib": ("INCLUDE_ZLIB", "CU_ALGO_ZLIB"),
    "bz2": ("INCLUDE_BZ2", "CU_ALGO_BZ2"),
    "xz": ("INCLUDE_XZ", "CU_ALGO_XZ"),
    "zstd": ("INCLUDE_ZSTD", "CU_ALGO_ZSTD"),
    "lz4": ("INCLUDE_LZ4", "CU_ALGO_LZ4"),
    "brotli": ("INCLUDE_BROTLI", "CU_ALGO_BROTLI"),
    "snappy": ("INCLUDE_SNAPPY", "CU_ALGO_SNAPPY"),
}
INCLUDE_OPTIONS = ["INCLUDE_ZSTD", "INCLUDE_BROTLI", "INCLUDE_ZLIB", "INCLUDE_BZ2", "INCLUDE_LZ4",
                   "INCLUDE_XZ", "INCLUDE_SNAPPY", "INCLUDE_GZIP", "INCLUDE_LZO"]


def vendored(codec: str, inc: str, archive: str | None = None) -> dict:
    """A codec as compress-utils vendors it, linked directly."""
    return {"cflags": [f"-I{TP / codec / inc}"],
            "libs": [str(VENDORED / codec / f"lib{archive or codec}_library.a")]}


def dep(name: str, extra_cflags: list[str] | None = None) -> dict:
    out = deps.build(name)
    d = deps.DEPS[name]
    libs = [str(out / "lib" / lib) for lib in d["libs"]]
    if d.get("cxx"):
        libs.append("-lc++" if sys.platform == "darwin" else "-lstdc++")
    return {"cflags": [f"-I{out / 'include'}", *(extra_cflags or [])], "libs": libs,
            "version": d["version"]}


# format -> [(impl, probe macro, link spec factory, decode-only only)]
ALTERNATIVES = {
    "zlib": [
        ("zlib", "ZLIB", lambda: vendored("zlib", "."), False),
        ("zlib-ng", "ZLIB_NG", lambda: dep("zlib-ng"), False),
        ("libdeflate", "LIBDEFLATE", lambda: dep("libdeflate"), False),
        ("isa-l", "ISA_L", lambda: dep("isa-l"), False),
        ("zlib-rs", "ZLIB_RS", lambda: dep("zlib-rs"), False),
        ("miniz", "MINIZ", lambda: dep("miniz"), False),
    ],
    "bz2": [
        ("libbz2", "BZ2", lambda: vendored("bz2", "."), False),
        ("libbz2-rs", "LIBBZ2_RS", lambda: dep("libbz2-rs"), False),
    ],
    "xz": [
        ("liblzma", "LZMA", lambda: {**vendored("xz", "src/liblzma/api"),
                                     "cflags": [f"-I{TP / 'xz/src/liblzma/api'}",
                                                "-DLZMA_API_STATIC"]}, False),
        ("lzma-sdk", "LZMA_SDK", lambda: dep("lzma-sdk"), False),
        ("xz-embedded", "XZ_EMBEDDED",
         lambda: dep("xz-embedded", ["-DXZ_DEC_ANY_CHECK", "-DXZ_USE_CRC64"]), True),
    ],
    "zstd": [("libzstd", "ZSTD", lambda: vendored("zstd", "lib"), False)],
    "lz4": [("liblz4", "LZ4", lambda: vendored("lz4", "lib"), False)],
    "brotli": [("libbrotli", "BROTLI", lambda: vendored("brotli", "c/include"), False)],
    "snappy": [
        ("snappy-c", "SNAPPY_C", lambda: vendored("snappy", "."), False),
        ("google-snappy", "GOOGLE_SNAPPY", lambda: dep("google-snappy"), False),
    ],
}


def run(cmd: list, **kw) -> subprocess.CompletedProcess:
    return subprocess.run([str(c) for c in cmd], check=True, capture_output=True, text=True, **kw)


def cu_archive(option: str | None, decode_only: bool) -> Path:
    """compress-utils' static archive with one codec (option) or all (None)."""
    name = f"cu-{(option or 'all').removeprefix('INCLUDE_').lower()}{'-dec' if decode_only else ''}"
    bdir = BUILD / name
    lib = bdir / "libcompress_utils_static.a"
    if not lib.exists():
        includes = [f"-D{o}={'ON' if option in (None, o) else 'OFF'}" for o in INCLUDE_OPTIONS]
        cflags = ["-DCMAKE_C_FLAGS=-DCU_OMIT_COMPRESS"] if decode_only else []
        print(f"[size] building compress-utils archive {name}")
        run(["cmake", "-S", REPO, "-B", bdir, "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_STATIC_LIB=ON",
             "-DBUILD_PYTHON_BINDINGS=OFF", "-DBUILD_CPP_BINDINGS=OFF", "-DBUILD_WASM_BINDINGS=OFF",
             "-DENABLE_TESTS=OFF", *includes, *cflags])
        run(["cmake", "--build", bdir, "--target", "compress_utils_static", "-j"])
    return lib


def section_bytes(binary: Path) -> int:
    """Code and data the stripped binary carries (not zero-fill, not link-edit)."""
    if sys.platform == "darwin":
        out = run(["size", "-m", binary]).stdout
        total, seg = 0, None
        for line in out.splitlines():
            if m := re.match(r"Segment (\S+):", line):
                seg = m.group(1)
            elif (m := re.match(r"\s+Section (\S+): (\d+)", line)) and seg in (
                    "__TEXT", "__DATA_CONST", "__DATA"):
                if m.group(1) not in ("__bss", "__common"):
                    total += int(m.group(2))
        return total
    out = run(["size", "-A", binary]).stdout
    total = 0
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[0].startswith(".") and parts[1].isdigit():
            if parts[0] not in (".bss", ".tbss", ".comment") and not parts[0].startswith(
                    (".debug", ".note")):
                total += int(parts[1])
    return total


def probe_size(macro: str, cflags: list[str], libs: list[str], decode_only: bool) -> int:
    tag = f"{macro.lower()}{'-dec' if decode_only else ''}-{abs(hash((tuple(cflags), tuple(libs))))}"
    binary = BUILD / "probes" / tag
    binary.parent.mkdir(parents=True, exist_ok=True)
    gc = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
    run(["cc", "-O2", "-std=c11", f"-DPROBE_{macro}", *(["-DDECODE_ONLY"] if decode_only else []),
         *cflags, HERE / "probe.c", "-o", binary, gc, *libs, "-lm"])
    run(["strip", binary])
    return section_bytes(binary)


def main() -> None:
    empty = probe_size("NONE", [], [], False)
    rows = []

    def add(fmt: str, impl: str, version: str, sizes: dict) -> None:
        rows.append({"format": fmt, "impl": impl, "version": version,
                     **{k: (v - empty if v is not None else None) for k, v in sizes.items()}})

    cu_version = bc.git_sha()
    for fmt, (option, algo) in CU.items():
        sizes = {}
        for key, decode_only in (("both_bytes", False), ("decode_bytes", True)):
            lib = cu_archive(option, decode_only)
            sizes[key] = probe_size("CU", [f"-I{REPO / 'include'}", f"-DCU_PROBE_ALGO={algo}"],
                                    [str(lib)], decode_only)
        add(fmt, "compress-utils", cu_version, sizes)
        for impl, macro, spec, decode_only_lib in ALTERNATIVES[fmt]:
            s = spec()
            both = None if decode_only_lib else probe_size(macro, s["cflags"], s["libs"], False)
            dec = probe_size(macro, s["cflags"], s["libs"], True)
            add(fmt, impl, s.get("version", "vendored"), {"both_bytes": both, "decode_bytes": dec})

    lib = cu_archive(None, False)
    all_codecs = probe_size("CU", [f"-I{REPO / 'include'}", "-DCU_PROBE_ALGO=CU_ALGO_ZSTD"],
                            [str(lib)], False)
    add("all", "compress-utils (all codecs, default build)", cu_version,
        {"both_bytes": all_codecs, "decode_bytes": None})

    def kb(v):
        return f"{v / 1024:9.1f}" if v is not None else f"{'—':>9}"

    print(f"\n  bytes added to a stripped static binary (KiB), {platform.machine()} {sys.platform}\n")
    print(f"  {'format':7} {'implementation':44} {'comp+dec':>9} {'dec only':>9}")
    print("  " + "-" * 72)
    last = None
    for r in rows:
        if last and r["format"] != last:
            print()
        last = r["format"]
        print(f"  {r['format']:7} {r['impl'] + ' ' + r['version']:44} {kb(r['both_bytes'])} "
              f"{kb(r['decode_bytes'])}")
    print()

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S")
    out = bc.RESULTS_DIR / f"size-{stamp}.json"
    bc.RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"machine": bc.machine_fingerprint(), "git_sha": bc.git_sha(),
                               "empty_probe_bytes": empty, "rows": rows}, indent=2) + "\n")
    print(f"[size] wrote {out}")


if __name__ == "__main__":
    main()
