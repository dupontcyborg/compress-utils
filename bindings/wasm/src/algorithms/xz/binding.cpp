/**
 * XZ/LZMA WebAssembly Bindings
 *
 * Provides the C interface for the XZ (LZMA2) compression algorithm
 * that can be compiled to WebAssembly using Emscripten.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "lzma.h"

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
    // Estimate maximum size
    size_t max_size = lzma_stream_buffer_bound(input_len);

    // Allocate output buffer
    uint8_t* output = static_cast<uint8_t*>(malloc(max_size));
    if (!output) {
        *output_len = 0;
        return nullptr;
    }

    size_t out_pos = 0;
    lzma_ret result = lzma_easy_buffer_encode(
        static_cast<uint32_t>(level),
        LZMA_CHECK_CRC64,
        nullptr,
        input,
        input_len,
        output,
        &out_pos,
        max_size
    );

    if (result != LZMA_OK) {
        free(output);
        *output_len = 0;
        return nullptr;
    }

    // Shrink buffer to actual size
    uint8_t* shrunk = static_cast<uint8_t*>(realloc(output, out_pos));
    if (shrunk) {
        output = shrunk;
    }

    *output_len = out_pos;
    return output;
}

uint8_t* cu_decompress(
    const uint8_t* input,
    size_t input_len,
    size_t* output_len
) {
    // Start with 4x the input size
    size_t alloc_size = input_len * 4;
    if (alloc_size < 1024) alloc_size = 1024;

    uint8_t* output = static_cast<uint8_t*>(malloc(alloc_size));
    if (!output) {
        *output_len = 0;
        return nullptr;
    }

    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_ret result = lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED);
    if (result != LZMA_OK) {
        free(output);
        *output_len = 0;
        return nullptr;
    }

    strm.next_in = input;
    strm.avail_in = input_len;
    strm.next_out = output;
    strm.avail_out = alloc_size;

    size_t total_out = 0;
    int retries = 0;

    while (true) {
        result = lzma_code(&strm, LZMA_FINISH);

        if (result == LZMA_STREAM_END) {
            total_out = strm.total_out;
            break;
        }

        if (result == LZMA_OK && strm.avail_out == 0) {
            // Need more output space
            if (retries >= 10) {
                lzma_end(&strm);
                free(output);
                *output_len = 0;
                return nullptr;
            }

            size_t new_size = alloc_size * 2;
            uint8_t* new_output = static_cast<uint8_t*>(realloc(output, new_size));
            if (!new_output) {
                lzma_end(&strm);
                free(output);
                *output_len = 0;
                return nullptr;
            }

            output = new_output;
            strm.next_out = output + (alloc_size - strm.avail_out);
            strm.avail_out = new_size - alloc_size;
            alloc_size = new_size;
            retries++;
            continue;
        }

        if (result != LZMA_OK) {
            lzma_end(&strm);
            free(output);
            *output_len = 0;
            return nullptr;
        }
    }

    lzma_end(&strm);
    *output_len = total_out;
    return output;
}

// ============================================================================
// Streaming Compression
// ============================================================================

struct CompressStreamContext {
    lzma_stream stream;
    bool initialized;
    bool finished;
};

void* cu_stream_compress_create(int level) {
    auto* ctx = static_cast<CompressStreamContext*>(
        malloc(sizeof(CompressStreamContext))
    );
    if (!ctx) {
        return nullptr;
    }

    ctx->stream = LZMA_STREAM_INIT;
    ctx->initialized = false;
    ctx->finished = false;

    lzma_ret result = lzma_easy_encoder(
        &ctx->stream,
        static_cast<uint32_t>(level),
        LZMA_CHECK_CRC64
    );

    if (result != LZMA_OK) {
        free(ctx);
        return nullptr;
    }

    ctx->initialized = true;
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
    if (!context || !context->initialized || context->finished) {
        return -1;
    }

    context->stream.next_in = input;
    context->stream.avail_in = input_len;
    context->stream.next_out = output;
    context->stream.avail_out = output_cap;

    lzma_ret result = lzma_code(&context->stream, LZMA_RUN);
    if (result != LZMA_OK) {
        return -1;
    }

    return static_cast<int32_t>(output_cap - context->stream.avail_out);
}

int32_t cu_stream_compress_finish(
    void* ctx,
    uint8_t* output,
    size_t output_cap
) {
    auto* context = static_cast<CompressStreamContext*>(ctx);
    if (!context || !context->initialized) {
        return -1;
    }

    context->stream.next_in = nullptr;
    context->stream.avail_in = 0;
    context->stream.next_out = output;
    context->stream.avail_out = output_cap;

    lzma_ret result = lzma_code(&context->stream, LZMA_FINISH);
    if (result == LZMA_STREAM_END) {
        context->finished = true;
    } else if (result != LZMA_OK) {
        return -1;
    }

    return static_cast<int32_t>(output_cap - context->stream.avail_out);
}

void cu_stream_compress_destroy(void* ctx) {
    auto* context = static_cast<CompressStreamContext*>(ctx);
    if (context) {
        if (context->initialized) {
            lzma_end(&context->stream);
        }
        free(context);
    }
}

// ============================================================================
// Streaming Decompression
// ============================================================================

struct DecompressStreamContext {
    lzma_stream stream;
    bool initialized;
    bool finished;
};

void* cu_stream_decompress_create() {
    auto* ctx = static_cast<DecompressStreamContext*>(
        malloc(sizeof(DecompressStreamContext))
    );
    if (!ctx) {
        return nullptr;
    }

    ctx->stream = LZMA_STREAM_INIT;
    ctx->initialized = false;
    ctx->finished = false;

    lzma_ret result = lzma_stream_decoder(
        &ctx->stream,
        UINT64_MAX,
        LZMA_CONCATENATED
    );

    if (result != LZMA_OK) {
        free(ctx);
        return nullptr;
    }

    ctx->initialized = true;
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
    if (!context || !context->initialized || context->finished) {
        return -1;
    }

    context->stream.next_in = input;
    context->stream.avail_in = input_len;
    context->stream.next_out = output;
    context->stream.avail_out = output_cap;

    lzma_ret result = lzma_code(&context->stream, LZMA_RUN);
    if (result == LZMA_STREAM_END) {
        context->finished = true;
    } else if (result != LZMA_OK) {
        return -1;
    }

    return static_cast<int32_t>(output_cap - context->stream.avail_out);
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
        if (context->initialized) {
            lzma_end(&context->stream);
        }
        free(context);
    }
}

} // extern "C"
