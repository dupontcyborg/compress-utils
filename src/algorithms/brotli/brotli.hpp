#ifndef COMPRESS_UTILS_BROTLI_HPP_
#define COMPRESS_UTILS_BROTLI_HPP_

#ifdef INCLUDE_BROTLI

#include <cstdint>
#include <span>
#include <vector>

namespace compress_utils::brotli {

/**
 * @brief Compresses the input data using Zstandard
 *
 * @param data Input data to compress
 * @param algorithm Compression algorithm to use
 * @param level Compression level (1 = fastest; 10 = smallest; default = 3)
 * @return std::vector<uint8_t> Compressed data
 */
std::vector<uint8_t> Compress(std::span<const uint8_t>& data, int level = 3);

/**
 * @brief Decompresses the input data using Zstandard
 *
 * @param data Input data to decompress
 * @param algorithm Compression algorithm to use
 * @return std::vector<uint8_t> Decompressed data
 */
std::vector<uint8_t> Decompress(std::span<const uint8_t>& data);

}  // namespace compress_utils::brotli

#endif  // INCLUDE_BROTLI

#endif  // COMPRESS_UTILS_BROTLI_HPP_