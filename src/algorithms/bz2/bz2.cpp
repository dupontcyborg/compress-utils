#include "utils/constants.hpp"
#include "bz2.hpp"
#include "bz2/bzlib.h"

#include <stdexcept>

namespace compress_utils::bz2 {

/**
 * @brief Translate the compression level to the bzip2 compression level
 *
 * @param level 1-10 compression level
 * @return int bzip2 compression level (1-9)
 */
inline int GetCompressionLevel(int level) {
    internal::ValidateLevel(level);
    // bzip2 compression levels are 1-9, so we scale 1-10 to 1-9
    return internal::MapLevel(level, internal::BZ2_MAX_LEVEL);
}

std::vector<uint8_t> Compress(std::span<const uint8_t> data, int level) {
    // Get bzip2 compression level
    int bz2_level = GetCompressionLevel(level);

    // Calculate initial buffer size (bzip2 can expand data slightly in worst case)
    // Use a conservative estimate: original size + 1% + 600 bytes
    unsigned int max_compressed_size = static_cast<unsigned int>(data.size() + (data.size() / 100) + 600);

    // Create a buffer to hold the compressed data
    std::vector<uint8_t> compressed_data(max_compressed_size);

    // Compress the data using bzip2
    unsigned int compressed_size = max_compressed_size;
    int result = BZ2_bzBuffToBuffCompress(
        reinterpret_cast<char*>(compressed_data.data()),
        &compressed_size,
        const_cast<char*>(reinterpret_cast<const char*>(data.data())),
        static_cast<unsigned int>(data.size()),
        bz2_level,
        0,  // verbosity
        0   // workFactor (default)
    );

    // Check if the compression was successful
    if (result != BZ_OK) {
        std::string error_msg;
        switch (result) {
            case BZ_MEM_ERROR:
                error_msg = "memory allocation failed";
                break;
            case BZ_OUTBUFF_FULL:
                error_msg = "output buffer full";
                break;
            default:
                error_msg = "error code " + std::to_string(result);
                break;
        }
        throw std::runtime_error("bzip2 compression failed: " + error_msg);
    }

    // Resize the compressed data buffer to the actual compressed size
    compressed_data.resize(compressed_size);

    return compressed_data;
}

std::vector<uint8_t> Decompress(std::span<const uint8_t> data) {
    // Start with an initial estimate for the decompressed size
    // bzip2 typically achieves 10:1 compression, so we start with 10x the compressed size
    size_t estimated_size = data.size() * internal::DECOMP_BUFFER_MULTIPLIER_BZ2;
    if (estimated_size < internal::MIN_DECOMP_BUFFER_SIZE) {
        estimated_size = internal::MIN_DECOMP_BUFFER_SIZE;
    }

    std::vector<uint8_t> decompressed_data(estimated_size);

    // Try decompression, growing buffer if needed
    for (int retry = 0; retry < internal::MAX_DECOMP_RETRIES; ++retry) {
        unsigned int decompressed_size = static_cast<unsigned int>(decompressed_data.size());
        int result = BZ2_bzBuffToBuffDecompress(
            reinterpret_cast<char*>(decompressed_data.data()),
            &decompressed_size,
            const_cast<char*>(reinterpret_cast<const char*>(data.data())),
            static_cast<unsigned int>(data.size()),
            0,  // small (use less memory if 1)
            0   // verbosity
        );

        if (result == BZ_OK) {
            decompressed_data.resize(decompressed_size);
            return decompressed_data;
        } else if (result == BZ_OUTBUFF_FULL) {
            // Buffer too small, grow and retry
            decompressed_data.resize(decompressed_data.size() * internal::BUFFER_GROWTH_FACTOR);
        } else {
            std::string error_msg;
            switch (result) {
                case BZ_MEM_ERROR:
                    error_msg = "memory allocation failed";
                    break;
                case BZ_DATA_ERROR:
                    error_msg = "data integrity error (corrupted data)";
                    break;
                case BZ_DATA_ERROR_MAGIC:
                    error_msg = "invalid bzip2 data (bad magic number)";
                    break;
                case BZ_UNEXPECTED_EOF:
                    error_msg = "unexpected end of compressed data";
                    break;
                default:
                    error_msg = "error code " + std::to_string(result);
                    break;
            }
            throw std::runtime_error("bzip2 decompression failed: " + error_msg);
        }
    }

    throw std::runtime_error("bzip2 decompression failed: buffer resize limit reached");
}

}  // namespace compress_utils::bz2
