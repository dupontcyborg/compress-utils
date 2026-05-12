/**
 * bz2 subpath — Node build. Reads the .wasm via fs.readFile; no
 * runtime branching. Selected via the "node" package.json export condition.
 */
import { Algorithm } from "../../core/types.js";
import { createBindings } from "../../core/dispatch.js";
import { resolveWasm } from "../../core/resolve-node.js";

const bindings = /*#__PURE__*/ createBindings(
    Algorithm.Bz2,
    "bz2",
    new URL("./bz2.wasm", import.meta.url),
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
