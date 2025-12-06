/**
 * ZSTD WebAssembly Bindings
 *
 * This file provides the C interface for the ZSTD compression algorithm
 * that can be compiled to WebAssembly using Emscripten.
 *
 * The interface exposes:
 * - One-shot compression/decompression
 * - Streaming compression/decompression
 * - Memory management helpers
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>

#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

extern "C" {

// ============================================================================
// One-shot Compression/Decompression
// ============================================================================

/**
 * Compress data using ZSTD.
 *
 * @param input       Pointer to input data
 * @param input_len   Length of input data
 * @param level       Compression level (ZSTD native: 1-22)
 * @param output_len  Pointer to store output length (must be pre-allocated)
 * @return            Pointer to compressed data (caller must free), or nullptr on error
 */
uint8_t* cu_compress(
    const uint8_t* input,
    size_t input_len,
    int level,
    size_t* output_len
) {
    // Calculate maximum compressed size
    size_t max_size = ZSTD_compressBound(input_len);
    if (ZSTD_isError(max_size)) {
        *output_len = 0;
        return nullptr;
    }

    // Allocate output buffer
    uint8_t* output = static_cast<uint8_t*>(malloc(max_size));
    if (!output) {
        *output_len = 0;
        return nullptr;
    }

    // Compress
    size_t result = ZSTD_compress(output, max_size, input, input_len, level);
    if (ZSTD_isError(result)) {
        free(output);
        *output_len = 0;
        return nullptr;
    }

    // Shrink buffer to actual size (optional optimization)
    uint8_t* shrunk = static_cast<uint8_t*>(realloc(output, result));
    if (shrunk) {
        output = shrunk;
    }

    *output_len = result;
    return output;
}

/**
 * Decompress ZSTD-compressed data.
 *
 * @param input       Pointer to compressed data
 * @param input_len   Length of compressed data
 * @param output_len  Pointer to store output length (must be pre-allocated)
 * @return            Pointer to decompressed data (caller must free), or nullptr on error
 */
uint8_t* cu_decompress(
    const uint8_t* input,
    size_t input_len,
    size_t* output_len
) {
    // Get decompressed size from frame header
    unsigned long long decompressed_size = ZSTD_getFrameContentSize(input, input_len);

    size_t alloc_size;
    if (decompressed_size == ZSTD_CONTENTSIZE_UNKNOWN) {
        // Size unknown - start with a reasonable estimate
        alloc_size = input_len * 4;
        if (alloc_size < 1024) alloc_size = 1024;
    } else if (decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
        *output_len = 0;
        return nullptr;
    } else {
        alloc_size = static_cast<size_t>(decompressed_size);
    }

    // Allocate output buffer
    uint8_t* output = static_cast<uint8_t*>(malloc(alloc_size));
    if (!output) {
        *output_len = 0;
        return nullptr;
    }

    // Decompress
    size_t result = ZSTD_decompress(output, alloc_size, input, input_len);
    if (ZSTD_isError(result)) {
        free(output);
        *output_len = 0;
        return nullptr;
    }

    *output_len = result;
    return output;
}

// ============================================================================
// Streaming Compression
// ============================================================================

/**
 * Streaming compression context.
 */
struct CompressStreamContext {
    ZSTD_CStream* stream;
    bool finished;
};

/**
 * Create a streaming compression context.
 *
 * @param level  Compression level (ZSTD native: 1-22)
 * @return       Pointer to context, or nullptr on error
 */
void* cu_stream_compress_create(int level) {
    ZSTD_CStream* stream = ZSTD_createCStream();
    if (!stream) {
        return nullptr;
    }

    size_t result = ZSTD_initCStream(stream, level);
    if (ZSTD_isError(result)) {
        ZSTD_freeCStream(stream);
        return nullptr;
    }

    auto* ctx = static_cast<CompressStreamContext*>(
        malloc(sizeof(CompressStreamContext))
    );
    if (!ctx) {
        ZSTD_freeCStream(stream);
        return nullptr;
    }

    ctx->stream = stream;
    ctx->finished = false;
    return ctx;
}

/**
 * Write data to streaming compressor.
 *
 * @param ctx         Compression context
 * @param input       Input data
 * @param input_len   Input length
 * @param output      Output buffer
 * @param output_cap  Output buffer capacity
 * @return            Number of bytes written to output, or -1 on error
 */
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

    ZSTD_inBuffer in_buf = { input, input_len, 0 };
    ZSTD_outBuffer out_buf = { output, output_cap, 0 };

    while (in_buf.pos < in_buf.size) {
        size_t result = ZSTD_compressStream(context->stream, &out_buf, &in_buf);
        if (ZSTD_isError(result)) {
            return -1;
        }

        // If output buffer is full but we have more input, we need more space
        if (out_buf.pos == out_buf.size && in_buf.pos < in_buf.size) {
            // Return what we have - caller should provide more space
            break;
        }
    }

    return static_cast<int32_t>(out_buf.pos);
}

/**
 * Finish streaming compression and flush remaining data.
 *
 * @param ctx         Compression context
 * @param output      Output buffer
 * @param output_cap  Output buffer capacity
 * @return            Number of bytes written to output, or -1 on error.
 *                    Returns 0 when all data has been flushed.
 */
int32_t cu_stream_compress_finish(
    void* ctx,
    uint8_t* output,
    size_t output_cap
) {
    auto* context = static_cast<CompressStreamContext*>(ctx);
    if (!context) {
        return -1;
    }

    ZSTD_outBuffer out_buf = { output, output_cap, 0 };

    size_t remaining = ZSTD_endStream(context->stream, &out_buf);
    if (ZSTD_isError(remaining)) {
        return -1;
    }

    if (remaining == 0) {
        context->finished = true;
    }

    return static_cast<int32_t>(out_buf.pos);
}

/**
 * Destroy a streaming compression context.
 *
 * @param ctx  Compression context to destroy
 */
void cu_stream_compress_destroy(void* ctx) {
    auto* context = static_cast<CompressStreamContext*>(ctx);
    if (context) {
        if (context->stream) {
            ZSTD_freeCStream(context->stream);
        }
        free(context);
    }
}

// ============================================================================
// Streaming Decompression
// ============================================================================

/**
 * Streaming decompression context.
 */
struct DecompressStreamContext {
    ZSTD_DStream* stream;
    bool finished;
};

/**
 * Create a streaming decompression context.
 *
 * @return  Pointer to context, or nullptr on error
 */
void* cu_stream_decompress_create() {
    ZSTD_DStream* stream = ZSTD_createDStream();
    if (!stream) {
        return nullptr;
    }

    size_t result = ZSTD_initDStream(stream);
    if (ZSTD_isError(result)) {
        ZSTD_freeDStream(stream);
        return nullptr;
    }

    auto* ctx = static_cast<DecompressStreamContext*>(
        malloc(sizeof(DecompressStreamContext))
    );
    if (!ctx) {
        ZSTD_freeDStream(stream);
        return nullptr;
    }

    ctx->stream = stream;
    ctx->finished = false;
    return ctx;
}

/**
 * Write data to streaming decompressor.
 *
 * @param ctx         Decompression context
 * @param input       Compressed input data
 * @param input_len   Input length
 * @param output      Output buffer
 * @param output_cap  Output buffer capacity
 * @return            Number of bytes written to output, or -1 on error
 */
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

    ZSTD_inBuffer in_buf = { input, input_len, 0 };
    ZSTD_outBuffer out_buf = { output, output_cap, 0 };

    while (in_buf.pos < in_buf.size) {
        size_t result = ZSTD_decompressStream(context->stream, &out_buf, &in_buf);
        if (ZSTD_isError(result)) {
            return -1;
        }

        // If result is 0, we've finished a frame
        if (result == 0) {
            context->finished = true;
            break;
        }

        // If output buffer is full but we have more input, we need more space
        if (out_buf.pos == out_buf.size && in_buf.pos < in_buf.size) {
            break;
        }
    }

    return static_cast<int32_t>(out_buf.pos);
}

/**
 * Check if streaming decompression is complete.
 *
 * @param ctx  Decompression context
 * @return     1 if finished, 0 if not, -1 on error
 */
int32_t cu_stream_decompress_finish(void* ctx) {
    auto* context = static_cast<DecompressStreamContext*>(ctx);
    if (!context) {
        return -1;
    }
    return context->finished ? 1 : 0;
}

/**
 * Destroy a streaming decompression context.
 *
 * @param ctx  Decompression context to destroy
 */
void cu_stream_decompress_destroy(void* ctx) {
    auto* context = static_cast<DecompressStreamContext*>(ctx);
    if (context) {
        if (context->stream) {
            ZSTD_freeDStream(context->stream);
        }
        free(context);
    }
}

} // extern "C"
