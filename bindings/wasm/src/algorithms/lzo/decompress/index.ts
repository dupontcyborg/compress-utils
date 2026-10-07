/** Raw LZO1X block decoder. No encoder, streaming, or container framing. */
import { defineAlgorithm } from "../../../core/algorithm.js";
import { Algorithm, type DecompressOptions } from "../../../core/types.js";
const bindings = /*#__PURE__*/ defineAlgorithm(
    Algorithm.Lzo,
    "lzo",
    new URL("./lzo.wasm", import.meta.url),
);
/** Decode one block into the supplied output capacity; return actual bytes. */
export function decompress(
    input: Uint8Array,
    options: DecompressOptions & { expectedSize: number },
): Promise<Uint8Array> {
    validateOutputCapacity(options);
    return bindings.decompress(input, options);
}
/** Decode synchronously after preload(), returning only the actual decoded bytes. */
export function decompressSync(
    input: Uint8Array,
    options: DecompressOptions & { expectedSize: number },
): Uint8Array {
    validateOutputCapacity(options);
    return bindings.decompressSync(input, options);
}
/** Load and initialize the decoder once, sharing concurrent initialization. */
export const preload = bindings.preload;
/** Reject missing or invalid capacity before initializing or allocating. */
function validateOutputCapacity(options: DecompressOptions & { expectedSize: number }): void {
    if (
        !Number.isSafeInteger(options?.expectedSize) ||
        options.expectedSize < 0 ||
        options.expectedSize > 0x7fffffff
    ) {
        throw new RangeError(
            "LZO expectedSize must be an integer output capacity between 0 and 2147483647",
        );
    }
}
export const version = bindings.version;
export const setMaxDecompressedSize = bindings.setMaxDecompressedSize;
export { CompressError } from "../../../core/types.js";
