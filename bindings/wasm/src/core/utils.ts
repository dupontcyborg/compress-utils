import type { CompressInput, DecompressInput } from './types.js';

/**
 * Default buffer encoding for string inputs.
 */
export const DEFAULT_ENCODING: BufferEncoding = 'utf-8';

/**
 * Default buffer size for streaming operations (64KB).
 */
export const DEFAULT_BUFFER_SIZE = 64 * 1024;

/**
 * Minimum buffer size for decompression (16KB).
 */
export const MIN_DECOMPRESS_BUFFER_SIZE = 16 * 1024;

/**
 * Normalizes any valid compression input to a Uint8Array.
 *
 * @param input - Input data (Uint8Array, ArrayBuffer, or string)
 * @param encoding - Text encoding for string inputs
 * @returns Normalized Uint8Array
 */
export function normalizeInput(
  input: CompressInput,
  encoding: BufferEncoding = DEFAULT_ENCODING
): Uint8Array {
  if (input instanceof Uint8Array) {
    return input;
  }

  if (input instanceof ArrayBuffer) {
    return new Uint8Array(input);
  }

  if (typeof input === 'string') {
    return encodeString(input, encoding);
  }

  // TypeScript should prevent this, but just in case
  throw new TypeError(
    `Invalid input type: expected Uint8Array, ArrayBuffer, or string, got ${typeof input}`
  );
}

/**
 * Normalizes decompression input to a Uint8Array.
 *
 * @param input - Compressed data (Uint8Array or ArrayBuffer)
 * @returns Normalized Uint8Array
 */
export function normalizeDecompressInput(input: DecompressInput): Uint8Array {
  if (input instanceof Uint8Array) {
    return input;
  }

  if (input instanceof ArrayBuffer) {
    return new Uint8Array(input);
  }

  throw new TypeError(
    `Invalid input type: expected Uint8Array or ArrayBuffer, got ${typeof input}`
  );
}

/**
 * Encodes a string to a Uint8Array using the specified encoding.
 *
 * @param str - String to encode
 * @param encoding - Text encoding
 * @returns Encoded bytes
 */
export function encodeString(
  str: string,
  encoding: BufferEncoding = DEFAULT_ENCODING
): Uint8Array {
  // Use TextEncoder for UTF-8 (most common case, available everywhere)
  if (encoding === 'utf-8' || encoding === 'utf8') {
    return new TextEncoder().encode(str);
  }

  // For other encodings, check if we're in Node.js
  if (typeof Buffer !== 'undefined') {
    return new Uint8Array(Buffer.from(str, encoding));
  }

  // In browser without Buffer, only UTF-8 is supported natively
  throw new Error(
    `Encoding '${encoding}' is not supported in this environment. ` +
      `Use 'utf-8' or include a polyfill for Buffer.`
  );
}

/**
 * Decodes a Uint8Array to a string using the specified encoding.
 *
 * @param data - Bytes to decode
 * @param encoding - Text encoding
 * @returns Decoded string
 */
export function decodeString(
  data: Uint8Array,
  encoding: BufferEncoding = DEFAULT_ENCODING
): string {
  // Use TextDecoder for UTF-8 (most common case, available everywhere)
  if (encoding === 'utf-8' || encoding === 'utf8') {
    return new TextDecoder().decode(data);
  }

  // For other encodings, check if we're in Node.js
  if (typeof Buffer !== 'undefined') {
    return Buffer.from(data).toString(encoding);
  }

  // In browser without Buffer, only UTF-8 is supported natively
  throw new Error(
    `Encoding '${encoding}' is not supported in this environment. ` +
      `Use 'utf-8' or include a polyfill for Buffer.`
  );
}

/**
 * Copies data from a Uint8Array into WASM linear memory.
 *
 * @param memory - WASM memory instance
 * @param data - Data to copy
 * @param ptr - Destination pointer in WASM memory
 */
export function copyToWasm(
  memory: WebAssembly.Memory,
  data: Uint8Array,
  ptr: number
): void {
  const view = new Uint8Array(memory.buffer);
  view.set(data, ptr);
}

/**
 * Copies data from WASM linear memory to a new Uint8Array.
 *
 * @param memory - WASM memory instance
 * @param ptr - Source pointer in WASM memory
 * @param length - Number of bytes to copy
 * @returns New Uint8Array with copied data
 */
export function copyFromWasm(
  memory: WebAssembly.Memory,
  ptr: number,
  length: number
): Uint8Array {
  const view = new Uint8Array(memory.buffer, ptr, length);
  // Create a copy since the WASM memory may be reused
  return new Uint8Array(view);
}

/**
 * Reads a 32-bit unsigned integer from WASM memory.
 *
 * @param memory - WASM memory instance
 * @param ptr - Pointer to the integer
 * @returns The integer value
 */
export function readU32(memory: WebAssembly.Memory, ptr: number): number {
  const view = new DataView(memory.buffer);
  return view.getUint32(ptr, true); // Little-endian
}

/**
 * Writes a 32-bit unsigned integer to WASM memory.
 *
 * @param memory - WASM memory instance
 * @param ptr - Pointer to write to
 * @param value - The integer value
 */
export function writeU32(
  memory: WebAssembly.Memory,
  ptr: number,
  value: number
): void {
  const view = new DataView(memory.buffer);
  view.setUint32(ptr, value, true); // Little-endian
}

/**
 * Concatenates multiple Uint8Arrays into a single array.
 *
 * @param arrays - Arrays to concatenate
 * @returns Concatenated array
 */
export function concatUint8Arrays(arrays: Uint8Array[]): Uint8Array {
  const totalLength = arrays.reduce((sum, arr) => sum + arr.length, 0);
  const result = new Uint8Array(totalLength);

  let offset = 0;
  for (const arr of arrays) {
    result.set(arr, offset);
    offset += arr.length;
  }

  return result;
}

/**
 * Estimates a reasonable output buffer size for compression.
 *
 * @param inputSize - Size of input data
 * @returns Estimated buffer size
 */
export function estimateCompressBufferSize(inputSize: number): number {
  // Compression typically reduces size, but worst case can be slightly larger
  // Add some overhead for headers and worst-case expansion
  return Math.max(inputSize + 1024, Math.ceil(inputSize * 1.1));
}

/**
 * Estimates a reasonable output buffer size for decompression.
 *
 * @param compressedSize - Size of compressed data
 * @returns Estimated buffer size
 */
export function estimateDecompressBufferSize(compressedSize: number): number {
  // Start with 4x the compressed size as initial estimate
  // This will be grown if needed
  return Math.max(
    MIN_DECOMPRESS_BUFFER_SIZE,
    Math.ceil(compressedSize * 4)
  );
}
