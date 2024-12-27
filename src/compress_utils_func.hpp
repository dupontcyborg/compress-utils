#ifndef COMPRESS_UTILS_FUNC_HPP_
#define COMPRESS_UTILS_FUNC_HPP_

#include "algorithms.hpp"
#include "symbol_exports.hpp"

#include <cstdint>
#include <vector>

namespace compress_utils {

// Functional Interface

/**
 * @brief Compresses the input data using the specified algorithm
 *
 * @param data Input data to compress
 * @param algorithm Compression algorithm to use
 * @param level Compression level (1 = fastest; 10 = smallest; default = 3)
 * @return std::vector<uint8_t> Compressed data
 *
 */
EXPORT std::vector<uint8_t> Compress(const std::vector<uint8_t>& data, Algorithm algorithm,
                                     int level = 3);

/**
 * @brief Compresses the input data using the specified algorithm
 *
 * @param data Pointer to the input data
 * @param size Size of the input data
 * @param algorithm Compression algorithm to use
 * @param level Compression level (1 = fastest; 10 = smallest; default = 3)
 * @return std::vector<uint8_t> Compressed data
 */
EXPORT std::vector<uint8_t> Compress(const uint8_t* data, size_t size, Algorithm algorithm,
                                     int level = 3);
/**
 * @brief Decompresses the input data using the specified algorithm
 *
 * @param data Input data to decompress
 * @param algorithm Compression algorithm to use
 * @return std::vector<uint8_t> Decompressed data
 *
 * @todo Make this smarter by trying to auto-detect the compressed format?
 */
EXPORT std::vector<uint8_t> Decompress(const std::vector<uint8_t>& data, Algorithm algorithm);

/**
 * @brief Decompresses the input data using the specified algorithm
 *
 * @param data Pointer to the input data
 * @param size Size of the input data
 * @param algorithm Compression algorithm to use
 * @return std::vector<uint8_t> Decompressed data
 */
EXPORT std::vector<uint8_t> Decompress(const uint8_t* data, size_t size, Algorithm algorithm);

}  // namespace compress_utils

#endif  // COMPRESS_UTILS_FUNC_HPP_