#ifndef CONSTANTS_HPP_
#define CONSTANTS_HPP_

#include <cstddef>
#include <stdexcept>

namespace compress_utils::internal {

// ============================================================================
// Compression Level Constants
// ============================================================================

// User-facing compression level range (1-10)
constexpr int MIN_LEVEL = 1;
constexpr int MAX_LEVEL = 10;

// Algorithm-specific maximum compression levels (for level mapping)
constexpr int ZSTD_MAX_LEVEL = 22;      // ZSTD supports 1-22
constexpr int BROTLI_MAX_LEVEL = 11;    // Brotli supports 0-11
constexpr int XZ_MAX_LEVEL = 9;         // XZ/LZMA supports 0-9
constexpr int ZLIB_MAX_LEVEL = 9;       // zlib supports 1-9

// ============================================================================
// Buffer Size Constants
// ============================================================================

// Default buffer size for streaming operations (64 KB)
constexpr size_t DEFAULT_BUFFER_SIZE = 64 * 1024;

// Initial buffer size for decompression (minimum)
constexpr size_t MIN_DECOMP_BUFFER_SIZE = 16 * 1024;

// Initial decompression buffer multiplier (relative to compressed size)
constexpr size_t DECOMP_BUFFER_MULTIPLIER_ZLIB = 4;
constexpr size_t DECOMP_BUFFER_MULTIPLIER_BROTLI = 2;
constexpr size_t DECOMP_BUFFER_MULTIPLIER_XZ = 2;

// Buffer growth factor when resizing
constexpr size_t BUFFER_GROWTH_FACTOR = 2;

// ============================================================================
// Retry Constants
// ============================================================================

// Maximum retries for zlib decompression buffer resizing
constexpr int MAX_DECOMP_RETRIES = 10;

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Validates that the compression level is within the valid range
 *
 * @param level Compression level to validate
 * @throws std::invalid_argument if the level is outside the valid range
 */
inline void ValidateLevel(int level) {
    if (level < MIN_LEVEL || level > MAX_LEVEL) {
        throw std::invalid_argument("Compression level must be between 1 and 10");
    }
}

/**
 * @brief Map user level (1-10) to algorithm-specific level
 *
 * @param level User level (1-10)
 * @param algo_max Maximum level for the algorithm
 * @return Mapped level for the algorithm
 */
inline int MapLevel(int level, int algo_max) {
    return (level * algo_max) / MAX_LEVEL;
}

/**
 * @brief Map user level (1-10) to algorithm-specific level (0-based)
 *
 * For algorithms like XZ that use 0-9 range
 * @param level User level (1-10)
 * @return Mapped level (0 to algo_max)
 */
inline int MapLevelZeroBased(int level) {
    return level - 1;
}

}  // namespace compress_utils::internal

#endif  // CONSTANTS_HPP_
