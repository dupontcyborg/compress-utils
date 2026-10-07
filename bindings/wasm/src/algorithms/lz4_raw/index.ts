/**
 * lz4_raw subpath. Resolver is selected via the package's `#resolver`
 * subpath import — see ../../core/algorithm.ts and package.json `imports`.
 */
import { defineAlgorithm } from "../../core/algorithm.js";
import { Algorithm } from "../../core/types.js";

import { validateDecompressOptions } from "./options.js";
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
    validateDecompressOptions(options);
    return bindings.decompress(input, options);
}
/** Decodes a raw block synchronously after preload(). */
export function decompressSync(input: Uint8Array, options: DecompressOptions = {}): Uint8Array {
    validateDecompressOptions(options);
    return bindings.decompressSync(input, options);
}
export const preload = bindings.preload;
export const version = bindings.version;
export const setMaxDecompressedSize = bindings.setMaxDecompressedSize;

export { CompressError } from "../../core/types.js";
export type { CompressOptions, DecompressOptions, AlgorithmName } from "../../core/types.js";
