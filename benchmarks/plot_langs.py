#!/usr/bin/env python3
"""
Cross-language comparison plot for the README.

Renders a grouped bar chart of compress/decompress throughput by algorithm and
language (C / WASM / Python) from a 3-way results file, at one representative
level + mode so each group compares the same codec across languages.

    python3 benchmarks/plot_langs.py [results.json] [--level 6] [--mode oneshot]

Default input: results/baseline-internal-mini-*.json. Output:
benchmarks/assets/lang-comparison.png (tracked, referenced by README.md).
"""

from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path

BENCH = Path(__file__).resolve().parent
ASSETS = BENCH / "assets"

LANGS = ["c", "wasm", "python", "go"]
# The "wasm" record lang is the WASM package, consumed from JS/TS — labelled as
# such. Colors follow what people expect: Go's gopher light-blue, a darker blue
# for Python, red for C/C++, and JS/TS on gold (a third blue would be unreadable).
LABELS = {"c": "C / C++", "wasm": "JS / TS", "python": "Python", "go": "Go"}
COLORS = {"c": "#d62728", "wasm": "#dd8452", "python": "#1f4e9c", "go": "#00add8"}
ALGOS = ["zstd", "brotli", "zlib", "bz2", "lz4", "xz", "snappy", "gzip"]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("results", nargs="?")
    ap.add_argument("--level", type=int, default=6)
    ap.add_argument("--mode", default="oneshot")
    args = ap.parse_args()

    path = Path(args.results) if args.results else next(
        iter(sorted((BENCH / "results").glob("baseline-internal-mini-*.json"))))
    data = json.loads(path.read_text())
    recs = data["records"]
    cpu = data["meta"]["machine"]["cpu"]

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    def med(lang, algo, field):
        vals = [r[field] for r in recs if r["lang"] == lang and r["algo"] == algo
                and r["level"] == args.level and r["mode"] == args.mode]
        return statistics.median(vals) if vals else 0.0

    # Only plot languages actually present, so bar groups center correctly
    # whether the run had 3 langs or 4.
    langs = [l for l in LANGS if any(r["lang"] == l for r in recs)]
    x = range(len(ALGOS))
    w = 0.8 / len(langs)
    fig, (ax_c, ax_d) = plt.subplots(1, 2, figsize=(13, 5))
    for i, lang in enumerate(langs):
        off = (i - (len(langs) - 1) / 2) * w
        ax_c.bar([j + off for j in x], [med(lang, a, "compress_mbps") for a in ALGOS],
                 w, label=LABELS[lang], color=COLORS[lang])
        ax_d.bar([j + off for j in x], [med(lang, a, "decompress_mbps") for a in ALGOS],
                 w, label=LABELS[lang], color=COLORS[lang])
    for ax, title in ((ax_c, "Compression"), (ax_d, "Decompression")):
        ax.set_yscale("log")
        ax.set_xticks(list(x))
        ax.set_xticklabels(ALGOS)
        ax.set_ylabel("throughput in MB/s")
        ax.set_title(title)
        ax.legend(fontsize=9)
        ax.grid(True, axis="y", alpha=0.25)
    fig.suptitle(f"compress-utils throughput by language & algorithm  "
                 f"(Silesia-mini, level {args.level}, {args.mode}, {cpu})")
    fig.tight_layout()
    ASSETS.mkdir(exist_ok=True)
    out = ASSETS / "lang-comparison.png"
    fig.savefig(out, dpi=130)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
