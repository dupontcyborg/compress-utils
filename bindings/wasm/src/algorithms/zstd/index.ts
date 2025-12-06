/**
 * ZSTD Compression Module
 *
 * ZSTD (Zstandard) is a fast compression algorithm developed by Facebook.
 * It provides an excellent balance of compression ratio and speed.
 *
 * @example
 * ```typescript
 * import { compress, decompress } from 'compress-utils/zstd';
 *
 * // One-shot compression
 * const data = new TextEncoder().encode("Hello, World!");
 * const compressed = await compress(data, { level: 5 });
 * const decompressed = await decompress(compressed);
 *
 * // String compression
 * const text = "Hello, World!";
 * const compressed = await compress(text, { level: 'fast' });
 *
 * // Streaming
 * import { CompressStream, DecompressStream } from 'compress-utils/zstd';
 *
 * const compressor = new CompressStream({ level: 7 });
 * const decompressor = new DecompressStream();
 *
 * inputStream
 *   .pipeThrough(compressor)
 *   .pipeThrough(decompressor)
 *   .pipeTo(outputStream);
 * ```
 *
 * @packageDocumentation
 */

// One-shot API
export { compress, decompress, decompressToString } from './compress.js';

// Streaming API
export {
  CompressStream,
  DecompressStream,
  type CompressStreamOptions,
  type DecompressStreamOptions,
} from './stream.js';

// Module utilities
export { preload, isZstdModuleLoaded } from './module.js';

// Re-export common types
export type {
  CompressInput,
  CompressOptions,
  CompressionLevel,
  DecompressInput,
} from '../../core/index.js';

// Re-export errors
export { CompressError, DecompressError } from '../../core/index.js';
