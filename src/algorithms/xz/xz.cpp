#include "utils/constants.hpp"
#include "xz.hpp"
#include "xz/lzma.h"

#include <stdexcept>
#include <string>

namespace compress_utils::xz {

/**
 * @brief Translate the compression level to the XZ compression level
 *
 * @param level 1-10 compression level
 * @return int XZ compression level (0-9)
 */
inline int GetCompressionLevel(int level) {
    internal::ValidateLevel(level);

    // XZ compression levels are 0-9, so we scale the input level 1-10 to 0-9
    const size_t xz_max_level = 9;
    return level - 1;
}

std::vector<uint8_t> Compress(std::span<const uint8_t>& data, int level) {
    // Get the XZ compression level
    int xz_level = GetCompressionLevel(level);

    lzma_stream strm = LZMA_STREAM_INIT;

    // Initialize XZ encoder with the desired level and CRC64 check
    if (lzma_easy_encoder(&strm, xz_level, LZMA_CHECK_CRC64) != LZMA_OK) {
        throw std::runtime_error("XZ encoder initialization failed");
    }

    // Initial buffer size (64 KB)
    size_t buffer_size = 64 * 1024;
    std::vector<uint8_t> compressed_data(buffer_size);

    // Set up input and output pointers
    strm.next_in = data.data();
    strm.avail_in = data.size();
    strm.next_out = compressed_data.data();
    strm.avail_out = buffer_size;

    // Perform the compression
    lzma_ret ret;
    while (strm.avail_in != 0) {
        ret = lzma_code(&strm, LZMA_RUN);

        // If we run out of buffer, resize it and continue
        if (ret == LZMA_OK && strm.avail_out == 0) {
            size_t compressed_size = compressed_data.size();
            compressed_data.resize(compressed_size * 2);  // Double the buffer size
            strm.next_out = compressed_data.data() + compressed_size;
            strm.avail_out = compressed_data.size() - compressed_size;
        } else if (ret != LZMA_OK) {
            lzma_end(&strm);
            throw std::runtime_error("XZ compression failed");
        }
    }

    // Finish the compression
    while ((ret = lzma_code(&strm, LZMA_FINISH)) == LZMA_OK) {
        if (strm.avail_out == 0) {
            size_t compressed_size = compressed_data.size();
            compressed_data.resize(compressed_size * 2);
            strm.next_out = compressed_data.data() + compressed_size;
            strm.avail_out = compressed_data.size() - compressed_size;
        }
    }

    // Check if the compression finished successfully
    if (ret != LZMA_STREAM_END) {
        lzma_end(&strm);
        throw std::runtime_error("XZ compression failed to finish");
    }

    // Resize the buffer to the actual compressed size
    compressed_data.resize(compressed_data.size() - strm.avail_out);

    // Clean up the stream
    lzma_end(&strm);

    return compressed_data;
}

std::vector<uint8_t> Decompress(std::span<const uint8_t>& data) {
    // Initialize the XZ decoder
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_auto_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK) {
        throw std::runtime_error("XZ decoder initialization failed");
    }

    // Set up the input data
    strm.next_in = data.data();
    strm.avail_in = data.size();

    // Allocate the output buffer (initial size = 16 KB)
    std::vector<uint8_t> decompressed_data;
    size_t buffer_size = std::max(data.size() * 2, static_cast<size_t>(16384));
    decompressed_data.resize(buffer_size);

    // Perform the decompression
    size_t total_output = 0;
    lzma_ret ret;
    do {
        // Resize the buffer if needed (double the size)
        if (total_output == decompressed_data.size()) {
            decompressed_data.resize(decompressed_data.size() * 2);
        }

        // Set up the output buffer
        strm.next_out = decompressed_data.data() + total_output;
        strm.avail_out = decompressed_data.size() - total_output;

        // Perform the decompression
        ret = lzma_code(&strm, LZMA_FINISH);

        // Update the total output size
        total_output = strm.total_out;

        // Check if the decompression failed
        if (ret != LZMA_OK && ret != LZMA_STREAM_END) {
            lzma_end(&strm);
            throw std::runtime_error("XZ decompression failed: " + std::to_string(ret));
        }
    } while (ret != LZMA_STREAM_END);

    // Resize the buffer to the actual decompressed size
    decompressed_data.resize(total_output);

    // Clean up the stream
    lzma_end(&strm);

    return decompressed_data;
}

}  // namespace compress_utils::xz