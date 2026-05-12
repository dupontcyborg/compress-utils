/**
 * Default resolver: detects the runtime at module load and picks fetch
 * vs. fs.readFile. Used as a fallback when the consumer doesn't honor
 * the package.json `browser` / `node` / `deno` / `bun` export conditions.
 *
 * Modern bundlers and runtimes hit the conditional exports instead and
 * never load this file. It's a belt-and-braces fallback.
 */

import type { WasmResolver } from "./dispatch.js";

declare const Deno: unknown;
declare const Bun: unknown;

export const resolveWasm: WasmResolver = async (url) => {
    const hasFetch = typeof fetch === "function";
    const isDeno = typeof Deno !== "undefined";
    const isBun = typeof Bun !== "undefined";
    const isNode =
        typeof process !== "undefined" &&
        process.versions?.node !== undefined &&
        !isDeno && !isBun;

    // Node's fetch can't do file://. Everyone else's can (Deno + Bun
    // natively; browsers only see http(s)). So Node is the only path
    // that needs fs.readFile.
    if (isNode && url.protocol === "file:") {
        const { readFile } = await import("node:fs/promises");
        return readFile(url);
    }
    if (hasFetch) return fetch(url);
    throw new Error("no wasm resolver available in this runtime");
};
