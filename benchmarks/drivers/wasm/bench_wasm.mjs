/*
 * WASM benchmark driver (Node).
 *
 * Speaks the same benchmark driver protocol as the C drivers (see
 * benchmarks/README.md): reads "<algo> <level> [<mode>] <path>" job lines from
 * stdin, emits one NDJSON result (or skip/error marker) per line, honours
 * BENCH_SAMPLES / BENCH_WARMUP / BENCH_CHUNK, and answers `--info`.
 *
 * It drives the built compress-utils WASM package (bindings/wasm/dist) the same
 * way a real consumer would: `import('compress-utils/<algo>')` →
 * compress/decompress + createCompressStream/createDecompressStream. Each
 * record also carries `wasm_size_bytes` (the module's on-disk size) — the
 * primary metric for the WASM size-reduction work.
 *
 * Run: node bench_wasm.mjs        (or `--info`)
 */

import { readFileSync, statSync } from "node:fs";
import { createInterface } from "node:readline";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, "../../..");
const DIST = resolve(REPO, "bindings/wasm/dist/algorithms");

const ALGOS = new Set(["zstd", "brotli", "zlib", "bz2", "lz4", "xz", "snappy"]);

const SAMPLES = Number(process.env.BENCH_SAMPLES) || 5;
const WARMUP = Number(process.env.BENCH_WARMUP) || 1;
const CHUNK = Number(process.env.BENCH_CHUNK) || 64 * 1024;

const modCache = new Map();
function loadModule(algo) {
    if (!modCache.has(algo)) {
        const url = pathToFileURL(resolve(DIST, algo, "index.js")).href;
        modCache.set(algo, import(url));
    }
    return modCache.get(algo);
}
function wasmSize(algo) {
    try {
        return statSync(resolve(DIST, algo, `${algo}.wasm`)).size;
    } catch {
        return 0;
    }
}

const now = () => process.hrtime.bigint();

function stats(samples) {
    const s = [...samples].sort((a, b) => a - b);
    const n = s.length;
    const median = n % 2 ? s[(n - 1) / 2] : (s[n / 2 - 1] + s[n / 2]) / 2;
    const dev = s.map((x) => Math.abs(x - median)).sort((a, b) => a - b);
    const mad = n % 2 ? dev[(n - 1) / 2] : (dev[n / 2 - 1] + dev[n / 2]) / 2;
    return { median: Math.round(median), mad: Math.round(mad), min: s[0] };
}

function concat(parts) {
    let total = 0;
    for (const p of parts) total += p.length;
    const out = new Uint8Array(total);
    let off = 0;
    for (const p of parts) {
        out.set(p, off);
        off += p.length;
    }
    return out;
}

async function compressOnce(mod, input, level, isStream) {
    if (!isStream) return mod.compress(input, { level });
    const cs = await mod.createCompressStream({ level });
    try {
        const parts = [];
        for (let off = 0; off < input.length; off += CHUNK) {
            parts.push(cs.write(input.subarray(off, off + CHUNK)));
        }
        parts.push(cs.finish());
        return concat(parts);
    } finally {
        cs.destroy();
    }
}

async function decompressOnce(mod, comp, isStream) {
    if (!isStream) return mod.decompress(comp);
    const ds = await mod.createDecompressStream();
    try {
        const parts = [];
        for (let off = 0; off < comp.length; off += CHUNK) {
            parts.push(ds.write(comp.subarray(off, off + CHUNK)));
        }
        parts.push(ds.finish());
        return concat(parts);
    } finally {
        ds.destroy();
    }
}

function bytesEqual(a, b) {
    if (a.length !== b.length) return false;
    return Buffer.compare(Buffer.from(a.buffer, a.byteOffset, a.length),
                          Buffer.from(b.buffer, b.byteOffset, b.length)) === 0;
}

async function runJob(algo, level, isStream, path) {
    const mod = await loadModule(algo);
    const input = readFileSync(path);

    let comp;
    for (let i = 0; i < WARMUP; i++) comp = await compressOnce(mod, input, level, isStream);
    const cT = [];
    for (let i = 0; i < SAMPLES; i++) {
        const t0 = now();
        comp = await compressOnce(mod, input, level, isStream);
        cT.push(Number(now() - t0));
    }

    let dec;
    for (let i = 0; i < WARMUP; i++) dec = await decompressOnce(mod, comp, isStream);
    const dT = [];
    for (let i = 0; i < SAMPLES; i++) {
        const t0 = now();
        dec = await decompressOnce(mod, comp, isStream);
        dT.push(Number(now() - t0));
    }

    const verified = bytesEqual(input, dec);
    const c = stats(cT);
    const d = stats(dT);
    return {
        lang: "wasm",
        impl: "compress-utils",
        algo,
        level,
        mode: isStream ? "stream" : "oneshot",
        chunk_bytes: isStream ? CHUNK : 0,
        input: path,
        input_bytes: input.length,
        output_bytes: comp.length,
        compress_ns_median: c.median,
        compress_ns_mad: c.mad,
        compress_ns_min: c.min,
        decompress_ns_median: d.median,
        decompress_ns_mad: d.mad,
        decompress_ns_min: d.min,
        samples: SAMPLES,
        warmup: WARMUP,
        verified,
        wasm_size_bytes: wasmSize(algo),
    };
}

function emit(obj) {
    process.stdout.write(JSON.stringify(obj) + "\n");
}

async function main() {
    if (process.argv[2] === "--info") {
        const pkg = JSON.parse(
            readFileSync(resolve(REPO, "bindings/wasm/package.json"), "utf8"),
        );
        emit({ lang: "wasm", version: pkg.version, driver: "wasm" });
        return;
    }

    const rl = createInterface({ input: process.stdin });
    for await (const raw of rl) {
        const line = raw.trim();
        if (!line) continue;
        // "<algo> <level> [<mode>] <path>"; path may contain spaces.
        const m = line.match(/^(\S+)\s+(\S+)\s+(.*)$/);
        if (!m) { emit({ error: true }); continue; }
        const algo = m[1];
        const level = parseInt(m[2], 10);
        let rest = m[3];
        let isStream = false;
        if (rest.startsWith("stream ")) { isStream = true; rest = rest.slice(7); }
        else if (rest.startsWith("oneshot ")) { rest = rest.slice(8); }
        const path = rest.trim();

        if (!ALGOS.has(algo)) { emit({ skipped: true }); continue; }
        try {
            emit(await runJob(algo, level, isStream, path));
        } catch (e) {
            process.stderr.write(`bench-wasm: ${algo} L${level} ${isStream ? "stream" : "oneshot"} failed: ${e?.message || e}\n`);
            emit({ error: true });
        }
    }
}

main().catch((e) => {
    process.stderr.write(`bench-wasm fatal: ${e?.stack || e}\n`);
    process.exit(1);
});
