/**
 * Zlib WASM Module Loader
 */

import {
  getModule,
  isModuleLoaded,
  preloadModule,
  type WasmModule,
} from '../../core/index.js';

let wasmBase64Cache: string | null = null;

async function loadWasmBase64(): Promise<string> {
  if (wasmBase64Cache !== null) {
    return wasmBase64Cache;
  }

  try {
    const module = await import('./wasm.generated.js') as { WASM_BASE64: string };
    wasmBase64Cache = module.WASM_BASE64;
    return wasmBase64Cache;
  } catch {
    throw new Error(
      'Zlib WASM module not found. Run the build script first: npm run build:wasm'
    );
  }
}

export async function getZlibModule(): Promise<WasmModule> {
  const wasmBase64 = await loadWasmBase64();
  return getModule('zlib', wasmBase64);
}

export function isZlibModuleLoaded(): boolean {
  return isModuleLoaded('zlib');
}

export async function preload(): Promise<void> {
  const wasmBase64 = await loadWasmBase64();
  await preloadModule('zlib', wasmBase64);
}
