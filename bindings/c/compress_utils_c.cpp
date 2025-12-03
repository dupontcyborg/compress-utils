// C bindings for the compress_utils library

#include "compress_utils.h"
#include "compress_utils_func.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// Thread-local storage for error messages
thread_local std::string g_last_error;

void SetLastError(const std::string& error) {
    g_last_error = error;
}

void ClearLastError() {
    g_last_error.clear();
}

}  // namespace

extern "C" {

// Compression function implementation
int64_t compress_data(const uint8_t* data, size_t size, uint8_t** output, Algorithm algorithm,
                      int level) {
    ClearLastError();

    try {
        // Call the C++ Compress function
        std::vector<uint8_t> compressed_data = compress_utils::Compress(
            data, size, static_cast<compress_utils::Algorithm>(algorithm), level);

        // Allocate memory for the output buffer
        *output = static_cast<uint8_t*>(malloc(compressed_data.size()));

        // Return -1 if memory allocation fails
        if (*output == nullptr) {
            SetLastError("Memory allocation failed");
            return -1;
        }

        // Copy the compressed data to the output buffer
        memcpy(*output, compressed_data.data(), compressed_data.size());

        // Return the size of the compressed data
        return static_cast<int64_t>(compressed_data.size());
    } catch (const std::exception& e) {
        SetLastError(e.what());
        return -1;
    } catch (...) {
        SetLastError("Unknown error occurred during compression");
        return -1;
    }
}

// Decompression function implementation
int64_t decompress_data(const uint8_t* data, size_t size, uint8_t** output, Algorithm algorithm) {
    ClearLastError();

    try {
        // Call the C++ Decompress function
        std::vector<uint8_t> decompressed_data = compress_utils::Decompress(
            data, size, static_cast<compress_utils::Algorithm>(algorithm));

        // Allocate memory for the output buffer
        *output = static_cast<uint8_t*>(malloc(decompressed_data.size()));

        // Return -1 if memory allocation fails
        if (*output == nullptr) {
            SetLastError("Memory allocation failed");
            return -1;
        }

        // Copy the decompressed data to the output buffer
        memcpy(*output, decompressed_data.data(), decompressed_data.size());

        // Return the size of the decompressed data
        return static_cast<int64_t>(decompressed_data.size());
    } catch (const std::exception& e) {
        SetLastError(e.what());
        return -1;
    } catch (...) {
        SetLastError("Unknown error occurred during decompression");
        return -1;
    }
}

// Get the last error message
const char* compress_utils_last_error(void) {
    return g_last_error.c_str();
}

// Clear the last error message
void compress_utils_clear_error(void) {
    ClearLastError();
}

}  // extern "C"
