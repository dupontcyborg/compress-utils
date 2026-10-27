/**
 * Deterministic inputs for the zstd tests that target the wasm32-specific
 * 64-bit-register paths (third_party/patches/zstd). Plain TypeScript with no
 * test-runner imports so scripts can load it too.
 */

/** mulberry32: small, seedable PRNG with a stable sequence across runtimes. */
export function prng(seed: number): () => number {
    let a = seed >>> 0;
    return () => {
        a = (a + 0x6d2b79f5) >>> 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

/** Incompressible bytes. */
export function randomBytes(n: number, seed: number): Uint8Array {
    const next = prng(seed);
    const out = new Uint8Array(n);
    for (let i = 0; i < n; i++) out[i] = Math.floor(next() * 256);
    return out;
}

/**
 * `n` bytes drawn from `k` distinct byte values with a geometric-style skew
 * (larger `p` is more skewed). Literals like these are what zstd entropy-codes
 * with Huffman; the skew and size pick which decoder zstd's `HUF_selectDecoder`
 * chooses.
 */
export function skewedBytes(n: number, seed: number, k: number, p: number): Uint8Array {
    const next = prng(seed);
    const alphabet = new Uint8Array(k);
    const seen = new Set<number>();
    for (let i = 0; i < k; ) {
        const v = Math.floor(next() * 256);
        if (!seen.has(v)) {
            seen.add(v);
            alphabet[i++] = v;
        }
    }
    const out = new Uint8Array(n);
    for (let i = 0; i < n; i++) {
        const idx = Math.min(Math.floor(-Math.log(1 - next()) / p), k - 1);
        out[i] = alphabet[idx]!;
    }
    return out;
}

/** Layout of the far-offset fixture: head | zeros | head. */
export const FAR_HEAD_BYTES = 2048;
export const FAR_GAP_BYTES = 40 * 1024 * 1024;

/**
 * `head | 40 MiB of zeros | head`, with `head` random. The second copy of
 * `head` is a match ~40 MiB back, which needs a 26-bit offset: it only decodes
 * if the decoder reads and reloads the offset bits correctly. Compressed with
 * zstd's CLI as `zstd --long=27 -19` this is a ~2 KiB stream
 * (fixtures/zstd-far-offset.zst), so it is checked in rather than generated.
 */
export function farOffsetInput(): Uint8Array {
    const head = randomBytes(FAR_HEAD_BYTES, 0xfa40ff5e);
    const out = new Uint8Array(FAR_HEAD_BYTES * 2 + FAR_GAP_BYTES);
    out.set(head, 0);
    out.set(head, FAR_HEAD_BYTES + FAR_GAP_BYTES);
    return out;
}

/**
 * Inputs whose literal sections take each Huffman decoder shape of zstd's
 * decompressor. Checked with an instrumented native build of the vendored
 * zstd at levels 1 and 5; zstd picks the decoder from (literal size,
 * compressed size), so the choice is the same for every build of the library.
 */
export interface HufCase {
    name: string;
    shape: "4-stream X1 (single-symbol table)" | "4-stream X2 (double-symbol table)" | "1-stream";
    data: Uint8Array;
}

export function hufCases(): HufCase[] {
    return [
        {
            name: "600 B, 100 symbols",
            shape: "4-stream X1 (single-symbol table)",
            data: skewedBytes(600, 33, 100, 0.05),
        },
        {
            name: "4 KiB, 100 symbols",
            shape: "4-stream X1 (single-symbol table)",
            data: skewedBytes(4096, 48, 100, 0.05),
        },
        {
            name: "20 KB, 100 symbols",
            shape: "4-stream X2 (double-symbol table)",
            data: skewedBytes(20000, 62, 100, 0.15),
        },
        {
            name: "64 KiB, 100 symbols",
            shape: "4-stream X2 (double-symbol table)",
            data: skewedBytes(65536, 77, 100, 0.15),
        },
        { name: "200 B, 100 symbols", shape: "1-stream", data: skewedBytes(200, 17, 100, 0.15) },
    ];
}
