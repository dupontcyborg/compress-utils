/**
 * Per-algorithm binding factory.
 *
 * Each `algorithms/<algo>/index.ts` is a one-liner that calls this with its
 * enum value + canonical name + co-located .wasm URL. The runtime resolver
 * is selected via the package's `#resolver` subpath import (see
 * package.json `imports`), so this file has no node-vs-browser branching.
 *
 * Exports are aliased back to module scope so tree-shaking works at the
 * import-site granularity: a consumer pulling only `version` from this
 * subpath still triggers `createBindings`, but bundlers can drop the
 * unused stream constructors from the final bundle.
 */

import { resolveWasm } from "#resolver";
import { createBindings } from "./dispatch.js";
import { Algorithm, type AlgorithmName } from "./types.js";

export function defineAlgorithm(
    algorithm: Algorithm,
    name: AlgorithmName,
    wasmUrl: URL,
): ReturnType<typeof createBindings> {
    return createBindings(algorithm, name, wasmUrl, resolveWasm);
}
