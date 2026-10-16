/** Nonterminal flush: every flush makes everything written so far decodable on one open stream. */
import { describe, expect, it } from "vitest";
import * as brotli from "compress-utils/brotli";
import * as bz2 from "compress-utils/bz2";
import * as deflate from "compress-utils/deflate";
import * as gzip from "compress-utils/gzip";
import * as lz4 from "compress-utils/lz4";
import * as snappy from "compress-utils/snappy";
import * as xz from "compress-utils/xz";
import * as zlib from "compress-utils/zlib";
import * as zstd from "compress-utils/zstd";
import * as lz4Decoder from "compress-utils/lz4/decompress";
import * as zstdDecoder from "compress-utils/zstd/decompress";

const flushing = { brotli, deflate, gzip, lz4, xz, zlib, zstd };

function pseudoRandom(length: number, seed: number): Uint8Array {
    const out = new Uint8Array(length);
    let x = seed;
    for (let i = 0; i < length; i++) {
        x = (x * 1103515245 + 12345) >>> 0;
        out[i] = x >>> 16;
    }
    return out;
}

const messages = [
    new Uint8Array(131072).fill(65),
    pseudoRandom(65537, 7),
    new Uint8Array(37).fill(66),
];

function concat(parts: Uint8Array[]): Uint8Array {
    const out = new Uint8Array(parts.reduce((n, p) => n + p.byteLength, 0));
    let offset = 0;
    for (const p of parts) {
        out.set(p, offset);
        offset += p.byteLength;
    }
    return out;
}

describe("compress stream flush", () => {
    for (const [name, codec] of Object.entries(flushing)) {
        it(`${name}: each flush delivers everything written so far`, async () => {
            using encoder = await codec.createCompressStream();
            using decoder = await codec.createDecompressStream();
            const sent: Uint8Array[] = [];
            const received: Uint8Array[] = [];
            for (const message of messages) {
                const wire = concat([encoder.write(message), encoder.flush()]);
                sent.push(message);
                received.push(decoder.write(wire));
                expect(concat(received)).toEqual(concat(sent));
                expect(encoder.flush().byteLength).toBe(0); // nothing new to flush
            }
            received.push(decoder.write(encoder.finish()), decoder.finish());
            expect(concat(received)).toEqual(concat(sent));
            expect(() => encoder.flush()).toThrow(/finish/);
        });

        it(`${name}: compressionStream({ flushEachChunk }) chunks decode on arrival`, async () => {
            const transform = codec.compressionStream({ flushEachChunk: true });
            const writer = transform.writable.getWriter();
            const reader = transform.readable.getReader();
            using decoder = await codec.createDecompressStream();
            for (const message of messages) {
                void writer.write(message);
                const { value } = await reader.read();
                expect(decoder.write(value!)).toEqual(message);
            }
            void writer.close();
            const parts: Uint8Array[] = [];
            for (;;) {
                const { value, done } = await reader.read();
                if (done) break;
                parts.push(decoder.write(value));
            }
            expect(concat([...parts, decoder.finish()]).byteLength).toBe(0);
        });
    }

    // Browser receivers often load only the decode-only module, built separately.
    for (const [name, encoder, decoder] of [
        ["zstd", zstd, zstdDecoder],
        ["lz4", lz4, lz4Decoder],
    ] as const) {
        it(`${name}: the decode-only module receives each flush completely`, async () => {
            using cs = await encoder.createCompressStream();
            using ds = await decoder.createDecompressStream();
            const sent: Uint8Array[] = [];
            const received: Uint8Array[] = [];
            for (const message of messages) {
                sent.push(message);
                received.push(ds.write(concat([cs.write(message), cs.flush()])));
                expect(concat(received)).toEqual(concat(sent));
            }
        });
    }

    for (const [name, codec] of Object.entries({ bz2, snappy })) {
        it(`${name}: flush is unsupported`, async () => {
            using encoder = await codec.createCompressStream();
            encoder.write(new Uint8Array([1, 2, 3]));
            expect(() => encoder.flush()).toThrow(codec.CompressError);
        });
    }
});
