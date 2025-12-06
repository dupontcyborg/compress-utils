/**
 * Browser Integration Tests
 *
 * These tests verify that the WASM compression works correctly
 * in actual browser environments using Playwright.
 *
 * Prerequisites:
 *   - WASM modules must be built: npm run build:wasm
 *   - TypeScript must be compiled: npm run build:ts
 *
 * Run with: npx playwright test
 */

import { test, expect } from '@playwright/test';

const ALGORITHMS = ['zstd', 'brotli', 'zlib', 'bz2', 'lz4', 'xz'] as const;

test.describe('Browser Compression Tests', () => {
  test.beforeEach(async ({ page }) => {
    await page.goto('/');
  });

  test('should load and execute WASM compression', async ({ page }) => {
    // This test uses the test page which bundles the library
    const result = await page.evaluate(async () => {
      // @ts-expect-error - injected by test page
      const { compress, decompress } = window.compressUtils.zstd;

      const original = new TextEncoder().encode('Hello, Browser World!');
      const compressed = await compress(original);
      const decompressed = await decompress(compressed);

      return {
        originalLength: original.length,
        compressedLength: compressed.length,
        decompressedLength: decompressed.length,
        roundtripSuccess: JSON.stringify(Array.from(decompressed)) ===
                          JSON.stringify(Array.from(original)),
      };
    });

    expect(result.roundtripSuccess).toBe(true);
    expect(result.compressedLength).toBeLessThan(result.originalLength * 2);
    expect(result.decompressedLength).toBe(result.originalLength);
  });

  test('should handle string compression', async ({ page }) => {
    const result = await page.evaluate(async () => {
      // @ts-expect-error - injected by test page
      const { compress, decompressToString } = window.compressUtils.zstd;

      const original = 'Hello, Browser World! 🌍';
      const compressed = await compress(original);
      const decompressed = await decompressToString(compressed);

      return {
        original,
        decompressed,
        success: original === decompressed,
      };
    });

    expect(result.success).toBe(true);
    expect(result.decompressed).toBe(result.original);
  });

  test('should work with Web Streams API', async ({ page }) => {
    const result = await page.evaluate(async () => {
      // @ts-expect-error - injected by test page
      const { CompressStream, DecompressStream } = window.compressUtils.zstd;

      const original = new TextEncoder().encode('Streaming data test!');

      // Create a readable stream from the data
      const inputStream = new ReadableStream({
        start(controller) {
          controller.enqueue(original);
          controller.close();
        },
      });

      // Compress via stream
      const compressor = new CompressStream({ level: 5 });
      const compressedReader = inputStream.pipeThrough(compressor).getReader();

      const compressedChunks: Uint8Array[] = [];
      while (true) {
        const { done, value } = await compressedReader.read();
        if (done) break;
        compressedChunks.push(value);
      }

      // Combine chunks
      const totalLength = compressedChunks.reduce((acc, c) => acc + c.length, 0);
      const compressed = new Uint8Array(totalLength);
      let offset = 0;
      for (const chunk of compressedChunks) {
        compressed.set(chunk, offset);
        offset += chunk.length;
      }

      // Decompress via stream
      const decompressInputStream = new ReadableStream({
        start(controller) {
          controller.enqueue(compressed);
          controller.close();
        },
      });

      const decompressor = new DecompressStream();
      const decompressedReader = decompressInputStream.pipeThrough(decompressor).getReader();

      const decompressedChunks: Uint8Array[] = [];
      while (true) {
        const { done, value } = await decompressedReader.read();
        if (done) break;
        decompressedChunks.push(value);
      }

      const decompressedLength = decompressedChunks.reduce((acc, c) => acc + c.length, 0);
      const decompressed = new Uint8Array(decompressedLength);
      offset = 0;
      for (const chunk of decompressedChunks) {
        decompressed.set(chunk, offset);
        offset += chunk.length;
      }

      return {
        originalLength: original.length,
        compressedLength: compressed.length,
        decompressedLength: decompressed.length,
        success: JSON.stringify(Array.from(decompressed)) ===
                 JSON.stringify(Array.from(original)),
      };
    });

    expect(result.success).toBe(true);
  });

  test('should handle errors gracefully', async ({ page }) => {
    const result = await page.evaluate(async () => {
      // @ts-expect-error - injected by test page
      const { decompress, DecompressError } = window.compressUtils.zstd;

      try {
        // Invalid data should throw
        await decompress(new Uint8Array([1, 2, 3, 4, 5]));
        return { threw: false };
      } catch (e) {
        return {
          threw: true,
          isDecompressError: e instanceof Error && e.name === 'DecompressError',
          message: (e as Error).message,
        };
      }
    });

    expect(result.threw).toBe(true);
    expect(result.isDecompressError).toBe(true);
  });
});
