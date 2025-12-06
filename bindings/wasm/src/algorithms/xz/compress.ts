/**
 * XZ/LZMA One-shot Compression/Decompression
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
import { getXzModule } from './module.js';

const ALGORITHM = 'xz';

export async function compress(
  data: CompressInput,
  options?: CompressOptions
): Promise<Uint8Array> {
  const input = normalizeInput(data, options?.encoding);
  const level = getNativeLevel(options?.level, ALGORITHM);
  const wasm = await getXzModule();

  const inputPtr = wasm.malloc(input.length || 1);
  if (inputPtr === 0) {
    throw CompressError.wasmOom(ALGORITHM);
  }

  const outputLenPtr = wasm.malloc(4);
  if (outputLenPtr === 0) {
    wasm.free(inputPtr);
    throw CompressError.wasmOom(ALGORITHM);
  }

  try {
    if (input.length > 0) {
      copyToWasm(wasm.memory, input, inputPtr);
    }

    const outputPtr = wasm.compress(inputPtr, input.length, level, outputLenPtr);

    if (outputPtr === 0) {
      throw CompressError.compressionFailed(ALGORITHM);
    }

    try {
      const outputLen = readU32(wasm.memory, outputLenPtr);
      return copyFromWasm(wasm.memory, outputPtr, outputLen);
    } finally {
      wasm.free(outputPtr);
    }
  } finally {
    wasm.free(inputPtr);
    wasm.free(outputLenPtr);
  }
}

export async function decompress(data: DecompressInput): Promise<Uint8Array> {
  const input = normalizeDecompressInput(data);

  if (input.length === 0) {
    throw DecompressError.invalidInput(ALGORITHM, 'empty input');
  }

  const wasm = await getXzModule();

  const inputPtr = wasm.malloc(input.length);
  if (inputPtr === 0) {
    throw DecompressError.wasmOom(ALGORITHM);
  }

  const outputLenPtr = wasm.malloc(4);
  if (outputLenPtr === 0) {
    wasm.free(inputPtr);
    throw DecompressError.wasmOom(ALGORITHM);
  }

  try {
    copyToWasm(wasm.memory, input, inputPtr);

    const outputPtr = wasm.decompress(inputPtr, input.length, outputLenPtr);

    if (outputPtr === 0) {
      throw DecompressError.decompressionFailed(ALGORITHM);
    }

    try {
      const outputLen = readU32(wasm.memory, outputLenPtr);
      return copyFromWasm(wasm.memory, outputPtr, outputLen);
    } finally {
      wasm.free(outputPtr);
    }
  } finally {
    wasm.free(inputPtr);
    wasm.free(outputLenPtr);
  }
}

export async function decompressToString(
  data: DecompressInput,
  encoding: BufferEncoding = 'utf-8'
): Promise<string> {
  const decompressed = await decompress(data);

  if (encoding === 'utf-8' || encoding === 'utf8') {
    return new TextDecoder().decode(decompressed);
  }

  if (typeof Buffer !== 'undefined') {
    return Buffer.from(decompressed).toString(encoding);
  }

  throw new Error(
    `Encoding '${encoding}' is not supported in this environment. Use 'utf-8'.`
  );
}
