#include "compress_utils.hpp"
#include "compress_utils_func.hpp"

namespace compress_utils {

Compressor::Compressor(const Algorithm algorithm) : algorithm_(algorithm) {}

std::vector<uint8_t> Compressor::Compress(const std::vector<uint8_t>& data, int level) {
    return ::compress_utils::Compress(data, algorithm_, level);
}

std::vector<uint8_t> Compressor::Compress(const uint8_t* data, size_t size, int level) {
    return ::compress_utils::Compress(data, size, algorithm_, level);
}

std::vector<uint8_t> Compressor::Decompress(const std::vector<uint8_t>& data) {
    return ::compress_utils::Decompress(data, algorithm_);
}

std::vector<uint8_t> Compressor::Decompress(const uint8_t* data, size_t size) {
    return ::compress_utils::Decompress(data, size, algorithm_);
}

Algorithm Compressor::algorithm() const {
    return algorithm_;
}

}  // namespace compress_utils