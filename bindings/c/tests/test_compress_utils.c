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

// Define tests for each available algorithm (based on preprocessor directives)
#ifdef INCLUDE_BROTLI
DEFINE_ALGO_TESTS(BROTLI)
#endif

#ifdef INCLUDE_XZ
DEFINE_ALGO_TESTS(XZ)
#endif

#ifdef INCLUDE_ZLIB
DEFINE_ALGO_TESTS(ZLIB)
#endif

#ifdef INCLUDE_ZSTD
DEFINE_ALGO_TESTS(ZSTD)
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
#endif

#ifdef INCLUDE_ZLIB
    CU_pSuite pSuiteZlib = CU_add_suite("Zlib Compression Tests", 0, 0);
    if (pSuiteZlib != NULL) {
        RegisterZlibTests(pSuiteZlib);
    }
#endif

#ifdef INCLUDE_ZSTD
    CU_pSuite pSuiteZstd = CU_add_suite("Zstd Compression Tests", 0, 0);
    if (pSuiteZstd != NULL) {
        RegisterZstdTests(pSuiteZstd);
    }
#endif

#ifdef INCLUDE_XZ
    CU_pSuite pSuiteXZ = CU_add_suite("XZ/LZMA Compression Tests", 0, 0);
    if (pSuiteXZ != NULL) {
        RegisterXZTests(pSuiteXZ);
    }
#endif
    CU_basic_run_tests();
    CU_cleanup_registry();
    return CU_get_error();
}
