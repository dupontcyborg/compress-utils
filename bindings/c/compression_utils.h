#ifndef COMPRESSION_UTILS_C_H
#define COMPRESSION_UTILS_C_H

#ifdef __cplusplus
extern "C" {
#endif

#include "algorithms.h"

#include <stddef.h>
#include <stdint.h>

// Platform-specific macros for exporting and importing symbols
#if defined(_WIN32) || defined(_WIN64)
#if defined(COMPRESSION_UTILS_EXPORTS)
#define EXPORT __declspec(dllexport)  // Export when building the DLL
#else
#define EXPORT __declspec(dllimport)  // Import when using the DLL
#endif
#else
#define EXPORT __attribute__((visibility("default")))  // For non-Windows platforms (Linux/macOS)
#endif

/**
 * @brief Compresses the input data using the specified algorithm
 *
 * @param data Input data to compress
 * @param size Size of the input data
 * @param output Double pointer where output buffer will be allocated
 * @param algorithm Compression algorithm to use
 * @param level Compression level (1 = fastest; 10 = smallest)
 * @return int64_t Compressed data size, or -1 if an error occurred
 */
EXPORT int64_t compress(const uint8_t* data, size_t size, uint8_t** output,
                        const Algorithm algorithm, int level);

/**
 * @brief Decompresses the input data using the specified algorithm
 *
 * @param data Input data to decompress
 * @param size Size of the input data
 * @param output Double pointer where output buffer will be allocated
 * @param algorithm Compression algorithm to use
 * @return int64_t Compressed data size, or -1 if an error occurred
 */
EXPORT int64_t decompress(const uint8_t* data, size_t size, uint8_t** output,
                          const Algorithm algorithm);

#ifdef __cplusplus
}
#endif

#endif  // COMPRESSION_UTILS_C_H
