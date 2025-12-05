#include "utils/constants.hpp"
#include "lz4.hpp"
#include "lz4/lz4.h"
#include "lz4/lz4hc.h"

#include <cstring>
#include <stdexcept>

namespace compress_utils::lz4 {

/**
 * @brief Get the LZ4 compression parameters based on level
 *
 * Levels 1-3: Fast mode with acceleration (higher = faster, lower compression)
 * Levels 4-10: HC mode with compression level (higher = slower, better compression)
 *
 * @param level 1-10 compression level
 * @param use_hc Output: whether to use HC mode
 * @param lz4_param Output: acceleration (fast mode) or compression level (HC mode)
 */
inline void GetCompressionParams(int level, bool& use_hc, int& lz4_param) {
    internal::ValidateLevel(level);

    if (level <= 3) {
        // Fast mode: levels 1-3 map to acceleration 10, 5, 1
        // Higher acceleration = faster but worse compression
        use_hc = false;
        switch (level) {
            case 1: lz4_param = 10; break;  // Fastest
            case 2: lz4_param = 5; break;   // Balanced
            case 3: lz4_param = 1; break;   // Default (best fast-mode compression)
            default: lz4_param = 1; break;
        }
    } else {
        // HC mode: levels 4-10 map to LZ4HC levels 1-12
        // LZ4HC supports levels 1-12 (LZ4HC_CLEVEL_MIN to LZ4HC_CLEVEL_MAX)
        use_hc = true;
        // Map levels 4-10 to HC levels 1-12
        lz4_param = internal::MapLevel(level - 3, internal::LZ4_HC_MAX_LEVEL);
        if (lz4_param < 1) lz4_param = 1;
    }
}

std::vector<uint8_t> Compress(std::span<const uint8_t> data, int level) {
    bool use_hc;
    int lz4_param;
    GetCompressionParams(level, use_hc, lz4_param);

    // Calculate the maximum size of the compressed data
    int max_compressed_size = LZ4_compressBound(static_cast<int>(data.size()));
    if (max_compressed_size <= 0) {
        throw std::runtime_error("LZ4 compression failed: input too large");
    }

    // Allocate buffer: 4 bytes for original size + compressed data
    // We store the original size to enable decompression
    std::vector<uint8_t> compressed_data(sizeof(uint32_t) + max_compressed_size);

    // Store the original size at the beginning (little-endian)
    uint32_t original_size = static_cast<uint32_t>(data.size());
    std::memcpy(compressed_data.data(), &original_size, sizeof(uint32_t));

    // Compress the data
    int compressed_size;
    if (use_hc) {
        compressed_size = LZ4_compress_HC(
            reinterpret_cast<const char*>(data.data()),
            reinterpret_cast<char*>(compressed_data.data() + sizeof(uint32_t)),
            static_cast<int>(data.size()),
            max_compressed_size,
            lz4_param
        );
    } else {
        compressed_size = LZ4_compress_fast(
            reinterpret_cast<const char*>(data.data()),
            reinterpret_cast<char*>(compressed_data.data() + sizeof(uint32_t)),
            static_cast<int>(data.size()),
            max_compressed_size,
            lz4_param
        );
    }

    // Check if the compression was successful
    if (compressed_size <= 0) {
        throw std::runtime_error("LZ4 compression failed");
    }

    // Resize the buffer to the actual size (header + compressed data)
    compressed_data.resize(sizeof(uint32_t) + compressed_size);

    return compressed_data;
}

std::vector<uint8_t> Decompress(std::span<const uint8_t> data) {
    // Check minimum size (need at least the size header)
    if (data.size() < sizeof(uint32_t)) {
        throw std::runtime_error("LZ4 decompression failed: input too small");
    }

    // Read the original size from the header (little-endian)
    uint32_t original_size;
    std::memcpy(&original_size, data.data(), sizeof(uint32_t));

    // Sanity check on decompressed size
    if (original_size > 512 * 1024 * 1024) {  // 512 MB limit
        throw std::runtime_error("LZ4 decompression failed: claimed size too large");
    }

    // Create a buffer to hold the decompressed data
    std::vector<uint8_t> decompressed_data(original_size);

    // Decompress the data
    int decompressed_size = LZ4_decompress_safe(
        reinterpret_cast<const char*>(data.data() + sizeof(uint32_t)),
        reinterpret_cast<char*>(decompressed_data.data()),
        static_cast<int>(data.size() - sizeof(uint32_t)),
        static_cast<int>(original_size)
    );

    // Check if the decompression was successful
    if (decompressed_size < 0) {
        throw std::runtime_error("LZ4 decompression failed: corrupted data");
    }

    if (static_cast<uint32_t>(decompressed_size) != original_size) {
        throw std::runtime_error("LZ4 decompression failed: size mismatch");
    }

    return decompressed_data;
}

}  // namespace compress_utils::lz4
