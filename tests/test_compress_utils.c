/*
 * smoke_test.c — minimal coverage for the new C ABI.
 *
 * Exercises:
 *   - version + algorithm introspection
 *   - one-shot compress/decompress round-trip
 *   - size-hint probe
 *   - BUF_TOO_SMALL behavior
 *   - streaming round-trip with a chunked input and an undersized
 *     output buffer (proves the unconsumed-input drain protocol)
 *
 * Exits with 0 on success, nonzero with a message on failure. Built and
 * run via ctest.
 */

#include "compress_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do {                                       \
    if (!(cond)) {                                                  \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
        fprintf(stderr, __VA_ARGS__);                               \
        fprintf(stderr, "  cu_last_error: %s\n", cu_last_error());  \
        return 1;                                                   \
    }                                                               \
} while (0)

#define CHECK_OK(call) do {                                         \
    cu_status_t _s = (call);                                        \
    CHECK(_s == CU_OK, "%s -> %s (%s)\n",                           \
          #call, cu_strerror(_s), cu_last_error());                 \
} while (0)

static const cu_algorithm_t ALL_ALGOS[] = {
    CU_ALGO_ZSTD, CU_ALGO_BROTLI, CU_ALGO_ZLIB,
    CU_ALGO_BZ2,  CU_ALGO_LZ4,    CU_ALGO_XZ,
    CU_ALGO_SNAPPY, CU_ALGO_GZIP,
};
#define N_ALGOS (sizeof(ALL_ALGOS) / sizeof(ALL_ALGOS[0]))

static int test_version_and_introspection(void) {
    printf("version: %s\n", cu_version());
    /* Whatever algorithms are available, their names must round-trip. */
    int available_count = 0;
    for (size_t i = 0; i < N_ALGOS; i++) {
        cu_algorithm_t a = ALL_ALGOS[i];
        if (cu_algorithm_available(a)) {
            available_count++;
            const char* n = cu_algorithm_name(a);
            CHECK(n && *n, "available algo %d has no name\n", (int)a);
            printf("  available: %s\n", n);
        }
    }
    CHECK(available_count > 0, "no algorithms available in this build\n");
    return 0;
}

static int test_oneshot_roundtrip_one(cu_algorithm_t algo) {
    const char* name = cu_algorithm_name(algo);
    /* ~16KB of repeating ASCII — compresses well across all algorithms. */
    const char* msg = "The quick brown fox jumps over the lazy dog. ";
    size_t in_len = 0;
    uint8_t in[16 * 1024];
    while (in_len + strlen(msg) < sizeof(in)) {
        memcpy(in + in_len, msg, strlen(msg));
        in_len += strlen(msg);
    }

    size_t cbound = cu_compress_bound(in_len, algo);
    uint8_t* compressed = malloc(cbound);
    size_t c_len = cbound;
    cu_status_t s = cu_compress(algo, in, in_len, compressed, &c_len, 5);
    CHECK(s == CU_OK, "%s compress: %s (%s)\n", name, cu_strerror(s), cu_last_error());
    printf("  %s oneshot: %zu -> %zu\n", name, in_len, c_len);

    /* size hint: zstd should succeed, others return SIZE_UNKNOWN. Both
     * are valid; we just don't trust the value if the codec doesn't
     * support it. */
    size_t hint = 0;
    s = cu_decompress_size_hint(algo, compressed, c_len, &hint);
    CHECK(s == CU_OK || s == CU_ERR_SIZE_UNKNOWN,
          "%s size hint unexpected status %s\n", name, cu_strerror(s));
    if (s == CU_OK) {
        CHECK(hint == in_len, "%s size hint %zu != %zu\n", name, hint, in_len);
    }

    uint8_t* out = malloc(in_len);
    size_t out_len = in_len;
    s = cu_decompress(algo, compressed, c_len, out, &out_len);
    CHECK(s == CU_OK, "%s decompress: %s (%s)\n", name, cu_strerror(s), cu_last_error());
    CHECK(out_len == in_len, "%s decompressed length %zu != %zu\n", name, out_len, in_len);
    CHECK(memcmp(in, out, in_len) == 0, "%s round-trip data mismatch\n", name);

    free(compressed);
    free(out);
    return 0;
}

static int test_oneshot_roundtrip(void) {
    for (size_t i = 0; i < N_ALGOS; i++) {
        if (!cu_algorithm_available(ALL_ALGOS[i])) continue;
        if (test_oneshot_roundtrip_one(ALL_ALGOS[i])) return 1;
    }
    return 0;
}

static int test_buf_too_small(void) {
    /* ZSTD encodes size in the frame header — this confirms BUF_TOO_SMALL
     * reports the required size for codecs that can probe. Codecs that
     * can't (zlib/brotli/bz2/lz4-raw) return SIZE_UNKNOWN instead and are
     * exercised by the streaming test. */
    if (!cu_algorithm_available(CU_ALGO_ZSTD)) return 0;
    uint8_t in[] = "hello";
    size_t cbound = cu_compress_bound(sizeof(in), CU_ALGO_ZSTD);
    uint8_t* compressed = malloc(cbound);
    size_t c_len = cbound;
    CHECK_OK(cu_compress(CU_ALGO_ZSTD, in, sizeof(in), compressed, &c_len, 3));

    uint8_t small[1];
    size_t small_len = 1;
    cu_status_t s = cu_decompress(CU_ALGO_ZSTD, compressed, c_len, small, &small_len);
    CHECK(s == CU_ERR_BUF_TOO_SMALL,
          "expected BUF_TOO_SMALL, got %s\n", cu_strerror(s));
    CHECK(small_len == sizeof(in),
          "expected required size %zu, got %zu\n", sizeof(in), small_len);

    free(compressed);
    return 0;
}

/*
 * Stream compress in 3 small chunks into a tight output buffer, draining
 * with BUF_TOO_SMALL loops. Then stream-decompress the result through a
 * similarly tight buffer. This is the protocol the old C++ streaming
 * silently broke; if drains are missing, this test corrupts data or
 * deadlocks.
 */
static int test_streaming_with_tight_buffer_one(cu_algorithm_t algo) {
    const char* name = cu_algorithm_name(algo);
    /* ~64KB of repeating data (compresses very well). */
    size_t in_len = 64 * 1024;
    uint8_t* in = malloc(in_len);
    for (size_t i = 0; i < in_len; i++) in[i] = (uint8_t)(i & 0xff);

    /* Collect compressed output in a growable buffer. */
    size_t out_cap = 0, out_len = 0;
    uint8_t* compressed = NULL;

    cu_compress_stream_t* cs = NULL;
    CHECK_OK(cu_compress_stream_create(algo, 3, &cs));

    /* Feed in 3 chunks; drain through a 256-byte buffer to force the
     * BUF_TOO_SMALL path multiple times. */
    uint8_t scratch[256];
    size_t fed = 0;
    while (fed < in_len) {
        size_t chunk = in_len - fed;
        if (chunk > in_len / 3 + 1) chunk = in_len / 3 + 1;
        const uint8_t* p = in + fed;
        size_t p_len = chunk;
        for (;;) {
            size_t scratch_len = sizeof(scratch);
            cu_status_t s = cu_compress_stream_write(cs, p, p_len, scratch, &scratch_len);
            /* Append produced output. */
            if (scratch_len > 0) {
                if (out_len + scratch_len > out_cap) {
                    out_cap = (out_cap ? out_cap * 2 : 4096);
                    while (out_cap < out_len + scratch_len) out_cap *= 2;
                    compressed = realloc(compressed, out_cap);
                }
                memcpy(compressed + out_len, scratch, scratch_len);
                out_len += scratch_len;
            }
            if (s == CU_OK) break;
            CHECK(s == CU_ERR_BUF_TOO_SMALL,
                  "stream write: %s\n", cu_strerror(s));
            /* Drain with (NULL,0). */
            p = NULL;
            p_len = 0;
        }
        fed += chunk;
    }

    /* Finish — also drain through scratch. */
    for (;;) {
        size_t scratch_len = sizeof(scratch);
        cu_status_t s = cu_compress_stream_finish(cs, scratch, &scratch_len);
        if (scratch_len > 0) {
            if (out_len + scratch_len > out_cap) {
                out_cap = (out_cap ? out_cap * 2 : 4096);
                while (out_cap < out_len + scratch_len) out_cap *= 2;
                compressed = realloc(compressed, out_cap);
            }
            memcpy(compressed + out_len, scratch, scratch_len);
            out_len += scratch_len;
        }
        if (s == CU_OK) break;
        CHECK(s == CU_ERR_BUF_TOO_SMALL,
              "stream finish: %s\n", cu_strerror(s));
    }
    cu_compress_stream_destroy(cs);
    printf("  %s stream: %zu -> %zu\n", name, in_len, out_len);

    /* Stream-decompress through the same tight protocol. */
    cu_decompress_stream_t* ds = NULL;
    CHECK_OK(cu_decompress_stream_create(algo, &ds));

    uint8_t* recovered = malloc(in_len);
    size_t recovered_len = 0;

    const uint8_t* p = compressed;
    size_t p_len = out_len;
    for (;;) {
        size_t scratch_len = sizeof(scratch);
        cu_status_t s = cu_decompress_stream_write(ds, p, p_len, scratch, &scratch_len);
        if (scratch_len > 0) {
            CHECK(recovered_len + scratch_len <= in_len, "decompressed overflow\n");
            memcpy(recovered + recovered_len, scratch, scratch_len);
            recovered_len += scratch_len;
        }
        if (s == CU_OK) break;
        CHECK(s == CU_ERR_BUF_TOO_SMALL,
              "decompress stream write: %s\n", cu_strerror(s));
        p = NULL;
        p_len = 0;
    }

    /* Final flush. */
    for (;;) {
        size_t scratch_len = sizeof(scratch);
        cu_status_t s = cu_decompress_stream_finish(ds, scratch, &scratch_len);
        if (scratch_len > 0) {
            CHECK(recovered_len + scratch_len <= in_len, "decompressed overflow at finish\n");
            memcpy(recovered + recovered_len, scratch, scratch_len);
            recovered_len += scratch_len;
        }
        if (s == CU_OK) break;
        CHECK(s == CU_ERR_BUF_TOO_SMALL,
              "decompress stream finish: %s\n", cu_strerror(s));
    }
    cu_decompress_stream_destroy(ds);

    CHECK(recovered_len == in_len, "recovered %zu != in_len %zu\n", recovered_len, in_len);
    CHECK(memcmp(in, recovered, in_len) == 0, "stream round-trip data mismatch\n");

    free(in);
    free(compressed);
    free(recovered);
    return 0;
}

static int test_streaming_with_tight_buffer(void) {
    for (size_t i = 0; i < N_ALGOS; i++) {
        if (!cu_algorithm_available(ALL_ALGOS[i])) continue;
        if (test_streaming_with_tight_buffer_one(ALL_ALGOS[i])) return 1;
    }
    return 0;
}

/*
 * Cross-API round-trips: stream-compress then one-shot decompress, and
 * one-shot compress then stream-decompress. These are the tests that
 * catch wire-format mismatches between the two paths — the LZ4 bug in
 * the old C++ code (raw-block one-shot, frame-format streaming) would
 * have been caught here. Run for every available algorithm.
 */

static cu_status_t collect_stream_compress(cu_algorithm_t algo, int level,
                                           const uint8_t* in, size_t in_len,
                                           uint8_t** out, size_t* out_len) {
    cu_compress_stream_t* cs = NULL;
    cu_status_t s = cu_compress_stream_create(algo, level, &cs);
    if (s != CU_OK) return s;

    size_t cap = 0, total = 0;
    uint8_t* buf = NULL;
    uint8_t scratch[1024];

    const uint8_t* p = in;
    size_t p_len = in_len;
    for (;;) {
        size_t scratch_len = sizeof(scratch);
        cu_status_t ws = cu_compress_stream_write(cs, p, p_len, scratch, &scratch_len);
        if (scratch_len > 0) {
            if (total + scratch_len > cap) {
                cap = cap ? cap * 2 : 4096;
                while (cap < total + scratch_len) cap *= 2;
                buf = realloc(buf, cap);
            }
            memcpy(buf + total, scratch, scratch_len);
            total += scratch_len;
        }
        if (ws == CU_OK) break;
        if (ws != CU_ERR_BUF_TOO_SMALL) { cu_compress_stream_destroy(cs); free(buf); return ws; }
        p = NULL; p_len = 0;
    }
    for (;;) {
        size_t scratch_len = sizeof(scratch);
        cu_status_t fs = cu_compress_stream_finish(cs, scratch, &scratch_len);
        if (scratch_len > 0) {
            if (total + scratch_len > cap) {
                cap = cap ? cap * 2 : 4096;
                while (cap < total + scratch_len) cap *= 2;
                buf = realloc(buf, cap);
            }
            memcpy(buf + total, scratch, scratch_len);
            total += scratch_len;
        }
        if (fs == CU_OK) break;
        if (fs != CU_ERR_BUF_TOO_SMALL) { cu_compress_stream_destroy(cs); free(buf); return fs; }
    }
    cu_compress_stream_destroy(cs);
    *out = buf;
    *out_len = total;
    return CU_OK;
}

static cu_status_t collect_stream_decompress(cu_algorithm_t algo,
                                             const uint8_t* in, size_t in_len,
                                             uint8_t** out, size_t* out_len) {
    cu_decompress_stream_t* ds = NULL;
    cu_status_t s = cu_decompress_stream_create(algo, &ds);
    if (s != CU_OK) return s;

    size_t cap = 0, total = 0;
    uint8_t* buf = NULL;
    uint8_t scratch[1024];

    const uint8_t* p = in;
    size_t p_len = in_len;
    for (;;) {
        size_t scratch_len = sizeof(scratch);
        cu_status_t ws = cu_decompress_stream_write(ds, p, p_len, scratch, &scratch_len);
        if (scratch_len > 0) {
            if (total + scratch_len > cap) {
                cap = cap ? cap * 2 : 4096;
                while (cap < total + scratch_len) cap *= 2;
                buf = realloc(buf, cap);
            }
            memcpy(buf + total, scratch, scratch_len);
            total += scratch_len;
        }
        if (ws == CU_OK) break;
        if (ws != CU_ERR_BUF_TOO_SMALL) { cu_decompress_stream_destroy(ds); free(buf); return ws; }
        p = NULL; p_len = 0;
    }
    for (;;) {
        size_t scratch_len = sizeof(scratch);
        cu_status_t fs = cu_decompress_stream_finish(ds, scratch, &scratch_len);
        if (scratch_len > 0) {
            if (total + scratch_len > cap) {
                cap = cap ? cap * 2 : 4096;
                while (cap < total + scratch_len) cap *= 2;
                buf = realloc(buf, cap);
            }
            memcpy(buf + total, scratch, scratch_len);
            total += scratch_len;
        }
        if (fs == CU_OK) break;
        if (fs != CU_ERR_BUF_TOO_SMALL) { cu_decompress_stream_destroy(ds); free(buf); return fs; }
    }
    cu_decompress_stream_destroy(ds);
    *out = buf;
    *out_len = total;
    return CU_OK;
}

static int test_cross_api_one(cu_algorithm_t algo) {
    const char* name = cu_algorithm_name(algo);
    /* 32KB input — large enough to push past internal block boundaries
     * on every codec. */
    size_t in_len = 32 * 1024;
    uint8_t* in = malloc(in_len);
    for (size_t i = 0; i < in_len; i++) in[i] = (uint8_t)((i * 13 + 7) & 0xff);

    /* Path A: stream-compress -> one-shot decompress. */
    uint8_t* compressed_a = NULL;
    size_t compressed_a_len = 0;
    cu_status_t s = collect_stream_compress(algo, 5, in, in_len, &compressed_a, &compressed_a_len);
    CHECK(s == CU_OK, "%s stream-compress for cross test: %s\n", name, cu_strerror(s));

    /* Try one-shot decompress. Some codecs need a sized buffer; use
     * size_hint when available, otherwise allocate generously. */
    size_t expected = in_len;
    size_t hint = 0;
    if (cu_decompress_size_hint(algo, compressed_a, compressed_a_len, &hint) == CU_OK) {
        CHECK(hint == in_len, "%s: stream-produced frame size_hint=%zu != %zu\n", name, hint, in_len);
        expected = hint;
    }
    uint8_t* out_a = malloc(expected + 16);
    size_t out_a_len = expected + 16;
    s = cu_decompress(algo, compressed_a, compressed_a_len, out_a, &out_a_len);
    if (s == CU_ERR_SIZE_UNKNOWN) {
        /* Codec wire format doesn't carry size and our buffer happened
         * to be too small. Stream-decompress as fallback verification. */
        free(out_a);
        out_a = NULL;
        s = collect_stream_decompress(algo, compressed_a, compressed_a_len, &out_a, &out_a_len);
    }
    CHECK(s == CU_OK, "%s cross A decompress: %s (%s)\n", name, cu_strerror(s), cu_last_error());
    CHECK(out_a_len == in_len, "%s cross A length %zu != %zu\n", name, out_a_len, in_len);
    CHECK(memcmp(in, out_a, in_len) == 0, "%s cross A data mismatch\n", name);
    free(out_a);
    free(compressed_a);

    /* Path B: one-shot compress -> stream-decompress. */
    size_t cbound = cu_compress_bound(in_len, algo);
    uint8_t* compressed_b = malloc(cbound);
    size_t compressed_b_len = cbound;
    s = cu_compress(algo, in, in_len, compressed_b, &compressed_b_len, 5);
    CHECK(s == CU_OK, "%s cross B compress: %s\n", name, cu_strerror(s));

    uint8_t* out_b = NULL;
    size_t out_b_len = 0;
    s = collect_stream_decompress(algo, compressed_b, compressed_b_len, &out_b, &out_b_len);
    CHECK(s == CU_OK, "%s cross B stream-decompress: %s (%s)\n", name, cu_strerror(s), cu_last_error());
    CHECK(out_b_len == in_len, "%s cross B length %zu != %zu\n", name, out_b_len, in_len);
    CHECK(memcmp(in, out_b, in_len) == 0, "%s cross B data mismatch\n", name);
    free(out_b);
    free(compressed_b);

    free(in);
    printf("  %s cross-api: ok\n", name);
    return 0;
}

static int test_cross_api(void) {
    for (size_t i = 0; i < N_ALGOS; i++) {
        if (!cu_algorithm_available(ALL_ALGOS[i])) continue;
        if (test_cross_api_one(ALL_ALGOS[i])) return 1;
    }
    return 0;
}

int main(void) {
    if (test_version_and_introspection())   return 1;
    if (test_oneshot_roundtrip())           return 1;
    if (test_buf_too_small())               return 1;
    if (test_streaming_with_tight_buffer()) return 1;
    if (test_cross_api())                   return 1;
    printf("OK\n");
    return 0;
}
