#include "compress_utils_func.hpp"
#include "utils/algorithms_router.hpp"

#include <span>
#include <stdexcept>

// TODO (nico) Make the functions auto-default to ZSTD?

namespace compress_utils {

std::vector<uint8_t> Compress(const std::vector<uint8_t>& data, Algorithm algorithm, int level) {
    // Validate that level is between 1 and 10
    if (level < 1 || level > 10) {
        throw std::invalid_argument("Compression level must be between 1 and 10");
    }

    // Get the compression functions for the specified algorithm
    auto functions = internal::GetCompressionFunctions(algorithm);

    // Create span from the input data
    std::span<const uint8_t> data_span(data);

    // Call the compression function
    return functions.Compress(data_span, level);
}

std::vector<uint8_t> Compress(const uint8_t* data, size_t size, Algorithm algorithm, int level) {
    // Validate that level is between 1 and 10
    if (level < 1 || level > 10) {
        throw std::invalid_argument("Compression level must be between 1 and 10");
    }

    // Get the compression functions for the specified algorithm
    auto functions = internal::GetCompressionFunctions(algorithm);

    // Create span from the input data
    std::span<const uint8_t> data_span(data, size);

    // Call the compression function
    return functions.Compress(data_span, level);

}  // namespace compress_utils

std::vector<uint8_t> Decompress(const std::vector<uint8_t>& data, Algorithm algorithm) {
    // Get the decompression functions for the specified algorithm
    auto functions = internal::GetCompressionFunctions(algorithm);

    // Create span from the input data
    std::span<const uint8_t> data_span(data);

    // Call the decompression function
    return functions.Decompress(data_span);
}

std::vector<uint8_t> Decompress(const uint8_t* data, size_t size, Algorithm algorithm) {
    // Get the decompression functions for the specified algorithm
    auto functions = internal::GetCompressionFunctions(algorithm);

    // Create span from the input data
    std::span<const uint8_t> data_span(data, size);

    // Call the decompression function
    return functions.Decompress(data_span);
}

}  // namespace compress_utils