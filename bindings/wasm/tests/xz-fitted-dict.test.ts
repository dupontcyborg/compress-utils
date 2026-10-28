/** One-shot xz caps the dictionary at the input size (4 KiB floor, never above the preset's):
 *  round trips at, one byte under and one byte over each cap, with text and incompressible data. */
import { describe, expect, it } from "vitest";
import * as xz from "compress-utils/xz";

function pseudoRandom(length: number, seed: number): Uint8Array {
    const out = new Uint8Array(length);
    let x = seed;
    for (let i = 0; i < length; i++) {
        x = (x * 1103515245 + 12345) >>> 0;
        out[i] = x >>> 16;
    }
    return out;
}

function text(length: number): Uint8Array {
    const unit = new TextEncoder().encode("The quick brown fox jumps over the lazy dog. ");
    return Uint8Array.from({ length }, (_, i) => unit[i % unit.length]);
}

/* Byte-for-byte equality that reports the first difference (toEqual walks typed arrays slowly). */
function expectSameBytes(actual: Uint8Array, expected: Uint8Array): void {
    expect(actual.byteLength).toBe(expected.byteLength);
    const at = actual.findIndex((byte, i) => byte !== expected[i]);
    expect(at, `first differing byte at offset ${at}`).toBe(-1);
}

describe("xz fitted dictionary", () => {
    const sizes = [0, 1, 4095, 4096, 4097, 8191, 8192, 8193, 65535, 65536, 65537];
    for (const level of [1, 6, 10]) {
        it(`round-trips around the cap at level ${level}`, async () => {
            for (const n of sizes) {
                for (const data of [text(n), pseudoRandom(n, n)]) {
                    const packed = await xz.compress(data, { level });
                    expectSameBytes(await xz.decompress(packed), data);
                }
            }
        });
    }

    it("does not cap above the preset's dictionary (level 1 is 256 KiB)", async () => {
        const data = pseudoRandom(300_000, 7);
        const packed = await xz.compress(data, { level: 1 });
        expectSameBytes(await xz.decompress(packed), data);
    });
});
