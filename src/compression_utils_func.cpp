#include "compression_utils_func.hpp"
#include "utils/algorithms_router.hpp"

namespace compression_utils {

std::vector<uint8_t> Compress(const std::vector<uint8_t>& data, const Algorithm algorithm,
                              int level) {
    // Validate that level is between 1 and 10
    if (level < 1 || level > 10) {
        throw std::invalid_argument("Compression level must be between 1 and 10");
    }

    // Get the compression functions for the specified algorithm
    auto functions = internal::GetCompressionFunctions(algorithm);

    // Call the compression function
    return functions.Compress(data, level);
}

std::vector<uint8_t> Decompress(const std::vector<uint8_t>& data, const Algorithm algorithm) {
    // Get the decompression functions for the specified algorithm
    auto functions = internal::GetCompressionFunctions(algorithm);

    // Call the decompression function
    return functions.Decompress(data);
}

}  // namespace compression_utils