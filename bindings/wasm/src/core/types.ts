/**
 * Compression level - can be a number 1-10 or a named preset.
 *
 * Numeric levels:
 * - 1: Fastest compression, larger output
 * - 5: Balanced (default)
 * - 10: Best compression, slower
 *
 * Named presets:
 * - 'fast': Equivalent to level 1-2
 * - 'balanced': Equivalent to level 5
 * - 'best': Equivalent to level 9-10
 */
export type CompressionLevel =
  | 1
  | 2
  | 3
  | 4
  | 5
  | 6
  | 7
  | 8
  | 9
  | 10
  | 'fast'
  | 'balanced'
  | 'best';

/**
 * Options for compression operations.
 */
export interface CompressOptions {
  /**
   * Compression level (1-10 or named preset).
   * @default 5
   */
  readonly level?: CompressionLevel;

  /**
   * Text encoding to use when input is a string.
   * @default 'utf-8'
   */
  readonly encoding?: BufferEncoding;
}

/**
 * Options for decompression operations.
 */
export interface DecompressOptions {
  /**
   * Expected output size hint (optional optimization).
   * If provided, the decompressor may pre-allocate this much memory.
   */
  readonly expectedSize?: number;
}

/**
 * Valid input types for compression.
 * - Uint8Array: Raw binary data
 * - ArrayBuffer: Raw binary buffer
 * - string: Text data (will be encoded using options.encoding)
 */
export type CompressInput = Uint8Array | ArrayBuffer | string;

/**
 * Valid input types for decompression.
 * - Uint8Array: Compressed binary data
 * - ArrayBuffer: Compressed binary buffer
 */
export type DecompressInput = Uint8Array | ArrayBuffer;

/**
 * Algorithm identifiers for internal use.
 */
export type Algorithm = 'zstd' | 'brotli' | 'zlib' | 'bz2' | 'lz4' | 'xz';

/**
 * WASM module interface - the shape of the instantiated WASM module.
 * Each algorithm implements this interface.
 */
export interface WasmModule {
  readonly memory: WebAssembly.Memory;

  // Memory management
  malloc(size: number): number;
  free(ptr: number): void;

  // One-shot compression/decompression
  cu_compress(
    inputPtr: number,
    inputLen: number,
    level: number,
    outputLenPtr: number
  ): number;
  cu_decompress(
    inputPtr: number,
    inputLen: number,
    outputLenPtr: number
  ): number;

  // Streaming compression
  cu_stream_compress_create(level: number): number;
  cu_stream_compress_write(
    ctx: number,
    inputPtr: number,
    inputLen: number,
    outputPtr: number,
    outputCap: number
  ): number;
  cu_stream_compress_finish(
    ctx: number,
    outputPtr: number,
    outputCap: number
  ): number;
  cu_stream_compress_destroy(ctx: number): void;

  // Streaming decompression
  cu_stream_decompress_create(): number;
  cu_stream_decompress_write(
    ctx: number,
    inputPtr: number,
    inputLen: number,
    outputPtr: number,
    outputCap: number
  ): number;
  cu_stream_decompress_finish(ctx: number): number;
  cu_stream_decompress_destroy(ctx: number): void;
}

/**
 * State of a streaming operation.
 */
export const enum StreamState {
  /** Stream is active and accepting input */
  Active = 0,
  /** Stream has been finished */
  Finished = 1,
  /** Stream encountered an error */
  Error = 2,
}
