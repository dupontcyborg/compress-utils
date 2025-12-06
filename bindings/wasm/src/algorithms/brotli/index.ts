/**
 * Brotli Compression Module
 *
 * Brotli is a compression algorithm developed by Google, optimized for web content.
 *
 * @packageDocumentation
 */

export { compress, decompress, decompressToString } from './compress.js';

export {
  CompressStream,
  DecompressStream,
  type CompressStreamOptions,
  type DecompressStreamOptions,
} from './stream.js';

export { preload, isBrotliModuleLoaded } from './module.js';

export type {
  CompressInput,
  CompressOptions,
  CompressionLevel,
  DecompressInput,
} from '../../core/index.js';

export { CompressError, DecompressError } from '../../core/index.js';
