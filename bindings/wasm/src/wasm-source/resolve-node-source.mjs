/** Resolve a local WebAssembly module using Node's source-phase ESM loader. */
export const resolveWasm = (url) => import.source(url.href);
