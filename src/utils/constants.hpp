#ifndef CONSTANTS_HPP_
#define CONSTANTS_HPP_

#include <stdexcept>

namespace compress_utils::internal {

// Minimum & maximum compression levels
constexpr int MIN_LEVEL = 1;
constexpr int MAX_LEVEL = 10;

/**
 * @brief Validates that the compression level is within the valid range
 * 
 * @param level Compression level to validate
 * @throws std::invalid_argument if the level is outside the valid range
 */
inline void ValidateLevel(int level) {
    // Validate that level is between 1 and 10
    if (level < MIN_LEVEL || level > MAX_LEVEL) {
        throw std::invalid_argument("Compression level must be between 1 and 10");
    }
}

}  // namespace compress_utils::internal

#endif  // CONSTANTS_HPP_