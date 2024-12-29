#include "utils/constants.hpp"
#include "zlib.hpp"
#include "zlib/zlib.h"

#include <stdexcept>
#include <string>

namespace compress_utils::zlib {

/**
 * @brief Translate the compression level to the zlib compression level
 *
 * @param level 1-10 compression level
 * @return int zlib compression level (1 = fastest, 9 = best compression, 6 = default)
 */
inline int GetCompressionLevel(int level) {
    // Validate that level is between 1 and 10
    internal::ValidateLevel(level);

    // Map 1-10 to zlib's 1-9 range
    if (level > 9) level = 9;

    return level;
}

std::vector<uint8_t> Compress(std::span<const uint8_t>& data, int level) {
    // Get zlib compression level (1-9)
    int zlib_level = GetCompressionLevel(level);

    // Calculate the maximum size of the compressed data
    uLongf max_compressed_size = compressBound(data.size());

    // Create a buffer to hold the compressed data
    std::vector<uint8_t> compressed_data(max_compressed_size);

    // Compress the data using zlib
    int result = compress2(compressed_data.data(), &max_compressed_size, data.data(), data.size(),
                           zlib_level);

    // Check if the compression was successful
    if (result != Z_OK) {
        throw std::runtime_error("zlib compression failed: " + std::to_string(result));
    }

    // Resize the compressed data buffer to the actual compressed size
    compressed_data.resize(max_compressed_size);

    return compressed_data;
}

std::vector<uint8_t> Decompress(std::span<const uint8_t>& data) {
    // Start with a buffer 4 times the size of the compressed data
    size_t initial_size = data.size() * 4;
    std::vector<uint8_t> decompressed_data(initial_size);

    // Decompress the data using zlib
    uLongf decompressed_size = decompressed_data.size();
    int result = uncompress(decompressed_data.data(), &decompressed_size, data.data(), data.size());

    // If the buffer was too small, keep resizing it and try again (max 4 times)
    int retries = 10;
    while (result == Z_BUF_ERROR && retries-- > 0) {
        // Resize the buffer (double the size)
        decompressed_data.resize(decompressed_data.size() * 2);
        decompressed_size = decompressed_data.size();

        // Try again with the larger buffer
        result = uncompress(decompressed_data.data(), &decompressed_size, data.data(), data.size());
    }

    // Check if the decompression was successful
    if (result != Z_OK) {
        if (result == Z_BUF_ERROR) {
            throw std::runtime_error(
                "zlib decompression failed: Buffer too small after multiple retries.");
        } else {
            throw std::runtime_error("zlib decompression failed: " + std::to_string(result));
        }
    }

    // Resize the buffer to the actual decompressed size
    decompressed_data.resize(decompressed_size);

    return decompressed_data;
}

}  // namespace compress_utils::zlib