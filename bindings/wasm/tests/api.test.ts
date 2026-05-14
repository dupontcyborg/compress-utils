/**
 * Coverage for the polish layer added in 0.1: web-streams wrappers,
 * `using` (Symbol.dispose), the DecompressOptions object, and the
 * version/setMaxDecompressedSize helpers. Runs against zstd because
 * that's the most-exercised codec; other algos are covered by all.test.ts.
 */

import { describe, it, expect } from "vitest";
import {
    compress,
    decompress,
    createCompressStream,
    createDecompressStream,
    compressionStream,
    decompressionStream,
    version,
    setMaxDecompressedSize,
    CompressError,
} from "compress-utils/zstd";

const enc = new TextEncoder();
const dec = new TextDecoder();

describe("version + introspection", () => {
    it("returns a semver-shaped string", async () => {
        const v = await version();
        expect(v).toMatch(/^\d+\.\d+\.\d+$/);
    });
});

describe("decompress options", () => {
    it("accepts expectedSize and skips the size-hint probe", async () => {
        const input = enc.encode("hello".repeat(200));
        const compressed = await compress(input);
        const out = await decompress(compressed, { expectedSize: input.byteLength });
        expect(dec.decode(out)).toBe(dec.decode(input));
    });

    it("CompressError exposes the algorithm name as a string", async () => {
        const garbage = new Uint8Array([0xde, 0xad, 0xbe, 0xef]);
        try {
            await decompress(garbage);
            throw new Error("expected throw");
        } catch (e) {
            expect(e).toBeInstanceOf(CompressError);
            expect((e as CompressError).algorithm).toBe("zstd");
            expect(typeof (e as CompressError).code).toBe("number");
        }
    });
});

describe("setMaxDecompressedSize", () => {
    it("triggers SizeLimit when the cap is exceeded", async () => {
        const input = enc.encode("x".repeat(1024));
        const compressed = await compress(input);

        // Restore default after, regardless of outcome.
        try {
            await setMaxDecompressedSize(512);
            await expect(decompress(compressed)).rejects.toThrow(CompressError);
        } finally {
            await setMaxDecompressedSize(1024 * 1024 * 1024);
        }
    });
});

describe("Symbol.dispose / using", () => {
    it("destroys a CompressStream on scope exit", async () => {
        const input = enc.encode("dispose-me\n".repeat(100));
        let compressed: Uint8Array;
        {
            using cs = await createCompressStream({ level: 3 });
            compressed = cs.write(input);
            compressed = concat([compressed, cs.finish()]);
            // No explicit destroy — `using` handles it on block exit.
        }
        const out = await decompress(compressed);
        expect(dec.decode(out)).toBe(dec.decode(input));
    });

    it("destroys a DecompressStream on scope exit", async () => {
        const input = enc.encode("dispose-me\n".repeat(100));
        const compressed = await compress(input);
        let out: Uint8Array;
        {
            using ds = await createDecompressStream();
            out = concat([ds.write(compressed), ds.finish()]);
        }
        expect(dec.decode(out)).toBe(dec.decode(input));
    });

    it("double-destroy is a no-op", async () => {
        const cs = await createCompressStream();
        cs.destroy();
        expect(() => cs.destroy()).not.toThrow();
    });
});

describe("Web Streams API", () => {
    it("compressionStream + decompressionStream round-trip", async () => {
        const input = enc.encode("web-streams payload ".repeat(2000));
        const source = readable(input, 4096);

        const compressed = await collect(
            source.pipeThrough(compressionStream({ level: 3 })),
        );
        expect(compressed.byteLength).toBeGreaterThan(0);

        const out = await collect(
            readable(compressed, 8192).pipeThrough(decompressionStream()),
        );
        expect(dec.decode(out)).toBe(dec.decode(input));
    });

    it("pipes cleanly through TextEncoderStream-style chains", async () => {
        const text = "chained-stream ".repeat(500);
        const compressed = await collect(
            new ReadableStream<string>({
                start(c) { c.enqueue(text); c.close(); },
            })
                .pipeThrough(new TextEncoderStream())
                .pipeThrough(compressionStream()),
        );
        const out = await collect(
            readable(compressed).pipeThrough(decompressionStream()),
        );
        expect(dec.decode(out)).toBe(text);
    });
});

function readable(data: Uint8Array, chunkSize = 8192): ReadableStream<Uint8Array> {
    let off = 0;
    return new ReadableStream({
        pull(c) {
            if (off >= data.byteLength) { c.close(); return; }
            const end = Math.min(off + chunkSize, data.byteLength);
            c.enqueue(data.subarray(off, end));
            off = end;
        },
    });
}

async function collect(stream: ReadableStream<Uint8Array>): Promise<Uint8Array> {
    const chunks: Uint8Array[] = [];
    const reader = stream.getReader();
    while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        chunks.push(value);
    }
    return concat(chunks);
}

function concat(chunks: Uint8Array[]): Uint8Array {
    const total = chunks.reduce((n, c) => n + c.byteLength, 0);
    const out = new Uint8Array(total);
    let off = 0;
    for (const c of chunks) { out.set(c, off); off += c.byteLength; }
    return out;
}
