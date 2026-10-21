/**
 * Directional subpath exports: `compress-utils/<algo>/compress` and
 * `compress-utils/<algo>/decompress`. Each loads its own compress-only or
 * decompress-only .wasm, so these tests prove, per codec, that
 *   - each entry exports exactly its half of the API (and none of the other),
 *   - output from the compress-only entry decodes with the decompress-only
 *     entry and with the full subpath, and vice versa (same wire format),
 *   - one-shot, sync, incremental and Web Streams paths all work per entry,
 *   - errors surface as CompressError with the right status and name.
 */

import { describe, expect, it } from "vitest";

import * as brotli from "compress-utils/brotli";
import * as brotliCompress from "compress-utils/brotli/compress";
import * as brotliDecompress from "compress-utils/brotli/decompress";
import * as bz2 from "compress-utils/bz2";
import * as bz2Compress from "compress-utils/bz2/compress";
import * as bz2Decompress from "compress-utils/bz2/decompress";
import * as deflate from "compress-utils/deflate";
import * as deflateCompress from "compress-utils/deflate/compress";
import * as deflateDecompress from "compress-utils/deflate/decompress";
import * as gzip from "compress-utils/gzip";
import * as gzipCompress from "compress-utils/gzip/compress";
import * as gzipDecompress from "compress-utils/gzip/decompress";
import * as lz4 from "compress-utils/lz4";
import * as lz4Compress from "compress-utils/lz4/compress";
import * as lz4Decompress from "compress-utils/lz4/decompress";
import * as lz4Raw from "compress-utils/lz4_raw";
import * as lz4RawCompress from "compress-utils/lz4_raw/compress";
import * as lz4RawDecompress from "compress-utils/lz4_raw/decompress";
import * as lzoDecompress from "compress-utils/lzo/decompress";
import * as snappy from "compress-utils/snappy";
import * as snappyCompress from "compress-utils/snappy/compress";
import * as snappyDecompress from "compress-utils/snappy/decompress";
import * as xz from "compress-utils/xz";
import * as xzCompress from "compress-utils/xz/compress";
import * as xzDecompress from "compress-utils/xz/decompress";
import * as zlib from "compress-utils/zlib";
import * as zlibCompress from "compress-utils/zlib/compress";
import * as zlibDecompress from "compress-utils/zlib/decompress";
import * as zstd from "compress-utils/zstd";
import * as zstdCompress from "compress-utils/zstd/compress";
import * as zstdDecompress from "compress-utils/zstd/decompress";

/* CompressError status codes (compress_utils.h). */
const INVALID_ARG = 1;
const INVALID_LEVEL = 2;
const SIZE_LIMIT = 9;
const STREAM_FINISHED = 10;
const STREAM_STATE = 11;

const ENCODE_ONLY = ["compress", "compressSync", "createCompressStream", "compressionStream"];
const DECODE_ONLY = [
    "decompress",
    "decompressSync",
    "createDecompressStream",
    "createDecompressStreamSync",
    "decompressionStream",
    "setMaxDecompressedSize",
];
const ENCODE_STREAMS = ["createCompressStream", "createCompressStreamSync", "compressionStream"];
const SHARED = ["preload", "version", "CompressError"];

interface Stream {
    write(chunk: Uint8Array): Uint8Array;
    finish(): Uint8Array;
    destroy(): void;
}
interface Encoder {
    compress(input: Uint8Array, opts?: { level?: number }): Promise<Uint8Array>;
    compressSync(input: Uint8Array, opts?: { level?: number }): Uint8Array;
    preload(): Promise<void>;
    version(): Promise<string>;
    CompressError: typeof zstd.CompressError;
    createCompressStream?: (opts?: { level?: number }) => Promise<Stream & { flush(): Uint8Array }>;
    createCompressStreamSync?: (opts?: { level?: number }) => Stream;
    compressionStream?: (opts?: { level?: number }) => TransformStream<Uint8Array, Uint8Array>;
}
interface Decoder {
    decompress(input: Uint8Array, opts?: { expectedSize?: number }): Promise<Uint8Array>;
    decompressSync(input: Uint8Array, opts?: { expectedSize?: number }): Uint8Array;
    preload(): Promise<void>;
    version(): Promise<string>;
    setMaxDecompressedSize(bytes: number): Promise<void>;
    CompressError: typeof zstd.CompressError;
    createDecompressStream(): Promise<Stream>;
    createDecompressStreamSync(): Stream;
    decompressionStream(): TransformStream<Uint8Array, Uint8Array>;
}
interface Codec {
    name: string;
    full: Encoder & Decoder;
    encoder: Encoder;
    decoder: Decoder;
    /** Raw block formats take an output capacity and have no compress streams. */
    raw: boolean;
}

const codecs: Codec[] = [
    { name: "zstd", full: zstd, encoder: zstdCompress, decoder: zstdDecompress, raw: false },
    {
        name: "brotli",
        full: brotli,
        encoder: brotliCompress,
        decoder: brotliDecompress,
        raw: false,
    },
    { name: "zlib", full: zlib, encoder: zlibCompress, decoder: zlibDecompress, raw: false },
    { name: "bz2", full: bz2, encoder: bz2Compress, decoder: bz2Decompress, raw: false },
    { name: "lz4", full: lz4, encoder: lz4Compress, decoder: lz4Decompress, raw: false },
    { name: "xz", full: xz, encoder: xzCompress, decoder: xzDecompress, raw: false },
    {
        name: "snappy",
        full: snappy,
        encoder: snappyCompress,
        decoder: snappyDecompress,
        raw: false,
    },
    { name: "gzip", full: gzip, encoder: gzipCompress, decoder: gzipDecompress, raw: false },
    {
        name: "deflate",
        full: deflate,
        encoder: deflateCompress,
        decoder: deflateDecompress,
        raw: false,
    },
    {
        name: "lz4_raw",
        full: lz4Raw as unknown as Encoder & Decoder,
        encoder: lz4RawCompress,
        decoder: lz4RawDecompress,
        raw: true,
    },
];

const enc = new TextEncoder();
const input = enc.encode("directional entry point payload, mostly repetitive\n".repeat(400));

for (const { name, full, encoder, decoder, raw } of codecs) {
    const opts = raw ? { expectedSize: input.byteLength } : {};

    describe(`${name} directional subpaths`, () => {
        it("compress entry exports only the encode half", () => {
            const expected = raw ? ["compress", "compressSync"] : ENCODE_ONLY;
            for (const key of [...expected, ...SHARED]) {
                expect(encoder, key).toHaveProperty(key);
            }
            for (const key of DECODE_ONLY) {
                expect(key in encoder, key).toBe(false);
            }
            for (const key of ENCODE_STREAMS) {
                expect(key in encoder, key).toBe(!raw);
            }
        });

        it("decompress entry exports only the decode half", () => {
            for (const key of [...DECODE_ONLY, ...SHARED]) {
                expect(decoder, key).toHaveProperty(key);
            }
            for (const key of [...ENCODE_ONLY, "createCompressStreamSync"]) {
                expect(key in decoder, key).toBe(false);
            }
        });

        it("round-trips compress entry -> decompress entry and with the full subpath", async () => {
            const compressed = await encoder.compress(input);
            expect(compressed.byteLength).toBeGreaterThan(0);
            expect(compressed.byteLength).toBeLessThan(input.byteLength);
            expect(await decoder.decompress(compressed, opts)).toEqual(input);
            // Same wire format as the full subpath, both ways.
            expect(await full.decompress(compressed, opts)).toEqual(input);
            expect(await decoder.decompress(await full.compress(input), opts)).toEqual(input);
        });

        it("round-trips empty input", async () => {
            const empty = new Uint8Array(0);
            const compressed = await encoder.compress(empty);
            expect(await decoder.decompress(compressed, raw ? { expectedSize: 0 } : {})).toEqual(
                empty,
            );
        });

        it("synchronous methods work after each entry is preloaded", async () => {
            await Promise.all([encoder.preload(), decoder.preload()]);
            for (const level of [1, 5, 10]) {
                const compressed = encoder.compressSync(input, { level });
                expect(decoder.decompressSync(compressed, opts)).toEqual(input);
            }
        });

        it("reports the same version as the full subpath", async () => {
            const v = await full.version();
            expect(v).toMatch(/^\d+\.\d+\.\d+$/);
            expect(await encoder.version()).toBe(v);
            expect(await decoder.version()).toBe(v);
        });

        it("decompress entry streams incrementally across chunk boundaries", async () => {
            const compressed = await encoder.compress(input);
            const ds = await decoder.createDecompressStream();
            const out: Uint8Array[] = [];
            try {
                for (let i = 0; i < compressed.byteLength; i += 7) {
                    out.push(ds.write(compressed.subarray(i, i + 7)));
                }
                out.push(ds.finish());
            } finally {
                ds.destroy();
            }
            expect(concat(out)).toEqual(input);

            const sync = decoder.createDecompressStreamSync();
            try {
                expect(concat([sync.write(compressed), sync.finish()])).toEqual(input);
            } finally {
                sync.destroy();
            }
        });

        it("decompress entry works as a Web Stream", async () => {
            const compressed = await encoder.compress(input);
            const out = await collect(
                readable(compressed, 100).pipeThrough(decoder.decompressionStream()),
            );
            expect(out).toEqual(input);
        });

        it("compress entry rejects an out-of-range level with InvalidLevel", async () => {
            const err = await encoder.compress(input, { level: 0 }).catch((e: unknown) => e);
            expect(err).toBeInstanceOf(encoder.CompressError);
            expect(err).toMatchObject({ code: INVALID_LEVEL, algorithm: name });
            expect(() => encoder.compressSync(input, { level: 11 })).toThrow(encoder.CompressError);
        });

        it("decompress entry rejects corrupt input with a CompressError", async () => {
            const corrupt = new Uint8Array([0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff]);
            const err = await decoder
                .decompress(corrupt, raw ? { expectedSize: 64 } : {})
                .catch((e: unknown) => e);
            expect(err).toBeInstanceOf(decoder.CompressError);
            expect((err as InstanceType<typeof zstd.CompressError>).algorithm).toBe(name);
            expect((err as InstanceType<typeof zstd.CompressError>).code).toBeGreaterThan(0);
        });

        if (!raw) {
            it("compress entry streams incrementally and via Web Streams", async () => {
                const e = encoder as Required<Encoder>;
                const cs = await e.createCompressStream({ level: 3 });
                const parts: Uint8Array[] = [];
                try {
                    for (let i = 0; i < input.byteLength; i += 1000) {
                        parts.push(cs.write(input.subarray(i, i + 1000)));
                    }
                    parts.push(cs.finish());
                } finally {
                    cs.destroy();
                }
                expect(await decoder.decompress(concat(parts))).toEqual(input);

                const sync = e.createCompressStreamSync();
                let streamed: Uint8Array;
                try {
                    streamed = concat([sync.write(input), sync.finish()]);
                } finally {
                    sync.destroy();
                }
                expect(await decoder.decompress(streamed)).toEqual(input);

                const piped = await collect(
                    readable(input, 512)
                        .pipeThrough(e.compressionStream({ level: 3 }))
                        .pipeThrough(decoder.decompressionStream()),
                );
                expect(piped).toEqual(input);
            });

            it("compress entry streams reject use after finish or destroy", async () => {
                const cs = await (encoder as Required<Encoder>).createCompressStream();
                cs.write(input);
                cs.finish();
                expect(() => cs.write(input)).toThrow(
                    expect.objectContaining({ code: STREAM_FINISHED, algorithm: name }),
                );
                cs.destroy();
                expect(() => cs.write(input)).toThrow(
                    expect.objectContaining({ code: STREAM_STATE, algorithm: name }),
                );
                expect(() => cs.finish()).toThrow(decoder.CompressError);
            });
        }
    });
}

describe("lz4_raw directional subpaths", () => {
    it("decompress entry validates the output capacity before decoding", async () => {
        const compressed = await lz4RawCompress.compress(input);
        for (const expectedSize of [-1, 1.5, 0x80000000, Number.NaN]) {
            const err = await lz4RawDecompress
                .decompress(compressed, { expectedSize })
                .catch((e: unknown) => e);
            expect(err).toBeInstanceOf(lz4RawDecompress.CompressError);
            expect(err).toMatchObject({ code: INVALID_ARG, algorithm: "lz4_raw" });
            expect(() => lz4RawDecompress.decompressSync(compressed, { expectedSize })).toThrow(
                expect.objectContaining({ code: INVALID_ARG }),
            );
        }
    });

    it("decompress entry decodes without a capacity via the incremental decoder", async () => {
        const compressed = await lz4RawCompress.compress(input);
        expect(await lz4RawDecompress.decompress(compressed)).toEqual(input);
        await lz4RawDecompress.preload();
        expect(lz4RawDecompress.decompressSync(compressed)).toEqual(input);
    });
});

describe("lzo/decompress entry", () => {
    // One LZO1X block: literal run "hello" (first byte 17 + 5), then end marker.
    const block = new Uint8Array([22, ...enc.encode("hello"), 0x11, 0x00, 0x00]);

    it("is decode-only and decodes a literal LZO1X block", async () => {
        for (const key of [...DECODE_ONLY, ...SHARED]) {
            expect(lzoDecompress, key).toHaveProperty(key);
        }
        for (const key of [...ENCODE_ONLY, "createCompressStreamSync"]) {
            expect(key in lzoDecompress, key).toBe(false);
        }
        expect(await lzoDecompress.decompress(block, { expectedSize: 5 })).toEqual(
            enc.encode("hello"),
        );
        expect(await lzoDecompress.decompress(block)).toEqual(enc.encode("hello"));
        await lzoDecompress.preload();
        expect(lzoDecompress.decompressSync(block, { expectedSize: 16 })).toEqual(
            enc.encode("hello"),
        );
        expect(await lzoDecompress.version()).toBe(await zstd.version());
    });

    it("rejects an invalid output capacity with InvalidArg", async () => {
        await expect(lzoDecompress.decompress(block, { expectedSize: -5 })).rejects.toMatchObject({
            code: INVALID_ARG,
            algorithm: "lzo",
        });
        expect(() => lzoDecompress.decompressSync(block, { expectedSize: 2 ** 31 })).toThrow(
            lzoDecompress.CompressError,
        );
    });
});

describe("decompress entry setMaxDecompressedSize", () => {
    it("caps one-shot output with SizeLimit, then restores", async () => {
        const compressed = await zstdCompress.compress(input);
        try {
            await zstdDecompress.setMaxDecompressedSize(1024);
            await expect(zstdDecompress.decompress(compressed)).rejects.toMatchObject({
                code: SIZE_LIMIT,
                algorithm: "zstd",
            });
        } finally {
            await zstdDecompress.setMaxDecompressedSize(1024 * 1024 * 1024);
        }
        expect(await zstdDecompress.decompress(compressed)).toEqual(input);
    });
});

function readable(data: Uint8Array, chunkSize: number): ReadableStream<Uint8Array> {
    let off = 0;
    return new ReadableStream({
        pull(c) {
            if (off >= data.byteLength) {
                c.close();
                return;
            }
            c.enqueue(data.subarray(off, off + chunkSize));
            off += chunkSize;
        },
    });
}

async function collect(stream: ReadableStream<Uint8Array>): Promise<Uint8Array> {
    const chunks: Uint8Array[] = [];
    const reader = stream.getReader();
    while (true) {
        const { value, done } = await reader.read();
        if (done) break;
        chunks.push(value);
    }
    return concat(chunks);
}

function concat(chunks: Uint8Array[]): Uint8Array {
    const total = chunks.reduce((n, c) => n + c.byteLength, 0);
    const out = new Uint8Array(total);
    let off = 0;
    for (const c of chunks) {
        out.set(c, off);
        off += c.byteLength;
    }
    return out;
}
