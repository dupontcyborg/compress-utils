#include "zstd.hpp"

namespace compression_utils::zstd {

std::vector<uint8_t> Compress(const std::vector<uint8_t>& data, int level) {
    return data;
}

std::vector<uint8_t> Decompress(const std::vector<uint8_t>& data) {
    return data;
}

}  // namespace compression_utils::zstd