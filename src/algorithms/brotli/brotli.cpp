#include "brotli.hpp"
#include "brotli/decode.h"
#include "brotli/encode.h"
#include "utils/constants.hpp"

#include <stdexcept>

namespace compress_utils::brotli {

// Size to start with when decompressing
constexpr int DECOMP_BUFFER_SIZE_MULTIPLE = 2;
constexpr int BROTLI_MAX_LEVEL = 11;

/**
 * @brief Translate the compression level to the Brotli compression level
 *
 * @param level 1-10 compression level
 * @return int Brotli compression level
 */
inline int GetCompressionLevel(int level) {
    // Validate that level is between 1 and 10
    internal::ValidateLevel(level);

    // Brotli compression levels are 0-11, so we need to scale the input level
    // from 1-10 to 0-11; so 1 -> 0, 10 -> 11
    level = (level * BROTLI_MAX_LEVEL) / internal::MAX_LEVEL;

    return level;
}

std::vector<uint8_t> Compress(std::span<const uint8_t>& data, int level) {
    // Get Brotli compression level
    int brotli_level = GetCompressionLevel(level);

    // Initialize Brotli encoder state
    BrotliEncoderState* encoder_state = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
    if (!encoder_state) {
        throw std::runtime_error("Brotli encoder state creation failed");
    }

    BrotliEncoderSetParameter(encoder_state, BROTLI_PARAM_QUALITY, brotli_level);

    // Reserve a buffer for the maximum compressed size
    size_t max_compressed_size = BrotliEncoderMaxCompressedSize(data.size());
    std::vector<uint8_t> compressed_data(max_compressed_size);

    // Compress the data using Brotli
    size_t compressed_size = max_compressed_size;
    BrotliEncoderOperation operation = BROTLI_OPERATION_FINISH;
    bool success =
        BrotliEncoderCompress(brotli_level, BROTLI_DEFAULT_WINDOW, BROTLI_DEFAULT_MODE, data.size(),
                              data.data(), &compressed_size, compressed_data.data());

    // Clean up the encoder state
    BrotliEncoderDestroyInstance(encoder_state);

    if (!success) {
        throw std::runtime_error("Brotli compression failed");
    }

    // Resize the compressed data buffer to the actual compressed size
    compressed_data.resize(compressed_size);

    return compressed_data;
}

std::vector<uint8_t> Decompress(std::span<const uint8_t>& data) {
    // Create Brotli decompressor state
    BrotliDecoderState* state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) {
        throw std::runtime_error("Failed to create Brotli decompressor state.");
    }

    // Allocate an initial buffer
    size_t buffer_size = data.size() * DECOMP_BUFFER_SIZE_MULTIPLE;
    std::vector<uint8_t> decompressed_data(buffer_size);

    // Set up input and output
    const uint8_t* next_in = data.data();
    size_t available_in = data.size();
    uint8_t* next_out = decompressed_data.data();
    size_t available_out = decompressed_data.size();

    // Decompress as a stream
    BrotliDecoderResult result;
    while (true) {
        result = BrotliDecoderDecompressStream(state, &available_in, &next_in, &available_out,
                                               &next_out, nullptr);

        if (result == BROTLI_DECODER_RESULT_SUCCESS) {
            // Finished successfully
            break;
        }
        if (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
            // Resize buffer when more output space is needed
            size_t decompressed_size = decompressed_data.size();
            decompressed_data.resize(decompressed_size * 2);  // Double the buffer size
            next_out = decompressed_data.data() +
                       decompressed_size;  // Move pointer to the new part of the buffer
            available_out =
                decompressed_data.size() - decompressed_size;  // Remaining space in buffer
        } else {
            // Any other result is an error
            BrotliDecoderDestroyInstance(state);

            // Get error
            BrotliDecoderErrorCode error_code = BrotliDecoderGetErrorCode(state);
            std::string error_message(BrotliDecoderErrorString(error_code));
            throw std::runtime_error("Brotli decompression failed: " + error_message);
        }
    }

    // Shrink the buffer to the actual decompressed size
    decompressed_data.resize(next_out - decompressed_data.data());

    // Clean up Brotli state
    BrotliDecoderDestroyInstance(state);

    return decompressed_data;
}

}  // namespace compress_utils::brotli
