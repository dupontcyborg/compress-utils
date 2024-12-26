#include "utils/constants.hpp"
#include "zstd.hpp"
#include "zstd/zstd.h"

#include <stdexcept>

namespace compress_utils::zstd {

/**
 * @brief Translate the compression level to the ZSTD compression level
 *
 * @param level 1-10 compression level
 * @return int ZSTD compression level
 */
inline int GetCompressionLevel(int level) {
    // Validate that level is between 1 and 10
    internal::ValidateLevel(level);

    // ZSTD compression levels are 1-22, so we need to scale the input level
    // from 1-10 to 1-22; so 1 -> 1, 10 -> 22
    const size_t zstd_max_level = 22;
    level = (level * zstd_max_level) / internal::MAX_LEVEL;

    return level;
}

std::vector<uint8_t> Compress(std::span<const uint8_t>& data, int level) {
    // Get Zstd compression level
    int zstd_level = GetCompressionLevel(level);

    // Calculate the maximum size of the compressed data
    size_t max_compressed_size = ZSTD_compressBound(data.size());

    // Create a buffer to hold the compressed data
    std::vector<uint8_t> compressed_data(max_compressed_size);

    // Compress the data using Zstd
    size_t compressed_size = ZSTD_compress(compressed_data.data(), max_compressed_size, data.data(),
                                           data.size(), zstd_level);

    // Check if the compression was successful
    if (ZSTD_isError(compressed_size)) {
        throw std::runtime_error("Zstd compression failed: " +
                                 std::string(ZSTD_getErrorName(compressed_size)));
    }

    // Resize the compressed data buffer to the actual compressed size
    compressed_data.resize(compressed_size);

    return compressed_data;
}

std::vector<uint8_t> Decompress(std::span<const uint8_t>& data) {
    // Get the decompressed size (Zstd allows embedding this information in the compressed data)
    unsigned long long decompressed_size = ZSTD_getFrameContentSize(data.data(), data.size());

    // Check if the size is valid
    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
        throw std::runtime_error("Zstd decompression failed: Not a valid compressed frame.");
    } else if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        throw std::runtime_error("Zstd decompression failed: Original size unknown.");
    }

    // Create a buffer to hold the decompressed data
    std::vector<uint8_t> decompressed_data(decompressed_size);

    // Decompress the data
    size_t actual_decompressed_size =
        ZSTD_decompress(decompressed_data.data(), decompressed_size, data.data(), data.size());

    // Check if the decompression was successful
    if (ZSTD_isError(actual_decompressed_size)) {
        throw std::runtime_error("Zstd decompression failed: " +
                                 std::string(ZSTD_getErrorName(actual_decompressed_size)));
    }

    return decompressed_data;
}

}  // namespace compress_utils::zstd