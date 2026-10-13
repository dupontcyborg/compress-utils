/**
 * lz4_raw compress-only subpath. The shared codec .wasm is built with the
 * compress-only export set, so the decoder and its codec closure are
 * dead-stripped. Surface is the encode half of `compress-utils/lz4_raw`;
 * for both directions import that instead.
 */
import {
    BufferedBlockStream,
    createBlockTransform,
    type BlockCompressOptions,
} from "../../../core/block-stream.js";
import { defineAlgorithm } from "../../../core/algorithm.js";
import { Algorithm } from "../../../core/types.js";

const bindings = /*#__PURE__*/ defineAlgorithm(
    Algorithm.Lz4Raw,
    "lz4_raw",
    new URL("../../lz4/compress/lz4.wasm", import.meta.url),
);

export const compress = bindings.compress;
export const compressSync = bindings.compressSync;
export const preload = bindings.preload;
export const version = bindings.version;

export { CompressError } from "../../../core/types.js";
export type { CompressOptions, AlgorithmName } from "../../../core/types.js";

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

export type { BufferedBlockStream, BlockCompressOptions } from "../../../core/block-stream.js";
