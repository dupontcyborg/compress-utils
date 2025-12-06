/**
 * Core Utilities Tests
 */

import { describe, it, expect } from 'vitest';
import {
  normalizeInput,
  normalizeDecompressInput,
  encodeString,
  decodeString,
  concatUint8Arrays,
  estimateCompressBufferSize,
  estimateDecompressBufferSize,
} from '../../src/core/utils.js';
import {
  normalizeLevel,
  getNativeLevel,
  mapLevelToAlgorithm,
} from '../../src/core/levels.js';
import { CompressError } from '../../src/core/errors.js';

describe('utils', () => {
  describe('normalizeInput', () => {
    it('should pass through Uint8Array', () => {
      const input = new Uint8Array([1, 2, 3]);
      const result = normalizeInput(input);
      expect(result).toBe(input);
    });

    it('should convert ArrayBuffer to Uint8Array', () => {
      const buffer = new ArrayBuffer(3);
      new Uint8Array(buffer).set([1, 2, 3]);
      const result = normalizeInput(buffer);
      expect(result).toBeInstanceOf(Uint8Array);
      expect(Array.from(result)).toEqual([1, 2, 3]);
    });

    it('should encode string to Uint8Array', () => {
      const result = normalizeInput('hello');
      expect(result).toBeInstanceOf(Uint8Array);
      expect(new TextDecoder().decode(result)).toBe('hello');
    });
  });

  describe('normalizeDecompressInput', () => {
    it('should pass through Uint8Array', () => {
      const input = new Uint8Array([1, 2, 3]);
      const result = normalizeDecompressInput(input);
      expect(result).toBe(input);
    });

    it('should convert ArrayBuffer to Uint8Array', () => {
      const buffer = new ArrayBuffer(3);
      new Uint8Array(buffer).set([1, 2, 3]);
      const result = normalizeDecompressInput(buffer);
      expect(result).toBeInstanceOf(Uint8Array);
    });
  });

  describe('encodeString', () => {
    it('should encode UTF-8 string', () => {
      const result = encodeString('hello');
      expect(new TextDecoder().decode(result)).toBe('hello');
    });

    it('should encode unicode characters', () => {
      const result = encodeString('你好世界');
      expect(new TextDecoder().decode(result)).toBe('你好世界');
    });
  });

  describe('decodeString', () => {
    it('should decode UTF-8 bytes', () => {
      const bytes = new TextEncoder().encode('hello');
      const result = decodeString(bytes);
      expect(result).toBe('hello');
    });
  });

  describe('concatUint8Arrays', () => {
    it('should concatenate arrays', () => {
      const a = new Uint8Array([1, 2]);
      const b = new Uint8Array([3, 4]);
      const c = new Uint8Array([5]);
      const result = concatUint8Arrays([a, b, c]);
      expect(Array.from(result)).toEqual([1, 2, 3, 4, 5]);
    });

    it('should handle empty array', () => {
      const result = concatUint8Arrays([]);
      expect(result.length).toBe(0);
    });
  });

  describe('estimateCompressBufferSize', () => {
    it('should return reasonable size', () => {
      const result = estimateCompressBufferSize(1000);
      expect(result).toBeGreaterThanOrEqual(1000);
    });
  });

  describe('estimateDecompressBufferSize', () => {
    it('should return at least 4x compressed size', () => {
      const result = estimateDecompressBufferSize(1000);
      expect(result).toBeGreaterThanOrEqual(4000);
    });
  });
});

describe('levels', () => {
  describe('normalizeLevel', () => {
    it('should return default level for undefined', () => {
      const result = normalizeLevel(undefined, 'zstd');
      expect(result).toBe(5);
    });

    it('should convert preset to numeric', () => {
      expect(normalizeLevel('fast', 'zstd')).toBe(1);
      expect(normalizeLevel('balanced', 'zstd')).toBe(5);
      expect(normalizeLevel('best', 'zstd')).toBe(10);
    });

    it('should pass through valid numeric levels', () => {
      for (let i = 1; i <= 10; i++) {
        expect(normalizeLevel(i as 1, 'zstd')).toBe(i);
      }
    });

    it('should throw for invalid levels', () => {
      expect(() => normalizeLevel(0 as 1, 'zstd')).toThrow(CompressError);
      expect(() => normalizeLevel(11 as 1, 'zstd')).toThrow(CompressError);
      expect(() => normalizeLevel('invalid' as 'fast', 'zstd')).toThrow(CompressError);
    });
  });

  describe('mapLevelToAlgorithm', () => {
    it('should map level 1 to algorithm minimum', () => {
      expect(mapLevelToAlgorithm(1, 'zstd')).toBe(1);
      expect(mapLevelToAlgorithm(1, 'brotli')).toBe(0);
    });

    it('should map level 10 to algorithm maximum', () => {
      expect(mapLevelToAlgorithm(10, 'zstd')).toBe(22);
      expect(mapLevelToAlgorithm(10, 'brotli')).toBe(11);
      expect(mapLevelToAlgorithm(10, 'zlib')).toBe(9);
    });
  });

  describe('getNativeLevel', () => {
    it('should convert preset to native level', () => {
      const fastZstd = getNativeLevel('fast', 'zstd');
      const bestZstd = getNativeLevel('best', 'zstd');
      expect(fastZstd).toBeLessThan(bestZstd);
    });
  });
});

describe('errors', () => {
  describe('CompressError', () => {
    it('should have correct properties', () => {
      const error = CompressError.invalidInput('zstd', 'test detail');
      expect(error.name).toBe('CompressError');
      expect(error.code).toBe('INVALID_INPUT');
      expect(error.algorithm).toBe('zstd');
      expect(error.message).toContain('zstd');
      expect(error.message).toContain('test detail');
    });

    it('should create specific error types', () => {
      expect(CompressError.invalidLevel('zstd', 100).code).toBe('INVALID_LEVEL');
      expect(CompressError.wasmOom('zstd').code).toBe('WASM_OOM');
      expect(CompressError.compressionFailed('zstd').code).toBe('COMPRESSION_FAILED');
    });
  });
});
