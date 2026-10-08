/**
 * lz4_raw compress-only subpath. The shared codec .wasm is built with the
 * compress-only export set, so the decoder and its codec closure are
 * dead-stripped. Surface is the encode half of `compress-utils/lz4_raw`;
 * for both directions import that instead.
 */
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
