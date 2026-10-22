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


def level_label(r: dict) -> str:
    """The compress-utils level, or "n<native>" for a native-sweep record."""
    return str(r["level"]) if r.get("level") is not None else f"n{r.get('native_level')}"


def level_key(r: dict) -> tuple:
    return (r["level"] if r.get("level") is not None else -1,
            r["native_level"] if r.get("native_level") is not None else -1)

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
        key=lambda r: (r["input_id"], r["algo"], r.get("impl", ""), r.get("mode", ""),
                       level_key(r), r.get("lang", "")),
    )
    multi_lang = len({r.get("lang") for r in recs}) > 1
    multi_impl = len({r.get("impl", "compress-utils") for r in recs}) > 1
    multi_mode = len({r.get("mode", "oneshot") for r in recs}) > 1
    has_msgs = any(r.get("messages") for r in recs)
    iw = max([8, *(len(r["input_id"]) for r in recs)])
    drivers = ", ".join(f"{d['key']} v{d['version']}" for d in meta.get("drivers", []))
    print(f"\n  {drivers}  "
          f"@ {meta['git_sha']}{'*' if meta.get('git_dirty') else ''}  "
          f"| {meta['machine']['cpu']} ({meta['machine']['arch']})")
    chunk_note = f", chunk={meta.get('chunk')}B" if multi_mode else ""
    print(f"  corpus={meta.get('corpus', 'smoke')}{chunk_note}  "
          f"{meta['samples']} samples + {meta['warmup']} warmup\n")

    impl_col = f"{'impl':16} " if multi_impl else ""
    lang_col = f"{'lang':6} " if multi_lang else ""
    mode_col = f"{'mode':8} " if multi_mode else ""
    msg_col = f" {'c µs/msg':>9} {'d µs/msg':>9}" if has_msgs else ""
    hdr = (f"  {'input':{iw}} {'algo':7} {lang_col}{impl_col}{mode_col}{'lvl':>3} "
           f"{'ratio':>7} {'c MB/s':>9} {'d MB/s':>9}{msg_col}  {'ok':>2}")
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
        lang_cell = f"{r.get('lang', ''):6} " if multi_lang else ""
        mode_cell = f"{r.get('mode', 'oneshot'):8} " if multi_mode else ""
        msg_cell = ""
        if has_msgs:
            msg_cell = (f" {r['compress_ns_per_msg'] / 1e3:>9.2f} {r['decompress_ns_per_msg'] / 1e3:>9.2f}"
                        if r.get("messages") else f" {'':>9} {'':>9}")
        print(
            f"  {r['input_id']:{iw}} {r['algo']:7} {lang_cell}{impl_cell}{mode_cell}{level_label(r):>3} "
            f"{r['ratio']:>7.3f} {r['compress_mbps']:>9.1f} {r['decompress_mbps']:>9.1f}"
            f"{msg_cell}  {ok:>2}"
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
    # Color encodes algorithm; line style encodes the (impl, mode) series, so a
    # native baseline or a streaming variant overlays its counterpart in the
    # same hue.
    multi_lang = len({r.get("lang") for r in recs}) > 1

    def series_of(r: dict) -> tuple:
        return (r.get("impl", "compress-utils"), r.get("mode", "oneshot"),
                r.get("lang", "") if multi_lang else "")

    series = sorted({series_of(r) for r in recs})
    styles = ["-o", "--s", ":^", "-.D", "--o", ":s"]
    series_style = {s: styles[i % len(styles)] for i, s in enumerate(series)}
    multi_series = len(series) > 1

    def series_label(s: tuple) -> str:
        impl, mode, lang = s
        parts = [lang] if lang else []
        parts.append(impl if impl != "compress-utils" else "compress-utils")
        if mode not in ("oneshot", "msgset"):
            parts.append(mode)
        return "/".join(parts)

    # 1) Pareto frontier per input: ratio (x) vs compress speed (y), points are
    #    levels in level order, one line per (algo, impl, mode, lang). "Which
    #    codec wins where." Color encodes the algorithm, or the implementation
    #    when the chart has only one algorithm (an ecosystem comparison).
    for inp in inputs:
        inp_recs = [r for r in recs if r["input_id"] == inp]
        inp_algos = sorted({r["algo"] for r in inp_recs})
        by_impl = len(inp_algos) == 1
        inp_series = sorted({series_of(r) for r in inp_recs})
        series_color = {s: c for s, c in zip(inp_series, plt.cm.tab10.colors + plt.cm.Dark2.colors)}
        fig, ax = plt.subplots(figsize=(8, 5.5))
        for algo in inp_algos:
            for s in inp_series:
                pts = sorted((r for r in inp_recs if r["algo"] == algo and series_of(r) == s),
                             key=level_key)
                if not pts:
                    continue
                xs = [p["ratio"] for p in pts]
                ys = [p["compress_mbps"] for p in pts]
                sl = series_label(s) or "compress-utils"
                if by_impl:
                    label = sl
                    style, color = ("-o" if s[0] == "compress-utils" else "--o"), series_color[s]
                else:
                    label = f"{algo} ({sl})" if multi_series else algo
                    style, color = series_style[s], cmap[algo]
                lw = 2.2 if s[0] == "compress-utils" else 1.2
                ax.plot(xs, ys, style, color=color, label=label, markersize=4, linewidth=lw)
                for p in pts:
                    ax.annotate(level_label(p), (p["ratio"], p["compress_mbps"]),
                                fontsize=6, alpha=0.6)
        ax.set_xlabel("compression ratio →")
        ax.set_ylabel("compress MB/s →")
        ax.set_yscale("log")
        title = f"ratio vs compress speed — {inp}"
        ax.set_title(f"{inp_algos[0]}: {title}" if by_impl else title)
        ax.legend(fontsize=8)
        ax.grid(True, which="both", alpha=0.2)
        out = PLOTS_DIR / f"pareto-{inp}.png"
        fig.tight_layout()
        fig.savefig(out, dpi=120)
        plt.close(fig)
        print(f"[report] wrote {out}")

    # 2) Throughput bars per input at the median level present. Restrict to one
    #    series (prefer compress-utils one-shot) so bars don't double up when
    #    multiple impls/modes are present.
    levels = sorted({r["level"] for r in recs if r.get("level") is not None})
    mid = levels[len(levels) // 2] if levels else None
    preferred = [x for x in series if x[0] == "compress-utils" and x[1] in ("oneshot", "msgset")]
    bar_series = preferred[0] if preferred else series[0]
    for inp in inputs:
        sub = [r for r in recs if r["input_id"] == inp and r["level"] == mid
               and series_of(r) == bar_series]
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

    # 3) Message size: throughput and ratio against message size.
    plot_message_sizes(recs, plt)


def plot_message_sizes(recs: list[dict], plt) -> None:
    """For message sets cut from one source at several block sizes (the
    silesia-blocks tiers, ids like "dickens.4k"), plot compress throughput and
    ratio against message size, one line per (algo, impl) at the middle level
    present. Shows what small messages cost each codec."""
    import re

    blocks = [r for r in recs if r.get("messages") and re.fullmatch(r".+\.\d+k", r["input_id"])
              and r.get("level") is not None]
    sources = sorted({r["input_id"].rsplit(".", 1)[0] for r in blocks})
    levels = sorted({r["level"] for r in blocks})
    if not sources or not levels:
        return
    mid = levels[len(levels) // 2]
    algos = sorted({r["algo"] for r in blocks})
    impls = sorted({(r.get("impl", "compress-utils"), r.get("lang", "")) for r in blocks})
    cmap = {a: c for a, c in zip(algos, plt.cm.tab10.colors)}
    styles = ["-o", "--s", ":^", "-.D"]
    for src in sources:
        fig, (ax_speed, ax_ratio) = plt.subplots(1, 2, figsize=(11, 4.5))
        for algo in algos:
            for i, impl in enumerate(impls):
                pts = sorted((r for r in blocks
                              if r["input_id"].rsplit(".", 1)[0] == src and r["algo"] == algo
                              and (r.get("impl", "compress-utils"), r.get("lang", "")) == impl
                              and r["level"] == mid),
                             key=lambda r: r["input_bytes"] / r["messages"])
                if not pts:
                    continue
                xs = [r["input_bytes"] / r["messages"] for r in pts]
                name = impl[1] if impl[0] == "compress-utils" else f"{impl[1]}/{impl[0]}"
                label = algo if len(impls) == 1 else f"{algo} ({name})"
                style = styles[i % len(styles)]
                ax_speed.plot(xs, [r["compress_mbps"] for r in pts], style, color=cmap[algo],
                              label=label, markersize=4)
                ax_ratio.plot(xs, [r["ratio"] for r in pts], style, color=cmap[algo],
                              label=label, markersize=4)
        for ax in (ax_speed, ax_ratio):
            ax.set_xscale("log", base=2)
            ax.set_xlabel("message size (bytes)")
            ax.grid(True, which="both", alpha=0.2)
        ax_speed.set_yscale("log")
        ax_speed.set_ylabel("compress MB/s →")
        ax_ratio.set_ylabel("compression ratio →")
        ax_speed.legend(fontsize=8)
        fig.suptitle(f"cost of small messages — {src}, level {mid}")
        out = PLOTS_DIR / f"msgsize-{src}-L{mid}.png"
        fig.tight_layout()
        fig.savefig(out, dpi=120)
        plt.close(fig)
        print(f"[report] wrote {out}")


# --------------------------------------------------------------------------- #
# Regression
# --------------------------------------------------------------------------- #


def key(r: dict) -> tuple:
    return (r["input_id"], r["algo"], r.get("impl", "compress-utils"),
            r.get("mode", "oneshot"), r.get("lang", ""), level_key(r))


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
    iw = max([8, *(len(r["input_id"]) for r in new["records"])])
    hdr = f"  {'input':{iw}} {'algo':7} {'lvl':>3} {'Δratio%':>9} {'Δc%':>8} {'Δd%':>8}  flag"
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
            f"  {r['input_id']:{iw}} {r['algo']:7} {level_label(r):>3} "
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
