"""
Corpus tier registry — the single entry point the runner calls.

Tiers:
  smoke    deterministic synthetic set (no network, fast, CI-safe). Default.
  silesia  the standard 12-file real-world corpus (fetched).
  enwik8   100 MB Wikipedia text (fetched).
  all      every tier above.

`resolve("smoke,silesia")` accepts a comma-separated list and merges, deduping
by dataset id. Each returned dataset is {id, path, bytes, sha256, tier, ...}.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fetch  # noqa: E402
import generate as synthetic  # noqa: E402

TIER_RESOLVERS = {
    "smoke": synthetic.resolve,
    "silesia": fetch.resolve_silesia,
    "enwik8": fetch.resolve_enwik8,
}
TIERS = list(TIER_RESOLVERS)


def resolve(spec: str) -> list[dict]:
    tiers = [t.strip() for t in spec.split(",") if t.strip()]
    if "all" in tiers:
        tiers = TIERS
    out: list[dict] = []
    seen: set[str] = set()
    for t in tiers:
        if t not in TIER_RESOLVERS:
            raise SystemExit(f"[corpus] unknown tier '{t}'. Known: {', '.join(TIERS)}, all")
        for ds in TIER_RESOLVERS[t]():
            if ds["id"] not in seen:
                seen.add(ds["id"])
                out.append(ds)
    return out
