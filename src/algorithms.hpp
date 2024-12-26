#ifndef ALGORITHMS_HPP_
#define ALGORITHMS_HPP_

namespace compress_utils {

/**
 * @brief Enum class that defines the available compression algorithms
 *
 * @note A copy of this enum is present in `algorithms.hpp.in` which gets overwritten by the build
 * system to include only the algorithms that are available and remove the preprocessor directives
 */
enum class Algorithm {
#ifdef INCLUDE_BROTLI
    BROTLI,
#endif
#ifdef INCLUDE_XZ
    LZMA,
    XZ,
#endif
#ifdef INCLUDE_ZLIB
    ZLIB,
#endif
#ifdef INCLUDE_ZSTD
    ZSTD
#endif
};

}  // namespace compress_utils

#endif  // ALGORITHMS_HPP_