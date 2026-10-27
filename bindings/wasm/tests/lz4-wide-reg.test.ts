/**
 * lz4 on wasm32 compiles with a 64-bit `reg_t` (third_party/patches/lz4), which switches the
 * fast compressor (levels 1 and 2) to the 5-byte hash that native 64-bit builds use. These
 * tests pin that behaviour, so a re-vendor that silently drops the patch (or an lz4 bump that
 * changes the fast compressor) is noticed, and check interop with the lz4 CLI when one is
 * installed (skipped otherwise).
 *
 * The pinned sizes come from this generator. After an intended change, recompute them by
 * compressing `textInput(...)` through the frame API at the stated levels.
 */

import { spawnSync } from "node:child_process";
import { describe, expect, it } from "vitest";
import { compress, decompress } from "compress-utils/lz4";

const prng = (seed: number) => {
    let a = seed >>> 0;
    return () => {
        a = (a + 0x6d2b79f5) >>> 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
};

// Word-like text from a skewed vocabulary: compressible, with enough hash collisions that the
// 4-byte and 5-byte hashes give different output.
const textInput = (n: number): Uint8Array => {
    const rnd = prng(0x4c5a3401);
    const words = Array.from({ length: 700 }, () => {
        let w = "";
        for (let i = 3 + Math.floor(rnd() * 8); i > 0; i--)
            w += String.fromCharCode(97 + Math.floor(rnd() * 26));
        return w;
    });
    const out = new Uint8Array(n);
    let pos = 0;
    while (pos < n) {
        const sep = rnd() < 0.1 ? "\n" : rnd() < 0.05 ? `${Math.floor(rnd() * 1e6)} ` : " ";
        const w = words[Math.floor(rnd() ** 2 * words.length)] + sep;
        for (let i = 0; i < w.length && pos < n; i++) out[pos++] = w.charCodeAt(i);
    }
    return out;
};

const same = (a: Uint8Array, b: Uint8Array) =>
    a.byteLength === b.byteLength && Buffer.compare(a, b) === 0;

const cli = (args: string[], input: Uint8Array): Uint8Array | null => {
    const r = spawnSync("lz4", args, { input, maxBuffer: 1 << 30 });
    return r.status === 0 ? new Uint8Array(r.stdout) : null;
};
const hasCli = spawnSync("lz4", ["--version"]).status === 0;

describe("fast compressor uses the 64-bit register path", () => {
    const big = textInput(512 * 1024);
    const small = textInput(40_000);

    // 261,961 bytes with the 5-byte hash (patched); 252,175 with the 4-byte hash (unpatched wasm32).
    it("level 1 on 512 KiB gives the 5-byte-hash output", async () => {
        const packed = await compress(big, { level: 1 });
        expect(packed.byteLength).toBe(261_961);
        expect(same(await decompress(packed), big)).toBe(true);
    });

    // HC levels and inputs under 64 KB (16-bit table) do not use the register-width hash: unchanged by the patch.
    it("level 5 on 512 KiB is unchanged by the patch", async () => {
        const packed = await compress(big, { level: 5 });
        expect(packed.byteLength).toBe(210_454);
        expect(same(await decompress(packed), big)).toBe(true);
    });

    it("level 1 on 40 KB (16-bit table) is unchanged by the patch", async () => {
        const packed = await compress(small, { level: 1 });
        expect(packed.byteLength).toBe(20_377);
        expect(same(await decompress(packed), small)).toBe(true);
    });
});

describe.skipIf(!hasCli)("interop with the lz4 CLI", () => {
    const inputs = [
        { name: "512 KiB text", data: textInput(512 * 1024) },
        { name: "40 KB text", data: textInput(40_000) },
        { name: "empty", data: new Uint8Array(0) },
    ];
    for (const { name, data } of inputs) {
        it(`lz4 decodes our frames of ${name}`, async () => {
            for (const level of [1, 5, 10]) {
                const unpacked = cli(["-d", "-q", "-c"], await compress(data, { level }));
                expect(unpacked).not.toBeNull();
                expect(same(unpacked!, data)).toBe(true);
            }
        });

        it(`we decode lz4 frames of ${name}`, async () => {
            for (const level of ["-1", "-9"]) {
                const packed = cli([level, "-q", "-c"], data);
                expect(packed).not.toBeNull();
                expect(same(await decompress(packed!), data)).toBe(true);
            }
        });
    }
});
