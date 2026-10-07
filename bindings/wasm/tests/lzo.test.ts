import { readFileSync } from "node:fs";
import { describe, expect, test } from "vitest";
import * as lzo from "../dist/algorithms/lzo/decompress/index.js";
const fixture = (name: string) =>
    new Uint8Array(
        readFileSync(new URL(`../../../tests/fixtures/lzo/${name}.bin`, import.meta.url)),
    );
describe("raw LZO1X decoder", () => {
    test("requires preload before synchronous decoding and shares concurrent preload", async () => {
        const input = new Uint8Array([22, 104, 101, 108, 108, 111, 17, 0, 0]);
        expect(() => lzo.decompressSync(input, { expectedSize: 10 })).toThrow(/preload/);
        await Promise.all([lzo.preload(), lzo.preload()]);
        expect(lzo.decompressSync(input, { expectedSize: 10 })).toEqual(
            new TextEncoder().encode("hello"),
        );
        for (const expectedSize of [-1, 0.5, Number.NaN, 0x80000000]) {
            expect(() => lzo.decompressSync(input, { expectedSize })).toThrow(RangeError);
            expect(() => lzo.decompress(input, { expectedSize })).toThrow(RangeError);
        }
        expect(() => lzo.decompressSync(input, { expectedSize: 1 })).toThrow();
    });
    test.each(["1", "999"])("decodes independently generated LZO1X-%s", async (variant) => {
        const expected = fixture("input");
        expect(
            await lzo.decompress(fixture(variant), { expectedSize: expected.length + 100 }),
        ).toEqual(expected);
        await expect(
            lzo.decompress(fixture(variant), { expectedSize: expected.length - 1 }),
        ).rejects.toMatchObject({ code: 3 });
    });
    test("empty output, malformed input and trailing bytes", async () => {
        expect(await lzo.decompress(new Uint8Array([17, 0, 0]), { expectedSize: 0 })).toEqual(
            new Uint8Array(),
        );
        for (const input of [[], [17], [64, 0, 17, 0, 0], [17, 0, 0, 0], [0, 0, 0]]) {
            await expect(
                lzo.decompress(new Uint8Array(input), { expectedSize: 100 }),
            ).rejects.toThrow();
        }
    });
    test("validates capacity and enforces configured output limit", async () => {
        expect(() => lzo.decompress(new Uint8Array(), { expectedSize: -1 })).toThrow(RangeError);
        await lzo.setMaxDecompressedSize(100);
        try {
            await expect(
                lzo.decompress(fixture("1"), { expectedSize: 32768 }),
            ).rejects.toMatchObject({ code: 9 });
        } finally {
            await lzo.setMaxDecompressedSize(1024 * 1024 * 1024);
        }
    });
    test("ships one decoder asset and no compression exports", () => {
        expect("compress" in lzo).toBe(false);
        expect("createDecompressStream" in lzo).toBe(false);
        const module = new WebAssembly.Module(
            readFileSync(new URL("../dist/algorithms/lzo/decompress/lzo.wasm", import.meta.url)),
        );
        expect(
            WebAssembly.Module.exports(module).some(({ name }) => name.startsWith("cu_compress")),
        ).toBe(false);
    });
});
