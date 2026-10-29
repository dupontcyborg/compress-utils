"""
Corpus tier registry — the single entry point the runner calls.

Tiers:
  smoke             deterministic synthetic set (no network, fast, CI-safe). Default.
  silesia           the standard 12-file real-world corpus (fetched).
  silesia-mini      6 of the Silesia files, for faster runs (fetched).
  enwik8            100 MB Wikipedia text (fetched).
  canterbury        the 11-file Canterbury corpus, 3.7 KB to 1 MB per file (fetched).
  canterbury-large  the Large Canterbury corpus: E.coli, bible.txt, world192.txt (fetched).
  github-users      zstd's ~9K JSON records of ~1 KB, as one message set (fetched).
  silesia-blocks-1k, -4k, -16k, -64k
                    silesia-mini cut into fixed-size blocks, as message sets
                    (derived from silesia-mini).
  silesia-blocks    all four block sizes.
  ecosystem-large   dickens (10 MB of text): the one large file of the ecosystem comparison.
  ecosystem-extra   xml (repetitive) and x-ray (hard to compress): one-shot checks on dickens.
  ecosystem-small   dickens in 4 KB blocks, as a message set: its one small-message tier.
  ecosystem         all three of the above.
  all               every tier above.

A dataset with kind="msgset" holds many small messages (see msgset.py); the
runner benchmarks it in msgset mode, one call per message.

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
    "silesia-mini": fetch.resolve_silesia_mini,
    "enwik8": fetch.resolve_enwik8,
    "canterbury": fetch.resolve_canterbury,
    "canterbury-large": fetch.resolve_canterbury_large,
    "github-users": fetch.resolve_github_users,
    "ecosystem-large": fetch.resolve_ecosystem_large,
    "ecosystem-extra": fetch.resolve_ecosystem_extra,
    "ecosystem-small": fetch.resolve_ecosystem_small,
    **{f"silesia-blocks-{label}": (lambda label=label: fetch.resolve_silesia_blocks(label))
       for label in fetch.BLOCK_SIZES},
}
# Groups: expand to several tiers.
TIER_GROUPS = {
    "silesia-blocks": [f"silesia-blocks-{label}" for label in fetch.BLOCK_SIZES],
    "ecosystem": ["ecosystem-large", "ecosystem-extra", "ecosystem-small"],
}
TIERS = list(TIER_RESOLVERS)


def resolve(spec: str) -> list[dict]:
    tiers = [t.strip() for t in spec.split(",") if t.strip()]
    if "all" in tiers:
        tiers = TIERS
    tiers = [x for t in tiers for x in TIER_GROUPS.get(t, [t])]
    out: list[dict] = []
    seen: set[str] = set()
    for t in tiers:
        if t not in TIER_RESOLVERS:
            known = ", ".join([*TIERS, *TIER_GROUPS])
            raise SystemExit(f"[corpus] unknown tier '{t}'. Known: {known}, all")
        for ds in TIER_RESOLVERS[t]():
            if ds["id"] not in seen:
                seen.add(ds["id"])
                out.append(ds)
    return out
