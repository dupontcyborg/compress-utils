/**
 * Browser / Deno / Bun resolver: fetch returns a Response, the loader
 * passes it to `WebAssembly.instantiateStreaming` for concurrent
 * download+compile.
 *
 * This file contains zero references to `node:*` modules, so bundlers
 * targeting the browser never see them in the import graph — no
 * `external: ["node:*"]` config required at the consumer.
 */

import type { WasmResolver } from "./dispatch.js";

export const resolveWasm: WasmResolver = (url) => fetch(url);
