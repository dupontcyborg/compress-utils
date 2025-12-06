/**
 * Zlib WebAssembly Bindings
 *
 * Provides the C interface for the zlib compression algorithm
 * that can be compiled to WebAssembly using Emscripten.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "zlib.h"

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
    uLong max_size = compressBound(static_cast<uLong>(input_len));

    // Allocate output buffer
    uint8_t* output = static_cast<uint8_t*>(malloc(max_size));
    if (!output) {
        *output_len = 0;
        return nullptr;
    }

    uLongf dest_len = max_size;
    int result = compress2(
        output,
        &dest_len,
        input,
        static_cast<uLong>(input_len),
        level
    );

    if (result != Z_OK) {
        free(output);
        *output_len = 0;
        return nullptr;
    }

    // Shrink buffer to actual size
    uint8_t* shrunk = static_cast<uint8_t*>(realloc(output, dest_len));
    if (shrunk) {
        output = shrunk;
    }

    *output_len = dest_len;
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

    uLongf dest_len = static_cast<uLongf>(alloc_size);
    int result = uncompress(
        output,
        &dest_len,
        input,
        static_cast<uLong>(input_len)
    );

    // If buffer too small, retry with larger buffer
    int retries = 0;
    while (result == Z_BUF_ERROR && retries < 10) {
        alloc_size *= 2;
        uint8_t* new_output = static_cast<uint8_t*>(realloc(output, alloc_size));
        if (!new_output) {
            free(output);
            *output_len = 0;
            return nullptr;
        }
        output = new_output;
        dest_len = static_cast<uLongf>(alloc_size);

        result = uncompress(
            output,
            &dest_len,
            input,
            static_cast<uLong>(input_len)
        );
        retries++;
    }

    if (result != Z_OK) {
        free(output);
        *output_len = 0;
        return nullptr;
    }

    *output_len = dest_len;
    return output;
}

// ============================================================================
// Streaming Compression
// ============================================================================

struct CompressStreamContext {
    z_stream stream;
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

    memset(&ctx->stream, 0, sizeof(z_stream));
    ctx->initialized = false;
    ctx->finished = false;

    int result = deflateInit(&ctx->stream, level);
    if (result != Z_OK) {
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

    context->stream.next_in = const_cast<Bytef*>(input);
    context->stream.avail_in = static_cast<uInt>(input_len);
    context->stream.next_out = output;
    context->stream.avail_out = static_cast<uInt>(output_cap);

    int result = deflate(&context->stream, Z_NO_FLUSH);
    if (result != Z_OK && result != Z_BUF_ERROR) {
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
    context->stream.avail_out = static_cast<uInt>(output_cap);

    int result = deflate(&context->stream, Z_FINISH);
    if (result == Z_STREAM_END) {
        context->finished = true;
    } else if (result != Z_OK && result != Z_BUF_ERROR) {
        return -1;
    }

    return static_cast<int32_t>(output_cap - context->stream.avail_out);
}

void cu_stream_compress_destroy(void* ctx) {
    auto* context = static_cast<CompressStreamContext*>(ctx);
    if (context) {
        if (context->initialized) {
            deflateEnd(&context->stream);
        }
        free(context);
    }
}

// ============================================================================
// Streaming Decompression
// ============================================================================

struct DecompressStreamContext {
    z_stream stream;
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

    memset(&ctx->stream, 0, sizeof(z_stream));
    ctx->initialized = false;
    ctx->finished = false;

    int result = inflateInit(&ctx->stream);
    if (result != Z_OK) {
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

    context->stream.next_in = const_cast<Bytef*>(input);
    context->stream.avail_in = static_cast<uInt>(input_len);
    context->stream.next_out = output;
    context->stream.avail_out = static_cast<uInt>(output_cap);

    int result = inflate(&context->stream, Z_NO_FLUSH);
    if (result == Z_STREAM_END) {
        context->finished = true;
    } else if (result != Z_OK && result != Z_BUF_ERROR) {
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
            inflateEnd(&context->stream);
        }
        free(context);
    }
}

} // extern "C"
