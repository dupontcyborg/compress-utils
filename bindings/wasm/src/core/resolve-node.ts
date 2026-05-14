/**
 * Node resolver: read the wasm bytes from disk via `fs.readFile`, hand
 * the loader an ArrayBuffer for the non-streaming `WebAssembly.instantiate`
 * path.
 *
 * Why not `fetch`: Node's built-in fetch (undici) deliberately rejects
 * the `file:` scheme with ERR_INVALID_URL_SCHEME. The streaming-compile
 * benefit is also moot here — disk reads aren't latency-bound the way
 * network fetches are.
 */

import { readFile } from "node:fs/promises";
import type { WasmResolver } from "./dispatch.js";

export const resolveWasm: WasmResolver = (url) => readFile(url);
