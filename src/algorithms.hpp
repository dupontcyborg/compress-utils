#ifndef ALGORITHMS_HPP_
#define ALGORITHMS_HPP_

namespace compression_utils {

/**
 * @brief Enum class that defines the available compression algorithms
 *
 * @note This gets overwritten by the build system to include only the algorithms that are available
 * and remove the preprocessor directives
 */
enum class Algorithm {
#ifdef INCLUDE_ZLIB
    ZLIB,
#endif
#ifdef INCLUDE_ZSTD
    ZSTD
#endif
};

}  // namespace compression_utils

#endif  // ALGORITHMS_HPP_