import type { Algorithm, WasmModule } from './types.js';
import { CompressError } from './errors.js';

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
 * Decodes a base64-encoded string to a Uint8Array.
 *
 * @param base64 - Base64-encoded string
 * @returns Decoded bytes
 */
export function decodeBase64(base64: string): Uint8Array {
  // Check for Node.js Buffer
  if (typeof Buffer !== 'undefined') {
    return new Uint8Array(Buffer.from(base64, 'base64'));
  }

  // Browser environment - use atob
  const binaryString = atob(base64);
  const bytes = new Uint8Array(binaryString.length);
  for (let i = 0; i < binaryString.length; i++) {
    bytes[i] = binaryString.charCodeAt(i);
  }
  return bytes;
}

/**
 * Instantiates a WASM module from base64-encoded binary.
 *
 * @param wasmBase64 - Base64-encoded WASM binary
 * @param algorithm - Algorithm name for error messages
 * @returns Instantiated WASM module
 */
async function instantiateModule(
  wasmBase64: string,
  algorithm: Algorithm
): Promise<WasmModule> {
  try {
    const wasmBytes = decodeBase64(wasmBase64);

    // Use streaming instantiation if available (better performance)
    let instance: WebAssembly.Instance;

    // Use non-streaming instantiation (simpler and works everywhere)
    // Note: instantiateStreaming requires a fetch Response with proper MIME type
    const result = await WebAssembly.instantiate(
      wasmBytes.buffer as ArrayBuffer
    );
    instance = (result as WebAssembly.WebAssemblyInstantiatedSource).instance;

    // Validate that all required exports exist
    const exports = instance.exports as Record<string, unknown>;
    const required = [
      'memory',
      'malloc',
      'free',
      'compress',
      'decompress',
    ];

    for (const name of required) {
      if (!(name in exports)) {
        throw new Error(`Missing required WASM export: ${name}`);
      }
    }

    return exports as unknown as WasmModule;
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
 * @param wasmBase64 - Base64-encoded WASM binary
 * @returns The initialized WASM module
 */
export async function getModule(
  algorithm: Algorithm,
  wasmBase64: string
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
  initPromise = instantiateModule(wasmBase64, algorithm);
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
 * @param wasmBase64 - Base64-encoded WASM binary
 */
export async function preloadModule(
  algorithm: Algorithm,
  wasmBase64: string
): Promise<void> {
  await getModule(algorithm, wasmBase64);
}

/**
 * Clears the module cache. Mainly useful for testing.
 */
export function clearModuleCache(): void {
  moduleCache.clear();
}
