#include "algorithms/zlib.hpp"
#include "algorithms/zstd.hpp"
#include "compression_utils_func.hpp"

namespace compression_utils {

std::vector<uint8_t> Compress(const std::vector<uint8_t>& data, const Algorithm algorithm,
                              int level) {
    // Validate that level is between 1 and 10
    if (level < 1 || level > 10) {
        throw std::invalid_argument("Compression level must be between 1 and 10");
    }

    // Route the request to the appropriate compression algorithm
    switch (algorithm) {
#ifdef INCLUDE_ZLIB
        case Algorithm::ZLIB:
            return zlib::Compress(data, level);
#endif
#ifdef INCLUDE_ZSTD
        case Algorithm::ZSTD:
            return zstd::Compress(data, level);
#endif
        default:
            throw std::invalid_argument("Unsupported compression algorithm");
    }
}

std::vector<uint8_t> Decompress(const std::vector<uint8_t>& data, const Algorithm algorithm) {
    // Route the request to the appropriate decompression algorithm
    switch (algorithm) {
        case Algorithm::ZLIB:
            return zlib::Decompress(data);
        case Algorithm::ZSTD:
            return zstd::Decompress(data);
        default:
            throw std::invalid_argument("Unsupported compression algorithm");
    }
}

}  // namespace compression_utils