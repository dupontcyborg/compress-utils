#ifndef COMPRESS_UTILS_C_H
#define COMPRESS_UTILS_C_H

#ifdef __cplusplus
extern "C" {
#endif

#include "algorithms.h"
#include "symbol_exports.h"

#include <stddef.h>
#include <stdint.h>

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
EXPORT_C int64_t compress_data(const uint8_t* data, size_t size, uint8_t** output,
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
EXPORT_C int64_t decompress_data(const uint8_t* data, size_t size, uint8_t** output,
                                 const Algorithm algorithm);

#ifdef __cplusplus
}
#endif

#endif  // COMPRESS_UTILS_C_H
