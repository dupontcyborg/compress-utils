/**
 * ZSTD WASM Module Loader
 *
 * Handles loading and initialization of the ZSTD WebAssembly module.
 * The module is lazily loaded on first use and cached for subsequent calls.
 */

import {
  getModule,
  isModuleLoaded,
  preloadModule,
  type EmscriptenModuleFactory,
  type WasmModule,
} from '../../core/index.js';

// Cache for the module factory
let factoryCache: EmscriptenModuleFactory | null = null;

// Dynamic import for the generated WASM module
async function loadFactory(): Promise<EmscriptenModuleFactory> {
  if (factoryCache !== null) {
    return factoryCache;
  }

  try {
    // Import the Emscripten-generated module
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const module = (await import('./wasm.generated.js')) as any;
    factoryCache = module.default as EmscriptenModuleFactory;
    return factoryCache;
  } catch {
    throw new Error(
      'ZSTD WASM module not found. Run the build script first: npm run build:wasm'
    );
  }
}

/**
 * Gets the initialized ZSTD WASM module.
 *
 * This function lazily loads and initializes the WASM module on first call.
 * Subsequent calls return the cached module instance.
 *
 * @returns Promise resolving to the WASM module
 * @throws CompressError if module initialization fails
 */
export async function getZstdModule(): Promise<WasmModule> {
  const factory = await loadFactory();
  return getModule('zstd', factory);
}

/**
 * Checks if the ZSTD WASM module is already loaded.
 *
 * @returns True if the module is loaded and ready
 */
export function isZstdModuleLoaded(): boolean {
  return isModuleLoaded('zstd');
}

/**
 * Preloads the ZSTD WASM module.
 *
 * Call this early in your application to avoid first-call latency.
 * This is optional - the module will be automatically loaded on first use.
 *
 * @example
 * ```typescript
 * // Preload during app initialization
 * import { preload } from 'compress-utils/zstd';
 *
 * await preload();
 *
 * // Later, compression is instant (no loading delay)
 * const compressed = await compress(data);
 * ```
 */
export async function preload(): Promise<void> {
  const factory = await loadFactory();
  await preloadModule('zstd', factory);
}
