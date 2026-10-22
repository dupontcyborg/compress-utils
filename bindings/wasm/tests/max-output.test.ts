/**
 * The decompressed-size cap holds for every algorithm, including those that
 * don't record their size and so decode through a stream, and a stream's own
 * `maxOutput` stops it. Each subpath is its own module instance, so the cap
 * set on one doesn't affect the others.
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
import * as deflate from "compress-utils/deflate";
import { CompressError } from "compress-utils/zstd";

const SIZE_LIMIT = 9;
const DEFAULT_CAP = 1024 * 1024 * 1024;

const algos = { zstd, brotli, zlib, bz2, lz4, xz, snappy, gzip, deflate };
const input = new TextEncoder().encode(
    "The quick brown fox jumps over the lazy dog. ".repeat(1500),
);

function expectSizeLimit(e: unknown): void {
    expect(e).toBeInstanceOf(CompressError);
    expect((e as CompressError).code).toBe(SIZE_LIMIT);
}

describe.each(Object.entries(algos))("%s", (_name, m) => {
    it("decompress respects the cap", async () => {
        const compressed = await m.compress(input);
        try {
            await m.setMaxDecompressedSize(input.byteLength - 1);
            await m
                .decompress(compressed)
                .then(
                    () => expect.unreachable("decompress over the cap must throw"),
                    expectSizeLimit,
                );
            await m.setMaxDecompressedSize(input.byteLength);
            expect(await m.decompress(compressed)).toEqual(input);
            await m.setMaxDecompressedSize(0);
            expect(await m.decompress(compressed)).toEqual(input);
        } finally {
            await m.setMaxDecompressedSize(DEFAULT_CAP);
        }
    });

    it("a stream stops at its maxOutput", async () => {
        const compressed = await m.compress(input);
        const over = await m.createDecompressStream({ maxOutput: input.byteLength - 1 });
        try {
            over.write(compressed);
            over.finish();
            expect.unreachable("stream over its limit must throw");
        } catch (e) {
            expectSizeLimit(e);
        } finally {
            over.destroy();
        }

        const at = await m.createDecompressStream({ maxOutput: input.byteLength });
        const body = at.write(compressed);
        const tail = at.finish();
        at.destroy();
        const out = new Uint8Array(body.byteLength + tail.byteLength);
        out.set(body);
        out.set(tail, body.byteLength);
        expect(out).toEqual(input);
    });
});

describe("decompressionStream maxOutput", () => {
    it("errors the TransformStream past the limit", async () => {
        const compressed = await gzip.compress(input);
        const piped = new Blob([compressed])
            .stream()
            .pipeThrough(gzip.decompressionStream({ maxOutput: input.byteLength - 1 }));
        await new Response(piped)
            .arrayBuffer()
            .then(
                () => expect.unreachable("decompressionStream over its limit must error"),
                expectSizeLimit,
            );
    });
});
