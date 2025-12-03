// C bindings for the compress_utils library

#include "compress_utils.h"
#include "compress_utils_func.hpp"
#include "compress_utils_stream.hpp"

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

// ============================================================================
// Streaming API Implementation
// ============================================================================

// Helper to copy vector to malloc'd buffer
static int64_t CopyToMallocBuffer(const std::vector<uint8_t>& data, uint8_t** output) {
    if (data.empty()) {
        *output = nullptr;
        return 0;
    }

    *output = static_cast<uint8_t*>(malloc(data.size()));
    if (*output == nullptr) {
        SetLastError("Memory allocation failed");
        return -1;
    }

    memcpy(*output, data.data(), data.size());
    return static_cast<int64_t>(data.size());
}

// CompressStream wrapper - we cast between the C opaque type and C++ class
CompressStream* compress_stream_create(Algorithm algorithm, int level) {
    ClearLastError();
    try {
        auto* stream =
            new compress_utils::CompressStream(static_cast<compress_utils::Algorithm>(algorithm), level);
        return reinterpret_cast<CompressStream*>(stream);
    } catch (const std::exception& e) {
        SetLastError(e.what());
        return nullptr;
    } catch (...) {
        SetLastError("Unknown error occurred creating compression stream");
        return nullptr;
    }
}

int64_t compress_stream_write(CompressStream* stream, const uint8_t* data, size_t size,
                              uint8_t** output) {
    ClearLastError();
    if (stream == nullptr) {
        SetLastError("Invalid stream handle (null)");
        return -1;
    }

    try {
        auto* cpp_stream = reinterpret_cast<compress_utils::CompressStream*>(stream);
        std::vector<uint8_t> result = cpp_stream->Compress(std::span<const uint8_t>(data, size));
        return CopyToMallocBuffer(result, output);
    } catch (const std::exception& e) {
        SetLastError(e.what());
        return -1;
    } catch (...) {
        SetLastError("Unknown error occurred during stream compression");
        return -1;
    }
}

int64_t compress_stream_finish(CompressStream* stream, uint8_t** output) {
    ClearLastError();
    if (stream == nullptr) {
        SetLastError("Invalid stream handle (null)");
        return -1;
    }

    try {
        auto* cpp_stream = reinterpret_cast<compress_utils::CompressStream*>(stream);
        std::vector<uint8_t> result = cpp_stream->Finish();
        return CopyToMallocBuffer(result, output);
    } catch (const std::exception& e) {
        SetLastError(e.what());
        return -1;
    } catch (...) {
        SetLastError("Unknown error occurred finishing compression stream");
        return -1;
    }
}

int compress_stream_is_finished(const CompressStream* stream) {
    if (stream == nullptr) {
        SetLastError("Invalid stream handle (null)");
        return -1;
    }

    auto* cpp_stream = reinterpret_cast<const compress_utils::CompressStream*>(stream);
    return cpp_stream->IsFinished() ? 1 : 0;
}

int compress_stream_algorithm(const CompressStream* stream) {
    if (stream == nullptr) {
        SetLastError("Invalid stream handle (null)");
        return -1;
    }

    auto* cpp_stream = reinterpret_cast<const compress_utils::CompressStream*>(stream);
    return static_cast<int>(cpp_stream->algorithm());
}

void compress_stream_destroy(CompressStream* stream) {
    if (stream != nullptr) {
        auto* cpp_stream = reinterpret_cast<compress_utils::CompressStream*>(stream);
        delete cpp_stream;
    }
}

// DecompressStream wrapper
DecompressStream* decompress_stream_create(Algorithm algorithm) {
    ClearLastError();
    try {
        auto* stream =
            new compress_utils::DecompressStream(static_cast<compress_utils::Algorithm>(algorithm));
        return reinterpret_cast<DecompressStream*>(stream);
    } catch (const std::exception& e) {
        SetLastError(e.what());
        return nullptr;
    } catch (...) {
        SetLastError("Unknown error occurred creating decompression stream");
        return nullptr;
    }
}

int64_t decompress_stream_write(DecompressStream* stream, const uint8_t* data, size_t size,
                                uint8_t** output) {
    ClearLastError();
    if (stream == nullptr) {
        SetLastError("Invalid stream handle (null)");
        return -1;
    }

    try {
        auto* cpp_stream = reinterpret_cast<compress_utils::DecompressStream*>(stream);
        std::vector<uint8_t> result = cpp_stream->Decompress(std::span<const uint8_t>(data, size));
        return CopyToMallocBuffer(result, output);
    } catch (const std::exception& e) {
        SetLastError(e.what());
        return -1;
    } catch (...) {
        SetLastError("Unknown error occurred during stream decompression");
        return -1;
    }
}

int64_t decompress_stream_finish(DecompressStream* stream, uint8_t** output) {
    ClearLastError();
    if (stream == nullptr) {
        SetLastError("Invalid stream handle (null)");
        return -1;
    }

    try {
        auto* cpp_stream = reinterpret_cast<compress_utils::DecompressStream*>(stream);
        std::vector<uint8_t> result = cpp_stream->Finish();
        return CopyToMallocBuffer(result, output);
    } catch (const std::exception& e) {
        SetLastError(e.what());
        return -1;
    } catch (...) {
        SetLastError("Unknown error occurred finishing decompression stream");
        return -1;
    }
}

int decompress_stream_is_finished(const DecompressStream* stream) {
    if (stream == nullptr) {
        SetLastError("Invalid stream handle (null)");
        return -1;
    }

    auto* cpp_stream = reinterpret_cast<const compress_utils::DecompressStream*>(stream);
    return cpp_stream->IsFinished() ? 1 : 0;
}

int decompress_stream_algorithm(const DecompressStream* stream) {
    if (stream == nullptr) {
        SetLastError("Invalid stream handle (null)");
        return -1;
    }

    auto* cpp_stream = reinterpret_cast<const compress_utils::DecompressStream*>(stream);
    return static_cast<int>(cpp_stream->algorithm());
}

void decompress_stream_destroy(DecompressStream* stream) {
    if (stream != nullptr) {
        auto* cpp_stream = reinterpret_cast<compress_utils::DecompressStream*>(stream);
        delete cpp_stream;
    }
}

}  // extern "C"
