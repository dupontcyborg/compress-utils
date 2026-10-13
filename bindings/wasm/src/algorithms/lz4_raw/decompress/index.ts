/**
 * lz4_raw decompress-only subpath. The shared codec .wasm is built with the
 * decompress-only export set, so the encoder and its codec closure are
 * dead-stripped — a fraction of the full module. Surface is the decode half
 * of `compress-utils/lz4_raw`; for both directions import that instead.
 */
import {
    BufferedBlockStream,
    createBlockTransform,
    type BlockDecompressOptions,
} from "../../../core/block-stream.js";
import { defineAlgorithm } from "../../../core/algorithm.js";
import { Algorithm } from "../../../core/types.js";

import { validateCapacity } from "../../../core/capacity.js";
import type { DecompressOptions } from "../../../core/types.js";

const bindings = /*#__PURE__*/ defineAlgorithm(
    Algorithm.Lz4Raw,
    "lz4_raw",
    new URL("../../lz4/decompress/lz4.wasm", import.meta.url),
);

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

export { CompressError } from "../../../core/types.js";
export type { DecompressOptions, AlgorithmName } from "../../../core/types.js";

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

export type { BufferedBlockStream, BlockDecompressOptions } from "../../../core/block-stream.js";
