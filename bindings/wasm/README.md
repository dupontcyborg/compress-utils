# compress-utils WASM Bindings

High-performance compression library for JavaScript/TypeScript with WebAssembly backends.

## Features

- **Tree-shakeable**: Only bundle the algorithms you use
- **Async API**: Non-blocking compression/decompression
- **Streaming support**: Web Streams API (TransformStream)
- **Universal**: Works in browsers, Node.js, Deno, and Bun
- **TypeScript-first**: Full type safety with strict mode
- **6 algorithms**: zstd, brotli, zlib, bz2, lz4, xz

## Installation

```bash
npm install compress-utils
```

## Usage

### One-shot Compression

```typescript
import { compress, decompress } from 'compress-utils/zstd';

// Compress data
const compressed = await compress('Hello, World!');

// Decompress data
const decompressed = await decompress(compressed);

// With options
const compressed = await compress(data, { level: 'best' });
const compressed = await compress(data, { level: 7 });
```

### Streaming

```typescript
import { CompressStream, DecompressStream } from 'compress-utils/zstd';

// Compress a stream
const compressor = new CompressStream({ level: 5 });
const compressed = await fetch('/large-file')
  .then(r => r.body!.pipeThrough(compressor));

// Decompress a stream
const decompressor = new DecompressStream();
const decompressed = compressedStream.pipeThrough(decompressor);
```

### Using Different Algorithms

```typescript
// Each algorithm is a separate import for tree-shaking
import { compress as zstdCompress } from 'compress-utils/zstd';
import { compress as brotliCompress } from 'compress-utils/brotli';
import { compress as zlibCompress } from 'compress-utils/zlib';
import { compress as bz2Compress } from 'compress-utils/bz2';
import { compress as lz4Compress } from 'compress-utils/lz4';
import { compress as xzCompress } from 'compress-utils/xz';
```

### Preloading

```typescript
import { preload } from 'compress-utils/zstd';

// Preload during app initialization to avoid first-call latency
await preload();

// Later, compression is instant
const compressed = await compress(data);
```

## API Reference

### Compression Levels

All algorithms use a unified 1-10 scale:

| Level | Description |
|-------|-------------|
| 1 | Fastest, larger output |
| 5 | Balanced (default) |
| 10 | Best compression, slower |

Named presets are also supported:
- `'fast'` → Level 1
- `'balanced'` → Level 5
- `'best'` → Level 10

### Input Types

Compression accepts:
- `Uint8Array`
- `ArrayBuffer`
- `string` (UTF-8 encoded)

Decompression accepts:
- `Uint8Array`
- `ArrayBuffer`

### Output Type

All operations return `Uint8Array`.

### Errors

```typescript
import { CompressError, DecompressError } from 'compress-utils/zstd';

try {
  await decompress(data);
} catch (e) {
  if (e instanceof DecompressError) {
    console.log(e.code); // 'CORRUPTED_DATA', 'UNEXPECTED_EOF', etc.
    console.log(e.algorithm); // 'zstd'
  }
}
```

## Building from Source

### Prerequisites

- Node.js 18+
- Emscripten SDK (for WASM compilation)

### Build Steps

```bash
# Install dependencies
npm install

# Build WASM modules (requires Emscripten)
npm run build:wasm

# Build TypeScript
npm run build:ts

# Run tests
npm test
```

### Development

```bash
# Type check
npm run typecheck

# Watch mode for tests
npm run test:watch
```

## Algorithm Comparison

| Algorithm | Speed | Ratio | Best For |
|-----------|-------|-------|----------|
| **lz4** | ⚡⚡⚡⚡⚡ | ★★☆☆☆ | Real-time, speed critical |
| **zstd** | ⚡⚡⚡⚡ | ★★★★☆ | General purpose (recommended) |
| **brotli** | ⚡⚡⚡ | ★★★★☆ | Web content |
| **zlib** | ⚡⚡⚡ | ★★★☆☆ | Compatibility |
| **bz2** | ⚡⚡ | ★★★★☆ | High compression |
| **xz** | ⚡ | ★★★★★ | Maximum compression |

## License

MIT
