/*
 * JavaScript benchmark driver, for the compress-utils WASM package and for the
 * portable JavaScript/WASM libraries it competes with.
 *
 * Speaks the shared benchmark driver protocol (see benchmarks/README.md): reads
 * "<algo> <level> [<mode>] <path>" job lines from stdin (mode: oneshot, stream
 * or msgset), emits one NDJSON result (or skip/error marker) per line, honours
 * BENCH_SAMPLES / BENCH_WARMUP / BENCH_CHUNK, and answers `--info`.
 *
 * `--impl NAME` picks the implementation (default compress-utils). Every one is
 * called as a consumer would; the same file runs under Node and under Bun
 * (BENCH_JS_RUNTIME, set by the runner), so one set of competitors is measured
 * on V8 and on JavaScriptCore. Records carry `runtime`.
 *
 *   oneshot  one compress call, then one decompress call
 *   msgset   one synchronous call per message, each decompressed with its known
 *            size through the library's fastest path for that
 *   stream   the input fed in BENCH_CHUNK pieces through the library's
 *            streaming API; skipped for libraries without one
 *
 * Competitors are limited to what runs in browsers, edge runtimes and Deno as
 * well as Node and Bun: pure JavaScript and WASM libraries, plus the two built-ins
 * every server has (node:zlib, labelled Node-only because it is native code, and
 * CompressionStream, the browser standard). Native Node addons are out.
 *
 * Libraries that only decompress (fzstd, xz-decompress) get their input from
 * another library, untimed, and the record carries decode_only: true with
 * compress timings of 0. Competitors run at native levels; formats without levels
 * (lz4js, snappyjs, CompressionStream) run once. compress-utils decodes each
 * competitor's output once per job, untimed, and the record carries `interop`.
 *
 * Run: node bench_wasm.mjs [--impl NAME] [--info]
 */

import { readFileSync, readSync, statSync } from "node:fs";
import { createRequire } from "node:module";
import { dirname, resolve } from "node:path";
import { once } from "node:events";
import { fileURLToPath, pathToFileURL } from "node:url";

const HERE = dirname(fileURLToPath(import.meta.url));
const REPO = resolve(HERE, "../../..");
const DIST = resolve(REPO, "bindings/wasm/dist/algorithms");
const require = createRequire(import.meta.url);
const RUNTIME = process.versions.bun ? "bun" : "node";

const SAMPLES = Number(process.env.BENCH_SAMPLES) || 5;
const WARMUP = Number(process.env.BENCH_WARMUP) || 1;
const CHUNK = Number(process.env.BENCH_CHUNK) || 64 * 1024;

const now = () => process.hrtime.bigint();

/* ---- compress-utils WASM package -------------------------------------------------- */

const modCache = new Map();
function loadModule(algo) {
    if (!modCache.has(algo)) {
        modCache.set(algo, import(pathToFileURL(resolve(DIST, algo, "index.js")).href));
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

/* ---- helpers --------------------------------------------------------------------- */

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

const chunksOf = (data, size = CHUNK) => {
    const out = [];
    for (let off = 0; off < data.length; off += size) out.push(data.subarray(off, off + size));
    return out;
};

const u8 = (x) => (x instanceof Uint8Array ? x : new Uint8Array(x));

function bytesEqual(a, b) {
    a = u8(a);
    b = u8(b);
    if (a.length !== b.length) return false;
    return Buffer.compare(Buffer.from(a.buffer, a.byteOffset, a.length),
                          Buffer.from(b.buffer, b.byteOffset, b.length)) === 0;
}

function libVersion(pkg) {
    try {
        return JSON.parse(readFileSync(resolve(HERE, "node_modules", pkg, "package.json"), "utf8")).version;
    } catch {
        return "?";
    }
}

/* Reads a message set: records of a u32 little-endian length then that many bytes. */
function readMsgset(path) {
    const data = readFileSync(path);
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const msgs = [];
    for (let pos = 0; pos < data.length; ) {
        if (data.length - pos < 4) throw new Error("truncated message set");
        const n = view.getUint32(pos, true);
        pos += 4;
        if (n > data.length - pos) throw new Error("truncated message set");
        msgs.push(data.subarray(pos, pos + n));
        pos += n;
    }
    return msgs;
}

/* ---- implementations -------------------------------------------------------------
 *
 * A codec: { compress(data, level), decompress(comp, size | null) }, synchronous;
 * optional async cstream(data, level) and dstream(comp); `levels` [lo, hi] when the
 * library declares a native range, `levelless` when the format has none; decodeOnly
 * { produce(data, level) } for libraries that only decompress. A codec without
 * `compress` is stream-only (CompressionStream). Level 0 is what the runner sends a
 * levelless codec.
 */

async function implCompressUtils() {
    const pkg = JSON.parse(readFileSync(resolve(REPO, "bindings/wasm/package.json"), "utf8"));
    const codecs = {};
    for (const algo of ["zstd", "brotli", "zlib", "gzip", "deflate", "bz2", "lz4", "xz", "snappy"]) {
        let mod;
        try {
            mod = await loadModule(algo);
        } catch {
            continue;
        }
        await mod.preload();
        codecs[algo] = {
            async: false,
            compress: (d, level) => mod.compressSync(d, { level }),
            decompress: (c, n) => mod.decompressSync(c, n == null ? {} : { expectedSize: n }),
            cstream: async (d, level) => {
                const cs = await mod.createCompressStream({ level });
                try {
                    const parts = chunksOf(d).map((c) => cs.write(c));
                    parts.push(cs.finish());
                    return concat(parts);
                } finally {
                    cs.destroy();
                }
            },
            dstream: async (c) => {
                const ds = await mod.createDecompressStream();
                try {
                    const parts = chunksOf(c).map((p) => ds.write(p));
                    parts.push(ds.finish());
                    return concat(parts);
                } finally {
                    ds.destroy();
                }
            },
            wasmSize: wasmSize(algo),
        };
    }
    return { version: `${pkg.version} (${RUNTIME} ${process.versions.bun || process.versions.node})`, codecs };
}

async function implNodeZlib() {
    const z = await import("node:zlib");
    const c = z.constants;

    /* The streaming API: chunks written to a Transform, output collected. */
    const through = async (make, data) => {
        const t = make();
        const out = [];
        t.on("data", (x) => out.push(x));
        const done = new Promise((res, rej) => {
            t.on("end", res);
            t.on("error", rej);
        });
        for (const ch of chunksOf(data)) if (!t.write(ch)) await once(t, "drain");
        t.end();
        await done;
        return concat(out);
    };
    const deflateFamily = (compressSync, decompressSync, createC, createD) => ({
        compress: (d, level) => compressSync(d, { level }),
        decompress: (x) => decompressSync(x),
        cstream: (d, level) => through(() => createC({ level }), d),
        dstream: (x) => through(() => createD(), x),
        levels: [1, 9],
    });
    const codecs = {
        zlib: deflateFamily(z.deflateSync, z.inflateSync, z.createDeflate, z.createInflate),
        gzip: deflateFamily(z.gzipSync, z.gunzipSync, z.createGzip, z.createGunzip),
        deflate: deflateFamily(z.deflateRawSync, z.inflateRawSync, z.createDeflateRaw, z.createInflateRaw),
        brotli: {
            compress: (d, level) => z.brotliCompressSync(d, { params: { [c.BROTLI_PARAM_QUALITY]: level } }),
            decompress: (x) => z.brotliDecompressSync(x),
            cstream: (d, level) => through(() => z.createBrotliCompress({ params: { [c.BROTLI_PARAM_QUALITY]: level } }), d),
            dstream: (x) => through(() => z.createBrotliDecompress(), x),
            levels: [0, 11],
        },
    };
    if (typeof z.zstdCompressSync === "function") {
        codecs.zstd = {
            compress: (d, level) => z.zstdCompressSync(d, { params: { [c.ZSTD_c_compressionLevel]: level } }),
            decompress: (x) => z.zstdDecompressSync(x),
            cstream: (d, level) => through(() => z.createZstdCompress({ params: { [c.ZSTD_c_compressionLevel]: level } }), d),
            dstream: (x) => through(() => z.createZstdDecompress(), x),
            levels: [1, 22],
        };
    }
    return { version: `${RUNTIME} ${process.versions.bun || process.versions.node} (native code, Node-only)`, codecs };
}

async function implCompressionStream() {
    /* The browser standard: async, stream-only, no levels. */
    const run = async (Ctor, format, data) => {
        const t = new Ctor(format);
        const w = t.writable.getWriter();
        const r = t.readable.getReader();
        const out = [];
        const reading = (async () => {
            for (;;) {
                const { done, value } = await r.read();
                if (done) break;
                out.push(value);
            }
        })();
        for (const ch of chunksOf(data)) await w.write(ch);
        await w.close();
        await reading;
        return concat(out);
    };
    const codecs = {};
    for (const [algo, format] of [["zlib", "deflate"], ["gzip", "gzip"], ["deflate", "deflate-raw"], ["brotli", "brotli"]]) {
        try {
            new CompressionStream(format);
        } catch {
            continue;
        }
        codecs[algo] = {
            levelless: true,
            cstream: (d) => run(CompressionStream, format, d),
            dstream: (x) => run(DecompressionStream, format, x),
        };
    }
    return { version: `${RUNTIME} ${process.versions.bun || process.versions.node} (web standard)`, codecs };
}

async function implFflate() {
    const f = await import("fflate");
    const push = (Stream, opts, data, final) => {
        const parts = [];
        const s = new Stream(opts);
        s.ondata = (chunk) => parts.push(chunk); // the sync classes' signature: (chunk, final)
        const cs = chunksOf(data);
        cs.forEach((ch, i) => s.push(ch, final && i === cs.length - 1));
        if (!final) s.push(new Uint8Array(0), true);
        return concat(parts);
    };
    const fam = (cSync, dSync, CStream, DStream) => ({
        compress: (d, level) => cSync(d, { level }),
        decompress: (x, n) => (n ? dSync(x, { out: new Uint8Array(n) }) : dSync(x)),
        cstream: (d, level) => push(CStream, { level }, d, false),
        dstream: (x) => push(DStream, {}, x, true),
        levels: [0, 9],
    });
    return { version: libVersion("fflate"), codecs: {
        zlib: fam(f.zlibSync, f.unzlibSync, f.Zlib, f.Unzlib),
        gzip: fam(f.gzipSync, f.gunzipSync, f.Gzip, f.Gunzip),
        deflate: fam(f.deflateSync, f.inflateSync, f.Deflate, f.Inflate),
    } };
}

async function implPako() {
    const p = await import("pako");
    const stream = (Ctor, opts, data) => {
        const s = new Ctor(opts);
        const cs = chunksOf(data);
        cs.forEach((ch, i) => s.push(ch, i === cs.length - 1));
        if (s.err) throw new Error(s.msg);
        return s.result;
    };
    const fam = (c, d, copts, dopts) => ({
        compress: (data, level) => c(data, { level }),
        decompress: (x) => d(x),
        cstream: (data, level) => stream(p.Deflate, { level, ...copts }, data),
        dstream: (x) => stream(p.Inflate, dopts, x),
        levels: [0, 9],
    });
    return { version: libVersion("pako"), codecs: {
        zlib: fam(p.deflate, p.inflate, {}, {}),
        gzip: fam(p.gzip, p.ungzip, { gzip: true }, {}),
        deflate: fam(p.deflateRaw, p.inflateRaw, { raw: true }, { raw: true }),
    } };
}

async function implBrotliWasm() {
    const b = await require("brotli-wasm");
    const OUT = 64 * 1024;
    const drain = (step, input) => {
        /* One chunk through a brotli-wasm stream: loop while it wants more output. */
        const parts = [];
        let off = 0;
        for (;;) {
            const r = step(input ? input.subarray(off) : undefined);
            parts.push(r.buf);
            off += r.input_offset;
            if (r.code !== b.BrotliStreamResultCode.NeedsMoreOutput) return { parts, code: r.code };
        }
    };
    return { version: libVersion("brotli-wasm"), codecs: { brotli: {
        compress: (d, level) => b.compress(d, { quality: level }),
        decompress: (x) => b.decompress(x),
        cstream: async (d, level) => {
            const s = new b.CompressStream(level);
            const parts = [];
            for (const ch of chunksOf(d)) parts.push(...drain((i) => s.compress(i, OUT), ch).parts);
            for (;;) {
                const r = drain((i) => s.compress(i, OUT), undefined);
                parts.push(...r.parts);
                if (r.code === b.BrotliStreamResultCode.ResultSuccess) break;
            }
            s.free();
            return concat(parts);
        },
        dstream: async (x) => {
            const s = new b.DecompressStream();
            const parts = [];
            for (const ch of chunksOf(x)) parts.push(...drain((i) => s.decompress(i, OUT), ch).parts);
            s.free();
            return concat(parts);
        },
        levels: [0, 11],
    } } };
}

async function implZstdWasm() {
    const z = require("@bokuweb/zstd-wasm");
    await z.init();
    return { version: libVersion("@bokuweb/zstd-wasm"), codecs: { zstd: {
        compress: (d, level) => z.compress(d, level),
        decompress: (x) => z.decompress(x),
        levels: [1, 22],
    } } };
}

async function implFzstd() {
    const f = await import("fzstd");
    const z = require("@bokuweb/zstd-wasm");
    await z.init();
    return { version: libVersion("fzstd"), codecs: { zstd: {
        decodeOnly: { produce: (d, level) => z.compress(d, level) },
        decompress: (x, n) => f.decompress(x, n ? new Uint8Array(n) : undefined),
        dstream: async (x) => {
            const parts = [];
            const d = new f.Decompress((chunk) => parts.push(chunk));
            const cs = chunksOf(x);
            cs.forEach((ch, i) => d.push(ch, i === cs.length - 1));
            return concat(parts);
        },
        levels: [1, 22],
    } } };
}

async function implXzDecompress() {
    const { XzReadableStream } = (await import("xz-decompress")).default; // a CJS bundle
    const xz = await loadModule("xz");
    await xz.preload();
    const decode = async (x) => {
        const src = new ReadableStream({
            start(ctl) {
                for (const ch of chunksOf(x)) ctl.enqueue(ch);
                ctl.close();
            },
        });
        return new Uint8Array(await new Response(new XzReadableStream(src)).arrayBuffer());
    };
    return { version: libVersion("xz-decompress"), codecs: { xz: {
        // compress-utils' own xz makes the input; xz-decompress is async and stream-only.
        decodeOnly: { produce: (d, level) => xz.compressSync(d, { level: Math.min(10, level + 1) }) },
        dstream: decode,
        levels: [0, 9],
    } } };
}

async function implLz4js() {
    const l = require("lz4js");
    return { version: libVersion("lz4js"), codecs: { lz4: {
        levelless: true,
        compress: (d) => l.compress(d),
        decompress: (x, n) => (n ? l.decompress(x, n) : l.decompress(x)),
    } } };
}

async function implSnappyjs() {
    const s = require("snappyjs");
    return { version: libVersion("snappyjs"), codecs: { snappy: {
        levelless: true,
        compress: (d) => s.compress(d),
        decompress: (x, n) => (n ? s.uncompress(x, n) : s.uncompress(x)),
    } } };
}

const IMPLS = {
    "compress-utils": implCompressUtils,
    "node-zlib": implNodeZlib,
    "compression-stream": implCompressionStream,
    fflate: implFflate,
    pako: implPako,
    "brotli-wasm": implBrotliWasm,
    "zstd-wasm": implZstdWasm,
    fzstd: implFzstd,
    "xz-decompress": implXzDecompress,
    lz4js: implLz4js,
    snappyjs: implSnappyjs,
};
const IMPL_NAMES = { "node-zlib": "node:zlib", "compression-stream": "CompressionStream" };

/* ---- timing ---------------------------------------------------------------------- */

async function timedAsync(fn, runs) {
    let out;
    const times = [];
    for (let r = 0; r < WARMUP + runs; r++) {
        const t0 = now();
        out = await fn();
        if (r >= WARMUP) times.push(Number(now() - t0));
    }
    return { out, times };
}

function timedSync(fn, runs) {
    let out;
    const times = [];
    for (let r = 0; r < WARMUP + runs; r++) {
        const t0 = now();
        out = fn();
        if (r >= WARMUP) times.push(Number(now() - t0));
    }
    return { out, times };
}

/* Can compress-utils decode a competitor's output? null for compress-utils itself. */
async function interop(impl, algo, comp, data) {
    if (impl === "compress-utils") return null;
    try {
        const mod = await loadModule(algo);
        return bytesEqual(await mod.decompress(u8(comp)), data);
    } catch {
        return false;
    }
}

function record(impl, algo, level, nativeLevel, mode, path, inBytes, outBytes, cT, dT, verified, extra) {
    const c = stats(cT);
    const d = stats(dT);
    return {
        lang: "wasm",
        runtime: RUNTIME,
        impl: IMPL_NAMES[impl] || impl,
        algo,
        level,
        mode,
        chunk_bytes: mode === "stream" ? CHUNK : 0,
        input: path,
        input_bytes: inBytes,
        output_bytes: outBytes,
        compress_ns_median: c.median,
        compress_ns_mad: c.mad,
        compress_ns_min: c.min,
        decompress_ns_median: d.median,
        decompress_ns_mad: d.mad,
        decompress_ns_min: d.min,
        samples: SAMPLES,
        warmup: WARMUP,
        verified,
        ...extra,
    };
}

async function runJob(impl, algo, codec, level, mode, path) {
    const data = readFileSync(path);
    const decodeOnly = Boolean(codec.decodeOnly);
    const useStream = mode === "stream";
    const zero = Array(SAMPLES).fill(0);

    let comp;
    let cT;
    if (decodeOnly) {
        comp = codec.decodeOnly.produce(data, level);
        cT = zero;
    } else {
        ({ out: comp, times: cT } = await timedAsync(
            () => (useStream ? codec.cstream(data, level) : codec.compress(data, level)), SAMPLES));
    }
    const decode = useStream ? () => codec.dstream(comp) : () => codec.decompress(comp, null);
    const { out: dec, times: dT } = await timedAsync(decode, SAMPLES);

    const extra = {};
    if (decodeOnly) extra.decode_only = true;
    const ok = await interop(impl, algo, comp, data);
    if (ok !== null) extra.interop = ok;
    if (codec.wasmSize) extra.wasm_size_bytes = codec.wasmSize;
    return record(impl, algo, level, null, mode, path, data.length, u8(comp).length, cT, dT,
                  bytesEqual(dec, data), extra);
}

async function runMsgsetJob(impl, algo, codec, level, path) {
    const msgs = readMsgset(path);
    const decodeOnly = Boolean(codec.decodeOnly);

    let comp;
    let cT;
    if (decodeOnly) {
        comp = msgs.map((m) => codec.decodeOnly.produce(m, level));
        cT = Array(SAMPLES).fill(0);
    } else {
        ({ out: comp, times: cT } = timedSync(() => msgs.map((m) => codec.compress(m, level)), SAMPLES));
    }
    const { times: dT } = timedSync(() => {
        for (let i = 0; i < msgs.length; i++) codec.decompress(comp[i], msgs[i].length);
    }, SAMPLES);
    const verified = msgs.every((m, i) => bytesEqual(codec.decompress(comp[i], m.length), m));

    const extra = { messages: msgs.length, decompress_size_known: true };
    if (decodeOnly) extra.decode_only = true;
    const ok = msgs.length ? await interop(impl, algo, comp[0], msgs[0]) : null;
    if (ok !== null) extra.interop = ok;
    if (codec.wasmSize) extra.wasm_size_bytes = codec.wasmSize;
    return record(impl, algo, level, null, "msgset", path, msgs.reduce((a, m) => a + m.length, 0),
                  comp.reduce((a, m) => a + u8(m).length, 0), cT, dT, verified, extra);
}

/* ---- protocol -------------------------------------------------------------------- */

/* Lines from stdin, read synchronously from fd 0. The protocol is strictly request
 * and reply, so blocking between jobs costs nothing, and it avoids stdin streams:
 * under Bun, process.stdin (and node:readline over it) ends after two jobs while the
 * parent still has the pipe open, and the process then exits cleanly. */
function* lines() {
    const buf = Buffer.alloc(64 * 1024);
    let pending = "";
    for (;;) {
        let n;
        try {
            n = readSync(0, buf, 0, buf.length, null);
        } catch (e) {
            if (e.code === "EAGAIN") continue;
            if (e.code === "EOF") break;
            throw e;
        }
        if (n === 0) break;
        pending += buf.toString("utf8", 0, n);
        let i;
        while ((i = pending.indexOf("\n")) >= 0) {
            yield pending.slice(0, i);
            pending = pending.slice(i + 1);
        }
    }
    if (pending) yield pending;
}

function emit(obj) {
    process.stdout.write(JSON.stringify(obj) + "\n");
}

async function main() {
    const argv = process.argv.slice(2);
    const impl = argv.includes("--impl") ? argv[argv.indexOf("--impl") + 1] : "compress-utils";
    if (!IMPLS[impl]) throw new Error(`unknown --impl ${impl}; known: ${Object.keys(IMPLS).join(", ")}`);
    const { version, codecs } = await IMPLS[impl]();

    if (argv.includes("--info")) {
        const levels = {};
        const levelless = [];
        for (const [a, c] of Object.entries(codecs)) {
            if (c.levels) levels[a] = c.levels;
            if (c.levelless) levelless.push(a);
        }
        emit({ lang: "wasm", version, driver: impl === "compress-utils" ? "wasm" : `js-${impl}`,
               impl: IMPL_NAMES[impl] || impl, runtime: RUNTIME, levels, levelless });
        return;
    }

    for (const raw of lines()) {
        const line = raw.trim();
        if (!line) continue;
        const m = line.match(/^(\S+)\s+(\S+)\s+(.*)$/);
        if (!m) { emit({ error: true }); continue; }
        const algo = m[1];
        const level = parseInt(m[2], 10);
        let rest = m[3];
        let mode = "oneshot";
        for (const mo of ["stream", "msgset", "oneshot"]) {
            if (rest.startsWith(`${mo} `)) { mode = mo; rest = rest.slice(mo.length + 1); break; }
        }
        const path = rest.trim();

        const codec = codecs[algo];
        const supported = codec && (mode === "stream"
            ? codec.dstream && (codec.cstream || codec.decodeOnly)
            : codec.decompress && (codec.compress || codec.decodeOnly));
        if (!supported) { emit({ skipped: true }); continue; }
        try {
            emit(mode === "msgset"
                ? await runMsgsetJob(impl, algo, codec, level, path)
                : await runJob(impl, algo, codec, level, mode, path));
        } catch (e) {
            process.stderr.write(`bench-js[${impl}]: ${algo} L${level} ${mode} failed: ${String(e?.message || e).slice(0, 200)}\n`);
            emit({ error: true });
        }
    }
}

// Top-level await, not a detached promise: Bun exits silently (code 0) if the entry
// module has finished evaluating while a WebAssembly compile is still pending.
try {
    await main();
} catch (e) {
    process.stderr.write(`bench-js fatal: ${e?.stack || e}\n`);
    process.exitCode = 1;
}
