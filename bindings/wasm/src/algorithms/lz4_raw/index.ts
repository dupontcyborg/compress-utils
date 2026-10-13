/**
 * lz4_raw subpath. Resolver is selected via the package's `#resolver`
 * subpath import — see ../../core/algorithm.ts and package.json `imports`.
 */
import {
    BufferedBlockStream,
    createBlockTransform,
    type BlockCompressOptions,
    type BlockDecompressOptions,
} from "../../core/block-stream.js";
import { defineAlgorithm } from "../../core/algorithm.js";
import { Algorithm } from "../../core/types.js";

import { validateCapacity } from "../../core/capacity.js";
import type { DecompressOptions } from "../../core/types.js";

const bindings = /*#__PURE__*/ defineAlgorithm(
    Algorithm.Lz4Raw,
    "lz4_raw",
    new URL("../lz4/lz4.wasm", import.meta.url),
);

export const compress = bindings.compress;
export const compressSync = bindings.compressSync;
/** Decodes a raw block into the required output capacity. */
export async function decompress(
    input: Uint8Array,
    options: DecompressOptions = {},
): Promise<Uint8Array> {
    validateCapacity("lz4_raw", "raw LZ4", options);
    return bindings.decompress(input, options);
}
/** Decodes a raw block synchronously after preload(). */
export function decompressSync(input: Uint8Array, options: DecompressOptions = {}): Uint8Array {
    validateCapacity("lz4_raw", "raw LZ4", options);
    return bindings.decompressSync(input, options);
}
export const preload = bindings.preload;
export const version = bindings.version;
export const setMaxDecompressedSize = bindings.setMaxDecompressedSize;

export { CompressError } from "../../core/types.js";
export type { CompressOptions, DecompressOptions, AlgorithmName } from "../../core/types.js";

/** Creates a bounded single-block compress stream after preload(); output is deferred to finish(). */
export function createCompressStreamSync(options: BlockCompressOptions = {}): BufferedBlockStream {
    bindings.assertInitialized();
    const codecOptions = options.level === undefined ? {} : { level: options.level };
    return new BufferedBlockStream(
        "lz4_raw",
        (input) => bindings.compressSync(input, codecOptions),
        options.maxInputSize,
    );
}
/** Preloads the codec and creates a bounded single-block compress stream. */
export async function createCompressStream(
    options: BlockCompressOptions = {},
): Promise<BufferedBlockStream> {
    await bindings.preload();
    return createCompressStreamSync(options);
}
/** Accepts fragments of one raw block and emits output when the writable closes. */
export function compressionStream(
    options: BlockCompressOptions = {},
): TransformStream<Uint8Array, Uint8Array> {
    return createBlockTransform(() => createCompressStream(options));
}

/** Creates a bounded single-block decompress stream after preload(); output is deferred to finish(). */
export function createDecompressStreamSync(options: BlockDecompressOptions): BufferedBlockStream {
    validateCapacity("lz4_raw", "raw block", options);
    bindings.assertInitialized();
    const codecOptions = { expectedSize: options.expectedSize };
    return new BufferedBlockStream(
        "lz4_raw",
        (input) => bindings.decompressSync(input, codecOptions),
        options.maxInputSize,
    );
}
/** Preloads the codec and creates a bounded single-block decompress stream. */
export async function createDecompressStream(
    options: BlockDecompressOptions,
): Promise<BufferedBlockStream> {
    validateCapacity("lz4_raw", "raw block", options);
    await bindings.preload();
    return createDecompressStreamSync(options);
}
/** Accepts fragments of one raw block and emits output when the writable closes. */
export function decompressionStream(
    options: BlockDecompressOptions,
): TransformStream<Uint8Array, Uint8Array> {
    return createBlockTransform(() => createDecompressStream(options));
}

export type {
    BufferedBlockStream,
    BlockCompressOptions,
    BlockDecompressOptions,
} from "../../core/block-stream.js";
