/**
 * Round-trip coverage for every algorithm subpath. Each .wasm is its own
 * module instance — there's no shared state across algorithms, so we can
 * import them all in one test file without polluting global state.
 *
 * The matrix proves: zig→wasm build works for every algo, the dispatcher
 * works against every algo's vtable, the WASI reactor shim is sufficient
 * for the codec call paths each algo actually exercises.
 */

import { describe, it, expect } from "vitest";

import * as zstd from "compress-utils/zstd";
import * as brotli from "compress-utils/brotli";
import * as zlib from "compress-utils/zlib";
import * as bz2 from "compress-utils/bz2";
import * as lz4 from "compress-utils/lz4";
import * as xz from "compress-utils/xz";
import * as snappy from "compress-utils/snappy";
import * as gzip from "compress-utils/gzip";

const enc = new TextEncoder();
const dec = new TextDecoder();

interface AlgoModule {
    compress: (input: Uint8Array, opts?: { level?: number }) => Promise<Uint8Array>;
    decompress: (input: Uint8Array, expectedSize?: number) => Promise<Uint8Array>;
    createCompressStream: (opts?: { level?: number }) => Promise<{
        write: (chunk: Uint8Array) => Uint8Array;
        finish: () => Uint8Array;
        destroy: () => void;
    }>;
    createDecompressStream: () => Promise<{
        write: (chunk: Uint8Array) => Uint8Array;
        finish: () => Uint8Array;
        destroy: () => void;
    }>;
}

const algos: [string, AlgoModule][] = [
    ["zstd", zstd],
    ["brotli", brotli],
    ["zlib", zlib],
    ["bz2", bz2],
    ["lz4", lz4],
    ["xz", xz],
    ["snappy", snappy],
    ["gzip", gzip],
];

for (const [name, m] of algos) {
    describe(name, () => {
        it("round-trips a small ASCII payload (one-shot)", async () => {
            const input = enc.encode("the quick brown fox jumps over the lazy dog\n".repeat(50));
            const compressed = await m.compress(input);
            expect(compressed.byteLength).toBeGreaterThan(0);

            const decompressed = await m.decompress(compressed);
            expect(dec.decode(decompressed)).toBe(dec.decode(input));
        });

        it("round-trips a multi-chunk payload (streaming)", async () => {
            const input = enc.encode("streaming payload ".repeat(10_000));
            const cs = await m.createCompressStream({ level: 3 });
            const out: Uint8Array[] = [];
            for (let i = 0; i < input.byteLength; i += 4096) {
                out.push(cs.write(input.subarray(i, i + 4096)));
            }
            out.push(cs.finish());
            cs.destroy();

            const ds = await m.createDecompressStream();
            const decoded = ds.write(concat(out));
            const tail = ds.finish();
            ds.destroy();

            expect(dec.decode(concat([decoded, tail]))).toBe(dec.decode(input));
        });
    });
}

function concat(chunks: Uint8Array[]): Uint8Array {
    const total = chunks.reduce((n, c) => n + c.byteLength, 0);
    const out = new Uint8Array(total);
    let off = 0;
    for (const c of chunks) {
        out.set(c, off);
        off += c.byteLength;
    }
    return out;
}
