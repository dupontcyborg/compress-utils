/**
 * Bzip2 Compression Module
 *
 * Bzip2 is a high-compression algorithm that achieves excellent compression ratios.
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

export { preload, isBz2ModuleLoaded } from './module.js';

export type {
  CompressInput,
  CompressOptions,
  CompressionLevel,
  DecompressInput,
} from '../../core/index.js';

export { CompressError, DecompressError } from '../../core/index.js';
