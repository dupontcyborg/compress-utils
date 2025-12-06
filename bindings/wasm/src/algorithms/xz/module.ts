/**
 * XZ/LZMA WASM Module Loader
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
      'XZ WASM module not found. Run the build script first: npm run build:wasm'
    );
  }
}

export async function getXzModule(): Promise<WasmModule> {
  const wasmBase64 = await loadWasmBase64();
  return getModule('xz', wasmBase64);
}

export function isXzModuleLoaded(): boolean {
  return isModuleLoaded('xz');
}

export async function preload(): Promise<void> {
  const wasmBase64 = await loadWasmBase64();
  await preloadModule('xz', wasmBase64);
}
