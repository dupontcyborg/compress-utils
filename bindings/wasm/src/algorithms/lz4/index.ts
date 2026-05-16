/**
 * lz4 subpath. Resolver is selected via the package's `#resolver`
 * subpath import — see ../../core/algorithm.ts and package.json `imports`.
 */
import { defineAlgorithm } from "../../core/algorithm.js";
import { Algorithm } from "../../core/types.js";

const bindings = /*#__PURE__*/ defineAlgorithm(
    Algorithm.Lz4,
    "lz4",
    new URL("./lz4.wasm", import.meta.url),
);

export const compress = bindings.compress;
export const decompress = bindings.decompress;
export const createCompressStream = bindings.createCompressStream;
export const createDecompressStream = bindings.createDecompressStream;
export const compressionStream = bindings.compressionStream;
export const decompressionStream = bindings.decompressionStream;
export const version = bindings.version;
export const setMaxDecompressedSize = bindings.setMaxDecompressedSize;

export { CompressError } from "../../core/types.js";
export type { CompressOptions, DecompressOptions, AlgorithmName } from "../../core/types.js";
export type { CompressStream, DecompressStream } from "../../core/dispatch.js";
