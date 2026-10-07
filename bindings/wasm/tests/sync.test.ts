/** Initialization and synchronous API conformance against a real zlib WASM module. */
import { readFile } from "node:fs/promises";
import { describe, expect, it, vi } from "vitest";
import { createBindings } from "../src/core/dispatch.js";
import { Algorithm } from "../src/core/types.js";

const wasmUrl = new URL("../dist/algorithms/zlib/zlib.wasm", import.meta.url);
const input = new TextEncoder().encode("synchronous round trip\n".repeat(100));
const createFreshBindings = () => createBindings(Algorithm.Zlib, "zlib", wasmUrl, readFile);

describe("preload and synchronous operations", () => {
    it("requires completed initialization and supports repeated independent calls", async () => {
        const bindings = createFreshBindings();
        expect(() => bindings.compressSync(input)).toThrow(/preload/);
        expect(() => bindings.decompressSync(input)).toThrow(/preload/);
        expect(() => bindings.createCompressStreamSync()).toThrow(/preload/);
        expect(() => bindings.createDecompressStreamSync()).toThrow(/preload/);
        await bindings.preload();
        for (let iteration = 0; iteration < 3; iteration++) {
            const encoded = bindings.compressSync(input);
            expect(bindings.decompressSync(encoded)).toEqual(input);
        }
        expect(() => bindings.decompressSync(new Uint8Array([255]))).toThrow();
    });

    it("shares initialization and permits retry after a failed load", async () => {
        const resolveWasm = vi
            .fn()
            .mockRejectedValueOnce(new Error("load failed"))
            .mockImplementation(() => readFile(wasmUrl));
        const bindings = createBindings(Algorithm.Zlib, "zlib", wasmUrl, resolveWasm);
        await expect(bindings.preload()).rejects.toThrow("load failed");
        expect(() => bindings.compressSync(input)).toThrow(/preload/);
        await Promise.all([bindings.preload(), bindings.preload(), bindings.preload()]);
        expect(resolveWasm).toHaveBeenCalledTimes(2);
        expect(bindings.decompressSync(bindings.compressSync(input))).toEqual(input);
    });

    it("async initialization enables sync calls and sync stream factories", async () => {
        const bindings = createFreshBindings();
        expect(bindings.decompressSync(await bindings.compress(input))).toEqual(input);
        const encoder = bindings.createCompressStreamSync();
        const decoder = bindings.createDecompressStreamSync();
        try {
            const encodedChunks = [encoder.write(input), encoder.finish()];
            const decodedChunks = encodedChunks.map((chunk) => decoder.write(chunk));
            decodedChunks.push(decoder.finish());
            expect(Buffer.concat(decodedChunks)).toEqual(Buffer.from(input));
        } finally {
            encoder.destroy();
            decoder.destroy();
        }
    });
});

it("keeps independently preloaded encoder and decoder subpaths interoperable", async () => {
    const encoder = await import("compress-utils/zlib/compress");
    const decoder = await import("compress-utils/zlib/decompress");
    await Promise.all([encoder.preload(), decoder.preload()]);
    expect(decoder.decompressSync(encoder.compressSync(input))).toEqual(input);
    expect("decompressSync" in encoder).toBe(false);
    expect("compressSync" in decoder).toBe(false);
});
