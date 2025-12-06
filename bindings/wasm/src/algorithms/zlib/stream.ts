/**
 * Zlib Streaming Compression/Decompression
 */

import {
  CompressError,
  DecompressError,
  getNativeLevel,
  copyToWasm,
  copyFromWasm,
  DEFAULT_BUFFER_SIZE,
  type CompressionLevel,
  type WasmModule,
} from '../../core/index.js';
import { getZlibModule } from './module.js';

const ALGORITHM = 'zlib';

export interface CompressStreamOptions {
  readonly level?: CompressionLevel;
  readonly bufferSize?: number;
}

export interface DecompressStreamOptions {
  readonly bufferSize?: number;
}

export class CompressStream extends TransformStream<Uint8Array, Uint8Array> {
  constructor(options?: CompressStreamOptions) {
    const level = getNativeLevel(options?.level, ALGORITHM);
    const bufferSize = options?.bufferSize ?? DEFAULT_BUFFER_SIZE;

    let wasm: WasmModule | null = null;
    let ctx: number = 0;
    let outputPtr: number = 0;

    super({
      async start() {
        wasm = await getZlibModule();
        ctx = wasm.stream_compress_create(level);
        if (ctx === 0) {
          throw CompressError.wasmInitFailed(ALGORITHM);
        }
        outputPtr = wasm.malloc(bufferSize);
        if (outputPtr === 0) {
          wasm.stream_compress_destroy(ctx);
          throw CompressError.wasmOom(ALGORITHM);
        }
      },

      async transform(chunk, controller) {
        if (!wasm || ctx === 0) {
          throw CompressError.compressionFailed(ALGORITHM, 'stream not initialized');
        }

        const inputPtr = wasm.malloc(chunk.length);
        if (inputPtr === 0) {
          throw CompressError.wasmOom(ALGORITHM);
        }

        try {
          copyToWasm(wasm.memory, chunk, inputPtr);

          const result = wasm.stream_compress_write(
            ctx,
            inputPtr,
            chunk.length,
            outputPtr,
            bufferSize
          );

          if (result < 0) {
            throw CompressError.compressionFailed(ALGORITHM);
          }

          if (result > 0) {
            controller.enqueue(copyFromWasm(wasm.memory, outputPtr, result));
          }
        } finally {
          wasm.free(inputPtr);
        }
      },

      async flush(controller) {
        if (!wasm || ctx === 0) return;

        try {
          let finished = false;
          while (!finished) {
            const result = wasm.stream_compress_finish(ctx, outputPtr, bufferSize);
            if (result < 0) {
              throw CompressError.compressionFailed(ALGORITHM, 'failed to finish');
            }
            if (result > 0) {
              controller.enqueue(copyFromWasm(wasm.memory, outputPtr, result));
            }
            if (result === 0) finished = true;
          }
        } finally {
          if (outputPtr !== 0) wasm.free(outputPtr);
          if (ctx !== 0) wasm.stream_compress_destroy(ctx);
        }
      },
    });
  }
}

export class DecompressStream extends TransformStream<Uint8Array, Uint8Array> {
  constructor(options?: DecompressStreamOptions) {
    const bufferSize = options?.bufferSize ?? DEFAULT_BUFFER_SIZE;

    let wasm: WasmModule | null = null;
    let ctx: number = 0;
    let outputPtr: number = 0;

    super({
      async start() {
        wasm = await getZlibModule();
        ctx = wasm.stream_decompress_create();
        if (ctx === 0) {
          throw DecompressError.wasmInitFailed(ALGORITHM);
        }
        outputPtr = wasm.malloc(bufferSize);
        if (outputPtr === 0) {
          wasm.stream_decompress_destroy(ctx);
          throw DecompressError.wasmOom(ALGORITHM);
        }
      },

      async transform(chunk, controller) {
        if (!wasm || ctx === 0) {
          throw DecompressError.decompressionFailed(ALGORITHM, 'stream not initialized');
        }

        const inputPtr = wasm.malloc(chunk.length);
        if (inputPtr === 0) {
          throw DecompressError.wasmOom(ALGORITHM);
        }

        try {
          copyToWasm(wasm.memory, chunk, inputPtr);

          const result = wasm.stream_decompress_write(
            ctx,
            inputPtr,
            chunk.length,
            outputPtr,
            bufferSize
          );

          if (result < 0) {
            throw DecompressError.decompressionFailed(ALGORITHM);
          }

          if (result > 0) {
            controller.enqueue(copyFromWasm(wasm.memory, outputPtr, result));
          }
        } finally {
          wasm.free(inputPtr);
        }
      },

      async flush(controller) {
        if (!wasm || ctx === 0) return;

        try {
          const finished = wasm.stream_decompress_finish(ctx);
          if (finished < 0) {
            throw DecompressError.decompressionFailed(ALGORITHM, 'unexpected end');
          }
        } finally {
          if (outputPtr !== 0) wasm.free(outputPtr);
          if (ctx !== 0) wasm.stream_decompress_destroy(ctx);
        }
      },
    });
  }
}
