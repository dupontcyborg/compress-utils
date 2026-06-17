# compress-utils

[![npm version](https://img.shields.io/npm/v/compress-utils.svg)](https://www.npmjs.com/package/compress-utils)
[![npm downloads](https://img.shields.io/npm/dm/compress-utils.svg)](https://www.npmjs.com/package/compress-utils)
[![types: built-in](https://img.shields.io/badge/types-built--in-blue.svg)](https://www.npmjs.com/package/compress-utils)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

Unified WebAssembly bindings for six compression algorithms: Brotli, bzip2, LZ4, XZ/LZMA, zlib, and Zstandard. Same API for every algorithm, in every JavaScript runtime, with tree-shakeable imports so bundlers ship only what you use.

## Installation

```bash
npm install compress-utils
# or: pnpm add / yarn add / bun add / deno add npm:compress-utils
```

### CDN (no bundler)

The package is published as ESM. You can pull it straight from a CDN — no install step, no build step.

```html
<script type="module">
    import { compress, decompress } from 
      "https://esm.sh/compress-utils/zstd";

    const out = await compress(
      new TextEncoder().encode("hello")
      );
</script>
```

| CDN        | Per-algo URL                                                          |
|------------|-----------------------------------------------------------------------|
| esm.sh     | `https://esm.sh/compress-utils/<algo>`                                |
| jsDelivr   | `https://cdn.jsdelivr.net/npm/compress-utils/dist/algorithms/<algo>/index.js` |
| unpkg      | `https://unpkg.com/compress-utils/dist/algorithms/<algo>/index.js`    |

## Quick start

Each algorithm is its own subpath import. Pick the one you need; the rest stay out of your bundle.

```ts
import { compress, decompress } from "compress-utils/zstd";

const input = new TextEncoder().encode("the quick brown fox " .repeat(50));

const compressed = await compress(input, { level: 9 });
const restored   = await decompress(compressed);
```

Available subpaths: `compress-utils/zstd`, `/brotli`, `/zlib`, `/bz2`, `/lz4`, `/xz`. Identical surface across all six. Each also has `/<algo>/decompress` and `/<algo>/compress` variants that ship only one direction for a much smaller `.wasm` — see [Bundle size](#bundle-size).

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

| Algorithm | Subpath                  | Wire format produced                       |
|-----------|--------------------------|--------------------------------------------|
| zlib      | `compress-utils/zlib`    | zlib wrapper (RFC 1950)                    |
| bzip2     | `compress-utils/bz2`     | bzip2 stream                               |
| XZ/LZMA   | `compress-utils/xz`      | XZ stream with CRC64                       |
| LZ4       | `compress-utils/lz4`     | LZ4 frame (compatible with `.lz4` files)   |
| Zstandard | `compress-utils/zstd`    | ZSTD frame with content size               |
| Brotli    | `compress-utils/brotli`  | raw Brotli stream                          |

Imports are independent — `import "compress-utils/zstd"` and `import "compress-utils/brotli"` pull in two separate `.wasm` modules, not a combined bundle. Files marked `"sideEffects": false` so unused exports are tree-shaken aggressively. For per-module `.wasm` sizes (and the smaller decode-only / encode-only builds), see **[Bundle size](#bundle-size)**.

## Bundle size

Importing `compress-utils/<algo>` gives you the **full** codec — compress *and* decompress. If you only need one direction (a browser that just *decompresses* fetched assets is the common case), import a directional subpath and the other half is dead-stripped from the `.wasm` entirely:

```ts
// decoder only, 90 KB
import { decompress } from "compress-utils/zstd/decompress"; 

// or, encoder only:
import { compress }   from "compress-utils/zstd/compress";
```

- **`/decompress`** exposes `decompress`, `createDecompressStream`, `decompressionStream`, `setMaxDecompressedSize`, `version`.
- **`/compress`** exposes `compress`, `createCompressStream`, `compressionStream`, `version`.
- The default subpath keeps **both**

Comparison of the gzipped sizes:

| algo   | full          | decompress-only       | compress-only         |
|--------|--------------:|----------------------:|----------------------:|
| zstd   | 128K  | 36K    | 106K           |
| brotli | 169K  | 84K    | 154K           |
| lz4    | 39K   | 15K    | 35K             |
| bz2    | 33K   | 22K    | 25K             |
| xz     | 62K   | 43K    | 51K            |
| zlib   | 33K   | 25K    | 26K             |

The `.wasm` is the bulk of what ships; the JS glue is a few KB, shared, and tree-shaken to the methods you import.

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
| Node 20, 22, 24      | ✅     |
| Bun                  | ✅     |
| Deno 2.x             | ✅     |
| Chromium / Firefox / WebKit (Playwright) | ✅ |

## TypeScript

The package is TypeScript-first. `.d.ts` files ship alongside the JS for every subpath; no `@types/` package needed.

```ts
import { compress, type CompressOptions, type CompressError } from "compress-utils/zstd";

const opts: CompressOptions = { level: 7 };
const compressed: Uint8Array = await compress(input, opts);
```

## License

MIT. See [LICENSE](https://github.com/dupontcyborg/compress-utils/blob/main/LICENSE).

---

Built by [Nico Dupont](https://nico.codes).
