/*
 * test_decompress_alloc.c — cu_decompress_alloc / cu_buffer_free (src/compress_utils.c): one-shot decompress into a library-allocated buffer. For every compiled-in algorithm that can compress, checks:
 *   - round trips of every shape (empty, tiny, text, incompressible, input of many 256 KiB feed chunks), for formats with a recorded size and without;
 *   - output far larger than the input, so the buffer doubles many times;
 *   - the global cap: output of exactly the cap decodes and one byte more fails with CU_ERR_SIZE_LIMIT, in both the sized and the streamed path, and a header declaring a huge size is refused or survived without allocating it;
 *   - malformed, truncated and empty input fails as cu_decompress does (or decodes to the same bytes when it succeeds);
 *   - concatenated xz Streams, zstd frames, gzip members and LZ4 frames decode as one, including zstd frames of which one records no size;
 *   - argument checks, and (Linux) running out of memory as a status.
 *
 * Exits with 0 on success, nonzero with a message on failure.
 */

#include "compress_utils.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The out-of-memory test limits the address space, which ASan's shadow memory can't live with. */
#if defined(__SANITIZE_ADDRESS__)
#  define CU_TEST_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define CU_TEST_ASAN 1
#  endif
#endif
#if defined(__linux__) && !defined(CU_TEST_ASAN)
#  define CU_TEST_OOM 1
#  include <sys/resource.h>
#endif

#ifdef CU_TEST_ASAN
/* An allocation too big to ever succeed returns NULL, as the helper expects, instead of aborting the test. */
const char* __asan_default_options(void) { return "allocator_may_return_null=1"; }
#endif

static int failures = 0;

#define EXPECT(cond, ...)                                                          \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                   \
            fprintf(stderr, __VA_ARGS__);                                          \
            fprintf(stderr, "  cu_last_error: %s\n", cu_last_error());             \
            failures++;                                                            \
            return;                                                                \
        }                                                                          \
    } while (0)

static const cu_algorithm_t k_algos[] = {
    CU_ALGO_ZSTD, CU_ALGO_BROTLI, CU_ALGO_ZLIB, CU_ALGO_BZ2, CU_ALGO_LZ4,
    CU_ALGO_XZ, CU_ALGO_SNAPPY, CU_ALGO_GZIP, CU_ALGO_DEFLATE, CU_ALGO_LZ4_RAW,
};
#define N_ALGOS (sizeof k_algos / sizeof *k_algos)

/* Skewed bytes with repeats, so the output is neither tiny nor stored. */
static void fill_text(uint8_t* p, size_t n, uint32_t seed) {
    uint32_t x = seed;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        p[i] = (uint8_t)("etaoin shrdlu\n"[(x >> 24) % 14] + ((x >> 20) % 7 == 0 ? (x >> 8) % 5 : 0));
    }
}

static void fill_random(uint8_t* p, size_t n, uint32_t seed) {
    uint32_t x = seed;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        p[i] = (uint8_t)(x >> 24);
    }
}

static uint8_t* compress(cu_algorithm_t algo, const uint8_t* in, size_t n, size_t* out_len) {
    size_t cap = cu_compress_bound(n, algo);
    uint8_t* out = malloc(cap ? cap : 1);
    *out_len = cap;
    if (!out || cu_compress(algo, in, n, out, out_len, 5) != CU_OK) { free(out); return NULL; }
    return out;
}

/* A result of cu_decompress_alloc; the test frees `p` with cu_buffer_free. */
typedef struct { uint8_t* p; size_t n; } buf_t;

static cu_status_t alloc_decode(cu_algorithm_t algo, const uint8_t* in, size_t in_len, buf_t* b) {
    b->p = NULL;
    b->n = 12345;
    return cu_decompress_alloc(algo, in, in_len, &b->p, &b->n);
}

static int same(const buf_t* b, const uint8_t* want, size_t n) {
    return b->p && b->n == n && (n == 0 || memcmp(b->p, want, n) == 0);
}

/* Decodes with cu_decompress into `ref_cap` bytes: the reference answer. */
static cu_status_t reference(cu_algorithm_t algo, const uint8_t* in, size_t in_len,
                             size_t ref_cap, uint8_t** out, size_t* out_len) {
    *out = malloc(ref_cap);
    *out_len = ref_cap;
    if (!*out) return CU_ERR_OOM;
    return cu_decompress(algo, in, in_len, *out, out_len);
}

static int bad_input(cu_status_t s) {
    return s == CU_ERR_DECOMPRESSION || s == CU_ERR_TRUNCATED || s == CU_ERR_COMPRESSION;
}

/* Bad input is one class: the one-shot and streaming decoders of a codec don't always name it alike (raw LZ4 says DECOMPRESSION one-shot and TRUNCATED streamed; the xz stream decoder says COMPRESSION for a bad format; see TODO.md). Every other status must match exactly. */
static int same_failure(cu_status_t want, cu_status_t got) {
    return want == got || (bad_input(want) && bad_input(got));
}

/* cu_decompress_alloc must agree with cu_decompress on `in`, whose output must fit in `ref_cap`: the same bytes on success, the same status on failure, and no buffer on failure. */
static void agrees_with_reference(cu_algorithm_t algo, const uint8_t* in, size_t in_len,
                                  size_t ref_cap, const char* what) {
    uint8_t* ref = NULL;
    size_t ref_len = 0;
    cu_status_t want = reference(algo, in, in_len, ref_cap, &ref, &ref_len);
    EXPECT(want != CU_ERR_OOM, "%s %s: reference alloc", cu_algorithm_name(algo), what);
    buf_t got;
    cu_status_t s = alloc_decode(algo, in, in_len, &got);
    if (want == CU_OK && (algo == CU_ALGO_BROTLI || algo == CU_ALGO_LZ4_RAW) && bad_input(s)) {
        /* The one-shot decoders of brotli and raw LZ4 are more lenient than the streaming ones: brotli ignores bytes after the end of the stream, raw LZ4 takes a prefix that ends between sequences (see TODO.md). The helper streams these, as every binding always has. */
    } else if (want == CU_ERR_BUF_TOO_SMALL) {
        /* The reference buffer was too small (a corrupted size field, say), so there's no answer to match; the helper must not decode it either. */
        EXPECT(s != CU_OK || got.n > ref_cap, "%s %s: reference needs more than %zu bytes, alloc decoded %zu",
               cu_algorithm_name(algo), what, ref_cap, got.n);
    } else if (want == CU_OK) {
        EXPECT(s == CU_OK && same(&got, ref, ref_len), "%s %s: reference decoded %zu bytes, alloc status %d len %zu",
               cu_algorithm_name(algo), what, ref_len, (int)s, got.n);
    } else {
        EXPECT(same_failure(want, s) && !got.p && got.n == 0, "%s %s: reference status %d, alloc status %d (buf %p, len %zu)",
               cu_algorithm_name(algo), what, (int)want, (int)s, (void*)got.p, got.n);
    }
    cu_buffer_free(got.p);
    free(ref);
}

static void test_round_trips(void) {
    static const size_t sizes[] = {0, 1, 100, 4096, 70000, 300000, 1u << 20};
    for (size_t a = 0; a < N_ALGOS; a++) {
        cu_algorithm_t algo = k_algos[a];
        if (!cu_algorithm_available(algo)) continue;
        for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
            size_t n = sizes[k];
            for (int kind = 0; kind < 2; kind++) {
                uint8_t* in = malloc(n ? n : 1);
                EXPECT(in, "oom");
                if (kind == 0) fill_text(in, n, (uint32_t)k + 1);
                else           fill_random(in, n, (uint32_t)k + 1);
                size_t c_len = 0;
                uint8_t* c = compress(algo, in, n, &c_len);
                EXPECT(c, "%s n=%zu: compress failed", cu_algorithm_name(algo), n);
                buf_t got;
                cu_status_t s = alloc_decode(algo, c, c_len, &got);
                EXPECT(s == CU_OK && same(&got, in, n), "%s n=%zu kind %d: status %d, len %zu",
                       cu_algorithm_name(algo), n, kind, (int)s, got.n);
                EXPECT(got.p, "%s n=%zu: success must return a buffer even when empty", cu_algorithm_name(algo), n);
                cu_buffer_free(got.p);
                free(c);
                free(in);
            }
        }
    }
}

/* 16 MiB of one byte is a ratio of over 1000 for most codecs: from a start of four times the input, the buffer doubles about nine times. */
static void test_far_larger_than_input(void) {
    enum { N = 16 << 20 };
    uint8_t* in = calloc(N, 1);
    EXPECT(in, "oom");
    for (size_t a = 0; a < N_ALGOS; a++) {
        cu_algorithm_t algo = k_algos[a];
        if (!cu_algorithm_available(algo)) continue;
        size_t c_len = 0;
        uint8_t* c = compress(algo, in, N, &c_len);
        EXPECT(c, "%s: compress failed", cu_algorithm_name(algo));
        buf_t got;
        cu_status_t s = alloc_decode(algo, c, c_len, &got);
        EXPECT(s == CU_OK && same(&got, in, N), "%s: status %d, len %zu of %d (compressed to %zu)",
               cu_algorithm_name(algo), (int)s, got.n, N, c_len);
        cu_buffer_free(got.p);
        free(c);
    }
    free(in);
}

/* Exactly the cap decodes and one more byte is SIZE_LIMIT, for every format, around the sizes where the buffer grows (7/8 of the 64 KiB minimum, the minimum itself) and so meets its cap + 1 ceiling. */
static void test_cap(void) {
    static const size_t sizes[] = {2, 1000, 57343, 57344, 57345, 65535, 65536, 65537, 200000};
    size_t old_cap = cu_get_max_decompressed_size();
    for (size_t a = 0; a < N_ALGOS; a++) {
        cu_algorithm_t algo = k_algos[a];
        if (!cu_algorithm_available(algo)) continue;
        for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
            size_t n = sizes[k];
            uint8_t* in = malloc(n);
            EXPECT(in, "oom");
            fill_text(in, n, 9);
            size_t c_len = 0;
            uint8_t* c = compress(algo, in, n, &c_len);
            EXPECT(c, "%s n=%zu: compress failed", cu_algorithm_name(algo), n);
            buf_t at, over;
            cu_set_max_decompressed_size(n);
            cu_status_t s_at = alloc_decode(algo, c, c_len, &at);
            cu_set_max_decompressed_size(n - 1);
            cu_status_t s_over = alloc_decode(algo, c, c_len, &over);
            cu_set_max_decompressed_size(old_cap);
            EXPECT(s_at == CU_OK && same(&at, in, n), "%s cap == n=%zu: status %d len %zu",
                   cu_algorithm_name(algo), n, (int)s_at, at.n);
            EXPECT(s_over == CU_ERR_SIZE_LIMIT && !over.p && over.n == 0,
                   "%s cap == n-1 (n=%zu): status %d len %zu", cu_algorithm_name(algo), n, (int)s_over, over.n);
            cu_buffer_free(at.p);
            free(c);
            free(in);
        }
    }
    cu_set_max_decompressed_size(old_cap);
}

/* A tiny cap against a highly compressible input stops early, without decoding it all, and a disabled cap lets it through. */
static void test_cap_disabled_and_tiny(void) {
    enum { N = 4 << 20 };
    size_t old_cap = cu_get_max_decompressed_size();
    uint8_t* in = calloc(N, 1);
    EXPECT(in, "oom");
    for (size_t a = 0; a < N_ALGOS; a++) {
        cu_algorithm_t algo = k_algos[a];
        if (!cu_algorithm_available(algo)) continue;
        size_t c_len = 0;
        uint8_t* c = compress(algo, in, N, &c_len);
        EXPECT(c, "%s: compress failed", cu_algorithm_name(algo));
        buf_t tiny, off;
        cu_set_max_decompressed_size(10);
        cu_status_t s_tiny = alloc_decode(algo, c, c_len, &tiny);
        cu_set_max_decompressed_size(0);
        cu_status_t s_off = alloc_decode(algo, c, c_len, &off);
        cu_set_max_decompressed_size(old_cap);
        EXPECT(s_tiny == CU_ERR_SIZE_LIMIT && !tiny.p, "%s cap 10: status %d", cu_algorithm_name(algo), (int)s_tiny);
        EXPECT(s_off == CU_OK && same(&off, in, N), "%s cap off: status %d len %zu", cu_algorithm_name(algo), (int)s_off, off.n);
        cu_buffer_free(off.p);
        free(c);
    }
    free(in);
}

/* Every prefix of a small stream, and every single-byte corruption, give what cu_decompress gives. */
static void test_malformed_and_truncated(void) {
    enum { N = 3000 };
    uint8_t in[N];
    fill_text(in, N, 5);
    for (size_t a = 0; a < N_ALGOS; a++) {
        cu_algorithm_t algo = k_algos[a];
        if (!cu_algorithm_available(algo)) continue;
        size_t c_len = 0;
        uint8_t* c = compress(algo, in, N, &c_len);
        EXPECT(c, "%s: compress failed", cu_algorithm_name(algo));
        for (size_t cut = 0; cut < c_len; cut++) {
            agrees_with_reference(algo, c, cut, 4 * N, "truncated");
            if (failures) return;
        }
        uint8_t* bad = malloc(c_len);
        EXPECT(bad, "oom");
        for (size_t at = 0; at < c_len; at++) {
            memcpy(bad, c, c_len);
            bad[at] ^= 0x5a;
            agrees_with_reference(algo, bad, c_len, 64 * N, "corrupted");
            if (failures) return;
        }
        free(bad);
        free(c);

        uint8_t junk[64];
        fill_random(junk, sizeof junk, 3);
        agrees_with_reference(algo, junk, sizeof junk, 4 * N, "random bytes");
        uint8_t zeros[64] = {0};
        agrees_with_reference(algo, zeros, sizeof zeros, 4 * N, "zero bytes");
    }
}

static void test_empty(void) {
    for (size_t a = 0; a < N_ALGOS; a++) {
        cu_algorithm_t algo = k_algos[a];
        if (!cu_algorithm_available(algo)) continue;
        /* Empty input is an error. (Raw LZ4 says DECOMPRESSION one-shot and TRUNCATED streamed, so only the failure is compared.) */
        buf_t none;
        cu_status_t s0 = alloc_decode(algo, NULL, 0, &none);
        EXPECT(s0 != CU_OK && !none.p && none.n == 0, "%s empty input: status %d", cu_algorithm_name(algo), (int)s0);
        /* Empty output: a real, freeable buffer of length 0. */
        size_t c_len = 0;
        uint8_t* c = compress(algo, (const uint8_t*)"", 0, &c_len);
        EXPECT(c, "%s: compress of nothing failed", cu_algorithm_name(algo));
        buf_t got;
        cu_status_t s = alloc_decode(algo, c, c_len, &got);
        EXPECT(s == CU_OK && got.p && got.n == 0, "%s empty output: status %d len %zu",
               cu_algorithm_name(algo), (int)s, got.n);
        cu_buffer_free(got.p);
        free(c);
    }
}

/* `a` then `b` compressed separately and concatenated decode as one. */
static void test_concatenated(void) {
    static const cu_algorithm_t multi[] = {CU_ALGO_XZ, CU_ALGO_ZSTD, CU_ALGO_GZIP, CU_ALGO_LZ4};
    enum { NA = 150000, NB = 70000 };
    uint8_t* ab = malloc(NA + NB);
    EXPECT(ab, "oom");
    fill_text(ab, NA, 21);
    fill_random(ab + NA, NB, 22);
    for (size_t m = 0; m < sizeof multi / sizeof *multi; m++) {
        cu_algorithm_t algo = multi[m];
        if (!cu_algorithm_available(algo)) continue;
        size_t a_len = 0, b_len = 0;
        uint8_t* a = compress(algo, ab, NA, &a_len);
        uint8_t* b = compress(algo, ab + NA, NB, &b_len);
        EXPECT(a && b, "%s: compress failed", cu_algorithm_name(algo));
        uint8_t* cat = malloc(a_len + b_len + 8);
        EXPECT(cat, "oom");
        memcpy(cat, a, a_len);
        memcpy(cat + a_len, b, b_len);
        buf_t got;
        cu_status_t s = alloc_decode(algo, cat, a_len + b_len, &got);
        EXPECT(s == CU_OK && same(&got, ab, NA + NB), "%s two members: status %d len %zu of %d",
               cu_algorithm_name(algo), (int)s, got.n, NA + NB);
        cu_buffer_free(got.p);

        /* Zero padding after the second (xz Stream Padding, gzip zero padding; zstd and LZ4 reject or skip it as cu_decompress does). */
        memset(cat + a_len + b_len, 0, 4);
        agrees_with_reference(algo, cat, a_len + b_len + 4, NA + NB + 16, "padding after members");
        free(cat);
        free(a);
        free(b);
        if (failures) return;
    }
    free(ab);
}

/* A frame that records its size followed by one that doesn't (written by a stream, with no pledged size): the hint is SIZE_UNKNOWN for the whole. */
static void test_zstd_mixed_frames(void) {
    if (!cu_algorithm_available(CU_ALGO_ZSTD)) return;
    uint8_t a[5000], b[7000];
    fill_text(a, sizeof a, 1);
    fill_text(b, sizeof b, 2);
    size_t a_len = 0, b_cap = cu_compress_bound(sizeof b, CU_ALGO_ZSTD), b_len = b_cap;
    uint8_t* ca = compress(CU_ALGO_ZSTD, a, sizeof a, &a_len);
    uint8_t* cat = malloc(a_len + b_cap);
    EXPECT(ca && cat, "oom");
    memcpy(cat, ca, a_len);
    cu_compress_stream_t* cs = NULL;
    EXPECT(cu_compress_stream_create(CU_ALGO_ZSTD, 5, &cs) == CU_OK, "stream create");
    cu_status_t s = cu_compress_stream_write(cs, b, sizeof b, cat + a_len, &b_len);
    size_t tail = b_cap - b_len;
    if (s == CU_OK) s = cu_compress_stream_finish(cs, cat + a_len + b_len, &tail);
    cu_compress_stream_destroy(cs);
    EXPECT(s == CU_OK, "stream compress: %d", (int)s);
    size_t total = a_len + b_len + tail, hint = 0;
    EXPECT(cu_decompress_size_hint(CU_ALGO_ZSTD, cat, total, &hint) == CU_ERR_SIZE_UNKNOWN,
           "expected SIZE_UNKNOWN for a frame without a size");
    buf_t got;
    s = alloc_decode(CU_ALGO_ZSTD, cat, total, &got);
    uint8_t want[sizeof a + sizeof b];
    memcpy(want, a, sizeof a);
    memcpy(want + sizeof a, b, sizeof b);
    EXPECT(s == CU_OK && same(&got, want, sizeof want), "status %d len %zu", (int)s, got.n);
    cu_buffer_free(got.p);
    free(cat);
    free(ca);
}

/* A zstd frame header declaring 2^62 bytes of content. */
static void test_bogus_declared_size(void) {
    if (!cu_algorithm_available(CU_ALGO_ZSTD)) return;
    static const uint8_t frame[] = {
        0x28, 0xb5, 0x2f, 0xfd,  /* magic */
        0xe0,                    /* 8-byte content size, single segment */
        0, 0, 0, 0, 0, 0, 0, 0x40,
    };
    size_t old_cap = cu_get_max_decompressed_size();
    buf_t got;
    cu_status_t s = alloc_decode(CU_ALGO_ZSTD, frame, sizeof frame, &got);
    EXPECT(s == CU_ERR_SIZE_LIMIT && !got.p, "default cap: status %d", (int)s);
    /* No cap: the declared size can't be allocated, and the input is just a header, so the stream path fails on it. Never an abort, never success. */
    cu_set_max_decompressed_size(0);
    s = alloc_decode(CU_ALGO_ZSTD, frame, sizeof frame, &got);
    cu_set_max_decompressed_size(old_cap);
    EXPECT(s != CU_OK && !got.p && got.n == 0, "no cap: status %d", (int)s);
}

static void test_arguments(void) {
    uint8_t* out = (uint8_t*)1;
    size_t n = 7;
    uint8_t byte = 0;
    EXPECT(cu_decompress_alloc(CU_ALGO_ZLIB, &byte, 1, NULL, &n) == CU_ERR_INVALID_ARG, "NULL out");
    EXPECT(cu_decompress_alloc(CU_ALGO_ZLIB, &byte, 1, &out, NULL) == CU_ERR_INVALID_ARG, "NULL out_len");
    EXPECT(cu_decompress_alloc(CU_ALGO_ZLIB, NULL, 5, &out, &n) == CU_ERR_INVALID_ARG && !out && n == 0,
           "NULL input with a length: out %p len %zu", (void*)out, n);
    out = (uint8_t*)1;
    EXPECT(cu_decompress_alloc((cu_algorithm_t)999, &byte, 1, &out, &n) == CU_ERR_UNSUPPORTED_ALGO && !out,
           "unknown algorithm");
    cu_buffer_free(NULL);
}

#ifdef CU_TEST_OOM
/* Address space is limited to a little past what is mapped now, and a 256 MiB output (from a tiny input, no cap) can't be held: a status, not an abort. */
static void test_oom(void) {
    enum { N = 256 << 20 };
    if (!cu_algorithm_available(CU_ALGO_ZLIB)) return;
    uint8_t* in = calloc(N, 1);
    EXPECT(in, "oom");
    size_t c_len = 0;
    uint8_t* c = compress(CU_ALGO_ZLIB, in, N, &c_len);
    free(in);
    EXPECT(c, "compress failed");

    FILE* f = fopen("/proc/self/statm", "r");
    unsigned long pages = 0;
    int ok = f && fscanf(f, "%lu", &pages) == 1;
    if (f) fclose(f);
    EXPECT(ok, "cannot read /proc/self/statm");
    struct rlimit old, lim;
    EXPECT(getrlimit(RLIMIT_AS, &old) == 0, "getrlimit");
    lim = old;
    lim.rlim_cur = (rlim_t)pages * 4096 + ((rlim_t)64 << 20);
    if (old.rlim_max != RLIM_INFINITY && lim.rlim_cur > old.rlim_max) lim.rlim_cur = old.rlim_max;
    EXPECT(setrlimit(RLIMIT_AS, &lim) == 0, "setrlimit");

    size_t old_cap = cu_get_max_decompressed_size();
    cu_set_max_decompressed_size(0);
    buf_t got;
    cu_status_t s = alloc_decode(CU_ALGO_ZLIB, c, c_len, &got);
    cu_set_max_decompressed_size(old_cap);
    setrlimit(RLIMIT_AS, &old);
    EXPECT(s == CU_ERR_OOM && !got.p && got.n == 0, "status %d", (int)s);
    free(c);
}
#endif

int main(void) {
    test_arguments();
    test_round_trips();
    test_far_larger_than_input();
    test_cap();
    test_cap_disabled_and_tiny();
    test_malformed_and_truncated();
    test_empty();
    test_concatenated();
    test_zstd_mixed_frames();
    test_bogus_declared_size();
#ifdef CU_TEST_OOM
    test_oom();
#endif
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
