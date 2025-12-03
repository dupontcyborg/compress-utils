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
 * @return int64_t Compressed data size, or -1 if an error occurred.
 *         On error, call compress_utils_last_error() for details.
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
 * @return int64_t Decompressed data size, or -1 if an error occurred.
 *         On error, call compress_utils_last_error() for details.
 */
EXPORT_C int64_t decompress_data(const uint8_t* data, size_t size, uint8_t** output,
                                 const Algorithm algorithm);

/**
 * @brief Get the last error message from a failed compression/decompression operation
 *
 * This function returns a pointer to a thread-local error message buffer.
 * The returned string is valid until the next call to compress_data() or
 * decompress_data() on the same thread.
 *
 * @return const char* Error message, or empty string if no error occurred
 */
EXPORT_C const char* compress_utils_last_error(void);

/**
 * @brief Clear the last error message
 */
EXPORT_C void compress_utils_clear_error(void);

#ifdef __cplusplus
}
#endif

#endif  // COMPRESS_UTILS_C_H
