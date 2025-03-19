# Compression Utils WebAssembly Bindings

TypeScript/JavaScript bindings for the compression-utils library via WebAssembly.

## Features

- Supports multiple compression algorithms (Brotli, Zlib, Zstd, XZ/LZMA)
- Provides both functional and object-oriented APIs
- Works in both browsers and Node.js
- Fully typed with TypeScript definitions
- Uses WebAssembly for near-native performance
- Tiny footprint (minimal size overhead)

## Installation

```bash
npm install compression-utils
```

## Usage

### Functional API

```typescript
import { compress, decompress, Algorithm } from 'compression-utils';

// Compress data
const data = new TextEncoder().encode('Hello, world!');
const compressed = await compress(data, 'brotli', 3);

// Decompress data
const decompressed = await decompress(compressed, 'brotli');
console.log(new TextDecoder().decode(decompressed)); // 'Hello, world!'

// Using algorithm enum
const compressedZstd = await compress(data, Algorithm.ZSTD, 10);
const decompressedZstd = await decompress(compressedZstd, Algorithm.ZSTD);
```

### Object-Oriented API

```typescript
import { Compressor, Algorithm } from 'compression-utils';

// Create a compressor instance
const compressor = new Compressor('zlib');

// Compress data
const data = new TextEncoder().encode('Hello, world!');
const compressed = await compressor.compress(data, 3);

// Decompress data
const decompressed = await compressor.decompress(compressed);
console.log(new TextDecoder().decode(decompressed)); // 'Hello, world!'
```

## Supported Algorithms

- `brotli`: High-to-very-high compression rates
- `zlib`: General-purpose, compatible with gzip
- `zstd`: High-speed, high-ratio compression
- `xz`/`lzma`: Very-high compression ratio

## Compression Levels

The library standardizes compression levels across all algorithms between `1-10`:

- `1`: Fastest compression, larger file size
- `3`: Balanced compression (default)
- `10`: Best compression, slowest

## Browser Usage

When using with bundlers like webpack, Rollup, or Vite, make sure to configure them correctly to handle WebAssembly files.

### Webpack Example

```javascript
// webpack.config.js
module.exports = {
  // ...
  experiments: {
    asyncWebAssembly: true,
  },
  // ...
};
```

## Building from Source

To build the WebAssembly bindings from source, you need:

1. CMake 3.17+
2. Emscripten SDK (emsdk)
3. A C++ compiler

```bash
# Clone the repository
git clone https://github.com/yourusername/compression-utils.git
cd compression-utils

# Activate Emscripten
source /path/to/emsdk/emsdk_env.sh

# Build with CMake
mkdir build && cd build
emcmake cmake .. -DBUILD_WASM_BINDINGS=ON
emmake make

# The WebAssembly bindings will be in dist/wasm
```

## License

MIT