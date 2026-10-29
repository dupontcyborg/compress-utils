/** Formats that don't record their size decode through the C core's growing buffer: a ratio high enough that it doubles many times, input of many 256 KiB feed chunks, an empty result, an xz input that isn't exactly one Stream (its size hint says unknown), and corrupt or truncated input. */

import { describe, it, expect } from "vitest";

import * as zlib from "compress-utils/zlib";
import * as gzip from "compress-utils/gzip";
import * as deflate from "compress-utils/deflate";
import * as brotli from "compress-utils/brotli";
import * as bz2 from "compress-utils/bz2";
import * as xz from "compress-utils/xz";
import { CompressError } from "compress-utils/zlib";

const DECOMPRESSION = 7;
const TRUNCATED = 8;

const algos = { zlib, gzip, deflate, brotli, bz2, xz };

function pseudoRandom(length: number, seed: number): Uint8Array {
    const out = new Uint8Array(length);
    let x = seed;
    for (let i = 0; i < length; i++) {
        x = (x * 1103515245 + 12345) >>> 0;
        out[i] = x >>> 16;
    }
    return out;
}

/* Byte-for-byte equality that reports the first difference (toEqual walks typed arrays slowly). */
function expectSameBytes(actual: Uint8Array, expected: Uint8Array): void {
    expect(actual.byteLength).toBe(expected.byteLength);
    const at = actual.findIndex((byte, i) => byte !== expected[i]);
    expect(at, `first differing byte at offset ${at}`).toBe(-1);
}

const zeros = new Uint8Array(8 << 20);
const noise = pseudoRandom(700_000, 5);

describe.each(Object.entries(algos))("%s", (_name, m) => {
    it("round-trips a high ratio, a multi-chunk input and an empty one", async () => {
        for (const data of [zeros, noise, new Uint8Array(0)]) {
            expectSameBytes(await m.decompress(await m.compress(data)), data);
        }
    });
});

describe("xz without a recorded size", () => {
    const text = new TextEncoder().encode("first part ".repeat(5000));

    it("decodes Stream Padding and a second Stream", async () => {
        const packed = await xz.compress(text);
        const padded = new Uint8Array(packed.byteLength + 4);
        padded.set(packed);
        expectSameBytes(await xz.decompress(padded), text);

        const twice = new Uint8Array(padded.byteLength + packed.byteLength);
        twice.set(padded);
        twice.set(packed, padded.byteLength);
        const both = new Uint8Array(text.byteLength * 2);
        both.set(text);
        both.set(text, text.byteLength);
        expectSameBytes(await xz.decompress(twice), both);
    });
});

describe("errors from the growing decode", () => {
    it("reports truncated and corrupt zlib input", async () => {
        const packed = await zlib.compress(noise);
        const code = (e: unknown) => {
            expect(e).toBeInstanceOf(CompressError);
            return (e as CompressError).code;
        };
        const truncated = await zlib
            .decompress(packed.subarray(0, packed.byteLength >> 1))
            .then(() => expect.unreachable("truncated input must throw"), code);
        expect(truncated).toBe(TRUNCATED);
        const corrupt = await zlib
            .decompress(new Uint8Array(32).fill(0xff))
            .then(() => expect.unreachable("corrupt input must throw"), code);
        expect(corrupt).toBe(DECOMPRESSION);
    });
});
