#!/usr/bin/env python3
"""Sync the project version from the git tag → the committed version fallbacks.

The git tag (`v<MAJOR>.<MINOR>.<PATCH>`) is the canonical version. The C/C++ and
Python bindings already derive it live at build time — CMake injects
`CU_BUILD_VERSION` from `git describe` (see cmake/GitVersion.cmake), and the
Python wheel reads the tag via setuptools_scm (bindings/python/pyproject.toml).

But the bindings that compile from source with no CMake and no git at the
consumer's build — Go (`go get`, via cgo), Rust (`cargo`, via build.rs) and WASM
(`npm`, a published package.json) — can only see the values *committed* to the
tree. Those are:

  * include/compress_utils.h   CU_VERSION_{MAJOR,MINOR,PATCH}  (cu_version()'s
    fallback when CU_BUILD_VERSION is absent — this is what the Go binding
    reports, since cgo never runs CMake)
  * bindings/wasm/package.json "version"
  * CMakeLists.txt             DEFAULT_VERSION (the fallback for out-of-tree
    builds with no git history)
  * Cargo.toml                 [package] version (the Rust crate version; also
    what build.rs injects as CU_BUILD_VERSION and matches the release asset name)

This script rewrites those from the tag so a source build reports the release
version instead of a stale hardcoded one.

Usage:
    tools/sync-versions.py              # version from `git describe` tag
    tools/sync-versions.py 0.7.2        # explicit version (release workflow)
    tools/sync-versions.py --check      # CI mode: fail if any file has drifted
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
HEADER = REPO_ROOT / "include" / "compress_utils.h"
WASM_PKG = REPO_ROOT / "bindings" / "wasm" / "package.json"
CMAKELISTS = REPO_ROOT / "CMakeLists.txt"
CARGO_TOML = REPO_ROOT / "Cargo.toml"

VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)")


def version_from_git() -> str:
    """The most recent `v*` tag, with the leading `v` stripped (v0.7.1 → 0.7.1)."""
    out = subprocess.run(
        ["git", "describe", "--tags", "--match", "v*", "--abbrev=0"],
        cwd=REPO_ROOT, capture_output=True, text=True,
    )
    if out.returncode != 0 or not out.stdout.strip():
        sys.exit("error: no v* git tag found; pass an explicit version, e.g. "
                 "tools/sync-versions.py 0.7.2")
    return out.stdout.strip().lstrip("v")


def parse(version: str) -> tuple[int, int, int]:
    """Split a version into (major, minor, patch), ignoring any prerelease/build
    suffix (0.7.1-rc.1 → 0, 7, 1). The header macros carry only the numeric
    triple; the full string (with suffix) still flows through CU_BUILD_VERSION on
    a real CMake release build."""
    m = VERSION_RE.match(version)
    if not m:
        sys.exit(f"error: '{version}' is not a MAJOR.MINOR.PATCH version")
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


# --------------------------------------------------------------------------- #
# Per-file renderers: each returns the file's text with the version fields set.
# They operate on the committed text (not a full re-render) so unrelated
# formatting is preserved — only the version tokens change.
# --------------------------------------------------------------------------- #


def render_header(text: str, ver: tuple[int, int, int]) -> str:
    major, minor, patch = ver
    text, n1 = re.subn(r"(#define\s+CU_VERSION_MAJOR\s+)\d+", rf"\g<1>{major}", text)
    text, n2 = re.subn(r"(#define\s+CU_VERSION_MINOR\s+)\d+", rf"\g<1>{minor}", text)
    text, n3 = re.subn(r"(#define\s+CU_VERSION_PATCH\s+)\d+", rf"\g<1>{patch}", text)
    if not (n1 == n2 == n3 == 1):
        sys.exit(f"error: expected exactly one of each CU_VERSION_* macro in {HEADER}")
    return text


def render_wasm_pkg(text: str, version: str) -> str:
    # Replace only the first (top-level, package's own) "version" field.
    text, n = re.subn(r'("version"\s*:\s*")[^"]*(")', rf'\g<1>{version}\g<2>',
                      text, count=1)
    if n != 1:
        sys.exit(f'error: no "version" field found in {WASM_PKG}')
    return text


def render_cmakelists(text: str, version: str) -> str:
    text, n = re.subn(r'(set\(DEFAULT_VERSION\s+")[^"]*("\))',
                      rf"\g<1>{version}\g<2>", text)
    if n != 1:
        sys.exit(f"error: expected exactly one set(DEFAULT_VERSION ...) in {CMAKELISTS}")
    return text


def render_cargo_toml(text: str, version: str) -> str:
    # Only the [package] version — a line-start `version = "..."`. Dependency
    # versions live inside inline tables (`ureq = { version = "2" }`) or bare
    # (`flate2 = "1"`), never at column 0, so anchoring to ^ leaves them alone.
    text, n = re.subn(r'(?m)^(version\s*=\s*")[^"]*(")',
                      rf"\g<1>{version}\g<2>", text, count=1)
    if n != 1:
        sys.exit(f"error: expected a [package] version line in {CARGO_TOML}")
    return text


def targets(version: str) -> list[tuple[Path, str, str]]:
    """Return (path, committed_text, rendered_text) for every synced file."""
    ver = parse(version)
    return [
        (HEADER, HEADER.read_text(), render_header(HEADER.read_text(), ver)),
        (WASM_PKG, WASM_PKG.read_text(), render_wasm_pkg(WASM_PKG.read_text(), version)),
        (CMAKELISTS, CMAKELISTS.read_text(), render_cmakelists(CMAKELISTS.read_text(), version)),
        (CARGO_TOML, CARGO_TOML.read_text(), render_cargo_toml(CARGO_TOML.read_text(), version)),
    ]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("version", nargs="?",
                        help="version to write (default: derived from the latest git tag)")
    parser.add_argument("--check", action="store_true",
                        help="CI mode: fail if any committed file has drifted from the version.")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    version = args.version or version_from_git()
    parse(version)  # validate early

    drifted = []
    for path, existing, rendered in targets(version):
        if existing == rendered:
            continue
        if args.check:
            drifted.append(path)
            import difflib
            diff = difflib.unified_diff(
                existing.splitlines(keepends=True), rendered.splitlines(keepends=True),
                fromfile=str(path.relative_to(REPO_ROOT)),
                tofile=str(path.relative_to(REPO_ROOT)) + " (expected)",
            )
            sys.stderr.writelines(diff)
        else:
            path.write_text(rendered)
            if not args.quiet:
                print(f"wrote {path.relative_to(REPO_ROOT)}  → {version}")

    if args.check:
        if drifted:
            print(f"\nerror: {len(drifted)} file(s) out of sync with version {version}.",
                  file=sys.stderr)
            print("Run `tools/sync-versions.py` and commit the result.", file=sys.stderr)
            return 1
        if not args.quiet:
            print(f"all binding versions in sync with {version}.")
        return 0

    if not args.quiet:
        print(f"synced to {version}. Note: C/C++ and Python derive the version live "
              "(CMake git-describe / setuptools_scm) and need no committed change.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
