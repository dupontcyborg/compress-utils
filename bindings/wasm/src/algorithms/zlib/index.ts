/**
 * zlib subpath. Resolver is selected via the package's `#resolver`
 * subpath import — see ../../core/algorithm.ts and package.json `imports`.
 */
import { defineAlgorithm } from "../../core/algorithm.js";
import { Algorithm } from "../../core/types.js";

const bindings = /*#__PURE__*/ defineAlgorithm(
    Algorithm.Zlib,
    "zlib",
    new URL("./zlib.wasm", import.meta.url),
);

export const compress = bindings.compress;
/** Synchronous counterpart; requires completed preload(). */
export const compressSync = bindings.compressSync;
export const decompress = bindings.decompress;
/** Synchronous counterpart; requires completed preload(). */
export const decompressSync = bindings.decompressSync;
export const createCompressStream = bindings.createCompressStream;
/** Synchronous counterpart; requires completed preload(). */
export const createCompressStreamSync = bindings.createCompressStreamSync;
export const createDecompressStream = bindings.createDecompressStream;
/** Synchronous counterpart; requires completed preload(). */
export const createDecompressStreamSync = bindings.createDecompressStreamSync;
export const compressionStream = bindings.compressionStream;
export const decompressionStream = bindings.decompressionStream;
/** Initializes this independently loaded WASM subpath. */
export const preload = bindings.preload;
export const version = bindings.version;
export const setMaxDecompressedSize = bindings.setMaxDecompressedSize;

export { CompressError } from "../../core/types.js";
export type { CompressOptions, DecompressOptions, AlgorithmName } from "../../core/types.js";
export type { CompressStream, DecompressStream } from "../../core/dispatch.js";
