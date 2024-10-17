#ifndef COMPRESSION_UTILS_ALGORITHMS_ROUTER_HPP
#define COMPRESSION_UTILS_ALGORITHMS_ROUTER_HPP

#include "algorithms.hpp"
#include "algorithms/zlib/zlib.hpp"
#include "algorithms/zstd/zstd.hpp"

#include <functional>

namespace compression_utils::internal {

/**
 * @brief Struct that holds the compression and decompression functions for a specific algorithm
 */
struct CompressionFunctions {
    std::vector<uint8_t> (*Compress)(const std::vector<uint8_t>& data, int level);
    std::vector<uint8_t> (*Decompress)(const std::vector<uint8_t>& data);
};

/**
 * @brief Get the compression and decompression functions for the specified algorithm
 *
 * @param algorithm Compression algorithm
 * @return CompressionFunctions Compression and decompression functions
 */
CompressionFunctions GetCompressionFunctions(const Algorithm algorithm) {
    // Route to the desired algorithm
    switch (algorithm) {
#ifdef INCLUDE_ZLIB
        case Algorithm::ZLIB:
            return {zlib::Compress, zlib::Decompress};
#endif
#ifdef INCLUDE_ZSTD
        case Algorithm::ZSTD:
            return {zstd::Compress, zstd::Decompress};
#endif
        default:
            throw std::invalid_argument("Unsupported compression algorithm");
    }
}

}  // namespace compression_utils::internal

#endif  // COMPRESSION_UTILS_ALGORITHMS_ROUTER_HPP