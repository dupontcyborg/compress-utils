#include "compress_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep checks active in Release builds, where assert() is compiled out. */
#define CHECK(cond, ...) do {                                      \
    if (!(cond)) {                                                 \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);       \
        fprintf(stderr, __VA_ARGS__);                              \
        fprintf(stderr, "  cu_last_error: %s\n", cu_last_error()); \
        return 1;                                                  \
    }                                                              \
} while (0)
#define CHECK_OK(call) do {                                        \
    cu_status_t status = (call);                                   \
    CHECK(status == CU_OK, "%s -> %s\n", #call, cu_strerror(status)); \
} while (0)

static int read_fixture(const char* name, unsigned char* output, size_t* length) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.bin", LZO_FIXTURE_DIRECTORY, name);
    FILE* file = fopen(path, "rb");
    CHECK(file, "cannot open fixture %s\n", path);
    *length = fread(output, 1, 40000, file);
    int read_error = ferror(file);
    int close_error = fclose(file);
    CHECK(!read_error && !close_error && *length, "cannot read fixture %s\n", path);
    return 0;
}

static int test_match_copy(void) {
    /* Match distance equal to / greater than length uses memcpy; the final
     * block repeats distance-one bytes and requires forward overlap copying. */
    const unsigned char adjacent[] = {21, 'a', 'b', 'c', 'd', 34, 12, 0, 17, 0, 0};
    const unsigned char distant[] = {23, 'a', 'b', 'c', 'd', 'e', 'f', 34, 20, 0, 17, 0, 0};
    const unsigned char overlapping[] = {18, 'a', 36, 0, 0, 17, 0, 0};
    const unsigned char* inputs[] = {adjacent, distant, overlapping};
    const size_t lengths[] = {11, sizeof(distant), 8};
    const char* expected[] = {"abcdabcd", "abcdefabcd", "aaaaaaa"};
    for (size_t index = 0; index < 3; index++) {
        unsigned char output[16];
        size_t capacity = sizeof(output), hint = 0;
        CHECK_OK(cu_decompress(CU_ALGO_LZO, inputs[index], lengths[index], output, &capacity));
        CHECK(capacity == strlen(expected[index]) && !memcmp(output, expected[index], capacity),
              "match-copy case %zu mismatch\n", index);
        CHECK_OK(cu_decompress_size_hint(CU_ALGO_LZO, inputs[index], lengths[index], &hint));
        CHECK(hint == capacity, "match-copy case %zu size mismatch\n", index);
    }
    return 0;
}

int main(void) {
    unsigned char input[40000], expected[40000], output[40000];
    size_t expected_length = 0;
    CHECK(!read_fixture("input", expected, &expected_length), "input fixture failed\n");
    CHECK(!test_match_copy(), "match-copy checks failed\n");
    const char* variants[] = {"1", "999"};
    for (size_t variant = 0; variant < 2; variant++) {
        size_t length = 0, capacity = sizeof(output), hint = 0;
        CHECK(!read_fixture(variants[variant], input, &length), "compressed fixture failed\n");
        CHECK_OK(cu_decompress_size_hint(CU_ALGO_LZO, input, length, &hint));
        CHECK(hint == expected_length, "size hint mismatch\n");
        CHECK_OK(cu_decompress(CU_ALGO_LZO, input, length, output, &capacity));
        CHECK(capacity == expected_length && !memcmp(output, expected, capacity), "fixture mismatch\n");
        capacity = expected_length - 1;
        CHECK(cu_decompress(CU_ALGO_LZO, input, length, output, &capacity) == CU_ERR_BUF_TOO_SMALL,
              "undersized output must fail\n");
        for (size_t truncated = 0; truncated < length; truncated++) {
            capacity = sizeof(output);
            CHECK(cu_decompress(CU_ALGO_LZO, input, truncated, output, &capacity) != CU_OK,
                  "truncated input %zu must fail\n", truncated);
        }
        input[length] = 0;
        capacity = sizeof(output);
        CHECK(cu_decompress(CU_ALGO_LZO, input, length + 1, output, &capacity) == CU_ERR_DECOMPRESSION,
              "trailing input must fail\n");
    }
    unsigned char empty[] = {17, 0, 0}, invalid[] = {17, 4, 0};
    size_t capacity = 0;
    CHECK_OK(cu_decompress(CU_ALGO_LZO, empty, sizeof(empty), NULL, &capacity));
    CHECK(!capacity, "empty block must produce no bytes\n");
    capacity = sizeof(output);
    CHECK(cu_decompress(CU_ALGO_LZO, invalid, sizeof(invalid), output, &capacity) == CU_ERR_DECOMPRESSION,
          "invalid end marker must fail\n");
    cu_compress_stream_t* compressor = NULL;
    cu_decompress_stream_t* decompressor = NULL;
    CHECK(cu_compress_bound(10, CU_ALGO_LZO) == 0, "no encoder bound\n");
    CHECK(cu_compress(CU_ALGO_LZO, empty, 3, output, &capacity, 5) == CU_ERR_UNSUPPORTED_ALGO,
          "no encoder\n");
    CHECK(cu_compress_stream_create(CU_ALGO_LZO, 5, &compressor) == CU_ERR_UNSUPPORTED_ALGO && !compressor,
          "no streaming encoder\n");
    CHECK(cu_decompress_stream_create(CU_ALGO_LZO, &decompressor) == CU_ERR_UNSUPPORTED_ALGO && !decompressor,
          "no streaming decoder\n");
    unsigned random = 42;
    cu_set_max_decompressed_size(4096);
    for (size_t iteration = 0; iteration < 10000; iteration++) {
        size_t length = iteration % 512;
        for (size_t index = 0; index < length; index++) {
            random = random * 1664525 + 1013904223;
            input[index] = random >> 24;
        }
        capacity = 4096;
        cu_decompress(CU_ALGO_LZO, input, length, output, &capacity);
        size_t hint;
        cu_decompress_size_hint(CU_ALGO_LZO, input, length, &hint);
    }
    puts("LZO oracle, boundary, unsupported-operation and bounded fuzz tests passed");
    return 0;
}
