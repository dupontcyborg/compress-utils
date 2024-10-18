#include "compression_utils.hpp"
#include "compression_utils_func.hpp"

namespace compression_utils {

Compressor::Compressor(const Algorithm algorithm) : algorithm_(algorithm) {}

std::vector<uint8_t> Compressor::Compress(const std::vector<uint8_t>& data, int level) {
    return ::compression_utils::Compress(data, algorithm_, level);
}

std::vector<uint8_t> Compressor::Compress(const uint8_t* data, size_t size, int level) {
    return ::compression_utils::Compress(data, size, algorithm_, level);
}

std::vector<uint8_t> Compressor::Decompress(const std::vector<uint8_t>& data) {
    return ::compression_utils::Decompress(data, algorithm_);
}

std::vector<uint8_t> Compressor::Decompress(const uint8_t* data, size_t size) {
    return ::compression_utils::Decompress(data, size, algorithm_);
}

}  // namespace compression_utils