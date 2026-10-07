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
    if (
        !Number.isSafeInteger(options?.expectedSize) ||
        options.expectedSize < 0 ||
        options.expectedSize > 0x7fffffff
    ) {
        throw new RangeError(
            "LZO expectedSize must be an integer output capacity between 0 and 2147483647",
        );
    }
    return bindings.decompress(input, options);
}
export const version = bindings.version;
export const setMaxDecompressedSize = bindings.setMaxDecompressedSize;
export { CompressError } from "../../../core/types.js";
