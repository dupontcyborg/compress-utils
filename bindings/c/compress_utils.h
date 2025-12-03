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

/* ============================================================================
 * Streaming API
 * ============================================================================
 *
 * The streaming API allows compressing/decompressing data in chunks without
 * loading the entire dataset into memory. Useful for large files or streaming.
 *
 * Thread-safety: Stream objects are NOT thread-safe. Use separate instances
 * for different threads.
 *
 * Example usage (compression):
 *   CompressStream* stream = compress_stream_create(ZSTD, 5);
 *   while (has_more_data) {
 *       uint8_t* output;
 *       int64_t size = compress_stream_write(stream, chunk, chunk_size, &output);
 *       if (size > 0) { write_output(output, size); free(output); }
 *   }
 *   uint8_t* final;
 *   int64_t final_size = compress_stream_finish(stream, &final);
 *   if (final_size > 0) { write_output(final, final_size); free(final); }
 *   compress_stream_destroy(stream);
 */

/** @brief Opaque handle for compression stream */
typedef struct CompressStream CompressStream;

/** @brief Opaque handle for decompression stream */
typedef struct DecompressStream DecompressStream;

/**
 * @brief Create a new compression stream
 *
 * @param algorithm Compression algorithm to use
 * @param level Compression level (1 = fastest; 10 = smallest)
 * @return CompressStream* Stream handle, or NULL on error.
 *         On error, call compress_utils_last_error() for details.
 *         Caller must destroy with compress_stream_destroy().
 */
EXPORT_C CompressStream* compress_stream_create(Algorithm algorithm, int level);

/**
 * @brief Feed data to the compression stream
 *
 * @param stream Compression stream handle
 * @param data Input data chunk to compress
 * @param size Size of the input data
 * @param output Double pointer where output buffer will be allocated (may be NULL if no output)
 * @return int64_t Compressed output size (may be 0 if buffering), or -1 on error.
 *         Caller must free() the output buffer if size > 0.
 */
EXPORT_C int64_t compress_stream_write(CompressStream* stream, const uint8_t* data,
                                       size_t size, uint8_t** output);

/**
 * @brief Finish compression and flush remaining data
 *
 * Must be called after all input has been fed. After calling this,
 * the stream cannot be used again (except to destroy it).
 *
 * @param stream Compression stream handle
 * @param output Double pointer where final output buffer will be allocated
 * @return int64_t Final compressed output size, or -1 on error.
 *         Caller must free() the output buffer if size > 0.
 */
EXPORT_C int64_t compress_stream_finish(CompressStream* stream, uint8_t** output);

/**
 * @brief Check if the compression stream has been finished
 *
 * @param stream Compression stream handle
 * @return int 1 if finished, 0 if not finished, -1 on error
 */
EXPORT_C int compress_stream_is_finished(const CompressStream* stream);

/**
 * @brief Get the algorithm used by the compression stream
 *
 * @param stream Compression stream handle
 * @return Algorithm The compression algorithm, or -1 on error
 */
EXPORT_C int compress_stream_algorithm(const CompressStream* stream);

/**
 * @brief Destroy a compression stream and free resources
 *
 * @param stream Compression stream handle (may be NULL)
 */
EXPORT_C void compress_stream_destroy(CompressStream* stream);

/**
 * @brief Create a new decompression stream
 *
 * @param algorithm Decompression algorithm to use
 * @return DecompressStream* Stream handle, or NULL on error.
 *         On error, call compress_utils_last_error() for details.
 *         Caller must destroy with decompress_stream_destroy().
 */
EXPORT_C DecompressStream* decompress_stream_create(Algorithm algorithm);

/**
 * @brief Feed compressed data to the decompression stream
 *
 * @param stream Decompression stream handle
 * @param data Compressed data chunk to decompress
 * @param size Size of the compressed data
 * @param output Double pointer where output buffer will be allocated (may be NULL if no output)
 * @return int64_t Decompressed output size (may be 0 if buffering), or -1 on error.
 *         Caller must free() the output buffer if size > 0.
 */
EXPORT_C int64_t decompress_stream_write(DecompressStream* stream, const uint8_t* data,
                                         size_t size, uint8_t** output);

/**
 * @brief Finish decompression and verify stream completeness
 *
 * Must be called after all compressed input has been fed. Verifies that
 * the compressed stream was complete.
 *
 * @param stream Decompression stream handle
 * @param output Double pointer where final output buffer will be allocated
 * @return int64_t Final decompressed output size, or -1 on error.
 *         Caller must free() the output buffer if size > 0.
 */
EXPORT_C int64_t decompress_stream_finish(DecompressStream* stream, uint8_t** output);

/**
 * @brief Check if the decompression stream has been finished
 *
 * @param stream Decompression stream handle
 * @return int 1 if finished, 0 if not finished, -1 on error
 */
EXPORT_C int decompress_stream_is_finished(const DecompressStream* stream);

/**
 * @brief Get the algorithm used by the decompression stream
 *
 * @param stream Decompression stream handle
 * @return Algorithm The decompression algorithm, or -1 on error
 */
EXPORT_C int decompress_stream_algorithm(const DecompressStream* stream);

/**
 * @brief Destroy a decompression stream and free resources
 *
 * @param stream Decompression stream handle (may be NULL)
 */
EXPORT_C void decompress_stream_destroy(DecompressStream* stream);

#ifdef __cplusplus
}
#endif

#endif  // COMPRESS_UTILS_C_H
