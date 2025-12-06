/**
 * ZSTD Algorithm Tests
 *
 * Note: These tests require the WASM modules to be built first.
 * Run `npm run build:wasm` before running tests.
 */

import { describe, it, expect, beforeAll } from 'vitest';

// Tests will be enabled once WASM modules are built
describe.skip('zstd', () => {
  describe('compress', () => {
    it('should compress Uint8Array data', async () => {
      const { compress } = await import('../../src/algorithms/zstd/index.js');
      const input = new TextEncoder().encode('Hello, World!');
      const compressed = await compress(input);

      expect(compressed).toBeInstanceOf(Uint8Array);
      expect(compressed.length).toBeGreaterThan(0);
    });

    it('should compress string data', async () => {
      const { compress } = await import('../../src/algorithms/zstd/index.js');
      const compressed = await compress('Hello, World!');

      expect(compressed).toBeInstanceOf(Uint8Array);
      expect(compressed.length).toBeGreaterThan(0);
    });

    it('should accept compression level option', async () => {
      const { compress } = await import('../../src/algorithms/zstd/index.js');
      const input = 'Hello, World!';

      const fast = await compress(input, { level: 'fast' });
      const best = await compress(input, { level: 'best' });

      // Both should produce valid output
      expect(fast).toBeInstanceOf(Uint8Array);
      expect(best).toBeInstanceOf(Uint8Array);
    });

    it('should accept numeric compression levels', async () => {
      const { compress } = await import('../../src/algorithms/zstd/index.js');
      const input = 'Hello, World!';

      for (let level = 1; level <= 10; level++) {
        const compressed = await compress(input, { level: level as 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 });
        expect(compressed).toBeInstanceOf(Uint8Array);
      }
    });
  });

  describe('decompress', () => {
    it('should decompress to original data', async () => {
      const { compress, decompress } = await import('../../src/algorithms/zstd/index.js');
      const original = new TextEncoder().encode('Hello, World!');

      const compressed = await compress(original);
      const decompressed = await decompress(compressed);

      expect(decompressed).toEqual(original);
    });

    it('should throw on invalid input', async () => {
      const { decompress } = await import('../../src/algorithms/zstd/index.js');

      await expect(decompress(new Uint8Array([1, 2, 3]))).rejects.toThrow();
    });

    it('should throw on empty input', async () => {
      const { decompress } = await import('../../src/algorithms/zstd/index.js');

      await expect(decompress(new Uint8Array([]))).rejects.toThrow();
    });
  });

  describe('decompressToString', () => {
    it('should decompress to string', async () => {
      const { compress, decompressToString } = await import('../../src/algorithms/zstd/index.js');
      const original = 'Hello, World!';

      const compressed = await compress(original);
      const decompressed = await decompressToString(compressed);

      expect(decompressed).toBe(original);
    });
  });

  describe('roundtrip', () => {
    it('should handle empty data', async () => {
      const { compress, decompress } = await import('../../src/algorithms/zstd/index.js');
      const original = new Uint8Array([]);

      const compressed = await compress(original);
      const decompressed = await decompress(compressed);

      expect(decompressed).toEqual(original);
    });

    it('should handle large data', async () => {
      const { compress, decompress } = await import('../../src/algorithms/zstd/index.js');
      // 1MB of random-ish data
      const original = new Uint8Array(1024 * 1024);
      for (let i = 0; i < original.length; i++) {
        original[i] = i % 256;
      }

      const compressed = await compress(original);
      const decompressed = await decompress(compressed);

      expect(decompressed).toEqual(original);
    });

    it('should handle binary data', async () => {
      const { compress, decompress } = await import('../../src/algorithms/zstd/index.js');
      const original = new Uint8Array([0, 1, 255, 128, 64, 32, 16, 8, 4, 2, 1, 0]);

      const compressed = await compress(original);
      const decompressed = await decompress(compressed);

      expect(decompressed).toEqual(original);
    });
  });

  describe('streaming', () => {
    it('should compress via stream', async () => {
      const { CompressStream, decompress } = await import('../../src/algorithms/zstd/index.js');
      const input = new TextEncoder().encode('Hello, World!');

      const compressor = new CompressStream({ level: 5 });
      const reader = new ReadableStream({
        start(controller) {
          controller.enqueue(input);
          controller.close();
        },
      }).pipeThrough(compressor).getReader();

      const chunks: Uint8Array[] = [];
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value);
      }

      const compressed = new Uint8Array(chunks.reduce((acc, c) => acc + c.length, 0));
      let offset = 0;
      for (const chunk of chunks) {
        compressed.set(chunk, offset);
        offset += chunk.length;
      }

      const decompressed = await decompress(compressed);
      expect(decompressed).toEqual(input);
    });

    it('should decompress via stream', async () => {
      const { compress, DecompressStream } = await import('../../src/algorithms/zstd/index.js');
      const original = new TextEncoder().encode('Hello, World!');
      const compressed = await compress(original);

      const decompressor = new DecompressStream();
      const reader = new ReadableStream({
        start(controller) {
          controller.enqueue(compressed);
          controller.close();
        },
      }).pipeThrough(decompressor).getReader();

      const chunks: Uint8Array[] = [];
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        chunks.push(value);
      }

      const decompressed = new Uint8Array(chunks.reduce((acc, c) => acc + c.length, 0));
      let offset = 0;
      for (const chunk of chunks) {
        decompressed.set(chunk, offset);
        offset += chunk.length;
      }

      expect(decompressed).toEqual(original);
    });
  });
});
