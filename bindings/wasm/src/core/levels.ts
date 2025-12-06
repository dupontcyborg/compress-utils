import type { Algorithm, CompressionLevel } from './types.js';
import { CompressError } from './errors.js';

/**
 * Named preset to numeric level mapping.
 */
const PRESET_TO_LEVEL: Record<'fast' | 'balanced' | 'best', number> = {
  fast: 1,
  balanced: 5,
  best: 10,
} as const;

/**
 * Algorithm-specific maximum compression levels.
 * These are the native maximum levels for each algorithm.
 */
const ALGORITHM_MAX_LEVELS: Record<Algorithm, number> = {
  zstd: 22,
  brotli: 11,
  zlib: 9,
  bz2: 9,
  lz4: 12, // LZ4 HC mode
  xz: 9,
} as const;

/**
 * Algorithm-specific minimum compression levels.
 * Most algorithms start at 1, but some (like brotli) start at 0.
 */
const ALGORITHM_MIN_LEVELS: Record<Algorithm, number> = {
  zstd: 1,
  brotli: 0,
  zlib: 1,
  bz2: 1,
  lz4: 1,
  xz: 0,
} as const;

/**
 * Default compression level (unified scale 1-10).
 */
export const DEFAULT_LEVEL = 5;

/**
 * Validates and normalizes a compression level to a numeric value (1-10).
 *
 * @param level - The compression level to validate
 * @param algorithm - The algorithm name (for error messages)
 * @returns Normalized numeric level (1-10)
 * @throws CompressError if level is invalid
 */
export function normalizeLevel(
  level: CompressionLevel | undefined,
  algorithm: Algorithm
): number {
  if (level === undefined) {
    return DEFAULT_LEVEL;
  }

  // Handle named presets
  if (typeof level === 'string') {
    const numericLevel = PRESET_TO_LEVEL[level];
    if (numericLevel === undefined) {
      throw CompressError.invalidLevel(algorithm, level);
    }
    return numericLevel;
  }

  // Handle numeric levels
  if (typeof level === 'number') {
    if (!Number.isInteger(level) || level < 1 || level > 10) {
      throw CompressError.invalidLevel(algorithm, level);
    }
    return level;
  }

  throw CompressError.invalidLevel(algorithm, level);
}

/**
 * Maps a unified compression level (1-10) to an algorithm-specific level.
 *
 * The unified 1-10 scale is mapped linearly to each algorithm's native range:
 * - Level 1 → algorithm minimum
 * - Level 10 → algorithm maximum
 *
 * @param unifiedLevel - Unified compression level (1-10)
 * @param algorithm - Target compression algorithm
 * @returns Algorithm-specific compression level
 */
export function mapLevelToAlgorithm(
  unifiedLevel: number,
  algorithm: Algorithm
): number {
  const min = ALGORITHM_MIN_LEVELS[algorithm];
  const max = ALGORITHM_MAX_LEVELS[algorithm];

  // Linear interpolation from 1-10 to min-max
  // unifiedLevel 1 → min, unifiedLevel 10 → max
  const mapped = min + ((unifiedLevel - 1) / 9) * (max - min);

  // Round to nearest integer
  return Math.round(mapped);
}

/**
 * Gets the native level for an algorithm given a unified level or preset.
 *
 * @param level - Unified compression level (1-10 or preset)
 * @param algorithm - Target compression algorithm
 * @returns Algorithm-specific compression level
 */
export function getNativeLevel(
  level: CompressionLevel | undefined,
  algorithm: Algorithm
): number {
  const normalized = normalizeLevel(level, algorithm);
  return mapLevelToAlgorithm(normalized, algorithm);
}
