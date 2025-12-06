/**
 * LZ4 WASM Module Loader
 */

import {
  getModule,
  isModuleLoaded,
  preloadModule,
  type WasmModule,
} from '../../core/index.js';

let WASM_BASE64: string | undefined;

async function loadWasmBase64(): Promise<string> {
  if (WASM_BASE64) {
    return WASM_BASE64;
  }

  try {
    const module = await import('./wasm.generated.js');
    WASM_BASE64 = module.WASM_BASE64;
    return WASM_BASE64;
  } catch {
    throw new Error(
      'LZ4 WASM module not found. Run the build script first: npm run build:wasm'
    );
  }
}

export async function getLz4Module(): Promise<WasmModule> {
  const wasmBase64 = await loadWasmBase64();
  return getModule('lz4', wasmBase64);
}

export function isLz4ModuleLoaded(): boolean {
  return isModuleLoaded('lz4');
}

export async function preload(): Promise<void> {
  const wasmBase64 = await loadWasmBase64();
  await preloadModule('lz4', wasmBase64);
}
