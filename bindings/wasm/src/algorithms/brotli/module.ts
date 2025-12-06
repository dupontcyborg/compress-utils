/**
 * Brotli WASM Module Loader
 */

import {
  getModule,
  isModuleLoaded,
  preloadModule,
  type EmscriptenModuleFactory,
  type WasmModule,
} from '../../core/index.js';

let factoryCache: EmscriptenModuleFactory | null = null;

async function loadFactory(): Promise<EmscriptenModuleFactory> {
  if (factoryCache !== null) {
    return factoryCache;
  }

  try {
    // eslint-disable-next-line @typescript-eslint/no-explicit-any
    const module = (await import('./wasm.generated.js')) as any;
    factoryCache = module.default as EmscriptenModuleFactory;
    return factoryCache;
  } catch {
    throw new Error(
      'Brotli WASM module not found. Run the build script first: npm run build:wasm'
    );
  }
}

export async function getBrotliModule(): Promise<WasmModule> {
  const factory = await loadFactory();
  return getModule('brotli', factory);
}

export function isBrotliModuleLoaded(): boolean {
  return isModuleLoaded('brotli');
}

export async function preload(): Promise<void> {
  const factory = await loadFactory();
  await preloadModule('brotli', factory);
}
