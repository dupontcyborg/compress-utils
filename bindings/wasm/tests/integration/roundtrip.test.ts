/**
 * Roundtrip Integration Tests
 *
 * Tests compress/decompress roundtrip for all algorithms.
 * Note: These tests require the WASM modules to be built first.
 */

import { describe, it, expect } from 'vitest';
import { generateTestData, collectStream, createReadableStream } from '../helpers.js';

// Import all algorithm modules statically
import * as zstd from '../../src/algorithms/zstd/index.js';
import * as brotli from '../../src/algorithms/brotli/index.js';
import * as zlib from '../../src/algorithms/zlib/index.js';
import * as bz2 from '../../src/algorithms/bz2/index.js';
import * as lz4 from '../../src/algorithms/lz4/index.js';
import * as xz from '../../src/algorithms/xz/index.js';

interface AlgorithmModule {
  compress: (data: Uint8Array | string, options?: { level?: string | number }) => Promise<Uint8Array>;
  decompress: (data: Uint8Array) => Promise<Uint8Array>;
  decompressToString: (data: Uint8Array) => Promise<string>;
  CompressStream: new (options?: { level?: number }) => TransformStream<Uint8Array, Uint8Array>;
  DecompressStream: new () => TransformStream<Uint8Array, Uint8Array>;
}

const algorithms: Record<string, AlgorithmModule> = {
  zstd: zstd as unknown as AlgorithmModule,
  brotli: brotli as unknown as AlgorithmModule,
  zlib: zlib as unknown as AlgorithmModule,
  bz2: bz2 as unknown as AlgorithmModule,
  lz4: lz4 as unknown as AlgorithmModule,
  xz: xz as unknown as AlgorithmModule,
};

// Integration tests for roundtrip compression/decompression
describe('roundtrip integration tests', () => {
  for (const [name, algo] of Object.entries(algorithms)) {
    describe(name, () => {
      it('should roundtrip small text', async () => {
        const original = new TextEncoder().encode('Hello, World!');

        const compressed = await algo.compress(original);
        const decompressed = await algo.decompress(compressed);

        expect(decompressed).toEqual(original);
      });

      it('should roundtrip empty data', async () => {
        const original = new Uint8Array([]);

        const compressed = await algo.compress(original);
        const decompressed = await algo.decompress(compressed);

        expect(decompressed).toEqual(original);
      });

      it('should roundtrip 1KB data', async () => {
        const original = generateTestData(1024);

        const compressed = await algo.compress(original);
        const decompressed = await algo.decompress(compressed);

        expect(decompressed).toEqual(original);
      });

      it('should roundtrip 100KB data', async () => {
        const original = generateTestData(100 * 1024);

        const compressed = await algo.compress(original);
        const decompressed = await algo.decompress(compressed);

        expect(decompressed).toEqual(original);
      });

      it('should roundtrip string input', async () => {
        const original = 'The quick brown fox jumps over the lazy dog. '.repeat(100);

        const compressed = await algo.compress(original);
        const decompressed = await algo.decompressToString(compressed);

        expect(decompressed).toBe(original);
      });

      it('should work with different compression levels', async () => {
        const original = generateTestData(10 * 1024);

        for (const level of ['fast', 'balanced', 'best'] as const) {
          const compressed = await algo.compress(original, { level });
          const decompressed = await algo.decompress(compressed);
          expect(decompressed).toEqual(original);
        }
      });

      it('should roundtrip via streaming', async () => {
        const original = generateTestData(50 * 1024);

        // Compress via stream
        const compressor = new algo.CompressStream({ level: 5 });
        const compressed = await collectStream(
          createReadableStream(original).pipeThrough(compressor)
        );

        // Verify with one-shot decompress
        const decompressed1 = await algo.decompress(compressed);
        expect(decompressed1).toEqual(original);

        // Compress via one-shot, decompress via stream
        const compressed2 = await algo.compress(original);
        const decompressor = new algo.DecompressStream();
        const decompressed2 = await collectStream(
          createReadableStream(compressed2).pipeThrough(decompressor)
        );
        expect(decompressed2).toEqual(original);
      });
    });
  }
});
