// C bindings for the compress_utils library

#include "compress_utils.h"
#include "compress_utils_func.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

extern "C" {

// Compression function implementation
int64_t compress_data(const uint8_t* data, size_t size, uint8_t** output, Algorithm algorithm,
                      int level) {
    try {
        // Call the C++ Compress function
        std::vector<uint8_t> compressed_data = compress_utils::Compress(
            data, size, static_cast<compress_utils::Algorithm>(algorithm), level);

        // Allocate memory for the output buffer
        *output = static_cast<uint8_t*>(malloc(compressed_data.size()));

        // Return -1 if memory allocation fails
        if (*output == nullptr) {
            return -1;
        }

        // Copy the compressed data to the output buffer
        memcpy(*output, compressed_data.data(), compressed_data.size());

        // Return the size of the compressed data
        return static_cast<int64_t>(compressed_data.size());
    } catch (const std::exception& e) {
        // Handle any exceptions thrown by the C++ code and return -1 to indicate failure
        return -1;
    }
}

// Decompression function implementation
int64_t decompress_data(const uint8_t* data, size_t size, uint8_t** output, Algorithm algorithm) {
    try {
        // Call the C++ Decompress function
        std::vector<uint8_t> decompressed_data = compress_utils::Decompress(
            data, size, static_cast<compress_utils::Algorithm>(algorithm));

        // Allocate memory for the output buffer
        *output = static_cast<uint8_t*>(malloc(decompressed_data.size()));

        // Return -1 if memory allocation fails
        if (*output == nullptr) {
            return -1;
        }

        // Copy the decompressed data to the output buffer
        memcpy(*output, decompressed_data.data(), decompressed_data.size());

        // Return the size of the decompressed data
        return static_cast<int64_t>(decompressed_data.size());
    } catch (const std::exception& e) {
        // Handle any exceptions thrown by the C++ code and return -1 to indicate failure
        return -1;
    }
}

}  // extern "C"
