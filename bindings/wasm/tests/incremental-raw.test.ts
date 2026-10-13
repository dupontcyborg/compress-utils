import { readFileSync } from "node:fs";
import { describe, expect, test } from "vitest";
import * as raw from "compress-utils/lz4_raw";
import * as decoder from "compress-utils/lz4_raw/decompress";
import * as lzo from "compress-utils/lzo/decompress";

/** Decodes chunks while requiring observable output before finish. */
async function decode(codec: typeof decoder | typeof lzo, input: Uint8Array, chunkSize: number) {
    const stream = await codec.createDecompressStream();
    const chunks: Uint8Array[] = [];
    let produced = 0;
    try {
        for (let offset = 0; offset < input.length; offset += chunkSize) {
            const output = stream.write(input.subarray(offset, offset + chunkSize));
            produced += output.length;
            chunks.push(output);
        }
        expect(stream.finish()).toEqual(new Uint8Array());
        return { output: new Uint8Array(Buffer.concat(chunks)), produced };
    } finally {
        stream.destroy();
    }
}
/** Loads independently generated LZO1X-1 / -999 fixtures. */
const fixture = (name: string) =>
    new Uint8Array(
        readFileSync(new URL(`../../../tests/fixtures/lzo/${name}.bin`, import.meta.url)),
    );

describe("incremental native raw decoders", () => {
    test.each([1, 2, 7, 257, 65536])(
        "raw LZ4 matches one-shot decoding with %i-byte chunks",
        async (chunkSize) => {
            const input = new TextEncoder().encode("overlapping history ".repeat(10000));
            const encoded = await raw.compress(input);
            for (const codec of [raw, decoder]) {
                const result = await decode(codec, encoded, chunkSize);
                expect(result.output).toEqual(input);
                expect(result.produced).toBe(input.length);
            }
        },
    );
    test.each(["1", "999"])(
        "independent LZO1X-%s emits during write across one-byte fragments",
        async (variant) => {
            const expected = fixture("input");
            for (const chunkSize of [1, 2, 7, 257]) {
                const result = await decode(lzo, fixture(variant), chunkSize);
                expect(result.output).toEqual(expected);
                expect(result.produced).toBe(expected.length);
            }
        },
    );
    test("emits literals before receiving the final marker", async () => {
        const stream = await lzo.createDecompressStream();
        try {
            expect(new TextDecoder().decode(stream.write(new Uint8Array([22, 104, 101])))).toBe(
                "he",
            );
            expect(new TextDecoder().decode(stream.write(new Uint8Array([108, 108, 111])))).toBe(
                "llo",
            );
            expect(stream.write(new Uint8Array([17, 0, 0]))).toEqual(new Uint8Array());
            expect(stream.finish()).toEqual(new Uint8Array());
        } finally {
            stream.destroy();
        }
    });
    test("empty blocks, invalid offsets, truncation and trailing data", async () => {
        expect((await decode(decoder, new Uint8Array([0]), 1)).output).toEqual(new Uint8Array());
        expect((await decode(lzo, new Uint8Array([17, 0, 0]), 1)).output).toEqual(new Uint8Array());
        for (const [codec, input] of [
            [decoder, [0, 0, 0]],
            [lzo, [18, 65, 64, 1]],
            [lzo, [17, 0, 0, 1]],
        ] as const) {
            const stream = await codec.createDecompressStream();
            try {
                expect(() => stream.write(new Uint8Array(input))).toThrow();
            } finally {
                stream.destroy();
            }
        }
        for (const codec of [decoder, lzo]) {
            const stream = await codec.createDecompressStream();
            try {
                stream.write(new Uint8Array([0xf0]));
                expect(() => stream.finish()).toThrow();
            } finally {
                stream.destroy();
            }
        }
    });
    test("stream limit bounds cumulative output and is captured at creation", async () => {
        await lzo.setMaxDecompressedSize(4);
        const stream = await lzo.createDecompressStream();
        await lzo.setMaxDecompressedSize(1024 * 1024 * 1024);
        try {
            expect(() => stream.write(new Uint8Array([22, 104, 101, 108, 108, 111]))).toThrow(
                expect.objectContaining({ code: 9 }),
            );
        } finally {
            stream.destroy();
        }
    });
    test("sync and Web Stream methods share incremental behavior", async () => {
        await lzo.preload();
        const sync = lzo.createDecompressStreamSync();
        expect(
            new TextDecoder().decode(sync.write(new Uint8Array([22, 104, 101, 108, 108, 111]))),
        ).toBe("hello");
        sync.write(new Uint8Array([17, 0, 0]));
        sync.finish();
        sync.destroy();
        const transform = lzo.decompressionStream();
        const reader = transform.readable.getReader();
        const writer = transform.writable.getWriter();
        const first = reader.read();
        await writer.write(new Uint8Array([22, 104, 101, 108, 108, 111]));
        expect(new TextDecoder().decode((await first).value)).toBe("hello");
        const tail = (async () => {
            let length = 0;
            for (;;) {
                const chunk = await reader.read();
                if (chunk.done) return length;
                length += chunk.value.length;
            }
        })();
        await writer.write(new Uint8Array([17, 0, 0]));
        await writer.close();
        expect(await tail).toBe(0);
        expect("compressionStream" in raw).toBe(false);
        expect("compress" in lzo).toBe(false);
    });
});
