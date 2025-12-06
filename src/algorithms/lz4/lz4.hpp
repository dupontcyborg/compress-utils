#ifndef COMPRESS_UTILS_LZ4_HPP_
#define COMPRESS_UTILS_LZ4_HPP_

#ifdef INCLUDE_LZ4

#include <cstdint>
#include <span>
#include <vector>

namespace compress_utils::lz4 {

/**
 * @brief Compresses the input data using LZ4
 *
 * For levels 1-3, uses LZ4 fast compression with decreasing acceleration.
 * For levels 4-10, uses LZ4 HC (high compression) mode with increasing compression levels.
 *
 * @param data Input data to compress
 * @param level Compression level (1 = fastest; 10 = smallest; default = 3)
 * @return std::vector<uint8_t> Compressed data
 */
std::vector<uint8_t> Compress(std::span<const uint8_t> data, int level = 3);

/**
 * @brief Decompresses the input data using LZ4
 *
 * @param data Input data to decompress
 * @return std::vector<uint8_t> Decompressed data
 */
std::vector<uint8_t> Decompress(std::span<const uint8_t> data);

}  // namespace compress_utils::lz4

#endif  // INCLUDE_LZ4

#endif  // COMPRESS_UTILS_LZ4_HPP_
