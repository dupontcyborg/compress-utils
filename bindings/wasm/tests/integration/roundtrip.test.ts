/**
 * Roundtrip Integration Tests
 *
 * Tests compress/decompress roundtrip for all algorithms.
 * Note: These tests require the WASM modules to be built first.
 */

import { describe, it, expect } from 'vitest';
import { generateTestData, collectStream, createReadableStream } from '../helpers.js';

const ALGORITHMS = ['zstd', 'brotli', 'zlib', 'bz2', 'lz4', 'xz'] as const;

// Skip tests until WASM modules are built
describe.skip('roundtrip integration tests', () => {
  for (const algo of ALGORITHMS) {
    describe(algo, () => {
      it('should roundtrip small text', async () => {
        const { compress, decompress } = await import(`../../src/algorithms/${algo}/index.js`);
        const original = new TextEncoder().encode('Hello, World!');

        const compressed = await compress(original);
        const decompressed = await decompress(compressed);

        expect(decompressed).toEqual(original);
      });

      it('should roundtrip empty data', async () => {
        const { compress, decompress } = await import(`../../src/algorithms/${algo}/index.js`);
        const original = new Uint8Array([]);

        const compressed = await compress(original);
        const decompressed = await decompress(compressed);

        expect(decompressed).toEqual(original);
      });

      it('should roundtrip 1KB data', async () => {
        const { compress, decompress } = await import(`../../src/algorithms/${algo}/index.js`);
        const original = generateTestData(1024);

        const compressed = await compress(original);
        const decompressed = await decompress(compressed);

        expect(decompressed).toEqual(original);
      });

      it('should roundtrip 100KB data', async () => {
        const { compress, decompress } = await import(`../../src/algorithms/${algo}/index.js`);
        const original = generateTestData(100 * 1024);

        const compressed = await compress(original);
        const decompressed = await decompress(compressed);

        expect(decompressed).toEqual(original);
      });

      it('should roundtrip string input', async () => {
        const { compress, decompressToString } = await import(`../../src/algorithms/${algo}/index.js`);
        const original = 'The quick brown fox jumps over the lazy dog. '.repeat(100);

        const compressed = await compress(original);
        const decompressed = await decompressToString(compressed);

        expect(decompressed).toBe(original);
      });

      it('should work with different compression levels', async () => {
        const { compress, decompress } = await import(`../../src/algorithms/${algo}/index.js`);
        const original = generateTestData(10 * 1024);

        for (const level of ['fast', 'balanced', 'best'] as const) {
          const compressed = await compress(original, { level });
          const decompressed = await decompress(compressed);
          expect(decompressed).toEqual(original);
        }
      });

      it('should roundtrip via streaming', async () => {
        const { CompressStream, DecompressStream, decompress, compress } =
          await import(`../../src/algorithms/${algo}/index.js`);
        const original = generateTestData(50 * 1024);

        // Compress via stream
        const compressor = new CompressStream({ level: 5 });
        const compressed = await collectStream(
          createReadableStream(original).pipeThrough(compressor)
        );

        // Verify with one-shot decompress
        const decompressed1 = await decompress(compressed);
        expect(decompressed1).toEqual(original);

        // Compress via one-shot, decompress via stream
        const compressed2 = await compress(original);
        const decompressor = new DecompressStream();
        const decompressed2 = await collectStream(
          createReadableStream(compressed2).pipeThrough(decompressor)
        );
        expect(decompressed2).toEqual(original);
      });
    });
  }
});
