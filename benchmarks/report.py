#!/usr/bin/env python3
"""
Report + regression tool for benchmark results.

    python3 benchmarks/report.py [results.json]          # table + plots
    python3 benchmarks/report.py new.json --baseline old.json   # regression diff

Default (no path) reads results/latest.json. Plots land in results/plots/ and
need matplotlib; the table and regression diff are stdlib-only.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
import bench_common as bc  # noqa: E402

PLOTS_DIR = bc.RESULTS_DIR / "plots"

# Regression thresholds. Ratio is near-deterministic so we're strict; speed is
# noisy on shared hardware so we only flag large moves.
RATIO_DROP_PCT = 1.0
SPEED_DROP_PCT = 8.0


# --------------------------------------------------------------------------- #
# Table
# --------------------------------------------------------------------------- #


def print_table(data: dict) -> None:
    meta = data["meta"]
    recs = sorted(
        data["records"],
        key=lambda r: (r["input_id"], r["algo"], r.get("impl", ""), r["level"]),
    )
    multi_impl = len({r.get("impl", "compress-utils") for r in recs}) > 1
    print(f"\n  {meta['driver_lang']} v{meta['driver_version']}  "
          f"@ {meta['git_sha']}{'*' if meta.get('git_dirty') else ''}  "
          f"| {meta['machine']['cpu']} ({meta['machine']['arch']})")
    print(f"  {meta['samples']} samples + {meta['warmup']} warmup\n")

    impl_col = f"{'impl':16} " if multi_impl else ""
    hdr = (f"  {'input':8} {'algo':7} {impl_col}{'lvl':>3} "
           f"{'ratio':>7} {'c MB/s':>9} {'d MB/s':>9}  {'ok':>2}")
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))
    cur = None
    for r in recs:
        if r["input_id"] != cur:
            if cur is not None:
                print()
            cur = r["input_id"]
        ok = "✓" if r.get("verified") else "✗"
        impl_cell = f"{r.get('impl', 'compress-utils'):16} " if multi_impl else ""
        print(
            f"  {r['input_id']:8} {r['algo']:7} {impl_cell}{r['level']:>3} "
            f"{r['ratio']:>7.3f} {r['compress_mbps']:>9.1f} {r['decompress_mbps']:>9.1f}  {ok:>2}"
        )
    print()


# --------------------------------------------------------------------------- #
# Plots
# --------------------------------------------------------------------------- #


def make_plots(data: dict) -> None:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("[report] matplotlib not installed; skipping plots", file=sys.stderr)
        return

    PLOTS_DIR.mkdir(parents=True, exist_ok=True)
    recs = data["records"]
    inputs = sorted({r["input_id"] for r in recs})
    algos = sorted({r["algo"] for r in recs})
    cmap = {a: c for a, c in zip(algos, plt.cm.tab10.colors)}
    # Color encodes algorithm; line style encodes implementation, so a native
    # baseline overlays its compress-utils counterpart in the same hue.
    impls = sorted({r.get("impl", "compress-utils") for r in recs})
    styles = ["-o", "--s", ":^", "-.D"]
    impl_style = {im: styles[i % len(styles)] for i, im in enumerate(impls)}
    multi_impl = len(impls) > 1

    # 1) Pareto frontier per input: ratio (x) vs compress speed (y), points are
    #    levels, one line per (algo, impl). The classic "which codec wins where".
    for inp in inputs:
        fig, ax = plt.subplots(figsize=(7, 5))
        for algo in algos:
            for impl in impls:
                pts = sorted(
                    (r for r in recs if r["input_id"] == inp and r["algo"] == algo
                     and r.get("impl", "compress-utils") == impl),
                    key=lambda r: r["ratio"],
                )
                if not pts:
                    continue
                xs = [p["ratio"] for p in pts]
                ys = [p["compress_mbps"] for p in pts]
                label = f"{algo} ({impl})" if multi_impl else algo
                ax.plot(xs, ys, impl_style[impl], color=cmap[algo], label=label, markersize=4)
                for p in pts:
                    ax.annotate(str(p["level"]), (p["ratio"], p["compress_mbps"]),
                                fontsize=6, alpha=0.6)
        ax.set_xlabel("compression ratio →")
        ax.set_ylabel("compress MB/s →")
        ax.set_yscale("log")
        ax.set_title(f"ratio vs compress speed — {inp}")
        ax.legend(fontsize=8)
        ax.grid(True, which="both", alpha=0.2)
        out = PLOTS_DIR / f"pareto-{inp}.png"
        fig.tight_layout()
        fig.savefig(out, dpi=120)
        plt.close(fig)
        print(f"[report] wrote {out}")

    # 2) Throughput bars per input at the median level present.
    levels = sorted({r["level"] for r in recs})
    mid = levels[len(levels) // 2]
    for inp in inputs:
        sub = [r for r in recs if r["input_id"] == inp and r["level"] == mid]
        if not sub:
            continue
        sub.sort(key=lambda r: r["algo"])
        fig, ax = plt.subplots(figsize=(7, 4))
        x = range(len(sub))
        w = 0.4
        ax.bar([i - w / 2 for i in x], [r["compress_mbps"] for r in sub], w,
               label="compress", color="#4c72b0")
        ax.bar([i + w / 2 for i in x], [r["decompress_mbps"] for r in sub], w,
               label="decompress", color="#dd8452")
        ax.set_xticks(list(x))
        ax.set_xticklabels([r["algo"] for r in sub])
        ax.set_ylabel("MB/s")
        ax.set_yscale("log")
        ax.set_title(f"throughput @ level {mid} — {inp}")
        ax.legend(fontsize=8)
        ax.grid(True, axis="y", alpha=0.2)
        out = PLOTS_DIR / f"throughput-{inp}-L{mid}.png"
        fig.tight_layout()
        fig.savefig(out, dpi=120)
        plt.close(fig)
        print(f"[report] wrote {out}")


# --------------------------------------------------------------------------- #
# Regression
# --------------------------------------------------------------------------- #


def key(r: dict) -> tuple:
    return (r["input_id"], r["algo"], r.get("impl", "compress-utils"), r["level"])


def regress(new: dict, base: dict) -> int:
    nm, bm = new["meta"], base["meta"]
    if nm["machine"]["cpu"] != bm["machine"]["cpu"]:
        print(
            f"[report] WARNING: comparing across machines "
            f"('{bm['machine']['cpu']}' → '{nm['machine']['cpu']}'); "
            f"speed deltas are not meaningful.",
            file=sys.stderr,
        )
    bidx = {key(r): r for r in base["records"]}

    print(f"\n  regression: {bm['git_sha']} → {nm['git_sha']}")
    print(f"  flag if ratio ↓ >{RATIO_DROP_PCT}% or speed ↓ >{SPEED_DROP_PCT}%\n")
    hdr = f"  {'input':8} {'algo':7} {'lvl':>3} {'Δratio%':>9} {'Δc%':>8} {'Δd%':>8}  flag"
    print(hdr)
    print("  " + "-" * (len(hdr) - 2))

    regressions = 0
    for r in sorted(new["records"], key=key):
        b = bidx.get(key(r))
        if not b:
            continue

        def pct(new_v, old_v):
            return (new_v - old_v) / old_v * 100 if old_v else 0.0

        dr = pct(r["ratio"], b["ratio"])
        dc = pct(r["compress_mbps"], b["compress_mbps"])
        dd = pct(r["decompress_mbps"], b["decompress_mbps"])

        flags = []
        if dr < -RATIO_DROP_PCT:
            flags.append("RATIO")
        if dc < -SPEED_DROP_PCT:
            flags.append("CSPEED")
        if dd < -SPEED_DROP_PCT:
            flags.append("DSPEED")
        if flags:
            regressions += 1
        print(
            f"  {r['input_id']:8} {r['algo']:7} {r['level']:>3} "
            f"{dr:>+9.2f} {dc:>+8.1f} {dd:>+8.1f}  {','.join(flags)}"
        )

    print()
    if regressions:
        print(f"  ✗ {regressions} regression(s) detected")
    else:
        print("  ✓ no regressions")
    return regressions


# --------------------------------------------------------------------------- #


def main() -> None:
    ap = argparse.ArgumentParser(description="compress-utils benchmark report")
    ap.add_argument("results", nargs="?", help="results JSON (default: results/latest.json)")
    ap.add_argument("--baseline", help="baseline results JSON for a regression diff")
    ap.add_argument("--no-plots", action="store_true", help="skip plot generation")
    args = ap.parse_args()

    path = Path(args.results) if args.results else bc.RESULTS_DIR / "latest.json"
    if not path.exists():
        sys.exit(f"error: {path} not found. Run benchmarks/runner.py first.")
    data = bc.load_results(path)

    if args.baseline:
        base = bc.load_results(Path(args.baseline))
        sys.exit(1 if regress(data, base) else 0)

    print_table(data)
    if not args.no_plots:
        make_plots(data)


if __name__ == "__main__":
    main()
