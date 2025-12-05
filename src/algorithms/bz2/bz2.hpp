#ifndef COMPRESS_UTILS_BZ2_HPP_
#define COMPRESS_UTILS_BZ2_HPP_

#ifdef INCLUDE_BZ2

#include <cstdint>
#include <span>
#include <vector>

namespace compress_utils::bz2 {

/**
 * @brief Compresses the input data using bzip2
 *
 * @param data Input data to compress
 * @param level Compression level (1 = fastest; 10 = smallest; default = 3)
 * @return std::vector<uint8_t> Compressed data
 */
std::vector<uint8_t> Compress(std::span<const uint8_t> data, int level = 3);

/**
 * @brief Decompresses the input data using bzip2
 *
 * @param data Input data to decompress
 * @return std::vector<uint8_t> Decompressed data
 */
std::vector<uint8_t> Decompress(std::span<const uint8_t> data);

}  // namespace compress_utils::bz2

#endif  // INCLUDE_BZ2

#endif  // COMPRESS_UTILS_BZ2_HPP_
