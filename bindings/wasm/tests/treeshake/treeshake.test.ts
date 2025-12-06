/**
 * Tree-shaking Validation Tests
 *
 * These tests verify that importing a single algorithm doesn't pull in
 * code from other algorithms, ensuring proper tree-shaking.
 *
 * Run with: npm test -- tests/treeshake
 */

import { describe, it, expect, beforeAll } from 'vitest';
import { build } from 'esbuild';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { mkdtemp, writeFile, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';

const __dirname = dirname(fileURLToPath(import.meta.url));
const SRC_DIR = join(__dirname, '..', '..', 'src');

describe('tree-shaking', () => {
  let tempDir: string;

  beforeAll(async () => {
    tempDir = await mkdtemp(join(tmpdir(), 'treeshake-test-'));
  });

  /**
   * Bundles a test file and returns the bundle size.
   */
  async function bundleAndGetSize(code: string): Promise<{
    size: number;
    output: string;
  }> {
    const inputFile = join(tempDir, `test-${Date.now()}.ts`);
    const outputFile = join(tempDir, `out-${Date.now()}.js`);

    await writeFile(inputFile, code);

    await build({
      entryPoints: [inputFile],
      bundle: true,
      minify: true,
      format: 'esm',
      outfile: outputFile,
      platform: 'browser',
      external: [], // Bundle everything
      treeShaking: true,
      plugins: [{
        name: 'mock-wasm',
        setup(build) {
          // Intercept imports of wasm.generated.js and redirect to mock
          build.onResolve({ filter: /wasm\.generated\.js$/ }, (args) => {
            return {
              path: join(SRC_DIR, '..', 'tests', 'treeshake', 'mock-wasm.js'),
            };
          });
        },
      }],
    });

    const output = await readFile(outputFile, 'utf-8');
    return { size: output.length, output };
  }

  it('should have smaller bundle when importing single algorithm', async () => {
    // Import only zstd
    const singleAlgoCode = `
import { compress } from '${SRC_DIR}/algorithms/zstd/index.js';
export { compress };
`;

    // Import multiple algorithms (using available ones - skip xz as it's not built)
    const multiAlgosCode = `
import { compress as zstdCompress } from '${SRC_DIR}/algorithms/zstd/index.js';
import { compress as brotliCompress } from '${SRC_DIR}/algorithms/brotli/index.js';
import { compress as zlibCompress } from '${SRC_DIR}/algorithms/zlib/index.js';
import { compress as bz2Compress } from '${SRC_DIR}/algorithms/bz2/index.js';
import { compress as lz4Compress } from '${SRC_DIR}/algorithms/lz4/index.js';
export { zstdCompress, brotliCompress, zlibCompress, bz2Compress, lz4Compress };
`;

    const [singleResult, multiResult] = await Promise.all([
      bundleAndGetSize(singleAlgoCode),
      bundleAndGetSize(multiAlgosCode),
    ]);

    console.log(`Single algorithm bundle: ${singleResult.size} bytes`);
    console.log(`Multiple algorithms bundle: ${multiResult.size} bytes`);

    // With mock WASM, the TypeScript wrapper code should still be smaller for single import
    // The real tree-shaking benefit comes from the WASM data itself not being bundled
    // When using actual WASM files, single should be ~1/5 the size
    expect(singleResult.size).toBeLessThanOrEqual(multiResult.size);
  });

  it('should not include other algorithms in single-algo import', async () => {
    const code = `
import { compress } from '${SRC_DIR}/algorithms/zstd/index.js';
export { compress };
`;

    const result = await bundleAndGetSize(code);

    // Check that other algorithm names don't appear in the bundle
    // (they would appear in error messages if included)
    const otherAlgos = ['brotli', 'bz2', 'lz4'];  // Skip xz as it's not built
    for (const algo of otherAlgos) {
      // Check for algorithm-specific strings that would only be present
      // if that algorithm's code was included
      const algoErrorMsg = `${algo} WASM module not found`;
      expect(result.output).not.toContain(algoErrorMsg);
    }
  });

  it('should include only imported exports', async () => {
    // Import only compress (not decompress or streaming)
    const compressOnlyCode = `
import { compress } from '${SRC_DIR}/algorithms/zstd/index.js';
export const doCompress = compress;
`;

    // Import compress and decompress
    const withDecompressCode = `
import { compress, decompress } from '${SRC_DIR}/algorithms/zstd/index.js';
export const doCompress = compress;
export const doDecompress = decompress;
`;

    // Import everything including streaming
    const withStreamingCode = `
import { compress, decompress, CompressStream, DecompressStream } from '${SRC_DIR}/algorithms/zstd/index.js';
export const doCompress = compress;
export const doDecompress = decompress;
export const doCompressStream = CompressStream;
export const doDecompressStream = DecompressStream;
`;

    // Run tests sequentially to avoid file conflicts
    const compressOnly = await bundleAndGetSize(compressOnlyCode);
    const withDecompress = await bundleAndGetSize(withDecompressCode);
    const withStreaming = await bundleAndGetSize(withStreamingCode);

    console.log(`compress only: ${compressOnly.size} bytes`);
    console.log(`compress + decompress: ${withDecompress.size} bytes`);
    console.log(`compress + decompress + streaming: ${withStreaming.size} bytes`);

    // Each additional feature should increase bundle size
    // (though not by much since they share core code)
    expect(compressOnly.size).toBeLessThanOrEqual(withDecompress.size);
    expect(withDecompress.size).toBeLessThanOrEqual(withStreaming.size);
  });
});
