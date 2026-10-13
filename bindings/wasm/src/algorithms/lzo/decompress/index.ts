/** Raw LZO1X block decoder. No encoder or container framing; incremental block decoding is supported. */
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

/** Creates an incremental raw-block decoder; write() emits output as input arrives. */
export const createDecompressStream = bindings.createDecompressStream;
/** Requires completed preload(), like other synchronous stream factories. */
export const createDecompressStreamSync = bindings.createDecompressStreamSync;
/** Incrementally decodes one raw block through Web Streams. */
export const decompressionStream = bindings.decompressionStream;
export type { DecompressStream } from "../../../core/dispatch.js";
