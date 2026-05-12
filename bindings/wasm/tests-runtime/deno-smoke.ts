/**
 * Deno smoke test. Imports each algo subpath, round-trips a payload,
 * exits non-zero if anything fails. Run via:
 *
 *   deno run --allow-read --allow-env tests-runtime/deno-smoke.ts
 *
 * Why no vitest: vitest is Node-API-coupled (vite, esbuild plugins,
 * jest-like discovery). Deno supports `Deno.test` natively but importing
 * vitest pulls a long tail of Node APIs we'd shim for marginal benefit.
 * A 60-line smoke test is enough to catch Deno-specific regressions
 * (different fetch defaults, file:// URL handling, WASI shim mismatches).
 */

const PKG = new URL("../dist/algorithms/", import.meta.url);

// Deno's fetch handles file:// natively — pick the browser-shaped entry
// (streaming compile via fetch). The "default" entry would work too but
// would carry the runtime-detection branch we don't need here.
const algos = [
    { name: "zstd",   url: new URL("zstd/index.browser.js",   PKG) },
    { name: "brotli", url: new URL("brotli/index.browser.js", PKG) },
    { name: "zlib",   url: new URL("zlib/index.browser.js",   PKG) },
    { name: "bz2",    url: new URL("bz2/index.browser.js",    PKG) },
    { name: "lz4",    url: new URL("lz4/index.browser.js",    PKG) },
    { name: "xz",     url: new URL("xz/index.browser.js",     PKG) },
];

const enc = new TextEncoder();
const dec = new TextDecoder();
const input = enc.encode("deno payload ".repeat(500));

let failed = 0;
for (const { name, url } of algos) {
    try {
        const mod = await import(url.href);
        const compressed = await mod.compress(input);
        const back = await mod.decompress(compressed);
        const ok = dec.decode(back) === dec.decode(input);
        if (!ok) throw new Error("payload mismatch");
        console.log(`  ✓ ${name}`);
    } catch (e) {
        failed++;
        console.error(`  ✗ ${name}: ${e instanceof Error ? e.message : e}`);
    }
}

if (failed > 0) {
    console.error(`\n${failed} algorithm(s) failed`);
    Deno.exit(1);
}
console.log("\nAll algorithms passed under Deno");
