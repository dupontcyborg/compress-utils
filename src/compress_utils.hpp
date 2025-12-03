#ifndef COMPRESS_UTILS_HPP_
#define COMPRESS_UTILS_HPP_

/**
 * @file compress_utils.hpp
 * @brief Main header for the compress-utils library (OOP interface)
 *
 * This file provides the Compressor class for compression and decompression
 * operations using various algorithms (ZSTD, Brotli, zlib, XZ/LZMA).
 *
 * ## Thread Safety
 *
 * - **Compressor class**: NOT inherently thread-safe. If you share a Compressor
 *   instance between threads, you must provide external synchronization.
 *   Alternatively, create separate Compressor instances per thread.
 *
 * - **Functional API** (compress_utils_func.hpp): Thread-safe. Each call is
 *   stateless and can be safely called from multiple threads concurrently.
 *
 * - **Streaming API** (compress_utils_stream.hpp): NOT thread-safe. Each
 *   CompressStream/DecompressStream instance should be used by a single thread.
 *
 * ## Compression Levels
 *
 * All algorithms accept a unified compression level from 1-10:
 * - Level 1: Fastest compression, larger output
 * - Level 10: Slowest compression, smallest output
 * - Default: Level 3 (good balance)
 *
 * Level mappings per algorithm:
 * - ZSTD: 1-10 maps to native 2-22
 * - Brotli: 1-10 maps to native 0-11
 * - zlib: 1-10 maps to native 1-9 (levels 10 capped at 9)
 * - XZ/LZMA: 1-10 maps to native 0-9
 */

#include "algorithms.hpp"
#include "symbol_exports.hpp"

#include <cstdint>
#include <vector>

namespace compress_utils {

// OOP Interface

/**
 * @brief Compressor class that provides compression and decompression functionalities
 *
 * The class provides two methods, Compress and Decompress, that can be used to compress and
 * decompress data using a specified algorithm.
 *
 * @note This class is NOT thread-safe. For concurrent access, use separate instances
 *       per thread or provide external synchronization.
 */
class EXPORT Compressor {
   public:
    /**
     * @brief Construct a new Compressor object
     *
     * @param algorithm Compression algorithm to use
     */
    explicit Compressor(const Algorithm algorithm);

    /**
     * @brief Compresses the input data using the specified algorithm
     *
     * @param data Input data to compress
     * @param level Compression level (1 = fastest; 10 = smallest; default = 3)
     * @return std::vector<uint8_t> Compressed data
     * @throws std::runtime_error if the compression fails
     */
    std::vector<uint8_t> Compress(const std::vector<uint8_t>& data, int level = 3);

    /**
     * @brief Compresses the input data using the specified algorithm
     *
     * @param data Pointer to the input data
     * @param size Size of the input data
     * @param level Compression level (1 = fastest; 10 = smallest; default = 3)
     * @return std::vector<uint8_t>
     * @throws std::runtime_error if the compression fails
     */
    std::vector<uint8_t> Compress(const uint8_t* data, size_t size, int level = 3);

    /**
     * @brief Decompresses the input data using the specified algorithm
     *
     * @param data Input data to decompress
     * @return std::vector<uint8_t> Decompressed data
     * @throws std::runtime_error if the decompression fails
     */
    std::vector<uint8_t> Decompress(const std::vector<uint8_t>& data);

    /**
     * @brief Decompresses the input data using the specified algorithm
     *
     * @param data Pointer to the input data to decompress
     * @param size Size of the input data
     * @return std::vector<uint8_t> Decompressed data
     * @throws std::runtime_error if the decompression fails
     */
    std::vector<uint8_t> Decompress(const uint8_t* data, size_t size);

    /**
     * @brief Get the algorithm object
     *
     * @return Algorithm Compression algorithm
     */
    Algorithm algorithm() const;

   private:
    Algorithm algorithm_;
};

}  // namespace compress_utils

#endif  // COMPRESS_UTILS_HPP_