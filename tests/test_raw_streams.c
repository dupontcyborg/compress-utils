/* Public-ABI incremental decoder conformance and bounded malformed corpus. */
#include "compress_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(value)                                                                         \
    do {                                                                                     \
        if (!(value)) {                                                                      \
            fprintf(stderr, "raw stream check failed: %s:%d: %s (%s)\n", __FILE__, __LINE__, \
                    #value, cu_last_error());                                                \
            exit(1);                                                                         \
        }                                                                                    \
    } while (0)

static cu_status_t decode(cu_algorithm_t algorithm, const unsigned char* input, size_t length,
                          size_t chunk, size_t output_capacity, unsigned char* decoded,
                          size_t maximum, size_t* produced) {
    cu_decompress_stream_t* stream = NULL;
    CHECK(cu_decompress_stream_create(algorithm, &stream) == CU_OK);
    unsigned char output[65536];
    size_t total = 0;
    cu_status_t status = CU_OK;
    for (size_t offset = 0; offset < length && status == CU_OK;) {
        size_t count = length - offset < chunk ? length - offset : chunk;
        size_t written = output_capacity;
        status = cu_decompress_stream_write(stream, input + offset, count, output, &written);
        offset += count;
        for (;;) {
            CHECK(written <= output_capacity && written <= maximum - total);
            memcpy(decoded + total, output, written);
            total += written;
            if (status != CU_ERR_BUF_TOO_SMALL) break;
            written = output_capacity;
            status = cu_decompress_stream_write(stream, NULL, 0, output, &written);
        }
    }
    *produced = total;
    if (status == CU_OK) {
        size_t written = output_capacity;
        status = cu_decompress_stream_finish(stream, output, &written);
        CHECK(written <= maximum - total);
        memcpy(decoded + total, output, written);
        total += written;
    }
    cu_decompress_stream_destroy(stream);
    return status;
}
static void valid(cu_algorithm_t algorithm, const unsigned char* input, size_t length,
                  const unsigned char* expected, size_t expected_length) {
    unsigned char* output = (unsigned char*)malloc(expected_length + 64);
    size_t chunks[] = {1, 2, 7, 257, 65536, length};
    size_t capacities[] = {1, 7, 65536};
    for (size_t index = 0; index < sizeof(chunks) / sizeof(chunks[0]); index++) {
        for (size_t buffer = 0; buffer < sizeof(capacities) / sizeof(capacities[0]); buffer++) {
            size_t produced;
            CHECK(decode(algorithm, input, length, chunks[index], capacities[buffer], output,
                         expected_length + 64, &produced) == CU_OK);
            /* Both codecs must emit the complete payload before finish. */
            CHECK(produced == expected_length);
            CHECK(memcmp(output, expected, expected_length) == 0);
        }
    }
    free(output);
}
static unsigned char* fixture(const char* name, size_t* length) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.bin", LZO_FIXTURE_DIRECTORY, name);
    FILE* file = fopen(path, "rb");
    CHECK(file != NULL);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    CHECK(size >= 0);
    *length = (size_t)size;
    rewind(file);
    unsigned char* bytes = (unsigned char*)malloc(*length + 1);
    CHECK(fread(bytes, 1, *length, file) == *length);
    fclose(file);
    return bytes;
}
int main(void) {
    size_t expected_length;
    unsigned char* expected = fixture("input", &expected_length);
    const char* variants[] = {"1", "999"};
    for (size_t variant = 0; variant < 2; variant++) {
        size_t length;
        unsigned char* encoded = fixture(variants[variant], &length);
        valid(CU_ALGO_LZO, encoded, length, expected, expected_length);
        free(encoded);
    }
    free(expected);
    size_t size = 2 * 1024 * 1024;
    unsigned char* input = (unsigned char*)malloc(size);
    for (size_t index = 0; index < size; index++)
        input[index] = (unsigned char)((index * 17 + index / 97) % 251);
    size_t encoded_length = cu_compress_bound(size, CU_ALGO_LZ4_RAW);
    unsigned char* encoded = (unsigned char*)malloc(encoded_length);
    CHECK(cu_compress(CU_ALGO_LZ4_RAW, input, size, encoded, &encoded_length, 5) == CU_OK);
    valid(CU_ALGO_LZ4_RAW, encoded, encoded_length, input, size);
    free(encoded);
    free(input);
    /* Extended literals wrap history; an M4 match reaches the maximum LZO distance. */
    size_t literal_count = 70000, extension = literal_count - 18;
    unsigned char* literals = (unsigned char*)malloc(literal_count + 4);
    for (size_t index = 0; index < literal_count; index++)
        literals[index] = (unsigned char)(index % 251);
    memcpy(literals + literal_count, literals + literal_count - 49151, 4);
    unsigned char* lzo_block = (unsigned char*)malloc(literal_count + extension / 255 + 16);
    size_t lzo_length = 0;
    lzo_block[lzo_length++] = 0;
    while (extension > 255) {
        lzo_block[lzo_length++] = 0;
        extension -= 255;
    }
    lzo_block[lzo_length++] = (unsigned char)extension;
    memcpy(lzo_block + lzo_length, literals, literal_count);
    lzo_length += literal_count;
    lzo_block[lzo_length++] = 26;
    lzo_block[lzo_length++] = 252;
    lzo_block[lzo_length++] = 255;
    lzo_block[lzo_length++] = 17;
    lzo_block[lzo_length++] = 0;
    lzo_block[lzo_length++] = 0;
    unsigned char* oracle = (unsigned char*)malloc(literal_count + 4);
    size_t oracle_size = literal_count + 4;
    CHECK(cu_decompress(CU_ALGO_LZO, lzo_block, lzo_length, oracle, &oracle_size) == CU_OK);
    CHECK(oracle_size == literal_count + 4 && memcmp(oracle, literals, oracle_size) == 0);
    valid(CU_ALGO_LZO, lzo_block, lzo_length, literals, literal_count + 4);
    free(oracle);
    free(literals);
    free(lzo_block);
    unsigned char empty_lz4[] = {0}, empty_lzo[] = {17, 0, 0};
    valid(CU_ALGO_LZ4_RAW, empty_lz4, sizeof(empty_lz4), empty_lz4, 0);
    valid(CU_ALGO_LZO, empty_lzo, sizeof(empty_lzo), empty_lzo, 0);
    unsigned char hello[] = {22, 'h', 'e', 'l', 'l', 'o', 17, 0, 0};
    unsigned char output[256];
    size_t produced;
    for (size_t length = 0; length < sizeof(hello); length++)
        CHECK(decode(CU_ALGO_LZO, hello, length, 1, 1, output, sizeof(output), &produced) ==
              CU_ERR_TRUNCATED);
    unsigned char trailing[] = {17, 0, 0, 1};
    CHECK(decode(CU_ALGO_LZO, trailing, sizeof(trailing), 1, 7, output, sizeof(output),
                 &produced) == CU_ERR_DECOMPRESSION);
    unsigned char zero_offset[] = {0, 0, 0};
    CHECK(decode(CU_ALGO_LZ4_RAW, zero_offset, sizeof(zero_offset), 1, 7, output, sizeof(output),
                 &produced) == CU_ERR_DECOMPRESSION);
    cu_set_max_decompressed_size(4);
    CHECK(decode(CU_ALGO_LZO, hello, sizeof(hello), 1, 7, output, sizeof(output), &produced) ==
          CU_ERR_SIZE_LIMIT);
    cu_set_max_decompressed_size(256);
    unsigned int random = 123;
    for (size_t case_index = 0; case_index < 2000; case_index++) {
        unsigned char bytes[32];
        size_t length = case_index % sizeof(bytes);
        for (size_t index = 0; index < length; index++) {
            random = random * 1664525u + 1013904223u;
            bytes[index] = (unsigned char)(random >> 24);
        }
        for (unsigned int algorithm = 0; algorithm < 2; algorithm++)
            (void)decode(algorithm ? CU_ALGO_LZO : CU_ALGO_LZ4_RAW, bytes, length, 1, 7, output,
                         sizeof(output), &produced);
    }
    cu_set_max_decompressed_size(1024 * 1024 * 1024);
    puts("incremental raw stream tests passed");
    return 0;
}
