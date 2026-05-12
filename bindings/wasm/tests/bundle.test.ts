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

const ALL_ALGOS = ["zstd", "brotli", "zlib", "bz2", "lz4", "xz"] as const;
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
        // picks the "browser" condition (→ index.browser.js, fetch-only).
        // No `external: ["node:*"]` needed — the browser entry has no
        // node imports in its graph.
        nodePaths: [path.join(PKG_ROOT, "node_modules")],
        conditions: ["browser", "import"],
        alias: {
            "compress-utils/zstd":   path.join(PKG_ROOT, "dist/algorithms/zstd/index.browser.js"),
            "compress-utils/brotli": path.join(PKG_ROOT, "dist/algorithms/brotli/index.browser.js"),
            "compress-utils/zlib":   path.join(PKG_ROOT, "dist/algorithms/zlib/index.browser.js"),
            "compress-utils/bz2":    path.join(PKG_ROOT, "dist/algorithms/bz2/index.browser.js"),
            "compress-utils/lz4":    path.join(PKG_ROOT, "dist/algorithms/lz4/index.browser.js"),
            "compress-utils/xz":     path.join(PKG_ROOT, "dist/algorithms/xz/index.browser.js"),
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
    for (const algo of ALL_ALGOS) {
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
    // These caps are generous — set so a regression is obvious without
    // failing on routine codec drift. Tighten when we do the size pass.
    const BUDGET_KB: Record<string, number> = {
        zlib: 120,
        bz2: 140,
        xz: 200,
        lz4: 200,
        zstd: 700,
        brotli: 850,
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
