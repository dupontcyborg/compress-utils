/**
 * LZ4 WASM Module Loader
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
      'LZ4 WASM module not found. Run the build script first: npm run build:wasm'
    );
  }
}

export async function getLz4Module(): Promise<WasmModule> {
  const factory = await loadFactory();
  return getModule('lz4', factory);
}

export function isLz4ModuleLoaded(): boolean {
  return isModuleLoaded('lz4');
}

export async function preload(): Promise<void> {
  const factory = await loadFactory();
  await preloadModule('lz4', factory);
}
