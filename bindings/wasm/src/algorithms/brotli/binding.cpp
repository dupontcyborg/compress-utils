/**
 * Brotli WebAssembly Bindings
 *
 * Provides the C interface for the Brotli compression algorithm
 * that can be compiled to WebAssembly using Emscripten.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "brotli/encode.h"
#include "brotli/decode.h"

extern "C" {

// ============================================================================
// One-shot Compression/Decompression
// ============================================================================

uint8_t* cu_compress(
    const uint8_t* input,
    size_t input_len,
    int level,
    size_t* output_len
) {
    // Calculate maximum compressed size
    size_t max_size = BrotliEncoderMaxCompressedSize(input_len);
    if (max_size == 0) {
        // Fallback for very small inputs
        max_size = input_len + 1024;
    }

    // Allocate output buffer
    uint8_t* output = static_cast<uint8_t*>(malloc(max_size));
    if (!output) {
        *output_len = 0;
        return nullptr;
    }

    size_t encoded_size = max_size;
    BROTLI_BOOL result = BrotliEncoderCompress(
        level,
        BROTLI_DEFAULT_WINDOW,
        BROTLI_DEFAULT_MODE,
        input_len,
        input,
        &encoded_size,
        output
    );

    if (result != BROTLI_TRUE) {
        free(output);
        *output_len = 0;
        return nullptr;
    }

    // Shrink buffer to actual size
    uint8_t* shrunk = static_cast<uint8_t*>(realloc(output, encoded_size));
    if (shrunk) {
        output = shrunk;
    }

    *output_len = encoded_size;
    return output;
}

uint8_t* cu_decompress(
    const uint8_t* input,
    size_t input_len,
    size_t* output_len
) {
    // Start with a reasonable estimate
    size_t alloc_size = input_len * 4;
    if (alloc_size < 1024) alloc_size = 1024;

    uint8_t* output = static_cast<uint8_t*>(malloc(alloc_size));
    if (!output) {
        *output_len = 0;
        return nullptr;
    }

    size_t decoded_size = alloc_size;
    BrotliDecoderResult result = BrotliDecoderDecompress(
        input_len,
        input,
        &decoded_size,
        output
    );

    // If buffer too small, retry with larger buffer
    int retries = 0;
    while (result == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT && retries < 10) {
        alloc_size *= 2;
        uint8_t* new_output = static_cast<uint8_t*>(realloc(output, alloc_size));
        if (!new_output) {
            free(output);
            *output_len = 0;
            return nullptr;
        }
        output = new_output;
        decoded_size = alloc_size;

        result = BrotliDecoderDecompress(
            input_len,
            input,
            &decoded_size,
            output
        );
        retries++;
    }

    if (result != BROTLI_DECODER_RESULT_SUCCESS) {
        free(output);
        *output_len = 0;
        return nullptr;
    }

    *output_len = decoded_size;
    return output;
}

// ============================================================================
// Streaming Compression
// ============================================================================

struct CompressStreamContext {
    BrotliEncoderState* state;
    bool finished;
};

void* cu_stream_compress_create(int level) {
    BrotliEncoderState* state = BrotliEncoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) {
        return nullptr;
    }

    BrotliEncoderSetParameter(state, BROTLI_PARAM_QUALITY, level);

    auto* ctx = static_cast<CompressStreamContext*>(
        malloc(sizeof(CompressStreamContext))
    );
    if (!ctx) {
        BrotliEncoderDestroyInstance(state);
        return nullptr;
    }

    ctx->state = state;
    ctx->finished = false;
    return ctx;
}

int32_t cu_stream_compress_write(
    void* ctx,
    const uint8_t* input,
    size_t input_len,
    uint8_t* output,
    size_t output_cap
) {
    auto* context = static_cast<CompressStreamContext*>(ctx);
    if (!context || context->finished) {
        return -1;
    }

    size_t available_in = input_len;
    const uint8_t* next_in = input;
    size_t available_out = output_cap;
    uint8_t* next_out = output;

    BROTLI_BOOL result = BrotliEncoderCompressStream(
        context->state,
        BROTLI_OPERATION_PROCESS,
        &available_in,
        &next_in,
        &available_out,
        &next_out,
        nullptr
    );

    if (result != BROTLI_TRUE) {
        return -1;
    }

    return static_cast<int32_t>(output_cap - available_out);
}

int32_t cu_stream_compress_finish(
    void* ctx,
    uint8_t* output,
    size_t output_cap
) {
    auto* context = static_cast<CompressStreamContext*>(ctx);
    if (!context) {
        return -1;
    }

    size_t available_in = 0;
    const uint8_t* next_in = nullptr;
    size_t available_out = output_cap;
    uint8_t* next_out = output;

    BROTLI_BOOL result = BrotliEncoderCompressStream(
        context->state,
        BROTLI_OPERATION_FINISH,
        &available_in,
        &next_in,
        &available_out,
        &next_out,
        nullptr
    );

    if (result != BROTLI_TRUE) {
        return -1;
    }

    if (BrotliEncoderIsFinished(context->state)) {
        context->finished = true;
    }

    return static_cast<int32_t>(output_cap - available_out);
}

void cu_stream_compress_destroy(void* ctx) {
    auto* context = static_cast<CompressStreamContext*>(ctx);
    if (context) {
        if (context->state) {
            BrotliEncoderDestroyInstance(context->state);
        }
        free(context);
    }
}

// ============================================================================
// Streaming Decompression
// ============================================================================

struct DecompressStreamContext {
    BrotliDecoderState* state;
    bool finished;
};

void* cu_stream_decompress_create() {
    BrotliDecoderState* state = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!state) {
        return nullptr;
    }

    auto* ctx = static_cast<DecompressStreamContext*>(
        malloc(sizeof(DecompressStreamContext))
    );
    if (!ctx) {
        BrotliDecoderDestroyInstance(state);
        return nullptr;
    }

    ctx->state = state;
    ctx->finished = false;
    return ctx;
}

int32_t cu_stream_decompress_write(
    void* ctx,
    const uint8_t* input,
    size_t input_len,
    uint8_t* output,
    size_t output_cap
) {
    auto* context = static_cast<DecompressStreamContext*>(ctx);
    if (!context || context->finished) {
        return -1;
    }

    size_t available_in = input_len;
    const uint8_t* next_in = input;
    size_t available_out = output_cap;
    uint8_t* next_out = output;

    BrotliDecoderResult result = BrotliDecoderDecompressStream(
        context->state,
        &available_in,
        &next_in,
        &available_out,
        &next_out,
        nullptr
    );

    if (result == BROTLI_DECODER_RESULT_ERROR) {
        return -1;
    }

    if (result == BROTLI_DECODER_RESULT_SUCCESS) {
        context->finished = true;
    }

    return static_cast<int32_t>(output_cap - available_out);
}

int32_t cu_stream_decompress_finish(void* ctx) {
    auto* context = static_cast<DecompressStreamContext*>(ctx);
    if (!context) {
        return -1;
    }
    return context->finished ? 1 : 0;
}

void cu_stream_decompress_destroy(void* ctx) {
    auto* context = static_cast<DecompressStreamContext*>(ctx);
    if (context) {
        if (context->state) {
            BrotliDecoderDestroyInstance(context->state);
        }
        free(context);
    }
}

} // extern "C"
