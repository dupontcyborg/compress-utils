/**
 * ZSTD One-shot Compression/Decompression
 *
 * Provides simple async functions for compressing and decompressing
 * data in a single operation.
 */

import {
  CompressError,
  DecompressError,
  getNativeLevel,
  normalizeInput,
  normalizeDecompressInput,
  copyToWasm,
  copyFromWasm,
  readU32,
  type CompressInput,
  type CompressOptions,
  type DecompressInput,
} from '../../core/index.js';
import { getZstdModule } from './module.js';

const ALGORITHM = 'zstd';

/**
 * Compresses data using the ZSTD algorithm.
 *
 * @param data - Data to compress (Uint8Array, ArrayBuffer, or string)
 * @param options - Compression options
 * @returns Promise resolving to compressed data
 * @throws CompressError if compression fails
 *
 * @example
 * ```typescript
 * import { compress } from 'compress-utils/zstd';
 *
 * // Compress binary data
 * const compressed = await compress(new Uint8Array([1, 2, 3, 4, 5]));
 *
 * // Compress string with options
 * const text = "Hello, World!";
 * const compressed = await compress(text, { level: 'best' });
 *
 * // Compress with numeric level
 * const compressed = await compress(data, { level: 7 });
 * ```
 */
export async function compress(
  data: CompressInput,
  options?: CompressOptions
): Promise<Uint8Array> {
  // Normalize input
  const input = normalizeInput(data, options?.encoding);

  if (input.length === 0) {
    // Return empty compressed frame for empty input
    // ZSTD can handle empty input, but we should be explicit
    return compressEmpty();
  }

  // Get native compression level
  const level = getNativeLevel(options?.level, ALGORITHM);

  // Get WASM module
  const wasm = await getZstdModule();

  // Allocate memory for input
  const inputPtr = wasm.malloc(input.length);
  if (inputPtr === 0) {
    throw CompressError.wasmOom(ALGORITHM);
  }

  // Allocate memory for output length (4 bytes for size_t on wasm32)
  const outputLenPtr = wasm.malloc(4);
  if (outputLenPtr === 0) {
    wasm.free(inputPtr);
    throw CompressError.wasmOom(ALGORITHM);
  }

  try {
    // Copy input data to WASM memory
    copyToWasm(wasm.memory, input, inputPtr);

    // Call compression function
    const outputPtr = wasm.compress(inputPtr, input.length, level, outputLenPtr);

    if (outputPtr === 0) {
      throw CompressError.compressionFailed(ALGORITHM);
    }

    try {
      // Read output length
      const outputLen = readU32(wasm.memory, outputLenPtr);

      // Copy output data from WASM memory
      const output = copyFromWasm(wasm.memory, outputPtr, outputLen);

      return output;
    } finally {
      // Free output buffer allocated by WASM
      wasm.free(outputPtr);
    }
  } finally {
    // Free input and length buffers
    wasm.free(inputPtr);
    wasm.free(outputLenPtr);
  }
}

/**
 * Decompresses ZSTD-compressed data.
 *
 * @param data - Compressed data (Uint8Array or ArrayBuffer)
 * @returns Promise resolving to decompressed data
 * @throws DecompressError if decompression fails
 *
 * @example
 * ```typescript
 * import { compress, decompress } from 'compress-utils/zstd';
 *
 * // Roundtrip
 * const original = new TextEncoder().encode("Hello, World!");
 * const compressed = await compress(original);
 * const restored = await decompress(compressed);
 *
 * // restored deep equals original
 * ```
 */
export async function decompress(data: DecompressInput): Promise<Uint8Array> {
  // Normalize input
  const input = normalizeDecompressInput(data);

  if (input.length === 0) {
    throw DecompressError.invalidInput(ALGORITHM, 'empty input');
  }

  // Get WASM module
  const wasm = await getZstdModule();

  // Allocate memory for input
  const inputPtr = wasm.malloc(input.length);
  if (inputPtr === 0) {
    throw DecompressError.wasmOom(ALGORITHM);
  }

  // Allocate memory for output length
  const outputLenPtr = wasm.malloc(4);
  if (outputLenPtr === 0) {
    wasm.free(inputPtr);
    throw DecompressError.wasmOom(ALGORITHM);
  }

  try {
    // Copy input data to WASM memory
    copyToWasm(wasm.memory, input, inputPtr);

    // Call decompression function
    const outputPtr = wasm.decompress(inputPtr, input.length, outputLenPtr);

    if (outputPtr === 0) {
      throw DecompressError.decompressionFailed(ALGORITHM);
    }

    try {
      // Read output length
      const outputLen = readU32(wasm.memory, outputLenPtr);

      // Copy output data from WASM memory
      const output = copyFromWasm(wasm.memory, outputPtr, outputLen);

      return output;
    } finally {
      // Free output buffer allocated by WASM
      wasm.free(outputPtr);
    }
  } finally {
    // Free input and length buffers
    wasm.free(inputPtr);
    wasm.free(outputLenPtr);
  }
}

/**
 * Decompresses ZSTD-compressed data and decodes it as a string.
 *
 * @param data - Compressed data
 * @param encoding - Text encoding (default: 'utf-8')
 * @returns Promise resolving to decompressed string
 * @throws DecompressError if decompression fails
 *
 * @example
 * ```typescript
 * import { compress, decompressToString } from 'compress-utils/zstd';
 *
 * const compressed = await compress("Hello, World!");
 * const text = await decompressToString(compressed);
 * console.log(text); // "Hello, World!"
 * ```
 */
export async function decompressToString(
  data: DecompressInput,
  encoding: BufferEncoding = 'utf-8'
): Promise<string> {
  const decompressed = await decompress(data);

  if (encoding === 'utf-8' || encoding === 'utf8') {
    return new TextDecoder().decode(decompressed);
  }

  // For other encodings, use Node.js Buffer if available
  if (typeof Buffer !== 'undefined') {
    return Buffer.from(decompressed).toString(encoding);
  }

  throw new Error(
    `Encoding '${encoding}' is not supported in this environment. Use 'utf-8'.`
  );
}

/**
 * Compresses empty input to a valid ZSTD frame.
 * This is a minimal valid ZSTD frame representing empty content.
 */
async function compressEmpty(): Promise<Uint8Array> {
  // Instead of hardcoding, compress an empty buffer through WASM
  // This ensures compatibility with the ZSTD version being used
  const wasm = await getZstdModule();

  const outputLenPtr = wasm.malloc(4);
  if (outputLenPtr === 0) {
    throw CompressError.wasmOom(ALGORITHM);
  }

  try {
    // Compress with null input and 0 length
    // We need a valid pointer, so allocate 1 byte
    const dummyPtr = wasm.malloc(1);
    if (dummyPtr === 0) {
      throw CompressError.wasmOom(ALGORITHM);
    }

    try {
      const outputPtr = wasm.compress(dummyPtr, 0, 3, outputLenPtr);
      if (outputPtr === 0) {
        throw CompressError.compressionFailed(ALGORITHM, 'empty input');
      }

      try {
        const outputLen = readU32(wasm.memory, outputLenPtr);
        return copyFromWasm(wasm.memory, outputPtr, outputLen);
      } finally {
        wasm.free(outputPtr);
      }
    } finally {
      wasm.free(dummyPtr);
    }
  } finally {
    wasm.free(outputLenPtr);
  }
}
