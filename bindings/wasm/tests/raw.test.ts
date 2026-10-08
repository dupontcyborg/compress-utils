/** Raw formats are interoperable with independent Node zlib and literal LZ4 blocks. */
import { deflateRawSync, inflateRawSync } from "node:zlib";
import { expect, it, vi } from "vitest";
import { readFile } from "node:fs/promises";
import { createBindings } from "../src/core/dispatch.js";
import { Algorithm } from "../src/core/types.js";
import * as deflate from "compress-utils/deflate";
import * as lz4Raw from "compress-utils/lz4_raw";

it("cross-decodes raw DEFLATE, including empty input", async () => {
    for (const input of [new Uint8Array(), new TextEncoder().encode("raw payload".repeat(100))]) {
        expect(await deflate.decompress(deflateRawSync(input))).toEqual(input);
        expect(new Uint8Array(inflateRawSync(await deflate.compress(input)))).toEqual(input);
    }
    await expect(deflate.decompress(new Uint8Array([255]))).rejects.toThrow();
});
it("streams raw DEFLATE across arbitrary chunk boundaries", async () => {
    const input = new TextEncoder().encode("stream".repeat(1000));
    const encoder = await deflate.createCompressStream();
    const decoder = await deflate.createDecompressStream();
    try {
        const encoded = Buffer.concat([
            encoder.write(input.subarray(0, 13)),
            encoder.write(input.subarray(13)),
            encoder.finish(),
        ]);
        expect(inflateRawSync(encoded)).toEqual(Buffer.from(input));
        const decoded = Buffer.concat([
            decoder.write(encoded.subarray(0, 1)),
            decoder.write(encoded.subarray(1)),
            decoder.finish(),
        ]);
        expect(decoded).toEqual(Buffer.from(input));
    } finally {
        encoder.destroy();
        decoder.destroy();
    }
});
it("decodes an independent raw LZ4 block and returns actual output length", async () => {
    const input = new TextEncoder().encode("hello");
    expect(await lz4Raw.decompress(new Uint8Array([0x50, ...input]), { expectedSize: 32 })).toEqual(
        input,
    );
    expect(await lz4Raw.decompress(await lz4Raw.compress(input), { expectedSize: 5 })).toEqual(
        input,
    );
    await expect(lz4Raw.decompress(new Uint8Array([0x50, ...input]))).rejects.toThrow(
        /expectedSize/,
    );
    await expect(
        lz4Raw.decompress(new Uint8Array([0x50, ...input]), { expectedSize: 1 }),
    ).rejects.toThrow();
    await expect(lz4Raw.decompress(new Uint8Array([0xff]), { expectedSize: 5 })).rejects.toThrow();
    expect("createDecompressStream" in lz4Raw).toBe(false);
});

it("round-trips empty and match-heavy raw LZ4 blocks through directional subpaths", async () => {
    const encoder = await import("compress-utils/lz4_raw/compress");
    const decoder = await import("compress-utils/lz4_raw/decompress");
    for (const input of [new Uint8Array(), new Uint8Array(65536).fill(42)]) {
        expect(
            await decoder.decompress(await encoder.compress(input), { expectedSize: input.length }),
        ).toEqual(input);
    }
});

it("rejects invalid raw LZ4 capacities before allocating WASM memory", async () => {
    for (const expectedSize of [-1, 0.5, Number.NaN, 0x80000000]) {
        await expect(lz4Raw.decompress(new Uint8Array([0]), { expectedSize })).rejects.toThrow(
            /expectedSize/,
        );
    }
});

it("shares WASM initialization across framed and raw bindings", async () => {
    const wasmUrl = new URL("../dist/algorithms/zlib/zlib.wasm", import.meta.url);
    const resolver = vi.fn(() => readFile(wasmUrl));
    const framed = createBindings(Algorithm.Zlib, "zlib", wasmUrl, resolver);
    const raw = createBindings(Algorithm.Deflate, "deflate", wasmUrl, resolver);
    const input = new TextEncoder().encode("shared module");
    const [framedBytes, rawBytes] = await Promise.all([
        framed.compress(input),
        raw.compress(input),
    ]);
    expect(resolver).toHaveBeenCalledTimes(1);
    expect(await framed.decompress(framedBytes)).toEqual(input);
    expect(await raw.decompress(rawBytes)).toEqual(input);
});

it("validates raw LZ4 capacities in both async and sync directional bindings", async () => {
    const decoder = await import("compress-utils/lz4_raw/decompress");
    await Promise.all([lz4Raw.preload(), decoder.preload()]);
    const encoded = new Uint8Array([0x50, 104, 101, 108, 108, 111]);
    for (const binding of [lz4Raw, decoder]) {
        expect(binding.decompressSync(encoded, { expectedSize: 32 })).toEqual(
            new TextEncoder().encode("hello"),
        );
        for (const expectedSize of [undefined, -1, 0.5, Number.NaN, 0x80000000]) {
            expect(() => binding.decompressSync(encoded, { expectedSize })).toThrow(/expectedSize/);
            await expect(binding.decompress(encoded, { expectedSize })).rejects.toThrow(
                /expectedSize/,
            );
        }
    }
});
