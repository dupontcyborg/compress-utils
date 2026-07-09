/**
 * snappy compress-only subpath. The co-located .wasm is built with the
 * compress-only export set, so the decoder and its codec closure are
 * dead-stripped. Surface is the encode half of `compress-utils/snappy`;
 * for both directions import that instead.
 */
import { defineAlgorithm } from "../../../core/algorithm.js";
import { Algorithm } from "../../../core/types.js";

const bindings = /*#__PURE__*/ defineAlgorithm(
    Algorithm.Snappy,
    "snappy",
    new URL("./snappy.wasm", import.meta.url),
);

export const compress = bindings.compress;
export const createCompressStream = bindings.createCompressStream;
export const compressionStream = bindings.compressionStream;
export const version = bindings.version;

export { CompressError } from "../../../core/types.js";
export type { CompressOptions, AlgorithmName } from "../../../core/types.js";
export type { CompressStream } from "../../../core/dispatch.js";
