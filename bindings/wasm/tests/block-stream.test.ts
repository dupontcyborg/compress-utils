import { describe, expect, test } from "vitest";
import * as raw from "compress-utils/lz4_raw";
import * as encoder from "compress-utils/lz4_raw/compress";
import * as decoder from "compress-utils/lz4_raw/decompress";
import * as lzo from "compress-utils/lzo/decompress";

const hello = new TextEncoder().encode("hello");
const lzoHello = new Uint8Array([22, 104, 101, 108, 108, 111, 17, 0, 0]);

/** Feeds every fragment without relying on an output from intermediate writes. */
function writeFragments(stream: { write(chunk: Uint8Array): Uint8Array }, input: Uint8Array): void {
    expect(stream.write(new Uint8Array())).toEqual(new Uint8Array());
    for (const byte of input)
        expect(stream.write(new Uint8Array([byte]))).toEqual(new Uint8Array());
}

/** Drains while writing to avoid Web Streams backpressure deadlocks. */
async function transform(
    stream: TransformStream<Uint8Array, Uint8Array>,
    input: Uint8Array,
): Promise<Uint8Array> {
    const output = new Response(stream.readable).arrayBuffer();
    const writer = stream.writable.getWriter();
    for (const byte of input) await writer.write(new Uint8Array([byte]));
    await writer.close();
    return new Uint8Array(await output);
}

describe("bounded raw block streams", () => {
    test("synchronous factories require preload", async () => {
        expect(() => encoder.createCompressStreamSync()).toThrow(/preload/);
        expect(() => decoder.createDecompressStreamSync({ expectedSize: 10 })).toThrow(/preload/);
        expect(() => lzo.createDecompressStreamSync({ expectedSize: 10 })).toThrow(/preload/);
        await Promise.all([encoder.preload(), decoder.preload(), lzo.preload(), raw.preload()]);
    });
    test("LZ4 full and directional paths round-trip fragments into one raw block", async () => {
        const input = new TextEncoder().encode("overlapping matches ".repeat(500));
        for (const codec of [raw, encoder]) {
            const stream = await codec.createCompressStream();
            writeFragments(stream, input);
            const compressed = stream.finish();
            expect(await raw.decompress(compressed, { expectedSize: input.length })).toEqual(input);
            for (const decode of [raw, decoder]) {
                const restored = decode.createDecompressStreamSync({
                    expectedSize: input.length + 100,
                });
                writeFragments(restored, compressed);
                expect(restored.finish()).toEqual(input);
                restored.destroy();
            }
            stream.destroy();
        }
    });
    test("LZO copies input, snapshots capacity and emits only actual decoded bytes", async () => {
        const options = { expectedSize: 10 };
        const stream = await lzo.createDecompressStream(options);
        options.expectedSize = 1;
        const mutable = lzoHello.slice();
        stream.write(mutable.subarray(0, 4));
        mutable.fill(0);
        stream.write(lzoHello.subarray(4));
        expect(stream.finish()).toEqual(hello);
        expect(() => stream.finish()).toThrow(expect.objectContaining({ code: 10 }));
        expect(() => stream.write(hello)).toThrow(expect.objectContaining({ code: 10 }));
        stream.destroy();
        stream.destroy();
        expect(() => stream.finish()).toThrow(expect.objectContaining({ code: 11 }));
    });
    test("validates capacities and bounded input before retaining oversized chunks", async () => {
        for (const maxInputSize of [-1, 0.5, NaN, Infinity, 0x80000000]) {
            expect(() => encoder.createCompressStreamSync({ maxInputSize })).toThrow(
                expect.objectContaining({ code: 1 }),
            );
        }
        for (const codec of [raw, decoder, lzo]) {
            await expect(codec.createDecompressStream({ expectedSize: -1 })).rejects.toMatchObject({
                code: 1,
            });
            const stream = await codec.createDecompressStream({
                expectedSize: 10,
                maxInputSize: 2,
            });
            stream.write(new Uint8Array([1, 2]));
            expect(() => stream.write(new Uint8Array([3]))).toThrow(
                expect.objectContaining({ code: 9 }),
            );
            expect(() => stream.finish()).toThrow(expect.objectContaining({ code: 11 }));
        }
    });
    test("empty blocks, insufficient capacity and truncated LZO retain one-shot errors", async () => {
        const empty = encoder.createCompressStreamSync({ maxInputSize: 0 });
        const decoded = decoder.createDecompressStreamSync({ expectedSize: 0 });
        decoded.write(empty.finish());
        expect(decoded.finish()).toEqual(new Uint8Array());
        const emptyLzo = lzo.createDecompressStreamSync({ expectedSize: 0 });
        writeFragments(emptyLzo, new Uint8Array([17, 0, 0]));
        expect(emptyLzo.finish()).toEqual(new Uint8Array());
        for (const input of [lzoHello, lzoHello.subarray(0, 7)]) {
            const stream = lzo.createDecompressStreamSync({
                expectedSize: input.length === lzoHello.length ? 1 : 10,
            });
            stream.write(input);
            expect(() => stream.finish()).toThrow();
            expect(() => stream.finish()).toThrow(expect.objectContaining({ code: 10 }));
            stream.destroy();
        }
    });
    test("stream decoding honors the configured output limit", async () => {
        await lzo.setMaxDecompressedSize(3);
        try {
            const stream = lzo.createDecompressStreamSync({ expectedSize: 10 });
            stream.write(lzoHello);
            expect(() => stream.finish()).toThrow(expect.objectContaining({ code: 9 }));
            stream.destroy();
        } finally {
            await lzo.setMaxDecompressedSize(1024 * 1024 * 1024);
        }
    });
    test("Web Streams support both raw LZ4 directions and decoder-only LZO", async () => {
        const compressed = await transform(raw.compressionStream(), hello);
        expect(await transform(raw.decompressionStream({ expectedSize: 10 }), compressed)).toEqual(
            hello,
        );
        expect(
            await transform(
                decoder.decompressionStream({ expectedSize: 10 }),
                await transform(encoder.compressionStream(), hello),
            ),
        ).toEqual(hello);
        expect(await transform(lzo.decompressionStream({ expectedSize: 10 }), lzoHello)).toEqual(
            hello,
        );
        expect("createCompressStream" in lzo).toBe(false);
    });
});
