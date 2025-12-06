import type { Algorithm, WasmModule } from './types.js';
import { CompressError } from './errors.js';

/**
 * Emscripten module factory function type.
 * This is the default export from Emscripten-generated JS files.
 */
export type EmscriptenModuleFactory = (
  moduleArg?: Record<string, unknown>
) => Promise<EmscriptenModule>;

/**
 * Emscripten module instance type.
 */
export interface EmscriptenModule {
  readonly HEAPU8: Uint8Array;
  readonly wasmMemory: WebAssembly.Memory;
  _malloc(size: number): number;
  _free(ptr: number): void;
  _cu_compress(
    inputPtr: number,
    inputLen: number,
    level: number,
    outputLenPtr: number
  ): number;
  _cu_decompress(
    inputPtr: number,
    inputLen: number,
    outputLenPtr: number
  ): number;
  _cu_stream_compress_create(level: number): number;
  _cu_stream_compress_write(
    ctx: number,
    inputPtr: number,
    inputLen: number,
    outputPtr: number,
    outputLen: number
  ): number;
  _cu_stream_compress_finish(
    ctx: number,
    outputPtr: number,
    outputLen: number
  ): number;
  _cu_stream_compress_destroy(ctx: number): void;
  _cu_stream_decompress_create(): number;
  _cu_stream_decompress_write(
    ctx: number,
    inputPtr: number,
    inputLen: number,
    outputPtr: number,
    outputLen: number
  ): number;
  _cu_stream_decompress_finish(ctx: number): number;
  _cu_stream_decompress_destroy(ctx: number): void;
}

/**
 * Cache for initialized WASM modules.
 * Each algorithm's module is initialized once and reused.
 */
const moduleCache = new Map<Algorithm, WasmModule>();

/**
 * Promises for in-flight module initializations.
 * Prevents duplicate initialization when multiple calls happen concurrently.
 */
const initPromises = new Map<Algorithm, Promise<WasmModule>>();

/**
 * Wraps an Emscripten module to match our WasmModule interface.
 *
 * @param emModule - The initialized Emscripten module
 * @returns A WasmModule interface
 */
function wrapEmscriptenModule(emModule: EmscriptenModule): WasmModule {
  return {
    get memory(): WebAssembly.Memory {
      return emModule.wasmMemory;
    },
    malloc(size: number): number {
      return emModule._malloc(size);
    },
    free(ptr: number): void {
      emModule._free(ptr);
    },
    cu_compress(
      inputPtr: number,
      inputLen: number,
      level: number,
      outputLenPtr: number
    ): number {
      return emModule._cu_compress(inputPtr, inputLen, level, outputLenPtr);
    },
    cu_decompress(
      inputPtr: number,
      inputLen: number,
      outputLenPtr: number
    ): number {
      return emModule._cu_decompress(inputPtr, inputLen, outputLenPtr);
    },
    cu_stream_compress_create(level: number): number {
      return emModule._cu_stream_compress_create(level);
    },
    cu_stream_compress_write(
      ctx: number,
      inputPtr: number,
      inputLen: number,
      outputPtr: number,
      outputLen: number
    ): number {
      return emModule._cu_stream_compress_write(
        ctx,
        inputPtr,
        inputLen,
        outputPtr,
        outputLen
      );
    },
    cu_stream_compress_finish(
      ctx: number,
      outputPtr: number,
      outputLen: number
    ): number {
      return emModule._cu_stream_compress_finish(ctx, outputPtr, outputLen);
    },
    cu_stream_compress_destroy(ctx: number): void {
      emModule._cu_stream_compress_destroy(ctx);
    },
    cu_stream_decompress_create(): number {
      return emModule._cu_stream_decompress_create();
    },
    cu_stream_decompress_write(
      ctx: number,
      inputPtr: number,
      inputLen: number,
      outputPtr: number,
      outputLen: number
    ): number {
      return emModule._cu_stream_decompress_write(
        ctx,
        inputPtr,
        inputLen,
        outputPtr,
        outputLen
      );
    },
    cu_stream_decompress_finish(ctx: number): number {
      return emModule._cu_stream_decompress_finish(ctx);
    },
    cu_stream_decompress_destroy(ctx: number): void {
      emModule._cu_stream_decompress_destroy(ctx);
    },
  };
}

/**
 * Instantiates a WASM module from an Emscripten factory.
 *
 * @param factory - Emscripten module factory function
 * @param algorithm - Algorithm name for error messages
 * @returns Instantiated WASM module
 */
async function instantiateModule(
  factory: EmscriptenModuleFactory,
  algorithm: Algorithm
): Promise<WasmModule> {
  try {
    const emModule = await factory();
    return wrapEmscriptenModule(emModule);
  } catch (error) {
    throw CompressError.wasmInitFailed(
      algorithm,
      error instanceof Error ? error : undefined
    );
  }
}

/**
 * Gets or initializes a WASM module for the given algorithm.
 *
 * @param algorithm - The compression algorithm
 * @param factory - Emscripten module factory function
 * @returns The initialized WASM module
 */
export async function getModule(
  algorithm: Algorithm,
  factory: EmscriptenModuleFactory
): Promise<WasmModule> {
  // Check cache first
  const cached = moduleCache.get(algorithm);
  if (cached) {
    return cached;
  }

  // Check for in-flight initialization
  let initPromise = initPromises.get(algorithm);
  if (initPromise) {
    return initPromise;
  }

  // Start initialization
  initPromise = instantiateModule(factory, algorithm);
  initPromises.set(algorithm, initPromise);

  try {
    const module = await initPromise;
    moduleCache.set(algorithm, module);
    return module;
  } finally {
    // Clean up init promise regardless of success/failure
    initPromises.delete(algorithm);
  }
}

/**
 * Checks if a WASM module is already loaded for the given algorithm.
 *
 * @param algorithm - The compression algorithm
 * @returns True if the module is loaded
 */
export function isModuleLoaded(algorithm: Algorithm): boolean {
  return moduleCache.has(algorithm);
}

/**
 * Preloads a WASM module for the given algorithm.
 * This is useful to avoid first-call latency.
 *
 * @param algorithm - The compression algorithm
 * @param factory - Emscripten module factory function
 */
export async function preloadModule(
  algorithm: Algorithm,
  factory: EmscriptenModuleFactory
): Promise<void> {
  await getModule(algorithm, factory);
}

/**
 * Clears the module cache. Mainly useful for testing.
 */
export function clearModuleCache(): void {
  moduleCache.clear();
}
