# compress-utils for JavaScript: advanced guide

Details that the [package README](../../bindings/wasm/README.md) leaves out: raw blocks, stopping at the end of a stream, preloading and synchronous calls, bundle size, CDN use and Node source imports.

## Raw blocks and output capacity

Raw block formats such as those inside Parquet and ORC pages don't record their decompressed size. Pass `expectedSize`, an upper bound on the output, and the result holds only the bytes actually decoded:

```ts
import { decompress } from "compress-utils/lz4_raw";

const page = await decompress(block, { expectedSize: uncompressedPageSize });
```

Without `expectedSize`, `lz4_raw` blocks are decoded with the incremental stream decoder, which is much slower, so pass it when you know the size. Every other algorithm accepts it too, which skips a sizing pass and decodes straight into a buffer of that size.

## Stopping at the end of a stream

By default a decoder decodes concatenated frames (zstd, LZ4, gzip, xz) as one stream. To read one compressed stream followed by other data, such as the next message on a socket or a container's own bytes, create the decoder with `{ singleStream: true }`. It stops at the end of the first frame, sets `eof`, and returns the input bytes written after the end in `unusedData`:

```ts
import { createDecompressStream } from "compress-utils/zstd";

using ds = await createDecompressStream({ singleStream: true });
const message = ds.write(received);  // decodes up to the end of the frame
if (ds.eof) {
    const rest = ds.unusedData;      // bytes after the frame, e.g. the next message
}
```

Once `eof` is true, writing more data throws `CompressError` with code `10` (stream finished) and leaves `unusedData` unchanged; empty writes and `finish()` return empty output. Leading zstd/LZ4 skippable frames are skipped. Snappy emits nothing until its block is complete. Raw LZ4 (which has no end marker) and LZO don't support it and throw `CompressError` with code `5`. `decompressionStream()` doesn't take the option: a `TransformStream` has no way to hand back the bytes after the end, so use `createDecompressStream` for this.

If `using` isn't available in your toolchain, call `cs.destroy()` explicitly, or rely on the GC backstop: a `FinalizationRegistry` frees the C-side handle eventually.

## Preloading and synchronous calls

Each algorithm and direction-specific subpath exports `preload()`. It loads and initializes that subpath's WASM module once, and concurrent calls share initialization. A failed initialization can be retried. No codec work is performed by preload.

```ts
import { preload, compressSync, decompressSync } from "compress-utils/zlib";
await preload();
const compressed = compressSync(new TextEncoder().encode("hello"));
const restored = decompressSync(compressed);
```

`compressSync`, `decompressSync`, `createCompressStreamSync`, and `createDecompressStreamSync` are available for the directions exposed by the subpath. They throw with a `preload()` instruction before initialization completes. Existing asynchronous APIs initialize automatically and also enable subsequent synchronous calls. Initializing one subpath does not initialize another, because encoder and decoder subpaths use different WASM modules.

## Bundle size

Importing `compress-utils/<algo>` gives you the **full** codec, compress *and* decompress. If you only need one direction (a browser that just *decompresses* fetched assets is the common case), import a directional subpath and the other half is dead-stripped from the `.wasm` entirely:

```ts
// decoder only
import { decompress } from "compress-utils/zstd/decompress";

// or, encoder only:
import { compress } from "compress-utils/zstd/compress";
```

- **`/decompress`** exposes `decompress`, `createDecompressStream`, `decompressionStream`, `setMaxDecompressedSize` and `version`.
- **`/compress`** exposes `compress`, `createCompressStream`, `compressionStream` and `version`.
- The default subpath keeps **both**.

Module sizes change with every codec upgrade and build-flag change, so they are not quoted here; `benchmarks/size/js_size.py` measures the sizes of a build, and `docs/wasm-size.md` records what was done to reduce them.

The `.wasm` is the bulk of what ships. The JS glue is a few KB, shared, and tree-shaken to the methods you import.

## CDN (no bundler)

The package is published as ESM. You can pull it straight from a CDN, with no install step and no build step.

```html
<script type="module">
  import { compress, decompress } from "https://esm.sh/compress-utils/zstd";

  const out = await compress(new TextEncoder().encode("hello"));
</script>
```

| CDN      | Per-algo URL                                                                  |
|----------|-------------------------------------------------------------------------------|
| esm.sh   | `https://esm.sh/compress-utils/<algo>`                                        |
| jsDelivr | `https://cdn.jsdelivr.net/npm/compress-utils/dist/algorithms/<algo>/index.js` |
| unpkg    | `https://unpkg.com/compress-utils/dist/algorithms/<algo>/index.js`            |

## Opt in to Node.js WASM source imports

Node.js 24.5 and later can load the packaged `.wasm` files through its experimental source-phase module loader. To opt in, start Node with:

```sh
node --conditions=compress-utils-wasm-source app.mjs
```

This compiles the WASM through Node's ESM loader and still instantiates it with the library's WASI imports. The default Node resolver continues to read WASM bytes from disk. Source-phase imports are experimental in Node.js and this condition should only be enabled on runtimes that support them.
