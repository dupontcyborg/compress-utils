/**
 * ZSTD Streaming Compression/Decompression
 *
 * Provides Web Streams API (TransformStream) for streaming compression
 * and decompression of data.
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
import { getZstdModule } from './module.js';

const ALGORITHM = 'zstd';

/**
 * Options for streaming compression.
 */
export interface CompressStreamOptions {
  /**
   * Compression level (1-10 or preset).
   * @default 5
   */
  readonly level?: CompressionLevel;

  /**
   * Size of the internal buffer in bytes.
   * @default 65536 (64KB)
   */
  readonly bufferSize?: number;
}

/**
 * Options for streaming decompression.
 */
export interface DecompressStreamOptions {
  /**
   * Size of the internal buffer in bytes.
   * @default 65536 (64KB)
   */
  readonly bufferSize?: number;
}

/**
 * A TransformStream that compresses data using ZSTD.
 *
 * @example
 * ```typescript
 * import { CompressStream } from 'compress-utils/zstd';
 *
 * // Create a compression stream
 * const compressor = new CompressStream({ level: 5 });
 *
 * // Use with fetch and Response
 * const response = await fetch('/large-file.json');
 * const compressed = response.body!
 *   .pipeThrough(compressor)
 *   .pipeTo(writableStream);
 *
 * // Or collect all chunks
 * const chunks: Uint8Array[] = [];
 * const reader = inputStream.pipeThrough(compressor).getReader();
 * while (true) {
 *   const { done, value } = await reader.read();
 *   if (done) break;
 *   chunks.push(value);
 * }
 * ```
 */
export class CompressStream extends TransformStream<Uint8Array, Uint8Array> {
  /**
   * Creates a new ZSTD compression stream.
   *
   * @param options - Compression options
   */
  constructor(options?: CompressStreamOptions) {
    const level = getNativeLevel(options?.level, ALGORITHM);
    const bufferSize = options?.bufferSize ?? DEFAULT_BUFFER_SIZE;

    let wasm: WasmModule | null = null;
    let ctx: number = 0;
    let outputPtr: number = 0;

    super({
      async start() {
        // Initialize WASM module
        wasm = await getZstdModule();

        // Create compression context
        ctx = wasm.cu_stream_compress_create(level);
        if (ctx === 0) {
          throw CompressError.wasmInitFailed(ALGORITHM);
        }

        // Allocate output buffer
        outputPtr = wasm.malloc(bufferSize);
        if (outputPtr === 0) {
          wasm.cu_stream_compress_destroy(ctx);
          throw CompressError.wasmOom(ALGORITHM);
        }
      },

      async transform(chunk, controller) {
        if (!wasm || ctx === 0) {
          throw CompressError.compressionFailed(ALGORITHM, 'stream not initialized');
        }

        // Allocate input buffer
        const inputPtr = wasm.malloc(chunk.length);
        if (inputPtr === 0) {
          throw CompressError.wasmOom(ALGORITHM);
        }

        try {
          // Copy input to WASM memory
          copyToWasm(wasm.memory, chunk, inputPtr);

          // Compress in chunks
          let inputOffset = 0;
          while (inputOffset < chunk.length) {
            const remaining = chunk.length - inputOffset;
            const result = wasm.cu_stream_compress_write(
              ctx,
              inputPtr + inputOffset,
              remaining,
              outputPtr,
              bufferSize
            );

            if (result < 0) {
              throw CompressError.compressionFailed(ALGORITHM);
            }

            if (result > 0) {
              // Enqueue compressed output
              const output = copyFromWasm(wasm.memory, outputPtr, result);
              controller.enqueue(output);
            }

            // If we got output, assume all input was consumed
            // (ZSTD streaming API consumes all input on each call)
            inputOffset = chunk.length;
          }
        } finally {
          wasm.free(inputPtr);
        }
      },

      async flush(controller) {
        if (!wasm || ctx === 0) {
          return;
        }

        try {
          // Finish compression and flush remaining data
          let finished = false;
          while (!finished) {
            const result = wasm.cu_stream_compress_finish(ctx, outputPtr, bufferSize);

            if (result < 0) {
              throw CompressError.compressionFailed(ALGORITHM, 'failed to finish stream');
            }

            if (result > 0) {
              const output = copyFromWasm(wasm.memory, outputPtr, result);
              controller.enqueue(output);
            }

            // If result is 0, all data has been flushed
            if (result === 0) {
              finished = true;
            }
          }
        } finally {
          // Cleanup
          if (outputPtr !== 0) {
            wasm.free(outputPtr);
          }
          if (ctx !== 0) {
            wasm.cu_stream_compress_destroy(ctx);
          }
        }
      },
    });
  }
}

/**
 * A TransformStream that decompresses ZSTD data.
 *
 * @example
 * ```typescript
 * import { DecompressStream } from 'compress-utils/zstd';
 *
 * // Create a decompression stream
 * const decompressor = new DecompressStream();
 *
 * // Decompress streamed data
 * const response = await fetch('/compressed-file.zst');
 * const decompressed = response.body!
 *   .pipeThrough(decompressor)
 *   .pipeTo(writableStream);
 * ```
 */
export class DecompressStream extends TransformStream<Uint8Array, Uint8Array> {
  /**
   * Creates a new ZSTD decompression stream.
   *
   * @param options - Decompression options
   */
  constructor(options?: DecompressStreamOptions) {
    const bufferSize = options?.bufferSize ?? DEFAULT_BUFFER_SIZE;

    let wasm: WasmModule | null = null;
    let ctx: number = 0;
    let outputPtr: number = 0;

    super({
      async start() {
        // Initialize WASM module
        wasm = await getZstdModule();

        // Create decompression context
        ctx = wasm.cu_stream_decompress_create();
        if (ctx === 0) {
          throw DecompressError.wasmInitFailed(ALGORITHM);
        }

        // Allocate output buffer
        outputPtr = wasm.malloc(bufferSize);
        if (outputPtr === 0) {
          wasm.cu_stream_decompress_destroy(ctx);
          throw DecompressError.wasmOom(ALGORITHM);
        }
      },

      async transform(chunk, controller) {
        if (!wasm || ctx === 0) {
          throw DecompressError.decompressionFailed(ALGORITHM, 'stream not initialized');
        }

        // Allocate input buffer
        const inputPtr = wasm.malloc(chunk.length);
        if (inputPtr === 0) {
          throw DecompressError.wasmOom(ALGORITHM);
        }

        try {
          // Copy input to WASM memory
          copyToWasm(wasm.memory, chunk, inputPtr);

          // Decompress in chunks
          let inputOffset = 0;
          while (inputOffset < chunk.length) {
            const remaining = chunk.length - inputOffset;
            const result = wasm.cu_stream_decompress_write(
              ctx,
              inputPtr + inputOffset,
              remaining,
              outputPtr,
              bufferSize
            );

            if (result < 0) {
              throw DecompressError.decompressionFailed(ALGORITHM);
            }

            if (result > 0) {
              // Enqueue decompressed output
              const output = copyFromWasm(wasm.memory, outputPtr, result);
              controller.enqueue(output);
            }

            // Assume all input was consumed
            inputOffset = chunk.length;
          }
        } finally {
          wasm.free(inputPtr);
        }
      },

      async flush(controller) {
        if (!wasm || ctx === 0) {
          return;
        }

        try {
          // Check if decompression finished properly
          const finished = wasm.cu_stream_decompress_finish(ctx);
          if (finished < 0) {
            throw DecompressError.decompressionFailed(ALGORITHM, 'unexpected end of stream');
          }
        } finally {
          // Cleanup
          if (outputPtr !== 0) {
            wasm.free(outputPtr);
          }
          if (ctx !== 0) {
            wasm.cu_stream_decompress_destroy(ctx);
          }
        }
      },
    });
  }
}
