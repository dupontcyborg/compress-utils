#ifndef COMPRESSION_UTILS_FUNC_HPP_
#define COMPRESSION_UTILS_FUNC_HPP_

#include "algorithms.hpp"

#include <string>
#include <vector>

namespace compression_utils {

// Functional Interface

/**
 * @brief Compresses the input data using the specified algorithm
 *
 * @param data Input data to compress
 * @param algorithm Compression algorithm to use
 * @param level Compression level (1 = fastest; 10 = smallest; default = 3)
 * @return std::vector<uint8_t> Compressed data
 *
 * @todo Make this auto-default to ZSTD?
 */
std::vector<uint8_t> Compress(const std::vector<uint8_t>& data, const Algorithm algorithm,
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
std::vector<uint8_t> Decompress(const std::vector<uint8_t>& data, const Algorithm algorithm);

}  // namespace compression_utils

#endif  // COMPRESSION_UTILS_FUNC_HPP_