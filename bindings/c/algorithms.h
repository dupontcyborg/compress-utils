#ifndef ALGORITHMS_H_
#define ALGORITHMS_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enum class that defines the available compression algorithms
 *
 * @note A copy of this enum is present in `algorithms.h.in` which gets overwritten by the build
 * system to include only the algorithms that are available and remove the preprocessor directives
 */
typedef enum {
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
} Algorithm;

#ifdef __cplusplus
}
#endif

#endif  // ALGORITHMS_H_