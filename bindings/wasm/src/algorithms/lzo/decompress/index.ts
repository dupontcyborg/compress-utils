/** Raw LZO1X block decoder. No encoder or container framing; chunk-input streams buffer one block. */
import {
    BufferedBlockStream,
    createBlockTransform,
    type BlockDecompressOptions,
} from "../../../core/block-stream.js";
import { defineAlgorithm } from "../../../core/algorithm.js";
import { validateCapacity } from "../../../core/capacity.js";
import { Algorithm, type DecompressOptions } from "../../../core/types.js";
const bindings = /*#__PURE__*/ defineAlgorithm(
    Algorithm.Lzo,
    "lzo",
    new URL("./lzo.wasm", import.meta.url),
);
/** Decode one block into the supplied output capacity; return actual bytes. */
export async function decompress(
    input: Uint8Array,
    options: DecompressOptions & { expectedSize: number },
): Promise<Uint8Array> {
    validateCapacity("lzo", "raw LZO1X", options);
    return bindings.decompress(input, options);
}
/** Decode synchronously after preload(), returning only the actual decoded bytes. */
export function decompressSync(
    input: Uint8Array,
    options: DecompressOptions & { expectedSize: number },
): Uint8Array {
    validateCapacity("lzo", "raw LZO1X", options);
    return bindings.decompressSync(input, options);
}
/** Load and initialize the decoder once, sharing concurrent initialization. */
export const preload = bindings.preload;
export const version = bindings.version;
export const setMaxDecompressedSize = bindings.setMaxDecompressedSize;
export { CompressError } from "../../../core/types.js";

/** Creates a bounded single-block decompress stream after preload(); output is deferred to finish(). */
export function createDecompressStreamSync(options: BlockDecompressOptions): BufferedBlockStream {
    validateCapacity("lzo", "raw block", options);
    bindings.assertInitialized();
    const codecOptions = { expectedSize: options.expectedSize };
    return new BufferedBlockStream(
        "lzo",
        (input) => bindings.decompressSync(input, codecOptions),
        options.maxInputSize,
    );
}
/** Preloads the codec and creates a bounded single-block decompress stream. */
export async function createDecompressStream(
    options: BlockDecompressOptions,
): Promise<BufferedBlockStream> {
    validateCapacity("lzo", "raw block", options);
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
