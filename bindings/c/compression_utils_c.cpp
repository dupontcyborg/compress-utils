// C bindings for the compression_utils library

#include "compression_utils.h"
#include "compression_utils_func.hpp"
#include <cstdlib>   // For malloc/free
#include <cstring>   // For memcpy
#include <stdexcept> // For error handling

extern "C" {

// Compression function implementation
int64_t compress(const uint8_t* data, const size_t size, uint8_t** output, const Algorithm algorithm, const int level) {
    try {
        // Convert the input C array into a C++ std::vector<uint8_t>
        std::vector<uint8_t> input_data(data, data + size);

        // Call the C++ Compress function
        std::vector<uint8_t> compressed_data = compression_utils::Compress(input_data, static_cast<compression_utils::Algorithm>(algorithm), level);

        // Allocate memory for the output buffer
        *output = static_cast<uint8_t*>(malloc(compressed_data.size()));
        if (*output == nullptr) {
            return -1;  // Return -1 if memory allocation fails
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
int64_t decompress(const uint8_t* data, const size_t size, uint8_t** output, const Algorithm algorithm) {
    try {
        // Convert the input C array into a C++ std::vector<uint8_t>
        std::vector<uint8_t> input_data(data, data + size);

        // Call the C++ Decompress function
        std::vector<uint8_t> decompressed_data = compression_utils::Decompress(input_data, static_cast<compression_utils::Algorithm>(algorithm));

        // Allocate memory for the output buffer
        *output = static_cast<uint8_t*>(malloc(decompressed_data.size()));
        if (*output == nullptr) {
            return -1;  // Return -1 if memory allocation fails
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

}
