// Types
export type {
  Algorithm,
  CompressInput,
  CompressOptions,
  CompressionLevel,
  DecompressInput,
  DecompressOptions,
  WasmModule,
} from './types.js';

export { StreamState } from './types.js';

// Errors
export {
  CompressError,
  DecompressError,
  type CompressErrorCode,
  type DecompressErrorCode,
} from './errors.js';

// Levels
export {
  DEFAULT_LEVEL,
  getNativeLevel,
  mapLevelToAlgorithm,
  normalizeLevel,
} from './levels.js';

// Utils
export {
  concatUint8Arrays,
  copyFromWasm,
  copyToWasm,
  decodeString,
  DEFAULT_BUFFER_SIZE,
  DEFAULT_ENCODING,
  encodeString,
  estimateCompressBufferSize,
  estimateDecompressBufferSize,
  MIN_DECOMPRESS_BUFFER_SIZE,
  normalizeDecompressInput,
  normalizeInput,
  readU32,
  writeU32,
} from './utils.js';

// Loader
export {
  clearModuleCache,
  decodeBase64,
  getModule,
  isModuleLoaded,
  preloadModule,
} from './loader.js';
