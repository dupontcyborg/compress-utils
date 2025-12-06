/**
 * XZ/LZMA Compression Module
 *
 * XZ uses the LZMA2 algorithm to achieve very high compression ratios.
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

export { preload, isXzModuleLoaded } from './module.js';

export type {
  CompressInput,
  CompressOptions,
  CompressionLevel,
  DecompressInput,
} from '../../core/index.js';

export { CompressError, DecompressError } from '../../core/index.js';
