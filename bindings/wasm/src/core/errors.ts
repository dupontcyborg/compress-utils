/**
 * Error codes for compression/decompression operations.
 */
export type CompressErrorCode =
  | 'INVALID_INPUT'
  | 'INVALID_LEVEL'
  | 'BUFFER_TOO_SMALL'
  | 'WASM_INIT_FAILED'
  | 'WASM_OOM'
  | 'COMPRESSION_FAILED'
  | 'STREAM_ALREADY_FINISHED'
  | 'UNKNOWN';

/**
 * Error codes specific to decompression operations.
 */
export type DecompressErrorCode =
  | 'INVALID_INPUT'
  | 'CORRUPTED_DATA'
  | 'UNEXPECTED_EOF'
  | 'BUFFER_TOO_SMALL'
  | 'WASM_INIT_FAILED'
  | 'WASM_OOM'
  | 'DECOMPRESSION_FAILED'
  | 'STREAM_ALREADY_FINISHED'
  | 'UNKNOWN';

/**
 * Base error class for compression operations.
 */
export class CompressError extends Error {
  public readonly code: CompressErrorCode;
  public readonly algorithm: string;

  constructor(
    message: string,
    code: CompressErrorCode,
    algorithm: string,
    options?: ErrorOptions
  ) {
    super(message, options);
    this.name = 'CompressError';
    this.code = code;
    this.algorithm = algorithm;

    // Maintains proper stack trace in V8 environments
    if (Error.captureStackTrace) {
      Error.captureStackTrace(this, CompressError);
    }
  }

  /**
   * Creates a CompressError for invalid input.
   */
  static invalidInput(algorithm: string, detail?: string): CompressError {
    const message = detail
      ? `Invalid input for ${algorithm}: ${detail}`
      : `Invalid input for ${algorithm} compression`;
    return new CompressError(message, 'INVALID_INPUT', algorithm);
  }

  /**
   * Creates a CompressError for invalid compression level.
   */
  static invalidLevel(algorithm: string, level: unknown): CompressError {
    return new CompressError(
      `Invalid compression level for ${algorithm}: ${String(level)}. Expected 1-10 or 'fast' | 'balanced' | 'best'.`,
      'INVALID_LEVEL',
      algorithm
    );
  }

  /**
   * Creates a CompressError for WASM initialization failure.
   */
  static wasmInitFailed(algorithm: string, cause?: Error): CompressError {
    return new CompressError(
      `Failed to initialize ${algorithm} WASM module`,
      'WASM_INIT_FAILED',
      algorithm,
      cause ? { cause } : undefined
    );
  }

  /**
   * Creates a CompressError for WASM out-of-memory.
   */
  static wasmOom(algorithm: string): CompressError {
    return new CompressError(
      `Out of memory in ${algorithm} WASM module`,
      'WASM_OOM',
      algorithm
    );
  }

  /**
   * Creates a CompressError for general compression failure.
   */
  static compressionFailed(algorithm: string, detail?: string): CompressError {
    const message = detail
      ? `${algorithm} compression failed: ${detail}`
      : `${algorithm} compression failed`;
    return new CompressError(message, 'COMPRESSION_FAILED', algorithm);
  }

  /**
   * Creates a CompressError for operations on a finished stream.
   */
  static streamAlreadyFinished(algorithm: string): CompressError {
    return new CompressError(
      `Cannot write to ${algorithm} stream: stream has already been finished`,
      'STREAM_ALREADY_FINISHED',
      algorithm
    );
  }
}

/**
 * Error class for decompression operations.
 */
export class DecompressError extends Error {
  public readonly code: DecompressErrorCode;
  public readonly algorithm: string;

  constructor(
    message: string,
    code: DecompressErrorCode,
    algorithm: string,
    options?: ErrorOptions
  ) {
    super(message, options);
    this.name = 'DecompressError';
    this.code = code;
    this.algorithm = algorithm;

    if (Error.captureStackTrace) {
      Error.captureStackTrace(this, DecompressError);
    }
  }

  /**
   * Creates a DecompressError for invalid input.
   */
  static invalidInput(algorithm: string, detail?: string): DecompressError {
    const message = detail
      ? `Invalid input for ${algorithm} decompression: ${detail}`
      : `Invalid input for ${algorithm} decompression`;
    return new DecompressError(message, 'INVALID_INPUT', algorithm);
  }

  /**
   * Creates a DecompressError for corrupted data.
   */
  static corruptedData(algorithm: string, detail?: string): DecompressError {
    const message = detail
      ? `Corrupted ${algorithm} data: ${detail}`
      : `Corrupted ${algorithm} compressed data`;
    return new DecompressError(message, 'CORRUPTED_DATA', algorithm);
  }

  /**
   * Creates a DecompressError for unexpected end of input.
   */
  static unexpectedEof(algorithm: string): DecompressError {
    return new DecompressError(
      `Unexpected end of ${algorithm} compressed data`,
      'UNEXPECTED_EOF',
      algorithm
    );
  }

  /**
   * Creates a DecompressError for WASM initialization failure.
   */
  static wasmInitFailed(algorithm: string, cause?: Error): DecompressError {
    return new DecompressError(
      `Failed to initialize ${algorithm} WASM module`,
      'WASM_INIT_FAILED',
      algorithm,
      cause ? { cause } : undefined
    );
  }

  /**
   * Creates a DecompressError for WASM out-of-memory.
   */
  static wasmOom(algorithm: string): DecompressError {
    return new DecompressError(
      `Out of memory in ${algorithm} WASM module`,
      'WASM_OOM',
      algorithm
    );
  }

  /**
   * Creates a DecompressError for general decompression failure.
   */
  static decompressionFailed(
    algorithm: string,
    detail?: string
  ): DecompressError {
    const message = detail
      ? `${algorithm} decompression failed: ${detail}`
      : `${algorithm} decompression failed`;
    return new DecompressError(message, 'DECOMPRESSION_FAILED', algorithm);
  }

  /**
   * Creates a DecompressError for operations on a finished stream.
   */
  static streamAlreadyFinished(algorithm: string): DecompressError {
    return new DecompressError(
      `Cannot write to ${algorithm} stream: stream has already been finished`,
      'STREAM_ALREADY_FINISHED',
      algorithm
    );
  }
}
