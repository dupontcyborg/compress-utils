/**
 * LZ4 WebAssembly Bindings
 *
 * Provides the C interface for the LZ4 compression algorithm
 * that can be compiled to WebAssembly using Emscripten.
 *
 * Uses LZ4 HC (High Compression) mode for better compression ratios.
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "lz4.h"
#include "lz4hc.h"

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
    int max_size = LZ4_compressBound(static_cast<int>(input_len));
    if (max_size <= 0) {
        *output_len = 0;
        return nullptr;
    }

    // Add 4 bytes for storing original size (needed for decompression)
    size_t alloc_size = static_cast<size_t>(max_size) + 4;

    // Allocate output buffer
    uint8_t* output = static_cast<uint8_t*>(malloc(alloc_size));
    if (!output) {
        *output_len = 0;
        return nullptr;
    }

    // Store original size in first 4 bytes (little-endian)
    output[0] = static_cast<uint8_t>(input_len & 0xFF);
    output[1] = static_cast<uint8_t>((input_len >> 8) & 0xFF);
    output[2] = static_cast<uint8_t>((input_len >> 16) & 0xFF);
    output[3] = static_cast<uint8_t>((input_len >> 24) & 0xFF);

    // Use HC mode for better compression
    int compressed_size = LZ4_compress_HC(
        reinterpret_cast<const char*>(input),
        reinterpret_cast<char*>(output + 4),
        static_cast<int>(input_len),
        max_size,
        level
    );

    if (compressed_size <= 0) {
        free(output);
        *output_len = 0;
        return nullptr;
    }

    size_t total_size = static_cast<size_t>(compressed_size) + 4;

    // Shrink buffer to actual size
    uint8_t* shrunk = static_cast<uint8_t*>(realloc(output, total_size));
    if (shrunk) {
        output = shrunk;
    }

    *output_len = total_size;
    return output;
}

uint8_t* cu_decompress(
    const uint8_t* input,
    size_t input_len,
    size_t* output_len
) {
    if (input_len < 4) {
        *output_len = 0;
        return nullptr;
    }

    // Read original size from first 4 bytes (little-endian)
    size_t original_size =
        static_cast<size_t>(input[0]) |
        (static_cast<size_t>(input[1]) << 8) |
        (static_cast<size_t>(input[2]) << 16) |
        (static_cast<size_t>(input[3]) << 24);

    // Sanity check
    if (original_size > 2147483647) {  // LZ4 max block size
        *output_len = 0;
        return nullptr;
    }

    // Allocate output buffer
    uint8_t* output = static_cast<uint8_t*>(malloc(original_size));
    if (!output) {
        *output_len = 0;
        return nullptr;
    }

    int result = LZ4_decompress_safe(
        reinterpret_cast<const char*>(input + 4),
        reinterpret_cast<char*>(output),
        static_cast<int>(input_len - 4),
        static_cast<int>(original_size)
    );

    if (result < 0) {
        free(output);
        *output_len = 0;
        return nullptr;
    }

    *output_len = static_cast<size_t>(result);
    return output;
}

// ============================================================================
// Streaming Compression
// ============================================================================

struct CompressStreamContext {
    LZ4_streamHC_t* stream;
    uint8_t* ring_buffer;
    size_t ring_pos;
    bool finished;
    int level;
    static const size_t RING_BUFFER_SIZE = 65536;
    static const size_t BLOCK_SIZE = 65536;
};

void* cu_stream_compress_create(int level) {
    auto* ctx = static_cast<CompressStreamContext*>(
        malloc(sizeof(CompressStreamContext))
    );
    if (!ctx) {
        return nullptr;
    }

    ctx->stream = LZ4_createStreamHC();
    if (!ctx->stream) {
        free(ctx);
        return nullptr;
    }

    LZ4_resetStreamHC_fast(ctx->stream, level);

    ctx->ring_buffer = static_cast<uint8_t*>(malloc(CompressStreamContext::RING_BUFFER_SIZE));
    if (!ctx->ring_buffer) {
        LZ4_freeStreamHC(ctx->stream);
        free(ctx);
        return nullptr;
    }

    ctx->ring_pos = 0;
    ctx->finished = false;
    ctx->level = level;
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

    // For simplicity, process input in one block
    // In production, you'd want to handle larger inputs
    if (input_len > CompressStreamContext::BLOCK_SIZE) {
        input_len = CompressStreamContext::BLOCK_SIZE;
    }

    // Copy to ring buffer
    memcpy(context->ring_buffer + context->ring_pos, input, input_len);

    int compressed = LZ4_compress_HC_continue(
        context->stream,
        reinterpret_cast<const char*>(context->ring_buffer + context->ring_pos),
        reinterpret_cast<char*>(output + 4),  // Leave room for size prefix
        static_cast<int>(input_len),
        static_cast<int>(output_cap - 4)
    );

    if (compressed <= 0) {
        return -1;
    }

    // Write block size prefix
    output[0] = static_cast<uint8_t>(input_len & 0xFF);
    output[1] = static_cast<uint8_t>((input_len >> 8) & 0xFF);
    output[2] = static_cast<uint8_t>(compressed & 0xFF);
    output[3] = static_cast<uint8_t>((compressed >> 8) & 0xFF);

    context->ring_pos = (context->ring_pos + input_len) % CompressStreamContext::RING_BUFFER_SIZE;

    return compressed + 4;
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

    // Write end marker (zero-length block)
    if (output_cap >= 4) {
        output[0] = 0;
        output[1] = 0;
        output[2] = 0;
        output[3] = 0;
        context->finished = true;
        return 4;
    }

    return 0;
}

void cu_stream_compress_destroy(void* ctx) {
    auto* context = static_cast<CompressStreamContext*>(ctx);
    if (context) {
        if (context->stream) {
            LZ4_freeStreamHC(context->stream);
        }
        if (context->ring_buffer) {
            free(context->ring_buffer);
        }
        free(context);
    }
}

// ============================================================================
// Streaming Decompression
// ============================================================================

struct DecompressStreamContext {
    LZ4_streamDecode_t* stream;
    uint8_t* ring_buffer;
    size_t ring_pos;
    bool finished;
    static const size_t RING_BUFFER_SIZE = 65536;
};

void* cu_stream_decompress_create() {
    auto* ctx = static_cast<DecompressStreamContext*>(
        malloc(sizeof(DecompressStreamContext))
    );
    if (!ctx) {
        return nullptr;
    }

    ctx->stream = LZ4_createStreamDecode();
    if (!ctx->stream) {
        free(ctx);
        return nullptr;
    }

    ctx->ring_buffer = static_cast<uint8_t*>(malloc(DecompressStreamContext::RING_BUFFER_SIZE));
    if (!ctx->ring_buffer) {
        LZ4_freeStreamDecode(ctx->stream);
        free(ctx);
        return nullptr;
    }

    ctx->ring_pos = 0;
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

    if (input_len < 4) {
        return 0;  // Need more data for header
    }

    // Read block header
    size_t orig_size = static_cast<size_t>(input[0]) | (static_cast<size_t>(input[1]) << 8);
    size_t comp_size = static_cast<size_t>(input[2]) | (static_cast<size_t>(input[3]) << 8);

    // Check for end marker
    if (orig_size == 0 && comp_size == 0) {
        context->finished = true;
        return 0;
    }

    if (input_len < comp_size + 4) {
        return 0;  // Need more data
    }

    if (output_cap < orig_size) {
        return -1;  // Output buffer too small
    }

    int result = LZ4_decompress_safe_continue(
        context->stream,
        reinterpret_cast<const char*>(input + 4),
        reinterpret_cast<char*>(output),
        static_cast<int>(comp_size),
        static_cast<int>(output_cap)
    );

    if (result < 0) {
        return -1;
    }

    return result;
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
        if (context->stream) {
            LZ4_freeStreamDecode(context->stream);
        }
        if (context->ring_buffer) {
            free(context->ring_buffer);
        }
        free(context);
    }
}

} // extern "C"
