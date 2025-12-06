#!/usr/bin/env npx tsx
/**
 * Copies Emscripten-generated WASM JS modules to the source directory.
 *
 * This script copies the .js files from wasm-build/ that contain the
 * WASM binary embedded as base64, making them available for import.
 */

import { existsSync, copyFileSync, statSync } from 'node:fs';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const WASM_BUILD_DIR = join(__dirname, '..', 'wasm-build');
const ALGORITHMS_DIR = join(__dirname, '..', 'src', 'algorithms');

const ALGORITHMS = ['zstd', 'brotli', 'zlib', 'bz2', 'lz4', 'xz'] as const;

interface WasmStats {
  algorithm: string;
  jsSize: number;
}

function copyWasmModule(algorithm: string): WasmStats | null {
  const jsPath = join(WASM_BUILD_DIR, `${algorithm}.js`);
  const outputPath = join(ALGORITHMS_DIR, algorithm, 'wasm.generated.js');

  if (!existsSync(jsPath)) {
    console.log(`  Skipping ${algorithm}: ${algorithm}.js not found`);
    return null;
  }

  // Copy the JS file
  copyFileSync(jsPath, outputPath);
  const jsSize = statSync(jsPath).size;

  console.log(`  ✓ ${algorithm}: ${(jsSize / 1024).toFixed(1)} KB`);

  return {
    algorithm,
    jsSize,
  };
}

function main(): void {
  console.log('Copying WASM modules to source directory...\n');

  if (!existsSync(WASM_BUILD_DIR)) {
    console.error(`Error: Build directory not found: ${WASM_BUILD_DIR}`);
    console.error('Run build-wasm.sh first to compile WASM modules.');
    process.exit(1);
  }

  const stats: WasmStats[] = [];

  for (const algorithm of ALGORITHMS) {
    const result = copyWasmModule(algorithm);
    if (result) {
      stats.push(result);
    }
  }

  if (stats.length === 0) {
    console.log('\nNo WASM files found to process.');
    return;
  }

  // Print summary
  console.log('\n─────────────────────────────────────────────────');
  console.log('Summary:');
  console.log('─────────────────────────────────────────────────');

  const totalSize = stats.reduce((sum, s) => sum + s.jsSize, 0);

  console.log(`  Algorithms: ${stats.length}`);
  console.log(`  Total Size: ${(totalSize / 1024).toFixed(1)} KB`);
  console.log('');
}

main();
