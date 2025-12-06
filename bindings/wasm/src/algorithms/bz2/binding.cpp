/**
 * Bzip2 WebAssembly Bindings
 *
 * Provides the C interface for the bzip2 compression algorithm
 * that can be compiled to WebAssembly using Emscripten.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "bzlib.h"

extern "C" {

// ============================================================================
// One-shot Compression/Decompression
// ============================================================================

uint8_t* compress(
    const uint8_t* input,
    size_t input_len,
    int level,
    size_t* output_len
) {
    // Bzip2 can expand data in worst case by about 1% + 600 bytes
    size_t max_size = input_len + (input_len / 100) + 600 + 1024;

    // Allocate output buffer
    uint8_t* output = static_cast<uint8_t*>(malloc(max_size));
    if (!output) {
        *output_len = 0;
        return nullptr;
    }

    unsigned int dest_len = static_cast<unsigned int>(max_size);
    int result = BZ2_bzBuffToBuffCompress(
        reinterpret_cast<char*>(output),
        &dest_len,
        const_cast<char*>(reinterpret_cast<const char*>(input)),
        static_cast<unsigned int>(input_len),
        level,  // blockSize100k
        0,      // verbosity
        30      // workFactor
    );

    if (result != BZ_OK) {
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

uint8_t* decompress(
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

    unsigned int dest_len = static_cast<unsigned int>(alloc_size);
    int result = BZ2_bzBuffToBuffDecompress(
        reinterpret_cast<char*>(output),
        &dest_len,
        const_cast<char*>(reinterpret_cast<const char*>(input)),
        static_cast<unsigned int>(input_len),
        0,  // small
        0   // verbosity
    );

    // If buffer too small, retry with larger buffer
    int retries = 0;
    while (result == BZ_OUTBUFF_FULL && retries < 10) {
        alloc_size *= 2;
        uint8_t* new_output = static_cast<uint8_t*>(realloc(output, alloc_size));
        if (!new_output) {
            free(output);
            *output_len = 0;
            return nullptr;
        }
        output = new_output;
        dest_len = static_cast<unsigned int>(alloc_size);

        result = BZ2_bzBuffToBuffDecompress(
            reinterpret_cast<char*>(output),
            &dest_len,
            const_cast<char*>(reinterpret_cast<const char*>(input)),
            static_cast<unsigned int>(input_len),
            0,
            0
        );
        retries++;
    }

    if (result != BZ_OK) {
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
    bz_stream stream;
    bool initialized;
    bool finished;
};

void* stream_compress_create(int level) {
    auto* ctx = static_cast<CompressStreamContext*>(
        malloc(sizeof(CompressStreamContext))
    );
    if (!ctx) {
        return nullptr;
    }

    memset(&ctx->stream, 0, sizeof(bz_stream));
    ctx->initialized = false;
    ctx->finished = false;

    int result = BZ2_bzCompressInit(&ctx->stream, level, 0, 30);
    if (result != BZ_OK) {
        free(ctx);
        return nullptr;
    }

    ctx->initialized = true;
    return ctx;
}

int32_t stream_compress_write(
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

    context->stream.next_in = const_cast<char*>(reinterpret_cast<const char*>(input));
    context->stream.avail_in = static_cast<unsigned int>(input_len);
    context->stream.next_out = reinterpret_cast<char*>(output);
    context->stream.avail_out = static_cast<unsigned int>(output_cap);

    int result = BZ2_bzCompress(&context->stream, BZ_RUN);
    if (result != BZ_RUN_OK) {
        return -1;
    }

    return static_cast<int32_t>(output_cap - context->stream.avail_out);
}

int32_t stream_compress_finish(
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
    context->stream.next_out = reinterpret_cast<char*>(output);
    context->stream.avail_out = static_cast<unsigned int>(output_cap);

    int result = BZ2_bzCompress(&context->stream, BZ_FINISH);
    if (result == BZ_STREAM_END) {
        context->finished = true;
    } else if (result != BZ_FINISH_OK) {
        return -1;
    }

    return static_cast<int32_t>(output_cap - context->stream.avail_out);
}

void stream_compress_destroy(void* ctx) {
    auto* context = static_cast<CompressStreamContext*>(ctx);
    if (context) {
        if (context->initialized) {
            BZ2_bzCompressEnd(&context->stream);
        }
        free(context);
    }
}

// ============================================================================
// Streaming Decompression
// ============================================================================

struct DecompressStreamContext {
    bz_stream stream;
    bool initialized;
    bool finished;
};

void* stream_decompress_create() {
    auto* ctx = static_cast<DecompressStreamContext*>(
        malloc(sizeof(DecompressStreamContext))
    );
    if (!ctx) {
        return nullptr;
    }

    memset(&ctx->stream, 0, sizeof(bz_stream));
    ctx->initialized = false;
    ctx->finished = false;

    int result = BZ2_bzDecompressInit(&ctx->stream, 0, 0);
    if (result != BZ_OK) {
        free(ctx);
        return nullptr;
    }

    ctx->initialized = true;
    return ctx;
}

int32_t stream_decompress_write(
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

    context->stream.next_in = const_cast<char*>(reinterpret_cast<const char*>(input));
    context->stream.avail_in = static_cast<unsigned int>(input_len);
    context->stream.next_out = reinterpret_cast<char*>(output);
    context->stream.avail_out = static_cast<unsigned int>(output_cap);

    int result = BZ2_bzDecompress(&context->stream);
    if (result == BZ_STREAM_END) {
        context->finished = true;
    } else if (result != BZ_OK) {
        return -1;
    }

    return static_cast<int32_t>(output_cap - context->stream.avail_out);
}

int32_t stream_decompress_finish(void* ctx) {
    auto* context = static_cast<DecompressStreamContext*>(ctx);
    if (!context) {
        return -1;
    }
    return context->finished ? 1 : 0;
}

void stream_decompress_destroy(void* ctx) {
    auto* context = static_cast<DecompressStreamContext*>(ctx);
    if (context) {
        if (context->initialized) {
            BZ2_bzDecompressEnd(&context->stream);
        }
        free(context);
    }
}

} // extern "C"
