/**
 * LZ4 Compression Module
 *
 * LZ4 is an extremely fast compression algorithm with reasonable compression ratios.
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

export { preload, isLz4ModuleLoaded } from './module.js';

export type {
  CompressInput,
  CompressOptions,
  CompressionLevel,
  DecompressInput,
} from '../../core/index.js';

export { CompressError, DecompressError } from '../../core/index.js';
