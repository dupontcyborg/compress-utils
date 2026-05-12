# compress-utils

[![npm version](https://img.shields.io/npm/v/compress-utils.svg)](https://www.npmjs.com/package/compress-utils)
[![npm downloads](https://img.shields.io/npm/dm/compress-utils.svg)](https://www.npmjs.com/package/compress-utils)
[![types: built-in](https://img.shields.io/badge/types-built--in-blue.svg)](https://www.npmjs.com/package/compress-utils)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

Unified WebAssembly bindings for six compression algorithms — Brotli, bzip2, LZ4, XZ/LZMA, zlib, and Zstandard. Same API for every algorithm, in every JavaScript runtime, with per-algorithm imports so bundlers ship only what you use.

## Installation

```bash
npm install compress-utils
# or: pnpm add / yarn add / bun add / deno add npm:compress-utils
```

No native dependencies, no `postinstall` scripts. Pure WebAssembly. TypeScript types are bundled.

## Quick start

Each algorithm is its own subpath import. Pick the one you need; the rest stay out of your bundle.

```ts
import { compress, decompress } from "compress-utils/zstd";

const input = new TextEncoder().encode("the quick brown fox " .repeat(50));

const compressed = await compress(input, { level: 9 });
const restored   = await decompress(compressed);
```

Available subpaths: `compress-utils/zstd`, `/brotli`, `/zlib`, `/bz2`, `/lz4`, `/xz`. Identical surface across all six.

### Web Streams

```ts
import { compressionStream, decompressionStream } from "compress-utils/zstd";

await fetch("/big-file.json")
    .then(r => r.body!.pipeThrough(compressionStream({ level: 3 })))
    .then(s => s.pipeTo(uploadDestination));
```

### Manual streaming (with `using` for auto-cleanup)

```ts
import { createCompressStream } from "compress-utils/brotli";

using cs = await createCompressStream({ level: 5 });
const chunks: Uint8Array[] = [];
for (const chunk of input) chunks.push(cs.write(chunk));
chunks.push(cs.finish());
// cs is destroyed automatically on scope exit (TS 5.2+ / Node 22+).
```

If `using` isn't available in your toolchain, call `cs.destroy()` explicitly — or rely on the GC backstop (a `FinalizationRegistry` frees the C-side handle eventually).

## Supported algorithms

| Algorithm | Subpath                  | Approximate `.wasm` size | Wire format produced                       |
|-----------|--------------------------|---------------------------|--------------------------------------------|
| zlib      | `compress-utils/zlib`    | 80 KB                     | zlib wrapper (RFC 1950)                    |
| bzip2     | `compress-utils/bz2`     | 95 KB                     | bzip2 stream                               |
| XZ/LZMA   | `compress-utils/xz`      | 135 KB                    | XZ stream with CRC64                       |
| LZ4       | `compress-utils/lz4`     | 140 KB                    | LZ4 frame (compatible with `.lz4` files)   |
| Zstandard | `compress-utils/zstd`    | 545 KB                    | ZSTD frame with content size               |
| Brotli    | `compress-utils/brotli`  | 730 KB                    | raw Brotli stream                          |

Imports are independent — `import "compress-utils/zstd"` and `import "compress-utils/brotli"` pull in two separate `.wasm` modules, not a combined bundle. Files marked `"sideEffects": false` so unused exports are tree-shaken aggressively.

## Other helpful APIs

```ts
import {
    version,
    setMaxDecompressedSize,
    CompressError,
} from "compress-utils/zstd";

await version();                          // "0.1.0"
await setMaxDecompressedSize(256 * 1024 ** 2);
                                          // bound one-shot decompression (default: 1 GiB; 0 = unbounded)

try {
    await decompress(garbage);
} catch (e) {
    if (e instanceof CompressError) {
        console.log(e.algorithm, e.code, e.message);
    }
}
```

## Compression levels

Every algorithm accepts a `level` from **1 (fastest) to 10 (smallest)**. The binding maps that to each algorithm's native range internally — you don't need to know that Zstandard goes 1–22 or zlib goes 1–9. Defaults to `5`.

```ts
await compress(input, { level: 1 });   // fastest
await compress(input, { level: 10 });  // smallest
```

## Runtime support

Tested in CI on every release:

| Runtime              | Status |
|----------------------|--------|
| Node 20, 22          | ✅     |
| Bun                  | ✅     |
| Deno 2.x             | ✅     |
| Chromium / Firefox / WebKit (Playwright) | ✅ |

The package exposes the [`browser`](https://nodejs.org/api/packages.html#community-conditions-definitions), `node`, `deno`, and `bun` export conditions:

- Browser, Deno, and Bun consumers get a `fetch`-based loader → `WebAssembly.instantiateStreaming` (concurrent download + compile).
- Node consumers get a `fs.readFile`-based loader (Node's built-in `fetch` doesn't handle `file://`).

You don't need to configure anything — the right entry is picked automatically. The browser entry has zero references to `node:*` modules, so it bundles cleanly under Vite, esbuild, webpack 5, Rollup, etc.

## TypeScript

The package is TypeScript-first. `.d.ts` files ship alongside the JS for every subpath; no `@types/` package needed.

```ts
import { compress, type CompressOptions, type CompressError } from "compress-utils/zstd";

const opts: CompressOptions = { level: 7 };
const compressed: Uint8Array = await compress(input, opts);
```

## Performance notes

- The first call on a given subpath instantiates the `.wasm` module (~5–20 ms depending on size). Subsequent calls reuse the same instance.
- Streaming compresses/decompresses in 64 KB chunks internally — multi-GB inputs don't hold the whole result in memory.
- The JS layer copies bytes into and out of WebAssembly linear memory rather than handing out views, so memory growth inside the codec can never invalidate a `Uint8Array` you hold.
- Browsers, Deno, and Bun get `WebAssembly.instantiateStreaming` — bytes compile while they download. Node uses the buffered path because its `fetch` doesn't speak `file://`.

## Bundler notes

Per-algorithm imports + `sideEffects: false` mean modern bundlers tree-shake aggressively. A consumer that imports only `compress-utils/zstd` ships exactly one `.wasm` and only the zstd-bound TypeScript glue.

## License

MIT. See [LICENSE](https://github.com/dupontcyborg/compress-utils/blob/main/LICENSE).
