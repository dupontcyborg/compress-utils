#!/usr/bin/env python3
"""
JavaScript bundle size and cold start: what each library costs a web page or a
serverless function that uses it for one format.

Size. Each entry imports compress and decompress (or just decompress, for
decode-only libraries and compress-utils' `/decompress` subpaths) from one library,
and esbuild bundles it for the browser, minified. Reported: minified JS bytes, the
WASM binary's bytes (zero for pure JS), and the gzip of both, which is what a CDN
sends. compress-utils is measured per format, as `import ... from
"compress-utils/zstd"` pulls in only that format.

Cold start. A fresh process imports the library and compresses and decompresses 1 KB
once; the median of five, minus an empty process, under Node and Bun. It includes
loading and compiling any WASM.

Needs the competitors installed (benchmarks/drivers/wasm: npm ci) and the WASM
package built (bindings/wasm: npm run build).

    python3 benchmarks/size/js_size.py           # table + results/js-size-<stamp>.json
"""

from __future__ import annotations

import gzip
import json
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
BENCH = HERE.parent
REPO = BENCH.parent
JS = BENCH / "drivers" / "wasm"
NM = JS / "node_modules"
CU = REPO / "bindings" / "wasm" / "dist" / "algorithms"

sys.path.insert(0, str(BENCH / "lib"))
import bench_common as bc  # noqa: E402

# (format, library, import specifier, names, wasm files next to the library)
# names: the pair imported; decode-only entries import one name.
E = []


def cu(fmt: str, sub: str = "", decode_only: bool = False) -> tuple:
    path = CU / fmt / sub / "index.js"
    return (fmt, "compress-utils" + (" (decode only)" if decode_only else ""), str(path),
            ["decompress"] if decode_only else ["compress", "decompress"], [CU / fmt / sub / f"{fmt}.wasm"])


E += [cu("zstd"), cu("zstd", "decompress", True)]
E += [("zstd", "zstd-wasm", "@bokuweb/zstd-wasm", ["init", "compress", "decompress"],
       [NM / "@bokuweb" / "zstd-wasm" / "dist" / "web" / "zstd.wasm"]),
      ("zstd", "fzstd (decode only)", "fzstd", ["decompress"], [])]
E += [cu("brotli"), cu("brotli", "decompress", True),
      ("brotli", "brotli-wasm", "brotli-wasm", ["default"],
       list((NM / "brotli-wasm").glob("pkg.web/*.wasm")))]
E += [cu("gzip"), cu("gzip", "decompress", True),
      ("gzip", "fflate", "fflate", ["gzipSync", "gunzipSync"], []),
      ("gzip", "pako", "pako", ["gzip", "ungzip"], [])]
E += [cu("lz4"), ("lz4", "lz4js", "lz4js", ["compress", "decompress"], [])]
E += [cu("snappy"), ("snappy", "snappyjs", "snappyjs", ["compress", "uncompress"], [])]
E += [cu("xz"), cu("xz", "decompress", True),
      ("xz", "xz-decompress (decode only)", "xz-decompress", ["XzReadableStream"], [])]
E += [cu("bz2"), cu("bz2", "decompress", True)]


def esbuild(src: str, out: Path) -> int:
    exe = NM / ".bin" / "esbuild"
    subprocess.run([str(exe), src, "--bundle", "--minify", "--format=esm", "--platform=browser",
                    "--log-level=error", "--external:node:*", f"--outfile={out}",
                    "--loader:.wasm=empty"], check=True, cwd=JS)
    return out.stat().st_size


def bundle_size(entry: tuple) -> dict:
    fmt, lib, spec, names, wasm = entry
    with tempfile.TemporaryDirectory(dir=JS) as td:
        td = Path(td)
        entry_js = td / "entry.js"
        if names == ["default"]:
            entry_js.write_text(f'import lib from "{spec}";\nglobalThis.__bench = lib;\n')
        else:
            entry_js.write_text(f'import {{ {", ".join(names)} }} from "{spec}";\n'
                                f'globalThis.__bench = [{", ".join(names)}];\n')
        out = td / "out.js"
        js_bytes = esbuild(str(entry_js), out)
        js_gz = len(gzip.compress(out.read_bytes(), 9))
    wasm_bytes = sum(w.stat().st_size for w in wasm if w.exists())
    wasm_gz = sum(len(gzip.compress(w.read_bytes(), 9)) for w in wasm if w.exists())
    return {"format": fmt, "library": lib, "js_bytes": js_bytes, "wasm_bytes": wasm_bytes,
            "total_bytes": js_bytes + wasm_bytes, "gzip_bytes": js_gz + wasm_gz}


# Cold start: an entry that imports the library by its real specifier and does one
# round trip on 1 KB, run unbundled in a fresh process.
ROUND_TRIP = {
    "zstd-wasm": 'const z = require("@bokuweb/zstd-wasm"); await z.init(); z.decompress(z.compress(d, 3));',
    "fzstd (decode only)": 'const z = require("@bokuweb/zstd-wasm"); const f = await import("fzstd"); await z.init(); f.decompress(z.compress(d, 3));',
    "brotli-wasm": 'const m = await require("brotli-wasm"); m.decompress(m.compress(d));',
    "fflate": 'const f = await import("fflate"); f.gunzipSync(f.gzipSync(d));',
    "pako": 'const p = await import("pako"); p.ungzip(p.gzip(d));',
    "lz4js": 'const l = require("lz4js"); l.decompress(l.compress(d));',
    "snappyjs": 'const s = require("snappyjs"); s.uncompress(s.compress(d));',
}


def cold_start(entry: tuple, runtime: str, baseline: float) -> float | None:
    fmt, lib, spec, names, wasm = entry
    if lib.startswith("compress-utils"):
        mod = f'file://{CU / fmt / ("decompress" if "decode" in lib else "") / "index.js"}'
        body = (f'const m = await import("{mod}"); await m.preload();'
                + ("" if "decode" in lib else ' m.decompressSync(m.compressSync(d));'))
        if "decode" in lib:
            return None  # needs compressed input from the full build; same module cost
    elif lib in ROUND_TRIP:
        body = ROUND_TRIP[lib]
    else:
        return None
    with tempfile.TemporaryDirectory(dir=JS) as td:
        f = Path(td) / "cold.mjs"
        f.write_text('import { createRequire } from "node:module";\n'
                     'const require = createRequire(import.meta.url);\n'
                     f"const d = new Uint8Array(1024).map((_, i) => (i * 7) & 31);\n{body}\n")
        times = []
        for _ in range(5):
            t0 = time.perf_counter()
            r = subprocess.run([runtime, str(f)], capture_output=True, text=True, cwd=JS)
            times.append(time.perf_counter() - t0)
            if r.returncode != 0:
                print(f"[js-size] cold start failed: {lib} on {runtime}: "
                      f"{(r.stderr.strip().splitlines() or ['?'])[-1][:160]}", file=sys.stderr)
                return None
    return max(0.0, statistics.median(times) - baseline) * 1000


def empty_baseline(runtime: str) -> float:
    with tempfile.TemporaryDirectory(dir=JS) as td:
        f = Path(td) / "empty.mjs"
        f.write_text("")
        times = []
        for _ in range(7):
            t0 = time.perf_counter()
            subprocess.run([runtime, str(f)], check=True, capture_output=True)
            times.append(time.perf_counter() - t0)
    return statistics.median(times)


def main() -> None:
    if not (NM / ".bin" / "esbuild").exists():
        sys.exit("error: install the competitors first:  (cd benchmarks/drivers/wasm && npm ci)")
    if not (CU / "zstd" / "zstd.wasm").exists():
        sys.exit("error: WASM package not built:  (cd bindings/wasm && npm run build)")
    runtimes = [r for r in ("node", "bun") if shutil.which(r)]
    base = {r: empty_baseline(r) for r in runtimes}

    rows = []
    for entry in E:
        row = bundle_size(entry)
        for r in runtimes:
            row[f"cold_ms_{r}"] = cold_start(entry, r, base[r])
        rows.append(row)

    kb = lambda v: f"{v / 1024:8.1f}"
    ms = lambda v: f"{v:7.0f}" if v is not None else f"{'-':>7}"
    print(f"\n  JS bundle size (browser, minified) and cold start (median of 5, minus an empty process)\n")
    print(f"  {'format':7} {'library':30} {'JS KiB':>8} {'WASM KiB':>9} {'total':>8} {'gzip':>8}"
          + "".join(f"  {'cold ms ' + r:>12}" for r in runtimes))
    print("  " + "-" * (82 + 14 * len(runtimes)))
    last = None
    for r in rows:
        if last and r["format"] != last:
            print()
        last = r["format"]
        print(f"  {r['format']:7} {r['library']:30} {kb(r['js_bytes'])} {kb(r['wasm_bytes']):>9} "
              f"{kb(r['total_bytes'])} {kb(r['gzip_bytes'])}"
              + "".join(f"  {ms(r.get(f'cold_ms_{rt}')):>12}" for rt in runtimes))
    print()

    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S")
    out = bc.RESULTS_DIR / f"js-size-{stamp}.json"
    bc.RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps({"machine": bc.machine_fingerprint(), "git_sha": bc.git_sha(),
                               "empty_process_s": base, "rows": rows}, indent=2) + "\n")
    print(f"[js-size] wrote {out}")


if __name__ == "__main__":
    main()
