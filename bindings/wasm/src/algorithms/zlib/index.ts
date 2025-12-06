/**
 * Zlib Compression Module
 *
 * Zlib is a widely-used, general-purpose compression library.
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

export { preload, isZlibModuleLoaded } from './module.js';

export type {
  CompressInput,
  CompressOptions,
  CompressionLevel,
  DecompressInput,
} from '../../core/index.js';

export { CompressError, DecompressError } from '../../core/index.js';
