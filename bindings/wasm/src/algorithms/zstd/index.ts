/**
 * zstd subpath — default build with runtime detection. Honored when the
 * consumer doesn't pick a more specific export condition.
 */
import { Algorithm } from "../../core/types.js";
import { createBindings } from "../../core/dispatch.js";
import { resolveWasm } from "../../core/resolve-default.js";

const bindings = /*#__PURE__*/ createBindings(
    Algorithm.Zstd,
    "zstd",
    new URL("./zstd.wasm", import.meta.url),
    resolveWasm,
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
