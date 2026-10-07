/**
 * lz4_raw decompress-only subpath. The shared codec .wasm is built with the
 * decompress-only export set, so the encoder and its codec closure are
 * dead-stripped — a fraction of the full module. Surface is the decode half
 * of `compress-utils/lz4_raw`; for both directions import that instead.
 */
import { defineAlgorithm } from "../../../core/algorithm.js";
import { Algorithm } from "../../../core/types.js";

const bindings = /*#__PURE__*/ defineAlgorithm(
    Algorithm.Lz4Raw,
    "lz4_raw",
    new URL("../../lz4/decompress/lz4.wasm", import.meta.url),
);

export const decompress = bindings.decompress;
export const version = bindings.version;
export const setMaxDecompressedSize = bindings.setMaxDecompressedSize;

export { CompressError } from "../../../core/types.js";
export type { DecompressOptions, AlgorithmName } from "../../../core/types.js";
