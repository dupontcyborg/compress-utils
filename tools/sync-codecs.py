#!/usr/bin/env python3
"""Sync upstream codec versions from codec-versions.json → build.zig.zon.

The JSON file is the canonical source. CMake reads it directly via
cmake/CodecVersions.cmake. This script regenerates the Zig binding's
build.zig.zon so its dependency declarations stay in lockstep.

Usage:
    tools/sync-codecs.py            # update bindings/zig/build.zig.zon
    tools/sync-codecs.py --check    # CI mode: fail if regen produces drift

Requires `zig` on PATH to compute content-addressed hashes.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST = REPO_ROOT / "codec-versions.json"
ZIG_ZON = REPO_ROOT / "bindings" / "zig" / "build.zig.zon"


def archive_url(git_url: str, tag: str) -> str:
    """Translate a `.git` repo URL + tag into an archive tarball URL.

    GitHub: https://github.com/<owner>/<repo>.git           → /archive/refs/tags/<tag>.tar.gz
    GitLab: https://gitlab.com/<group>/<repo>.git           → /-/archive/<tag>/<repo>-<tag>.tar.gz
    """
    m = re.fullmatch(r"https://github\.com/([^/]+)/([^/]+?)(?:\.git)?", git_url)
    if m:
        return f"https://github.com/{m.group(1)}/{m.group(2)}/archive/refs/tags/{tag}.tar.gz"

    m = re.fullmatch(r"https://gitlab\.com/([^/]+)/([^/]+?)(?:\.git)?", git_url)
    if m:
        group, repo = m.group(1), m.group(2)
        return f"https://gitlab.com/{group}/{repo}/-/archive/{tag}/{repo}-{tag}.tar.gz"

    raise RuntimeError(f"unsupported codec URL host: {git_url}")


def zig_fetch_hash(url: str) -> str:
    """Run `zig fetch` to compute the package manager's content hash for a URL."""
    if shutil.which("zig") is None:
        raise RuntimeError("'zig' not found on PATH; install Zig >= 0.13 and retry")
    # `zig fetch <url>` prints the computed hash to stdout. No side effects.
    out = subprocess.run(
        ["zig", "fetch", url],
        check=True,
        capture_output=True,
        text=True,
    )
    return out.stdout.strip()


def render_zon(codecs: dict[str, dict[str, str]]) -> str:
    """Render the build.zig.zon contents for the given codecs."""
    lines = [
        ".{",
        '    .name = .compress_utils,',
        '    .version = "0.1.0",',
        '    .fingerprint = 0x0,  // regenerate with `zig init` when binding lands',
        '    .minimum_zig_version = "0.13.0",',
        '    .dependencies = .{',
    ]
    for name, meta in codecs.items():
        if name.startswith("_"):
            continue
        url = archive_url(meta["url"], meta["tag"])
        digest = zig_fetch_hash(url)
        lines.append(f"        .{name} = .{{")
        lines.append(f'            .url = "{url}",')
        lines.append(f'            .hash = "{digest}",')
        lines.append("        },")
    lines.append("    },")
    lines.append('    .paths = .{"build.zig", "build.zig.zon", "src"},')
    lines.append("}")
    lines.append("")
    return "\n".join(lines)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="CI mode: regenerate in a tmpfile and diff against committed file. "
                             "Exit non-zero on drift.")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args(argv)

    if not MANIFEST.exists():
        print(f"error: {MANIFEST} not found", file=sys.stderr)
        return 2

    codecs = json.loads(MANIFEST.read_text())

    # No Zig binding yet → nothing to sync. Skip BEFORE calling `zig fetch`,
    # which would otherwise hit the network for no reason.
    if not ZIG_ZON.parent.exists():
        if not args.quiet:
            print(f"note: {ZIG_ZON.parent} doesn't exist yet — nothing to sync "
                  "(Zig binding not present).")
        return 0

    rendered = render_zon(codecs)

    if args.check:
        existing = ZIG_ZON.read_text() if ZIG_ZON.exists() else ""
        if existing == rendered:
            if not args.quiet:
                print("codec-versions.json and build.zig.zon are in sync.")
            return 0
        print("error: build.zig.zon is out of sync with codec-versions.json.",
              file=sys.stderr)
        print("Run `tools/sync-codecs.py` and commit the result.",
              file=sys.stderr)
        # Print a unified diff for debugging.
        import difflib
        diff = difflib.unified_diff(
            existing.splitlines(keepends=True),
            rendered.splitlines(keepends=True),
            fromfile=str(ZIG_ZON), tofile=str(ZIG_ZON) + " (regenerated)",
        )
        sys.stderr.writelines(diff)
        return 1

    ZIG_ZON.write_text(rendered)
    if not args.quiet:
        print(f"wrote {ZIG_ZON}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
