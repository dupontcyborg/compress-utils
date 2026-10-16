/*
 * Concatenated zstd frames and gzip members, data after the end of zlib and
 * raw DEFLATE streams, flushing when the output fills exactly, and new input
 * arriving while earlier input is still retained.
 *
 * zstd and gzip decode every frame/member and append the output, as the zstd
 * and gzip CLIs do; zlib rejects trailing data; raw DEFLATE ignores it.
 */
#include "compress_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value)                                                                \
    do {                                                                            \
        if (!(value)) {                                                             \
            fprintf(stderr, "concat check failed: %s:%d: %s (%s)\n", __FILE__,      \
                    __LINE__, #value, cu_last_error());                             \
            exit(1);                                                                \
        }                                                                           \
    } while (0)

#define MAX_OUT 4096

/* Stream-decodes `in`, writing it in two pieces split at `split`, into output
 * buffers of `cap` bytes. Returns the first failing status, or finish()'s. */
static cu_status_t decode(cu_algorithm_t algo, const uint8_t* in, size_t n, size_t split,
                          size_t cap, uint8_t* out, size_t* total) {
    cu_decompress_stream_t* s = NULL;
    CHECK(cu_decompress_stream_create(algo, &s) == CU_OK);
    uint8_t buf[65536];
    cu_status_t st = CU_OK;
    *total = 0;
    const uint8_t* pieces[2] = {in, in + split};
    size_t lengths[2] = {split, n - split};
    for (int p = 0; p < 2 && st == CU_OK; p++) {
        const uint8_t* src = pieces[p];
        size_t len = lengths[p];
        do {
            size_t got = cap;
            st = cu_decompress_stream_write(s, src, len, buf, &got);
            src = NULL;
            len = 0;
            if (st != CU_OK && st != CU_ERR_BUF_TOO_SMALL) break;
            CHECK(got <= cap && *total + got <= MAX_OUT);
            memcpy(out + *total, buf, got);
            *total += got;
        } while (st == CU_ERR_BUF_TOO_SMALL);
    }
    while (st == CU_OK) {
        size_t got = cap;
        st = cu_decompress_stream_finish(s, buf, &got);
        if (st != CU_OK && st != CU_ERR_BUF_TOO_SMALL) break;
        CHECK(got <= cap && *total + got <= MAX_OUT);
        memcpy(out + *total, buf, got);
        *total += got;
        if (st == CU_OK) break;
        st = CU_OK;
    }
    cu_decompress_stream_destroy(s);
    return st;
}

/* Every split point and several output sizes must decode to `expected`. */
static void decodes_everywhere(cu_algorithm_t algo, const uint8_t* in, size_t n,
                               const uint8_t* expected, size_t expected_len) {
    static uint8_t out[MAX_OUT];
    const size_t caps[] = {1, 7, expected_len ? expected_len : 1, 65536};
    for (size_t split = 0; split <= n; split++) {
        for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++) {
            size_t total = 0;
            cu_status_t st = decode(algo, in, n, split, caps[c], out, &total);
            if (st != CU_OK || total != expected_len || memcmp(out, expected, total)) {
                fprintf(stderr, "%s: split %zu cap %zu: status %d, %zu of %zu bytes\n",
                        cu_algorithm_name(algo), split, caps[c], st, total, expected_len);
                exit(1);
            }
        }
    }
}

/* Every split point and output size must fail with `want`. */
static void fails_everywhere(cu_algorithm_t algo, const uint8_t* in, size_t n, cu_status_t want) {
    static uint8_t out[MAX_OUT];
    const size_t caps[] = {1, 65536};
    for (size_t split = 0; split <= n; split++) {
        for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++) {
            size_t total = 0;
            cu_status_t st = decode(algo, in, n, split, caps[c], out, &total);
            if (st != want) {
                fprintf(stderr, "%s: split %zu cap %zu: status %d, want %d\n",
                        cu_algorithm_name(algo), split, caps[c], st, want);
                exit(1);
            }
        }
    }
}

static size_t compress_into(cu_algorithm_t algo, const char* text, uint8_t* dst) {
    size_t cap = cu_compress_bound(strlen(text), algo);
    CHECK(cu_compress(algo, (const uint8_t*)text, strlen(text), dst, &cap, 5) == CU_OK);
    return cap;
}

/* Stream-compressed zstd frames don't record their content size. */
static size_t stream_compress_into(cu_algorithm_t algo, const char* text, uint8_t* dst) {
    cu_compress_stream_t* s = NULL;
    CHECK(cu_compress_stream_create(algo, 5, &s) == CU_OK);
    size_t a = 1024, b = 1024;
    CHECK(cu_compress_stream_write(s, (const uint8_t*)text, strlen(text), dst, &a) == CU_OK);
    CHECK(cu_compress_stream_finish(s, dst + a, &b) == CU_OK);
    cu_compress_stream_destroy(s);
    return a + b;
}

static const char* const PARTS[] = {
    "first frame: the quick brown fox jumps over the lazy dog, the quick brown fox. ",
    "second frame, different text 0123456789 0123456789 0123456789 0123456789. ",
    "third.",
};

/* Concatenates compressed PARTS[0..count), optionally with `gap` bytes of
 * `gap_byte` after each one, and the matching plain text. */
static size_t build(cu_algorithm_t algo, int count, int streamed, size_t gap, uint8_t gap_byte,
                    uint8_t* in, char* text) {
    size_t n = 0;
    text[0] = '\0';
    for (int i = 0; i < count; i++) {
        n += streamed ? stream_compress_into(algo, PARTS[i], in + n)
                      : compress_into(algo, PARTS[i], in + n);
        memset(in + n, gap_byte, gap);
        n += gap;
        strcat(text, PARTS[i]);
    }
    return n;
}

static void test_concatenated(cu_algorithm_t algo) {
    uint8_t in[2048];
    char text[512];
    for (int streamed = 0; streamed < 2; streamed++) {
        for (int count = 1; count <= 3; count++) {
            size_t n = build(algo, count, streamed, 0, 0, in, text);
            decodes_everywhere(algo, in, n, (const uint8_t*)text, strlen(text));

            /* One-shot, with an exact and a generous capacity. */
            uint8_t out[MAX_OUT];
            size_t len = strlen(text);
            CHECK(cu_decompress(algo, in, n, out, &len) == CU_OK);
            CHECK(len == strlen(text) && !memcmp(out, text, len));
            len = sizeof(out);
            CHECK(cu_decompress(algo, in, n, out, &len) == CU_OK);
            CHECK(len == strlen(text) && !memcmp(out, text, len));

            /* The size hint covers every frame, or reports unknown. */
            size_t hint = 0;
            cu_status_t st = cu_decompress_size_hint(algo, in, n, &hint);
            CHECK(st == CU_ERR_SIZE_UNKNOWN || (st == CU_OK && hint == strlen(text)));
            if (algo == CU_ALGO_ZSTD) CHECK(streamed ? st == CU_ERR_SIZE_UNKNOWN : st == CU_OK);
        }
    }

    /* A truncated last frame/member, and garbage after a complete one. */
    size_t n = build(algo, 2, 0, 0, 0, in, text);
    fails_everywhere(algo, in, n - 1, CU_ERR_TRUNCATED);
    in[n] = 0x42;
    /* One stray byte: zstd rejects a partial magic number at once; gzip needs
     * two header bytes before it can tell. */
    fails_everywhere(algo, in, n + 1,
                     algo == CU_ALGO_ZSTD ? CU_ERR_DECOMPRESSION : CU_ERR_TRUNCATED);
    memcpy(in + n, "garbage!", 8);
    fails_everywhere(algo, in, n + 8, CU_ERR_DECOMPRESSION);
}

static void test_zstd_skippable(void) {
    uint8_t in[2048];
    char text[512];
    size_t n = compress_into(CU_ALGO_ZSTD, PARTS[0], in);
    /* Skippable frame: magic 0x184D2A50 (little-endian), 4-byte size, payload. */
    const uint8_t skippable[] = {0x50, 0x2A, 0x4D, 0x18, 3, 0, 0, 0, 'x', 'y', 'z'};
    memcpy(in + n, skippable, sizeof(skippable));
    n += sizeof(skippable);
    n += compress_into(CU_ALGO_ZSTD, PARTS[1], in + n);
    strcpy(text, PARTS[0]);
    strcat(text, PARTS[1]);
    decodes_everywhere(CU_ALGO_ZSTD, in, n, (const uint8_t*)text, strlen(text));
    uint8_t out[MAX_OUT];
    size_t len = sizeof(out);
    CHECK(cu_decompress(CU_ALGO_ZSTD, in, n, out, &len) == CU_OK);
    CHECK(len == strlen(text) && !memcmp(out, text, len));
}

static void test_gzip_zero_padding(void) {
    uint8_t in[2048];
    char text[512];
    size_t n = build(CU_ALGO_GZIP, 2, 0, 5, 0, in, text);
    decodes_everywhere(CU_ALGO_GZIP, in, n, (const uint8_t*)text, strlen(text));
    uint8_t out[MAX_OUT];
    size_t len = sizeof(out);
    CHECK(cu_decompress(CU_ALGO_GZIP, in, n, out, &len) == CU_OK);
    CHECK(len == strlen(text) && !memcmp(out, text, len));
}

static void test_zlib_trailing_data(void) {
    uint8_t in[2048];
    char text[512];
    size_t n = build(CU_ALGO_ZLIB, 2, 0, 0, 0, in, text);
    fails_everywhere(CU_ALGO_ZLIB, in, n, CU_ERR_DECOMPRESSION);
    uint8_t out[MAX_OUT];
    size_t len = sizeof(out);
    CHECK(cu_decompress(CU_ALGO_ZLIB, in, n, out, &len) == CU_ERR_DECOMPRESSION);
}

static void test_raw_deflate_trailing_data(void) {
    /* Bytes after a raw stream belong to its container: both APIs ignore them,
     * wherever the input is split. */
    uint8_t in[2048];
    size_t n = compress_into(CU_ALGO_DEFLATE, PARTS[0], in);
    memcpy(in + n, "container bytes", 15);
    decodes_everywhere(CU_ALGO_DEFLATE, in, n + 15, (const uint8_t*)PARTS[0], strlen(PARTS[0]));
    uint8_t out[MAX_OUT];
    size_t len = sizeof(out);
    CHECK(cu_decompress(CU_ALGO_DEFLATE, in, n + 15, out, &len) == CU_OK);
    CHECK(len == strlen(PARTS[0]) && !memcmp(out, PARTS[0], len));
}

/* Output that fills the buffer exactly, then finish() with nothing pending. */
static void test_exact_output_flush(cu_algorithm_t algo) {
    static uint8_t text[300000];
    for (size_t i = 0; i < sizeof(text); i++) text[i] = (uint8_t)("abcab"[i % 5] + i / 4096 % 7);
    size_t cap = cu_compress_bound(sizeof(text), algo);
    uint8_t* in = malloc(cap);
    uint8_t* out = malloc(sizeof(text));
    CHECK(in && out);
    CHECK(cu_compress(algo, text, sizeof(text), in, &cap, 5) == CU_OK);

    cu_decompress_stream_t* s = NULL;
    CHECK(cu_decompress_stream_create(algo, &s) == CU_OK);
    size_t total = 0, got = sizeof(text);
    cu_status_t st = cu_decompress_stream_write(s, in, cap, out, &got);
    total += got;
    while (st == CU_ERR_BUF_TOO_SMALL && total < sizeof(text)) {
        got = sizeof(text) - total;
        st = cu_decompress_stream_write(s, NULL, 0, out + total, &got);
        total += got;
    }
    CHECK(total == sizeof(text) && !memcmp(out, text, total));
    uint8_t spare[16];
    got = sizeof(spare);
    CHECK(cu_decompress_stream_finish(s, spare, &got) == CU_OK);
    CHECK(got == 0);
    cu_decompress_stream_destroy(s);
    free(in);
    free(out);
}

/* New input while earlier input is still retained: each write passes the
 * next chunk without draining first, and the final drain collects the rest. */
static void test_write_without_draining(cu_algorithm_t algo) {
    static uint8_t text[200000], out[200000 + 64];
    for (size_t i = 0; i < sizeof(text); i++) text[i] = (uint8_t)("xyzzy"[i % 5] + i / 3000 % 5);
    size_t cap = cu_compress_bound(sizeof(text), algo);
    uint8_t* in = malloc(cap);
    CHECK(in);
    CHECK(cu_compress(algo, text, sizeof(text), in, &cap, 5) == CU_OK);

    cu_decompress_stream_t* s = NULL;
    CHECK(cu_decompress_stream_create(algo, &s) == CU_OK);
    size_t total = 0;
    cu_status_t st = CU_OK;
    for (size_t off = 0; off < cap; off += 997) {
        size_t n = cap - off < 997 ? cap - off : 997, got = 64;
        st = cu_decompress_stream_write(s, in + off, n, out + total, &got);
        CHECK(st == CU_OK || st == CU_ERR_BUF_TOO_SMALL);
        total += got;
    }
    do {
        size_t got = sizeof(out) - total;
        st = cu_decompress_stream_finish(s, out + total, &got);
        total += got;
    } while (st == CU_ERR_BUF_TOO_SMALL);
    CHECK(st == CU_OK && total == sizeof(text) && !memcmp(out, text, total));
    cu_decompress_stream_destroy(s);
    free(in);
}

int main(void) {
    const cu_algorithm_t pending_algos[] = {CU_ALGO_ZSTD, CU_ALGO_GZIP, CU_ALGO_ZLIB, CU_ALGO_DEFLATE};
    for (size_t i = 0; i < sizeof(pending_algos) / sizeof(pending_algos[0]); i++)
        if (cu_algorithm_available(pending_algos[i])) test_write_without_draining(pending_algos[i]);
    if (cu_algorithm_available(CU_ALGO_ZSTD)) {
        test_concatenated(CU_ALGO_ZSTD);
        test_zstd_skippable();
        test_exact_output_flush(CU_ALGO_ZSTD);
    }
    if (cu_algorithm_available(CU_ALGO_GZIP)) {
        test_concatenated(CU_ALGO_GZIP);
        test_gzip_zero_padding();
        test_exact_output_flush(CU_ALGO_GZIP);
    }
    if (cu_algorithm_available(CU_ALGO_ZLIB)) {
        test_zlib_trailing_data();
        test_exact_output_flush(CU_ALGO_ZLIB);
    }
    if (cu_algorithm_available(CU_ALGO_DEFLATE)) {
        test_raw_deflate_trailing_data();
        test_exact_output_flush(CU_ALGO_DEFLATE);
    }
    printf("OK\n");
    return 0;
}
