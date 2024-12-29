#ifndef COMPRESS_UTILS_ALGORITHMS_ROUTER_HPP
#define COMPRESS_UTILS_ALGORITHMS_ROUTER_HPP

#include "algorithms.hpp"
#include "algorithms/brotli/brotli.hpp"
#include "algorithms/xz/xz.hpp"
#include "algorithms/zlib/zlib.hpp"
#include "algorithms/zstd/zstd.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <vector>

namespace compress_utils::internal {

/**
 * @brief Struct that holds the compression and decompression functions for a specific algorithm
 */
struct CompressionFunctions {
    std::vector<uint8_t> (*Compress)(std::span<const uint8_t>& data, int level);
    std::vector<uint8_t> (*Decompress)(std::span<const uint8_t>& data);
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
#ifdef INCLUDE_BROTLI
        case Algorithm::BROTLI:
            return {brotli::Compress, brotli::Decompress};
#endif
#ifdef INCLUDE_XZ
        case Algorithm::XZ:
            return {xz::Compress, xz::Decompress};
        case Algorithm::LZMA:
            return {xz::Compress, xz::Decompress};
#endif
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

}  // namespace compress_utils::internal

#endif  // COMPRESS_UTILS_ALGORITHMS_ROUTER_HPP