/**
 * Bzip2 WASM Module Loader
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
      'Bzip2 WASM module not found. Run the build script first: npm run build:wasm'
    );
  }
}

export async function getBz2Module(): Promise<WasmModule> {
  const factory = await loadFactory();
  return getModule('bz2', factory);
}

export function isBz2ModuleLoaded(): boolean {
  return isModuleLoaded('bz2');
}

export async function preload(): Promise<void> {
  const factory = await loadFactory();
  await preloadModule('bz2', factory);
}
