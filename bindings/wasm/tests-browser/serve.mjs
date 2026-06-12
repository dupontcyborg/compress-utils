/**
 * Tiny static server for Playwright. Bundles the test consumer with
 * esbuild on startup, then serves the result over HTTP. esbuild handles
 * `new URL("./algo.wasm", import.meta.url)` correctly when targeting the
 * browser as long as we set `loader: { ".wasm": "file" }`.
 *
 * Listens on 127.0.0.1:4173 — matches playwright.config.ts baseURL.
 */

import http from "node:http";
import { readFile, mkdir, writeFile, copyFile, realpath } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";
import esbuild from "esbuild";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const PKG_ROOT = path.resolve(__dirname, "..");
const OUT = path.join(__dirname, ".serve");

await mkdir(OUT, { recursive: true });
const OUT_REAL = await realpath(OUT);

// The consumer references all six subpaths so the smoke test covers
// every algo in one page load.
const CONSUMER = `
import * as zstd   from "compress-utils/zstd";
import * as brotli from "compress-utils/brotli";
import * as zlib   from "compress-utils/zlib";
import * as bz2    from "compress-utils/bz2";
import * as lz4    from "compress-utils/lz4";
import * as xz     from "compress-utils/xz";

const enc = new TextEncoder();
const dec = new TextDecoder();
const algos = { zstd, brotli, zlib, bz2, lz4, xz };

async function main() {
    const results = {};
    const input = enc.encode("playwright payload ".repeat(200));
    for (const [name, m] of Object.entries(algos)) {
        try {
            const compressed = await m.compress(input);
            const back = await m.decompress(compressed);
            results[name] = dec.decode(back) === dec.decode(input) ? "ok" : "mismatch";
        } catch (e) {
            results[name] = "error: " + (e?.message || String(e));
        }
    }
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
        "compress-utils/zstd":   path.join(PKG_ROOT, "dist/algorithms/zstd/index.js"),
        "compress-utils/brotli": path.join(PKG_ROOT, "dist/algorithms/brotli/index.js"),
        "compress-utils/zlib":   path.join(PKG_ROOT, "dist/algorithms/zlib/index.js"),
        "compress-utils/bz2":    path.join(PKG_ROOT, "dist/algorithms/bz2/index.js"),
        "compress-utils/lz4":    path.join(PKG_ROOT, "dist/algorithms/lz4/index.js"),
        "compress-utils/xz":     path.join(PKG_ROOT, "dist/algorithms/xz/index.js"),
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
for (const algo of ["zstd", "brotli", "zlib", "bz2", "lz4", "xz"]) {
    await copyFile(
        path.join(PKG_ROOT, `dist/algorithms/${algo}/${algo}.wasm`),
        path.join(OUT, `${algo}.wasm`),
    );
}

const MIME = {
    ".html": "text/html; charset=utf-8",
    ".js":   "text/javascript; charset=utf-8",
    ".wasm": "application/wasm",
};

const server = http.createServer(async (req, res) => {
    let p;
    try {
        p = decodeURIComponent((req.url || "/").split("?")[0]);
    } catch {
        res.writeHead(400).end("bad request");
        return;
    }

    if (p === "/") p = "/index.html";
    if (p.includes("\0")) {
        res.writeHead(400).end("bad request");
        return;
    }

    const requestedPath = path.normalize(p).replace(/^([/\\])+/, "");
    if (
        path.isAbsolute(requestedPath) ||
        requestedPath.split(/[\\/]+/).includes("..")
    ) {
        res.writeHead(403).end("forbidden");
        return;
    }
    const file = path.resolve(OUT_REAL, requestedPath);

    let realFile;
    try {
        realFile = await realpath(file);
    } catch {
        res.writeHead(404).end("not found");
        return;
    }

    // Containment check on canonical paths: reject anything outside OUT.
    if (realFile !== OUT_REAL && !realFile.startsWith(OUT_REAL + path.sep)) {
        res.writeHead(403).end("forbidden");
        return;
    }

    try {
        const body = await readFile(realFile);
        const ext = path.extname(realFile);
        res.writeHead(200, { "Content-Type": MIME[ext] || "application/octet-stream" });
        res.end(body);
    } catch {
        res.writeHead(404).end("not found");
    }
});

server.listen(4173, "127.0.0.1", () => {
    console.log("Static server listening on http://127.0.0.1:4173");
});
