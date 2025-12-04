#include "compress_utils.h"

#include <CUnit/Basic.h>
#include <CUnit/CUnit.h>
#include <stdlib.h>
#include <string.h>

// Sample test data
#define SAMPLE_SIZE 11
const uint8_t SAMPLE_DATA[SAMPLE_SIZE] = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};

// Helper function to generate random data
uint8_t* GenerateData(size_t size_in_bytes) {
    uint8_t* data = malloc(size_in_bytes);
    if (!data) return NULL;

    for (size_t i = 0; i < size_in_bytes; ++i) {
        data[i] = rand() % 256;
    }
    return data;
}

// Helper function to generate repetitive data
uint8_t* GenerateRepetitiveData(size_t size_in_bytes, uint8_t value) {
    uint8_t* data = malloc(size_in_bytes);
    if (!data) return NULL;
    memset(data, value, size_in_bytes);
    return data;
}

// Helper function to check compression and decompression for a given algorithm
void CheckCompressionAndDecompression(Algorithm algorithm, const uint8_t* data, size_t data_size,
                                      int level) {
    uint8_t* compressed_data = NULL;
    uint8_t* decompressed_data = NULL;

    // Compress the data
    int64_t compressed_size = compress_data(data, data_size, &compressed_data, algorithm, level);
    CU_ASSERT(compressed_size > 0);  // Check that compression was successful

    // Decompress the data
    int64_t decompressed_size =
        decompress_data(compressed_data, compressed_size, &decompressed_data, algorithm);
    CU_ASSERT(decompressed_size == data_size);                   // Sizes must match
    CU_ASSERT(memcmp(decompressed_data, data, data_size) == 0);  // Data must match the original

    free(compressed_data);
    free(decompressed_data);
}

// Macro to define tests for multiple algorithms
#define DEFINE_ALGO_TESTS(ALGO)                                                  \
    void test_compress_decompress_sample_##ALGO(void) {                          \
        CheckCompressionAndDecompression(ALGO, SAMPLE_DATA, SAMPLE_SIZE, 5);     \
    }                                                                            \
    void test_compress_decompress_empty_##ALGO(void) {                           \
        uint8_t empty_data[1] = {0};                                             \
        CheckCompressionAndDecompression(ALGO, empty_data, 0, 5);                \
    }                                                                            \
    void test_compress_decompress_1b_##ALGO(void) {                              \
        uint8_t small_data[1] = {'A'};                                           \
        CheckCompressionAndDecompression(ALGO, small_data, 1, 5);                \
    }                                                                            \
    void test_compress_decompress_1MB_##ALGO(void) {                             \
        uint8_t* large_data = GenerateData(1024 * 1024);                         \
        CU_ASSERT_PTR_NOT_NULL(large_data);                                      \
        CheckCompressionAndDecompression(ALGO, large_data, 1024 * 1024, 5);      \
        free(large_data);                                                        \
    }                                                                            \
    void test_compress_decompress_32MB_##ALGO(void) {                            \
        uint8_t* large_data = GenerateData(1024 * 1024 * 32);                    \
        CU_ASSERT_PTR_NOT_NULL(large_data);                                      \
        CheckCompressionAndDecompression(ALGO, large_data, 1024 * 1024 * 32, 1); \
        free(large_data);                                                        \
    }                                                                            \
    void test_compress_decompress_repetitive_##ALGO(void) {                      \
        uint8_t* repetitive_data = GenerateRepetitiveData(1024 * 1024, 'A');     \
        CU_ASSERT_PTR_NOT_NULL(repetitive_data);                                 \
        CheckCompressionAndDecompression(ALGO, repetitive_data, 1024 * 1024, 1); \
        free(repetitive_data);                                                   \
    }

// ============================================================================
// Streaming API Tests
// ============================================================================

// Helper function to check streaming compression and decompression
void CheckStreamingCompressionAndDecompression(Algorithm algorithm, const uint8_t* data,
                                               size_t data_size, size_t chunk_size, int level) {
    // Create compression stream
    CompressStream* compress_stream = compress_stream_create(algorithm, level);
    CU_ASSERT_PTR_NOT_NULL(compress_stream);
    if (!compress_stream) return;

    // Compress in chunks, collecting output
    size_t compressed_capacity = data_size + 1024;  // Extra space for headers
    uint8_t* all_compressed = malloc(compressed_capacity);
    CU_ASSERT_PTR_NOT_NULL(all_compressed);
    size_t total_compressed = 0;

    for (size_t i = 0; i < data_size; i += chunk_size) {
        size_t this_chunk = (i + chunk_size > data_size) ? (data_size - i) : chunk_size;
        uint8_t* chunk_output = NULL;
        int64_t chunk_size_out = compress_stream_write(compress_stream, data + i, this_chunk, &chunk_output);
        CU_ASSERT(chunk_size_out >= 0);
        if (chunk_size_out > 0 && chunk_output) {
            memcpy(all_compressed + total_compressed, chunk_output, chunk_size_out);
            total_compressed += chunk_size_out;
            free(chunk_output);
        }
    }

    // Finish compression
    uint8_t* final_output = NULL;
    int64_t final_size = compress_stream_finish(compress_stream, &final_output);
    CU_ASSERT(final_size >= 0);
    if (final_size > 0 && final_output) {
        memcpy(all_compressed + total_compressed, final_output, final_size);
        total_compressed += final_size;
        free(final_output);
    }

    CU_ASSERT(compress_stream_is_finished(compress_stream) == 1);
    compress_stream_destroy(compress_stream);

    // Decompress with streaming API
    DecompressStream* decompress_stream = decompress_stream_create(algorithm);
    CU_ASSERT_PTR_NOT_NULL(decompress_stream);
    if (!decompress_stream) {
        free(all_compressed);
        return;
    }

    uint8_t* all_decompressed = malloc(data_size + 1024);
    CU_ASSERT_PTR_NOT_NULL(all_decompressed);
    size_t total_decompressed = 0;

    for (size_t i = 0; i < total_compressed; i += chunk_size) {
        size_t this_chunk = (i + chunk_size > total_compressed) ? (total_compressed - i) : chunk_size;
        uint8_t* chunk_output = NULL;
        int64_t chunk_size_out = decompress_stream_write(decompress_stream, all_compressed + i, this_chunk, &chunk_output);
        CU_ASSERT(chunk_size_out >= 0);
        if (chunk_size_out > 0 && chunk_output) {
            memcpy(all_decompressed + total_decompressed, chunk_output, chunk_size_out);
            total_decompressed += chunk_size_out;
            free(chunk_output);
        }
    }

    // Finish decompression
    final_output = NULL;
    final_size = decompress_stream_finish(decompress_stream, &final_output);
    CU_ASSERT(final_size >= 0);
    if (final_size > 0 && final_output) {
        memcpy(all_decompressed + total_decompressed, final_output, final_size);
        total_decompressed += final_size;
        free(final_output);
    }

    CU_ASSERT(decompress_stream_is_finished(decompress_stream) == 1);
    decompress_stream_destroy(decompress_stream);

    // Verify result
    CU_ASSERT(total_decompressed == data_size);
    CU_ASSERT(memcmp(all_decompressed, data, data_size) == 0);

    free(all_compressed);
    free(all_decompressed);
}

// Macro to define streaming tests for multiple algorithms
#define DEFINE_STREAMING_TESTS(ALGO)                                                           \
    void test_streaming_sample_##ALGO(void) {                                                  \
        CheckStreamingCompressionAndDecompression(ALGO, SAMPLE_DATA, SAMPLE_SIZE, 4, 5);       \
    }                                                                                          \
    void test_streaming_single_byte_chunks_##ALGO(void) {                                      \
        CheckStreamingCompressionAndDecompression(ALGO, SAMPLE_DATA, SAMPLE_SIZE, 1, 5);       \
    }                                                                                          \
    void test_streaming_large_##ALGO(void) {                                                   \
        size_t size = (ALGO == XZ) ? 256 * 1024 : 1024 * 1024;                                 \
        uint8_t* large_data = GenerateData(size);                                              \
        CU_ASSERT_PTR_NOT_NULL(large_data);                                                    \
        CheckStreamingCompressionAndDecompression(ALGO, large_data, size, 64 * 1024, 1);       \
        free(large_data);                                                                      \
    }                                                                                          \
    void test_streaming_is_finished_##ALGO(void) {                                             \
        CompressStream* stream = compress_stream_create(ALGO, 3);                              \
        CU_ASSERT_PTR_NOT_NULL(stream);                                                        \
        CU_ASSERT(compress_stream_is_finished(stream) == 0);                                   \
        uint8_t* output = NULL;                                                                \
        compress_stream_write(stream, SAMPLE_DATA, SAMPLE_SIZE, &output);                      \
        if (output) free(output);                                                              \
        CU_ASSERT(compress_stream_is_finished(stream) == 0);                                   \
        compress_stream_finish(stream, &output);                                               \
        if (output) free(output);                                                              \
        CU_ASSERT(compress_stream_is_finished(stream) == 1);                                   \
        compress_stream_destroy(stream);                                                       \
    }                                                                                          \
    void test_streaming_algorithm_##ALGO(void) {                                               \
        CompressStream* cstream = compress_stream_create(ALGO, 3);                             \
        CU_ASSERT(compress_stream_algorithm(cstream) == (int)ALGO);                            \
        compress_stream_destroy(cstream);                                                      \
        DecompressStream* dstream = decompress_stream_create(ALGO);                            \
        CU_ASSERT(decompress_stream_algorithm(dstream) == (int)ALGO);                          \
        decompress_stream_destroy(dstream);                                                    \
    }

// Define tests for each available algorithm (based on preprocessor directives)
#ifdef INCLUDE_BROTLI
DEFINE_ALGO_TESTS(BROTLI)
DEFINE_STREAMING_TESTS(BROTLI)
#endif

#ifdef INCLUDE_XZ
DEFINE_ALGO_TESTS(XZ)
DEFINE_STREAMING_TESTS(XZ)
#endif

#ifdef INCLUDE_ZLIB
DEFINE_ALGO_TESTS(ZLIB)
DEFINE_STREAMING_TESTS(ZLIB)
#endif

#ifdef INCLUDE_ZSTD
DEFINE_ALGO_TESTS(ZSTD)
DEFINE_STREAMING_TESTS(ZSTD)
#endif

#ifdef INCLUDE_BROTLI
void RegisterBrotliTests(CU_pSuite suite) {
    CU_add_test(suite, "Sample Data Compression/Decompression",
                test_compress_decompress_sample_BROTLI);
    CU_add_test(suite, "Empty Data Compression/Decompression", test_compress_decompress_empty_BROTLI);
    CU_add_test(suite, "1 Byte Compression/Decompression", test_compress_decompress_1b_BROTLI);
    CU_add_test(suite, "1MB Data Compression/Decompression", test_compress_decompress_1MB_BROTLI);
    CU_add_test(suite, "32MB Data Compression/Decompression", test_compress_decompress_32MB_BROTLI);
    CU_add_test(suite, "Repetitive Data Compression/Decompression",
                test_compress_decompress_repetitive_BROTLI);
}
void RegisterBrotliStreamingTests(CU_pSuite suite) {
    CU_add_test(suite, "Streaming Sample Data", test_streaming_sample_BROTLI);
    CU_add_test(suite, "Streaming Single Byte Chunks", test_streaming_single_byte_chunks_BROTLI);
    CU_add_test(suite, "Streaming Large Data", test_streaming_large_BROTLI);
    CU_add_test(suite, "Streaming IsFinished State", test_streaming_is_finished_BROTLI);
    CU_add_test(suite, "Streaming Algorithm Property", test_streaming_algorithm_BROTLI);
}
#endif

#ifdef INCLUDE_ZLIB
void RegisterZlibTests(CU_pSuite suite) {
    CU_add_test(suite, "Sample Data Compression/Decompression",
                test_compress_decompress_sample_ZLIB);
    CU_add_test(suite, "Empty Data Compression/Decompression", test_compress_decompress_empty_ZLIB);
    CU_add_test(suite, "1 Byte Compression/Decompression", test_compress_decompress_1b_ZLIB);
    CU_add_test(suite, "1MB Data Compression/Decompression", test_compress_decompress_1MB_ZLIB);
    CU_add_test(suite, "32MB Data Compression/Decompression", test_compress_decompress_32MB_ZLIB);
    CU_add_test(suite, "Repetitive Data Compression/Decompression",
                test_compress_decompress_repetitive_ZLIB);
}
void RegisterZlibStreamingTests(CU_pSuite suite) {
    CU_add_test(suite, "Streaming Sample Data", test_streaming_sample_ZLIB);
    CU_add_test(suite, "Streaming Single Byte Chunks", test_streaming_single_byte_chunks_ZLIB);
    CU_add_test(suite, "Streaming Large Data", test_streaming_large_ZLIB);
    CU_add_test(suite, "Streaming IsFinished State", test_streaming_is_finished_ZLIB);
    CU_add_test(suite, "Streaming Algorithm Property", test_streaming_algorithm_ZLIB);
}
#endif

#ifdef INCLUDE_ZSTD
void RegisterZstdTests(CU_pSuite suite) {
    CU_add_test(suite, "Sample Data Compression/Decompression",
                test_compress_decompress_sample_ZSTD);
    CU_add_test(suite, "Empty Data Compression/Decompression", test_compress_decompress_empty_ZSTD);
    CU_add_test(suite, "1 Byte Compression/Decompression", test_compress_decompress_1b_ZSTD);
    CU_add_test(suite, "1MB Data Compression/Decompression", test_compress_decompress_1MB_ZSTD);
    CU_add_test(suite, "32MB Data Compression/Decompression", test_compress_decompress_32MB_ZSTD);
    CU_add_test(suite, "Repetitive Data Compression/Decompression",
                test_compress_decompress_repetitive_ZSTD);
}
void RegisterZstdStreamingTests(CU_pSuite suite) {
    CU_add_test(suite, "Streaming Sample Data", test_streaming_sample_ZSTD);
    CU_add_test(suite, "Streaming Single Byte Chunks", test_streaming_single_byte_chunks_ZSTD);
    CU_add_test(suite, "Streaming Large Data", test_streaming_large_ZSTD);
    CU_add_test(suite, "Streaming IsFinished State", test_streaming_is_finished_ZSTD);
    CU_add_test(suite, "Streaming Algorithm Property", test_streaming_algorithm_ZSTD);
}
#endif

#ifdef INCLUDE_XZ
void RegisterXZTests(CU_pSuite suite) {
    CU_add_test(suite, "Sample Data Compression/Decompression",
                test_compress_decompress_sample_XZ);
    CU_add_test(suite, "Empty Data Compression/Decompression", test_compress_decompress_empty_XZ);
    CU_add_test(suite, "1 Byte Compression/Decompression", test_compress_decompress_1b_XZ);
    CU_add_test(suite, "1MB Data Compression/Decompression", test_compress_decompress_1MB_XZ);
    CU_add_test(suite, "32MB Data Compression/Decompression", test_compress_decompress_32MB_XZ);
    CU_add_test(suite, "Repetitive Data Compression/Decompression",
                test_compress_decompress_repetitive_XZ);
}
void RegisterXZStreamingTests(CU_pSuite suite) {
    CU_add_test(suite, "Streaming Sample Data", test_streaming_sample_XZ);
    CU_add_test(suite, "Streaming Single Byte Chunks", test_streaming_single_byte_chunks_XZ);
    CU_add_test(suite, "Streaming Large Data", test_streaming_large_XZ);
    CU_add_test(suite, "Streaming IsFinished State", test_streaming_is_finished_XZ);
    CU_add_test(suite, "Streaming Algorithm Property", test_streaming_algorithm_XZ);
}
#endif

int main() {
    if (CUE_SUCCESS != CU_initialize_registry()) {
        return CU_get_error();
    }

#ifdef INCLUDE_BROTLI
    CU_pSuite pSuiteBrotli = CU_add_suite("Brotli Compression Tests", 0, 0);
    if (pSuiteBrotli != NULL) {
        RegisterBrotliTests(pSuiteBrotli);
    }
    CU_pSuite pSuiteBrotliStream = CU_add_suite("Brotli Streaming Tests", 0, 0);
    if (pSuiteBrotliStream != NULL) {
        RegisterBrotliStreamingTests(pSuiteBrotliStream);
    }
#endif

#ifdef INCLUDE_ZLIB
    CU_pSuite pSuiteZlib = CU_add_suite("Zlib Compression Tests", 0, 0);
    if (pSuiteZlib != NULL) {
        RegisterZlibTests(pSuiteZlib);
    }
    CU_pSuite pSuiteZlibStream = CU_add_suite("Zlib Streaming Tests", 0, 0);
    if (pSuiteZlibStream != NULL) {
        RegisterZlibStreamingTests(pSuiteZlibStream);
    }
#endif

#ifdef INCLUDE_ZSTD
    CU_pSuite pSuiteZstd = CU_add_suite("Zstd Compression Tests", 0, 0);
    if (pSuiteZstd != NULL) {
        RegisterZstdTests(pSuiteZstd);
    }
    CU_pSuite pSuiteZstdStream = CU_add_suite("Zstd Streaming Tests", 0, 0);
    if (pSuiteZstdStream != NULL) {
        RegisterZstdStreamingTests(pSuiteZstdStream);
    }
#endif

#ifdef INCLUDE_XZ
    CU_pSuite pSuiteXZ = CU_add_suite("XZ/LZMA Compression Tests", 0, 0);
    if (pSuiteXZ != NULL) {
        RegisterXZTests(pSuiteXZ);
    }
    CU_pSuite pSuiteXZStream = CU_add_suite("XZ/LZMA Streaming Tests", 0, 0);
    if (pSuiteXZStream != NULL) {
        RegisterXZStreamingTests(pSuiteXZStream);
    }
#endif
    CU_basic_run_tests();
    CU_cleanup_registry();
    return CU_get_error();
}
