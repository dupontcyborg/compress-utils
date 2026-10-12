/**
 * Tiny static server for Playwright. Bundles the test consumer with
 * esbuild on startup, then serves the result over HTTP. esbuild handles
 * `new URL("./algo.wasm", import.meta.url)` correctly when targeting the
 * browser as long as we set `loader: { ".wasm": "file" }`.
 *
 * Listens on 127.0.0.1:4173 — matches playwright.config.ts baseURL.
 */

import http from "node:http";
import { readFile, mkdir, writeFile, copyFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import esbuild from "esbuild";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PKG_ROOT = path.resolve(__dirname, "..");
const OUT = path.join(__dirname, ".serve");

await mkdir(OUT, { recursive: true });

// The consumer references all eight subpaths so the smoke test covers
// every algo in one page load.
const CONSUMER = `
import * as zstd   from "compress-utils/zstd";
import * as brotli from "compress-utils/brotli";
import * as zlib   from "compress-utils/zlib";
import * as bz2    from "compress-utils/bz2";
import * as lz4    from "compress-utils/lz4";
import * as xz     from "compress-utils/xz";
import * as snappy from "compress-utils/snappy";
import * as gzip from "compress-utils/gzip";
import * as deflate from "compress-utils/deflate";
import * as lz4_raw from "compress-utils/lz4_raw";

import * as lzo from "compress-utils/lzo/decompress";
const enc = new TextEncoder();
const dec = new TextDecoder();
const algos = { zstd, brotli, zlib, bz2, lz4, xz, snappy, gzip, deflate, lz4_raw };

async function main() {
    const results = {};
    const input = enc.encode("playwright payload ".repeat(200));
    for (const [name, m] of Object.entries(algos)) {
        try {
            const asyncCompressed = await m.compress(input);
            const asyncRestored = await m.decompress(asyncCompressed, { expectedSize: input.length });
            if (dec.decode(asyncRestored) !== dec.decode(input)) {
                results[name] = "async mismatch";
                continue;
            }
            await m.preload();
            const compressed = m.compressSync(input);
            const back = m.decompressSync(compressed, { expectedSize: input.length });
            results[name] = dec.decode(back) === dec.decode(input) ? "ok" : "sync mismatch";
        } catch (e) {
            results[name] = "error: " + (e?.message || String(e));
        }
    }
    const lzoInput = new Uint8Array([22,104,101,108,108,111,17,0,0]);
    let lzoRequiresPreload = false;
    try { lzo.decompressSync(lzoInput, {expectedSize: 10}); }
    catch (error) { lzoRequiresPreload = /preload/.test(error.message); }
    await Promise.all([lzo.preload(), lzo.preload()]);
    const lzoSyncOutput = lzo.decompressSync(lzoInput, {expectedSize: 10});
    const lzoAsyncOutput = await lzo.decompress(lzoInput, {expectedSize: 10});
    results.lzo = lzoRequiresPreload && dec.decode(lzoSyncOutput) === "hello" && dec.decode(lzoAsyncOutput) === "hello" ? "ok" : "mismatch";
    globalThis.__cuResults = results;
    document.title = "ready";
}
main();
`;

const consumerEntry = path.join(OUT, "consumer.js");
await writeFile(consumerEntry, CONSUMER, "utf8");

await esbuild.build({
    entryPoints: [consumerEntry],
    bundle: true,
    format: "esm",
    platform: "browser",
    target: "es2022",
    outfile: path.join(OUT, "bundle.js"),
    loader: { ".wasm": "file" },
    conditions: ["browser", "import"],
    alias: {
        "compress-utils/lzo/decompress": path.join(PKG_ROOT, "dist/algorithms/lzo/decompress/index.js"),
        "compress-utils/zstd":   path.join(PKG_ROOT, "dist/algorithms/zstd/index.js"),
        "compress-utils/brotli": path.join(PKG_ROOT, "dist/algorithms/brotli/index.js"),
        "compress-utils/zlib":   path.join(PKG_ROOT, "dist/algorithms/zlib/index.js"),
        "compress-utils/bz2":    path.join(PKG_ROOT, "dist/algorithms/bz2/index.js"),
        "compress-utils/lz4":    path.join(PKG_ROOT, "dist/algorithms/lz4/index.js"),
        "compress-utils/xz":     path.join(PKG_ROOT, "dist/algorithms/xz/index.js"),
        "compress-utils/snappy": path.join(PKG_ROOT, "dist/algorithms/snappy/index.js"),
        "compress-utils/gzip": path.join(PKG_ROOT, "dist/algorithms/gzip/index.js"),
        "compress-utils/deflate": path.join(PKG_ROOT, "dist/algorithms/deflate/index.js"),
        "compress-utils/lz4_raw": path.join(PKG_ROOT, "dist/algorithms/lz4_raw/index.js"),
    },
    logLevel: "info",
});

const INDEX_HTML = `<!doctype html>
<title>loading</title>
<meta charset="utf-8">
<script type="module" src="./bundle.js"></script>
`;
await writeFile(path.join(OUT, "index.html"), INDEX_HTML, "utf8");

// esbuild's `file` loader only kicks in on `import "./x.wasm"`, not on
// `new URL("./x.wasm", import.meta.url)`. We use the latter (it's the
// pattern Vite/webpack5 understand natively); for this self-hosted test
// just copy the .wasm files alongside the bundle so the runtime URL
// resolves to a served file.
for (const algo of ["zstd", "brotli", "zlib", "bz2", "lz4", "xz", "snappy", "gzip"]) {
    await copyFile(
        path.join(PKG_ROOT, `dist/algorithms/${algo}/${algo}.wasm`),
        path.join(OUT, `${algo}.wasm`),
    );
}

// Raw bindings reference the same assets through sibling codec URLs.
for (const codec of ["zlib", "lz4"]) {
    await mkdir(path.join(OUT, codec), { recursive: true });
    await copyFile(path.join(PKG_ROOT, `dist/algorithms/${codec}/${codec}.wasm`),
        path.join(OUT, codec, `${codec}.wasm`));
}

await copyFile(path.join(PKG_ROOT, "dist/algorithms/lzo/decompress/lzo.wasm"), path.join(OUT, "lzo.wasm"));
const MIME = {
    ".html": "text/html; charset=utf-8",
    ".js":   "text/javascript; charset=utf-8",
    ".wasm": "application/wasm",
};

const server = http.createServer(async (req, res) => {
    let p = decodeURIComponent((req.url || "/").split("?")[0]);
    if (p === "/") p = "/index.html";
    const file = path.resolve(OUT, p.replace(/^\//, ""));
    // Containment check: reject anything that resolves outside OUT.
    if (file !== OUT && !file.startsWith(OUT + path.sep)) {
        res.writeHead(403).end("forbidden");
        return;
    }
    try {
        const body = await readFile(file);
        const ext = path.extname(file);
        res.writeHead(200, { "Content-Type": MIME[ext] || "application/octet-stream" });
        res.end(body);
    } catch {
        res.writeHead(404).end("not found");
    }
});

server.listen(4173, "127.0.0.1", () => {
    console.log("Static server listening on http://127.0.0.1:4173");
});
