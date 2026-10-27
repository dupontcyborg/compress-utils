/**
 * zstd on wasm32 compiles its bit readers, Huffman/FSE reload decisions, encoder
 * flush decisions and row-hash mask with 64-bit registers (third_party/patches/zstd,
 * enabled by ZSTD_WASM_WIDE_REGS in bindings/wasm/CMakeLists.txt). These tests cover
 * what the generic round-trip suites would miss: far match offsets read from a stream
 * written by native zstd, each Huffman decoder shape, and interop with the zstd CLI
 * when one is installed (skipped otherwise).
 */

import { spawnSync } from "node:child_process";
import { readFileSync } from "node:fs";
import path from "node:path";
import { describe, expect, it } from "vitest";
import { compress, createDecompressStream, decompress } from "compress-utils/zstd";
import { farOffsetInput, hufCases, randomBytes } from "./helpers/zstd-inputs";

const FAR_FIXTURE = path.join(__dirname, "fixtures", "zstd-far-offset.zst");

const cli = (args: string[], input: Uint8Array): Uint8Array | null => {
    const r = spawnSync("zstd", args, { input, maxBuffer: 1 << 30 });
    return r.status === 0 ? new Uint8Array(r.stdout) : null;
};
const hasCli = spawnSync("zstd", ["--version"]).status === 0;

const same = (a: Uint8Array, b: Uint8Array) =>
    a.byteLength === b.byteLength && Buffer.compare(a, b) === 0;

describe("far match offsets (stream written by native zstd --long=27 -19)", () => {
    // head | 40 MiB of zeros | head: the second head is a match 40 MiB back, a 26-bit
    // offset. Regenerate with the CLI as described in tests/helpers/zstd-inputs.ts.
    const fixture = new Uint8Array(readFileSync(FAR_FIXTURE));
    const expected = farOffsetInput();

    it("decodes in one shot", async () => {
        const out = await decompress(fixture);
        expect(same(out, expected)).toBe(true);
    });

    it("decodes through the streaming decoder in small chunks", async () => {
        const parts: Uint8Array[] = [];
        {
            using ds = await createDecompressStream();
            for (let i = 0; i < fixture.byteLength; i += 997) {
                parts.push(ds.write(fixture.subarray(i, i + 997)));
            }
            parts.push(ds.finish());
        }
        const out = new Uint8Array(parts.reduce((n, p) => n + p.byteLength, 0));
        let off = 0;
        for (const p of parts) {
            out.set(p, off);
            off += p.byteLength;
        }
        expect(same(out, expected)).toBe(true);
    });
});

describe("Huffman decoder shapes", () => {
    // Inputs chosen so the literal section takes each decoder of huf_decompress.c
    // (checked with an instrumented native build at zstd levels 1-22).
    for (const c of hufCases()) {
        for (const level of [1, 5]) {
            it(`round-trips ${c.name} (${c.shape}) at level ${level}`, async () => {
                const packed = await compress(c.data, { level });
                expect(packed.byteLength).toBeLessThan(c.data.byteLength);
                expect(same(await decompress(packed), c.data)).toBe(true);
            });
        }
    }
});

describe.skipIf(!hasCli)("interop with the zstd CLI", () => {
    const inputs = [
        ...hufCases().map((c) => ({ name: c.name, data: c.data })),
        { name: "incompressible 100 KB", data: randomBytes(100_000, 99) },
        { name: "empty", data: new Uint8Array(0) },
    ];
    for (const { name, data } of inputs) {
        it(`decodes native streams of ${name}`, async () => {
            for (const level of ["-1", "-5", "-19"]) {
                const packed = cli([level, "-q", "-c"], data);
                expect(packed).not.toBeNull();
                expect(same(await decompress(packed!), data)).toBe(true);
            }
        });

        it(`native zstd decodes our streams of ${name}`, async () => {
            for (const level of [1, 5, 10]) {
                const unpacked = cli(["-d", "-q", "-c"], await compress(data, { level }));
                expect(unpacked).not.toBeNull();
                expect(same(unpacked!, data)).toBe(true);
            }
        });
    }
});
