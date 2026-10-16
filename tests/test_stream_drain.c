/*
 * Stream drain contract, for every streaming codec: once a write (drained
 * until CU_OK) returns, nothing the codec could already emit is left inside it.
 *
 *   - Decoders: after a complete stream has been written and drained, all of
 *     the plaintext has come out, before finish(). The same holds with the
 *     trailer cut off (gzip, zlib, LZ4), which is what a flushed, still-open
 *     stream looks like. Repeated empty writes succeed and emit nothing.
 *   - zstd encoder: 128 KiB of random input written into 64 KiB output buffers
 *     comes out compressed once drained, not held inside zstd.
 *
 * Output buffers of 1 byte up to 64 KiB force BUF_TOO_SMALL drain cycles, and
 * inputs around 64 KiB and 128 KiB sit on typical internal buffer sizes.
 */
#include "compress_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value)                                                                \
    do {                                                                            \
        if (!(value)) {                                                             \
            fprintf(stderr, "drain check failed: %s:%d: %s (%s)\n", __FILE__,       \
                    __LINE__, #value, cu_last_error());                             \
            exit(1);                                                                \
        }                                                                           \
    } while (0)

static int failures = 0;

#define EXPECT(value, ...)                                                          \
    do {                                                                            \
        if (!(value)) {                                                             \
            fprintf(stderr, __VA_ARGS__);                                           \
            fprintf(stderr, "\n");                                                  \
            failures++;                                                             \
            return;                                                                 \
        }                                                                           \
    } while (0)

typedef cu_status_t (*write_fn)(void* stream, const uint8_t* in, size_t in_len, uint8_t* out,
                                size_t* out_len);

static cu_status_t cwrite(void* s, const uint8_t* in, size_t n, uint8_t* out, size_t* len) {
    return cu_compress_stream_write((cu_compress_stream_t*)s, in, n, out, len);
}
static cu_status_t dwrite(void* s, const uint8_t* in, size_t n, uint8_t* out, size_t* len) {
    return cu_decompress_stream_write((cu_decompress_stream_t*)s, in, n, out, len);
}

/* Writes `in`, then drains with empty writes until CU_OK, appending output to
 * dst. Returns the status that ended the loop. */
static cu_status_t write_drained(write_fn fn, void* s, const uint8_t* in, size_t n,
                                 size_t cap, uint8_t* dst, size_t* total, size_t max) {
    static uint8_t buf[65536];
    cu_status_t st;
    do {
        size_t got = cap;
        st = fn(s, in, n, buf, &got);
        in = NULL;
        n = 0;
        if (st != CU_OK && st != CU_ERR_BUF_TOO_SMALL) return st;
        CHECK(got <= cap && *total + got <= max);
        memcpy(dst + *total, buf, got);
        *total += got;
    } while (st == CU_ERR_BUF_TOO_SMALL);
    return st;
}

static void make_input(int kind, uint8_t* p, size_t n) {
    unsigned x = 12345;
    for (size_t i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        if (kind == 0) p[i] = 'A';
        else if (kind == 1) p[i] = (uint8_t)(x >> 16);
        else p[i] = (uint8_t)("drain contract text "[i % 20] + (i / 5000) % 3);
    }
}

static const char* const KINDS[] = {"repeated", "random", "text"};

/* Bytes after the last data block: gzip's CRC32 + ISIZE, zlib's Adler-32,
 * LZ4's end mark + content checksum. 0 = not cut. */
static size_t trailer(cu_algorithm_t algo) {
    return algo == CU_ALGO_GZIP || algo == CU_ALGO_LZ4 ? 8 : algo == CU_ALGO_ZLIB ? 4 : 0;
}

/* The issue #54 control: 128 KiB of random bytes in one write, drained through
 * 64 KiB buffers, must come out as a complete compressed block, not stop at
 * the first full buffer. */
static void check_zstd_encoder_drain(void) {
    static uint8_t plain[131072], packed[262144];
    make_input(1, plain, sizeof(plain));
    cu_compress_stream_t* cs = NULL;
    CHECK(cu_compress_stream_create(CU_ALGO_ZSTD, 5, &cs) == CU_OK);
    size_t packed_len = 0;
    CHECK(write_drained(cwrite, cs, plain, sizeof(plain), 65536, packed, &packed_len,
                        sizeof(packed)) == CU_OK);
    if (packed_len < sizeof(plain)) {
        fprintf(stderr, "zstd encoder: %zu compressed bytes out after draining 131072 random "
                "bytes; the rest is held inside zstd\n", packed_len);
        failures++;
    }
    cu_compress_stream_destroy(cs);
}

static void check_codec(cu_algorithm_t algo, int kind, size_t n, size_t in_chunk, size_t cap) {
    const char* name = cu_algorithm_name(algo);
    uint8_t* plain = malloc(n);
    size_t bound = cu_compress_bound(n, algo) + 1024;
    uint8_t* packed = malloc(bound);
    uint8_t* back = malloc(n + 1024);
    CHECK(plain && packed && back);
    make_input(kind, plain, n);

    /* Encoder: write in chunks, draining each. */
    cu_compress_stream_t* cs = NULL;
    CHECK(cu_compress_stream_create(algo, 5, &cs) == CU_OK);
    size_t packed_len = 0;
    for (size_t off = 0; off < n; off += in_chunk) {
        size_t len = n - off < in_chunk ? n - off : in_chunk;
        CHECK(write_drained(cwrite, cs, plain + off, len, cap, packed, &packed_len, bound) ==
              CU_OK);
    }
    cu_status_t st;
    do {
        static uint8_t buf[65536];
        size_t got = cap;
        st = cu_compress_stream_finish(cs, buf, &got);
        CHECK(st == CU_OK || st == CU_ERR_BUF_TOO_SMALL);
        CHECK(packed_len + got <= bound);
        memcpy(packed + packed_len, buf, got);
        packed_len += got;
    } while (st == CU_ERR_BUF_TOO_SMALL);
    cu_compress_stream_destroy(cs);

    /* Decoder: the whole stream (or all but its trailer) written and drained
     * must yield all of `plain` before finish. */
    for (size_t cut = 0; cut <= trailer(algo); cut += trailer(algo) ? trailer(algo) : 1) {
        size_t avail = packed_len - cut;
        cu_decompress_stream_t* ds = NULL;
        CHECK(cu_decompress_stream_create(algo, &ds) == CU_OK);
        size_t back_len = 0;
        for (size_t off = 0; off < avail; off += in_chunk) {
            size_t len = avail - off < in_chunk ? avail - off : in_chunk;
            CHECK(write_drained(dwrite, ds, packed + off, len, cap, back, &back_len, n + 1024) ==
                  CU_OK);
        }
        size_t before = back_len;
        for (int i = 0; i < 3; i++) {
            cu_status_t e = write_drained(dwrite, ds, NULL, 0, cap, back, &back_len, n + 1024);
            if (e != CU_OK) {
                fprintf(stderr, "%s decoder (%s, %zu bytes, cut %zu, chunk %zu, out %zu): empty "
                        "write %d returned %d (%s)\n",
                        name, KINDS[kind], n, cut, in_chunk, cap, i + 1, e, cu_last_error());
                failures++;
                cu_decompress_stream_destroy(ds);
                goto done;
            }
        }
        if (back_len != n || memcmp(back, plain, n)) {
            fprintf(stderr, "%s decoder (%s, %zu bytes, cut %zu, chunk %zu, out %zu): %zu of %zu "
                    "bytes out before finish (%zu from empty writes)\n",
                    name, KINDS[kind], n, cut, in_chunk, cap, back_len, n, back_len - before);
            failures++;
            cu_decompress_stream_destroy(ds);
            goto done;
        }
        size_t got = cap;
        static uint8_t tail[65536];
        st = cu_decompress_stream_finish(ds, tail, &got);
        CHECK(got == 0 && st == (cut ? CU_ERR_TRUNCATED : CU_OK));
        cu_decompress_stream_destroy(ds);
    }

done:
    free(plain);
    free(packed);
    free(back);
}

int main(void) {
    /* Snappy's raw block decodes only at finish, and LZO has no encoder. */
    const cu_algorithm_t algos[] = {CU_ALGO_ZSTD, CU_ALGO_LZ4,  CU_ALGO_ZLIB,  CU_ALGO_GZIP,
                                    CU_ALGO_DEFLATE, CU_ALGO_BROTLI, CU_ALGO_XZ, CU_ALGO_BZ2};
    if (cu_algorithm_available(CU_ALGO_ZSTD)) check_zstd_encoder_drain();
    const size_t sizes[] = {65535, 65536, 65537, 131072, 300000};
    const size_t chunks[] = {(size_t)-1, 4093};
    const size_t caps[] = {65536, 4096, 7};
    for (size_t a = 0; a < sizeof(algos) / sizeof(algos[0]); a++) {
        if (!cu_algorithm_available(algos[a])) continue;
        int before = failures;
        for (int kind = 0; kind < 3; kind++)
            for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++)
                for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++)
                    for (size_t o = 0; o < sizeof(caps) / sizeof(caps[0]); o++)
                        check_codec(algos[a], kind, sizes[s], chunks[c], caps[o]);
        printf("%-8s %s\n", cu_algorithm_name(algos[a]),
               failures == before ? "ok" : "FAILED");
    }
    if (failures) {
        fprintf(stderr, "%d drain contract failures\n", failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
