/**
 * snappy decompress-only subpath. The co-located .wasm is built with the
 * decompress-only export set, so the encoder and its codec closure are
 * dead-stripped — a fraction of the full module. Surface is the decode half
 * of `compress-utils/snappy`; for both directions import that instead.
 */
import { defineAlgorithm } from "../../../core/algorithm.js";
import { Algorithm } from "../../../core/types.js";

const bindings = /*#__PURE__*/ defineAlgorithm(
    Algorithm.Snappy,
    "snappy",
    new URL("./snappy.wasm", import.meta.url),
);

export const decompress = bindings.decompress;
export const createDecompressStream = bindings.createDecompressStream;
export const decompressionStream = bindings.decompressionStream;
export const version = bindings.version;
export const setMaxDecompressedSize = bindings.setMaxDecompressedSize;

export { CompressError } from "../../../core/types.js";
export type { DecompressOptions, AlgorithmName } from "../../../core/types.js";
export type { DecompressStream } from "../../../core/dispatch.js";
