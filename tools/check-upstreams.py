#!/usr/bin/env python3
"""
check-upstreams.py — report upstream codec releases newer than the versions
pinned in codec-versions.json.

Read-only: never touches the repo. Queries each upstream forge (GitHub/GitLab)
for its tags, picks the highest stable version, and compares to the pinned tag.
Intended for a scheduled CI job that opens/updates a tracking issue so a human
can run `build.sh --revendor` and review the diff.

Outputs:
  - a Markdown report to stdout (and to $GITHUB_STEP_SUMMARY if set)
  - when run in GitHub Actions, `has_updates` + `report_file` to $GITHUB_OUTPUT

Auth: uses $GITHUB_TOKEN for the GitHub API when present (higher rate limit).
Exit code is always 0 — the workflow decides what to do with the result.
"""

from __future__ import annotations

import json
import os
import re
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VERSIONS = REPO / "codec-versions.json"

# Grab the full trailing dotted-number version (2+ components), regardless of
# prefix — handles v1.5.7, 1.2.2, bzip2-1.0.8 (prefix has a digit), and lz4's
# historical 4-component tags like v1.8.1.2. Parsed as a full int tuple so
# (1,10,0) > (1,8,1,2) compares correctly. A commit hash has no dotted-number
# tail, so it yields no match and is treated as "not a version".
_VER_RE = re.compile(r"(\d+(?:\.\d+)+)$")
_PRERELEASE_RE = re.compile(r"(?i)(rc|alpha|beta|dev|pre|next|test)")


def parse_ver(tag: str):
    m = _VER_RE.search(tag.strip())
    if not m:
        return None
    return tuple(int(p) for p in m.group(1).split("."))


def _host(url: str) -> str:
    return (urllib.parse.urlparse(url).hostname or "").lower()


def _get_json(url: str, token: str | None):
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    if token and _host(url) == "api.github.com":
        req.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(req, timeout=30) as r:  # noqa: S310 (trusted forges)
        return json.load(r)


def fetch_tags(url: str, token: str | None) -> list[str]:
    base = url.removesuffix(".git")
    # Owner/repo path from the URL itself (parsed, not substring-matched, so a
    # host name can't appear in an unexpected position).
    path = urllib.parse.urlparse(base).path.strip("/")
    host = _host(base)
    if host == "github.com":
        owner, repo = path.split("/")[:2]
        data = _get_json(
            f"https://api.github.com/repos/{owner}/{repo}/tags?per_page=100", token)
        return [t["name"] for t in data]
    if host == "gitlab.com":
        proj = urllib.parse.quote(path, safe="")
        data = _get_json(
            f"https://gitlab.com/api/v4/projects/{proj}/repository/tags?per_page=100",
            token)
        return [t["name"] for t in data]
    raise ValueError(f"unknown forge: {url}")


def latest_stable(tags: list[str]):
    best_ver = None
    best_tag = None
    for t in tags:
        if _PRERELEASE_RE.search(t):
            continue
        v = parse_ver(t)
        if v and (best_ver is None or v > best_ver):
            best_ver, best_tag = v, t
    return best_ver, best_tag


def main() -> int:
    token = os.environ.get("GITHUB_TOKEN") or None
    codecs = {k: v for k, v in json.loads(VERSIONS.read_text()).items()
              if not k.startswith("_")}

    rows = []          # (codec, current, latest, status)
    updates = []       # codecs with a newer stable release
    for name, meta in codecs.items():
        cur = meta["tag"]
        try:
            _, latest_tag = latest_stable(fetch_tags(meta["url"], token))
        except (urllib.error.URLError, ValueError, KeyError) as e:
            rows.append((name, cur, "?", f"⚠️ check failed ({e})"))
            continue

        cur_v = parse_ver(cur)
        latest_v = parse_ver(latest_tag) if latest_tag else None
        if cur_v is None:
            # e.g. bz2 pinned to a commit hash — can't compare automatically.
            note = f"pinned to non-version ref; latest tag is `{latest_tag}`" \
                if latest_tag else "pinned to non-version ref"
            rows.append((name, cur, latest_tag or "?", f"🔒 {note} — review manually"))
        elif latest_v and latest_v > cur_v:
            rows.append((name, cur, latest_tag, "⬆️ **update available**"))
            updates.append((name, cur, latest_tag))
        else:
            rows.append((name, cur, latest_tag or cur, "✅ up to date"))

    # --- Markdown report ---
    lines = ["## Upstream codec version check", ""]
    if updates:
        lines.append(f"**{len(updates)} update(s) available.** To apply: bump the "
                     "tag in `codec-versions.json`, run `./build.sh --revendor`, "
                     "rebuild + test, and commit `third_party/` + "
                     "`third_party/manifest.json`.")
    else:
        lines.append("All upstream codecs are up to date. 🎉")
    lines += ["", "| Codec | Pinned | Latest stable | Status |",
              "|-------|--------|---------------|--------|"]
    for name, cur, latest, status in rows:
        lines.append(f"| {name} | `{cur}` | `{latest}` | {status} |")
    report = "\n".join(lines) + "\n"
    print(report)

    # --- GitHub Actions plumbing ---
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        Path(summary).write_text(report)
    report_file = REPO / "upstream-report.md"
    report_file.write_text(report)
    gh_out = os.environ.get("GITHUB_OUTPUT")
    if gh_out:
        with open(gh_out, "a") as f:
            f.write(f"has_updates={'true' if updates else 'false'}\n")
            f.write(f"report_file={report_file}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
