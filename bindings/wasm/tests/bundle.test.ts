/**
 * Bundle + tree-shake regression test.
 *
 * What we assert (and why):
 *   1. A consumer that imports only `compress-utils/zstd` builds without
 *      pulling brotli/lz4/xz/zlib/bz2 wasm into the bundle. The .wasm
 *      files are the bulk; if cross-algo references leak, those files
 *      become part of the bundle graph and the artifact size doubles.
 *   2. The final JS bundle (excluding .wasm) stays small. The TS layer
 *      is shared, so this is mostly an upper bound on glue code.
 *   3. Per-algo .wasm artifacts respect their size budgets. Set these
 *      generously today; tighten as we shrink the codecs.
 *
 * We use esbuild because it's small, fast, and the npm install is a
 * single binary. Vite would be more realistic but adds ~80 deps. The
 * tree-shaking we care about is ESM-spec-level — esbuild is sufficient.
 */

import { describe, it, expect } from "vitest";
import { build } from "esbuild";
import { writeFile, mkdtemp, readFile, readdir, stat } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";

const ALL_ALGOS = ["zstd", "brotli", "zlib", "bz2", "lz4", "xz", "snappy", "gzip"] as const;
const PKG_ROOT = path.resolve(__dirname, "..");

interface BundleResult {
    jsBytes: number;
    /** Algo names whose `.wasm` URL appears in the bundled JS source. */
    referencedAlgos: Set<string>;
}

async function bundleConsumer(consumerSrc: string): Promise<BundleResult> {
    const dir = await mkdtemp(path.join(tmpdir(), "cu-bundle-"));
    const entry = path.join(dir, "consumer.ts");
    await writeFile(entry, consumerSrc, "utf8");

    const outdir = path.join(dir, "out");
    await build({
        entryPoints: [entry],
        bundle: true,
        format: "esm",
        platform: "browser",
        target: "es2022",
        outdir,
        // Resolve through this package's package.json exports so esbuild
        // picks the "browser" condition. The single algo index.js routes
        // through the `#resolver` subpath import, which the conditions
        // below map to resolve-browser.js — fetch-only, no node imports.
        nodePaths: [path.join(PKG_ROOT, "node_modules")],
        conditions: ["browser", "import"],
        alias: {
            "compress-utils/lzo/decompress": path.join(
                PKG_ROOT,
                "dist/algorithms/lzo/decompress/index.js",
            ),
            "compress-utils/zstd": path.join(PKG_ROOT, "dist/algorithms/zstd/index.js"),
            "compress-utils/brotli": path.join(PKG_ROOT, "dist/algorithms/brotli/index.js"),
            "compress-utils/zlib": path.join(PKG_ROOT, "dist/algorithms/zlib/index.js"),
            "compress-utils/bz2": path.join(PKG_ROOT, "dist/algorithms/bz2/index.js"),
            "compress-utils/lz4": path.join(PKG_ROOT, "dist/algorithms/lz4/index.js"),
            "compress-utils/xz": path.join(PKG_ROOT, "dist/algorithms/xz/index.js"),
            "compress-utils/snappy": path.join(PKG_ROOT, "dist/algorithms/snappy/index.js"),
            "compress-utils/gzip": path.join(PKG_ROOT, "dist/algorithms/gzip/index.js"),
        },
        logLevel: "silent",
    });

    let jsBytes = 0;
    let jsSource = "";
    for (const name of await readdir(outdir)) {
        const full = path.join(outdir, name);
        if (name.endsWith(".js")) {
            const s = await stat(full);
            jsBytes += s.size;
            jsSource += await readFile(full, "utf8");
        }
    }
    // The dispatcher constructs each wasm URL as `new URL("./<algo>.wasm",
    // import.meta.url)`. After bundling, that literal becomes
    // `new URL("./zstd.wasm", ...)` — a stable, greppable marker that
    // proves the algo subpath ended up in the graph.
    const referencedAlgos = new Set<string>();
    for (const algo of [...ALL_ALGOS, "lzo"]) {
        if (jsSource.includes(`"./${algo}.wasm"`)) {
            referencedAlgos.add(algo);
        }
    }
    return { jsBytes, referencedAlgos };
}

describe("tree-shaking: per-algo subpath isolation", () => {
    it("zstd-only consumer references only zstd.wasm", async () => {
        const result = await bundleConsumer(`
            import { compress } from "compress-utils/zstd";
            globalThis.__cu = compress;
        `);
        expect([...result.referencedAlgos]).toEqual(["zstd"]);
    });

    it("multi-algo consumer references only the imported algos", async () => {
        const result = await bundleConsumer(`
            import { compress as cz } from "compress-utils/zstd";
            import { compress as cb } from "compress-utils/brotli";
            globalThis.__cu = [cz, cb];
        `);
        expect([...result.referencedAlgos].sort()).toEqual(["brotli", "zstd"]);
    });
});

describe("per-algo .wasm size budgets", () => {
    // Tightened after the WASM size pass (docs/wasm-size.md #1–#3 + brotli
    // EARLY static-init). Each cap is ~8–12% over the current artifact —
    // enough headroom for routine codec drift, tight enough that a real
    // regression trips it (e.g. --export-dynamic returning would blow zstd
    // past 440; brotli falling back to BROTLI_STATIC_INIT=NONE jumps it to
    // ~800, well past its cap). Current sizes: zlib 86.9, bz2 93.8, xz 141.1,
    // lz4 120.2, snappy 34.6, gzip 84.9, zstd 402.8, brotli 490.1 KB. Single-stream
    // decoding (CU_DSTREAM_SINGLE) added 1-3.6 KB to every decoder, which left
    // lz4, zlib, xz and gzip under 8% headroom: raised lz4 120 → 130, zlib
    // 88 → 96, xz 145 → 155, gzip 90 → 93.
    const BUDGET_KB: Record<string, number> = {
        zlib: 96,
        bz2: 102,
        xz: 155,
        lz4: 130,
        snappy: 60,
        gzip: 93,
        zstd: 440,
        brotli: 565,
    };

    for (const algo of ALL_ALGOS) {
        it(`${algo}.wasm ≤ ${BUDGET_KB[algo]} KB`, async () => {
            const p = path.join(PKG_ROOT, `dist/algorithms/${algo}/${algo}.wasm`);
            const buf = await readFile(p);
            const kb = buf.byteLength / 1024;
            expect(kb).toBeLessThan(BUDGET_KB[algo]!);
        });
    }
});

describe("direction-variant .wasm size budgets", () => {
    // decompress-only / compress-only builds (CU_WASM_DIR). The decode budgets
    // are the regression gate that matters most — if the encoder ever leaks
    // back into a decode-only module (e.g. a codec vtable stops honoring
    // CU_OMIT_COMPRESS), the size jumps obviously past these caps. ~10% over
    // current; current decode KB: zstd 93.4, brotli 186.3, lz4 46.8, bz2 53.7,
    // xz 85.7, zlib 54.5, snappy 30.1, gzip 53.1 — encode KB: zstd 339, brotli
    // 446, lz4 97, bz2 65, xz 105, zlib 62, snappy 25, gzip 61. Single-stream
    // decoding (CU_DSTREAM_SINGLE) added 1-3.6 KB to every decoder: raised the
    // decode caps for zstd 100 → 103, lz4 45 → 52, xz 92 → 94, zlib 58 → 60.
    const VARIANT_KB: Record<string, { decompress: number; compress: number }> = {
        zstd: { decompress: 103, compress: 380 },
        brotli: { decompress: 205, compress: 515 },
        zlib: { decompress: 60, compress: 68 },
        bz2: { decompress: 62, compress: 75 },
        lz4: { decompress: 52, compress: 108 },
        xz: { decompress: 94, compress: 112 },
        snappy: { decompress: 52, compress: 48 },
        gzip: { decompress: 60, compress: 70 },
    };

    for (const algo of ALL_ALGOS) {
        for (const dir of ["decompress", "compress"] as const) {
            const cap = VARIANT_KB[algo]![dir];
            it(`${algo}/${dir}/${algo}.wasm ≤ ${cap} KB`, async () => {
                const p = path.join(PKG_ROOT, `dist/algorithms/${algo}/${dir}/${algo}.wasm`);
                const buf = await readFile(p);
                expect(buf.byteLength / 1024).toBeLessThan(cap);
            });
        }
    }
});

describe("decoder-only LZO bundle", () => {
    it("references only lzo.wasm", async () => {
        const result = await bundleConsumer(
            `import {decompress} from "compress-utils/lzo/decompress"; globalThis.__cu = decompress;`,
        );
        expect([...result.referencedAlgos]).toEqual(["lzo"]);
    });
    it("keeps the incremental decoder below 32 KiB", async () => {
        const bytes = await readFile(
            path.join(PKG_ROOT, "dist/algorithms/lzo/decompress/lzo.wasm"),
        );
        // Includes the native incremental state machine and public stream ABI exports.
        // Raised 28 → 32 KiB (current 29.3) for the shared single-stream
        // decoding dispatch (CU_DSTREAM_SINGLE), which LZO links but can't use.
        expect(bytes.length).toBeLessThan(32 * 1024);
    });
});
