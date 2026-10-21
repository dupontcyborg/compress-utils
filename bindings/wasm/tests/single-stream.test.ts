/** Single-stream decoding: stop at the end of the first frame and hand back the bytes after it. */
import { describe, expect, it } from "vitest";
import * as brotli from "compress-utils/brotli";
import * as bz2 from "compress-utils/bz2";
import * as deflate from "compress-utils/deflate";
import * as gzip from "compress-utils/gzip";
import * as lz4 from "compress-utils/lz4";
import * as snappy from "compress-utils/snappy";
import * as xz from "compress-utils/xz";
import * as zlib from "compress-utils/zlib";
import * as zstd from "compress-utils/zstd";
import * as zstdDecoder from "compress-utils/zstd/decompress";
import * as lz4Raw from "compress-utils/lz4_raw";
import { CompressError } from "compress-utils/zstd";

const codecs = { brotli, bz2, deflate, gzip, lz4, snappy, xz, zlib, zstd };

/* What follows a zpickle message: the next message's header, then data. */
const TRAILER = new Uint8Array([0x5a, 0x50, 0x4b, 0x4c, 1, 1, 3, 0, ...new Uint8Array(24)]);
const STREAM_FINISHED = 10;
const UNSUPPORTED_ALGO = 5;

function pseudoRandom(length: number, seed: number): Uint8Array {
    const out = new Uint8Array(length);
    let x = seed;
    for (let i = 0; i < length; i++) {
        x = (x * 1103515245 + 12345) >>> 0;
        out[i] = x >>> 16;
    }
    return out;
}

function concat(parts: Uint8Array[]): Uint8Array {
    const out = new Uint8Array(parts.reduce((n, p) => n + p.byteLength, 0));
    let offset = 0;
    for (const p of parts) {
        out.set(p, offset);
        offset += p.byteLength;
    }
    return out;
}

const payload = concat([
    pseudoRandom(3000, 1),
    new TextEncoder().encode("single stream ".repeat(500)),
]);

describe("single-stream decoding", () => {
    for (const [name, codec] of Object.entries(codecs)) {
        it(`${name}: frame + trailing bytes in one write`, async () => {
            const frame = await codec.compress(payload);
            using decoder = await codec.createDecompressStream({ singleStream: true });
            expect(decoder.eof).toBe(false);
            expect(decoder.unusedData.byteLength).toBe(0);
            const out = decoder.write(concat([frame, TRAILER]));
            expect(out).toEqual(payload);
            expect(decoder.eof).toBe(true);
            expect(decoder.unusedData).toEqual(TRAILER);
            // unusedData is a copy: changing it doesn't change later reads.
            decoder.unusedData.fill(0xff);
            expect(decoder.unusedData).toEqual(TRAILER);
            expect(decoder.finish().byteLength).toBe(0);
        });

        it(`${name}: an error is final and never reaches eof`, async () => {
            const frame = (await codec.compress(payload)).slice();
            for (let i = 0; i < 16 && i < frame.byteLength; i++) frame[i] ^= 0xa5;
            using decoder = await codec.createDecompressStream({ singleStream: true });
            let first: unknown;
            try {
                decoder.write(frame);
                decoder.finish();
            } catch (e) {
                first = e;
            }
            expect(first).toBeInstanceOf(CompressError);
            expect(decoder.eof).toBe(false);
            expect(decoder.unusedData.byteLength).toBe(0);
        });

        it(`${name}: frame split at the boundary, then trailing bytes`, async () => {
            const frame = await codec.compress(payload);
            using decoder = await codec.createDecompressStream({ singleStream: true });
            const head = frame.subarray(0, frame.byteLength - 1);
            const parts = [decoder.write(head)];
            expect(decoder.eof).toBe(false);
            parts.push(decoder.write(concat([frame.subarray(frame.byteLength - 1), TRAILER])));
            expect(concat(parts)).toEqual(payload);
            expect(decoder.eof).toBe(true);
            expect(decoder.unusedData).toEqual(TRAILER);
        });

        it(`${name}: exactly one frame leaves nothing unused`, async () => {
            const frame = await codec.compress(payload);
            using decoder = await codec.createDecompressStream({ singleStream: true });
            expect(decoder.write(frame)).toEqual(payload);
            expect(decoder.eof).toBe(true);
            expect(decoder.unusedData.byteLength).toBe(0);
        });

        it(`${name}: two frames back to back`, async () => {
            const second = pseudoRandom(2000, 9);
            const [f1, f2] = [await codec.compress(payload), await codec.compress(second)];
            using first = await codec.createDecompressStream({ singleStream: true });
            expect(first.write(concat([f1, f2]))).toEqual(payload);
            expect(first.eof).toBe(true);
            expect(first.unusedData).toEqual(f2);
            using next = await codec.createDecompressStream({ singleStream: true });
            expect(next.write(first.unusedData)).toEqual(second);
            expect(next.eof).toBe(true);
            expect(next.unusedData.byteLength).toBe(0);
        });

        it(`${name}: writes after the end throw StreamFinished`, async () => {
            const frame = await codec.compress(payload);
            using decoder = await codec.createDecompressStream({ singleStream: true });
            decoder.write(concat([frame, TRAILER]));
            let error: unknown;
            try {
                decoder.write(new Uint8Array([1, 2, 3]));
            } catch (e) {
                error = e;
            }
            expect(error).toBeInstanceOf(CompressError);
            expect((error as CompressError).code).toBe(STREAM_FINISHED);
            expect(decoder.unusedData).toEqual(TRAILER);
            expect(decoder.write(new Uint8Array(0)).byteLength).toBe(0);
            expect(decoder.finish().byteLength).toBe(0);
            expect(decoder.eof).toBe(true);
        });

        it(`${name}: truncated input never reaches eof`, async () => {
            const frame = await codec.compress(payload);
            using decoder = await codec.createDecompressStream({ singleStream: true });
            try {
                decoder.write(frame.subarray(0, frame.byteLength - 1));
            } catch {
                // Some decoders reject a truncated frame as soon as they see it.
            }
            expect(() => decoder.finish()).toThrow(CompressError);
            expect(decoder.eof).toBe(false);
            expect(decoder.unusedData.byteLength).toBe(0);
        });
    }

    it("default mode still decodes concatenated zstd frames as one stream", async () => {
        const [f1, f2] = [await zstd.compress(payload), await zstd.compress(TRAILER)];
        using decoder = await zstd.createDecompressStream();
        expect(concat([decoder.write(concat([f1, f2])), decoder.finish()])).toEqual(
            concat([payload, TRAILER]),
        );
        expect(decoder.eof).toBe(false);
        expect(decoder.unusedData.byteLength).toBe(0);
    });

    it("works from a decompress-only subpath, synchronously after preload", async () => {
        const frame = await zstd.compress(payload);
        await zstdDecoder.preload();
        using decoder = zstdDecoder.createDecompressStreamSync({ singleStream: true });
        expect(decoder.write(concat([frame, TRAILER]))).toEqual(payload);
        expect(decoder.unusedData).toEqual(TRAILER);
    });

    // LZO is rejected the same way (tests/test_single_stream.c covers it), but
    // isn't loaded here: Bun runs every test file in one module registry, and
    // tests/lzo.test.ts needs LZO not yet initialized to check that sync
    // decoding requires preload().
    it("raw LZ4 doesn't support it", async () => {
        let error: unknown;
        try {
            await lz4Raw.createDecompressStream({ singleStream: true });
        } catch (e) {
            error = e;
        }
        expect(error).toBeInstanceOf(Error);
        expect((error as CompressError).code).toBe(UNSUPPORTED_ALGO);
    });
});
