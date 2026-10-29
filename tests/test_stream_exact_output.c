/*
 * Streaming decompression into an exactly-sized output buffer, for every
 * streaming codec.
 *
 * A caller that knows the decompressed size passes a buffer of exactly that
 * size. Once the output is full, a complete stream must still finish: finish()
 * returns CU_OK on its first call, not CU_ERR_BUF_TOO_SMALL (which tells the
 * caller to drain again and would never end). zstd and LZ4 used to answer
 * BUF_TOO_SMALL there, because a decoder past a frame end reports a nonzero
 * "wants the next frame" hint that read as output still held inside it.
 *
 * Covered, with the input fed in chunks from 1 byte up to larger than the whole
 * stream:
 *   - exactly-sized output: every write returns CU_OK and finish() returns
 *     CU_OK at once, also when the caller has no room left to offer (NULL, 0).
 *     Fed 1 byte at a time, a trailer split from the data that filled the
 *     buffer needs one spare byte of room: with none, a decoder can't tell
 *     held output from a trailer still to come, so its write returns
 *     CU_ERR_BUF_TOO_SMALL (zlib, gzip, deflate, bzip2, LZ4);
 *   - output windows that fill exactly, draining with BUF_TOO_SMALL;
 *   - truncated input is never reported as complete, into a full or a roomy
 *     buffer: finish() returns CU_ERR_TRUNCATED, or CU_ERR_BUF_TOO_SMALL while
 *     the caller offers no room (a decoder can't tell held output from missing
 *     input without room), and TRUNCATED once it does;
 *   - the max-output limit sits exactly at the output size: allowed at the
 *     limit, CU_ERR_SIZE_LIMIT one byte below it.
 */
#include "compress_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value)                                                                \
    do {                                                                            \
        if (!(value)) {                                                             \
            fprintf(stderr, "exact-output check failed: %s:%d: %s (%s)\n", __FILE__,\
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

#define DRAIN_MAX 64

static void make_input(uint8_t* p, size_t n) {
    unsigned x = 4242;
    for (size_t i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        p[i] = (uint8_t)("exact output text "[i % 18] + (i / 4000) % 3 + ((x >> 20) & 1));
    }
}

/* Raw LZ4 blocks carry no end marker, so a cut one is not an error. */
static int has_truncation(cu_algorithm_t algo) {
    return algo != CU_ALGO_LZ4_RAW;
}

typedef struct {
    cu_algorithm_t algo;
    const char* name;
    size_t n;
    uint8_t* plain;
    uint8_t* packed;
    size_t packed_len;
} fixture_t;

static void fixture_free(fixture_t* f) {
    free(f->plain);
    free(f->packed);
}

static int fixture_make(fixture_t* f, cu_algorithm_t algo, size_t n) {
    memset(f, 0, sizeof(*f));
    f->algo = algo;
    f->name = cu_algorithm_name(algo);
    f->n = n;
    f->plain = malloc(n);
    size_t cap = cu_compress_bound(n, algo) + 1024;
    f->packed = malloc(cap);
    CHECK(f->plain && f->packed);
    make_input(f->plain, n);
    f->packed_len = cap;
    return cu_compress(algo, f->plain, n, f->packed, &f->packed_len, 3) == CU_OK;
}

/* Feeds packed[0, len) in `chunk`-byte writes into out[0, cap), draining any
 * BUF_TOO_SMALL with empty writes. Returns the first status that is neither OK
 * nor BUF_TOO_SMALL (CU_OK if there was none); *pos is the output length. */
static cu_status_t feed(cu_decompress_stream_t* s, const uint8_t* packed, size_t len,
                        size_t chunk, uint8_t* out, size_t cap, size_t* pos) {
    for (size_t off = 0; off < len; off += chunk) {
        size_t m = len - off < chunk ? len - off : chunk;
        size_t ol = cap - *pos;
        cu_status_t st = cu_decompress_stream_write(s, packed + off, m, out + *pos, &ol);
        *pos += ol;
        for (int g = 0; st == CU_ERR_BUF_TOO_SMALL && g < DRAIN_MAX; g++) {
            ol = cap - *pos;
            st = cu_decompress_stream_write(s, NULL, 0, out + *pos, &ol);
            *pos += ol;
        }
        if (st != CU_OK) return st;
    }
    return CU_OK;
}

/* Output buffer of n + slack bytes: all writes OK, finish OK on the first call.
 * With `no_room`, and when every byte is already out (Snappy only produces its
 * output at finish), finish gets (NULL, 0) instead of the empty tail. */
static void check_exact(const fixture_t* f, size_t chunk, size_t slack, int no_room) {
    cu_decompress_stream_t* s = NULL;
    CHECK(cu_decompress_stream_create(f->algo, &s) == CU_OK);
    size_t cap = f->n + slack;
    uint8_t* out = malloc(cap);
    CHECK(out);
    size_t pos = 0;
    cu_status_t st = feed(s, f->packed, f->packed_len, chunk, out, cap, &pos);
    EXPECT(st == CU_OK, "%s chunk=%zu slack=%zu: write returned %d (%s)", f->name, chunk,
           slack, (int)st, cu_last_error());
    size_t ol = cap - pos;
    int null_finish = no_room && pos == cap;
    st = null_finish ? cu_decompress_stream_finish(s, NULL, &(size_t){0})
                     : cu_decompress_stream_finish(s, out + pos, &ol);
    if (null_finish) ol = 0;
    pos += ol;
    EXPECT(st == CU_OK, "%s chunk=%zu slack=%zu%s: finish into a %zu-byte buffer returned %d "
           "(expected CU_OK on the first call)", f->name, chunk, slack,
           null_finish ? " (NULL,0)" : "", cap, (int)st);
    EXPECT(pos == f->n && memcmp(out, f->plain, f->n) == 0,
           "%s chunk=%zu: wrong output (%zu of %zu bytes)", f->name, chunk, pos, f->n);
    cu_decompress_stream_destroy(s);
    free(out);
}

/* Output through windows of `win` bytes; the last one fills exactly (n is a
 * multiple of win), then finish gets a fresh window and must return OK. */
static void check_windows(const fixture_t* f, size_t win) {
    cu_decompress_stream_t* s = NULL;
    CHECK(cu_decompress_stream_create(f->algo, &s) == CU_OK);
    uint8_t* out = malloc(f->n + win);
    CHECK(out);
    size_t pos = 0;
    cu_status_t st = CU_OK;
    size_t chunk = f->packed_len;  /* the whole stream in one write, then drain */
    for (size_t off = 0; off < f->packed_len && st == CU_OK; off += chunk) {
        const uint8_t* in = f->packed + off;
        size_t m = f->packed_len - off < chunk ? f->packed_len - off : chunk;
        do {
            size_t ol = win;
            st = cu_decompress_stream_write(s, in, m, out + pos, &ol);
            pos += ol;
            in = NULL;
            m = 0;
        } while (st == CU_ERR_BUF_TOO_SMALL);
    }
    EXPECT(st == CU_OK, "%s win=%zu: write returned %d", f->name, win, (int)st);
    int calls = 0;
    /* Snappy decodes at finish, so finish may need one call per window. */
    int max_calls = (int)(f->n / win) + DRAIN_MAX;
    do {
        size_t ol = win;
        st = cu_decompress_stream_finish(s, out + pos, &ol);
        pos += ol;
        calls++;
    } while (st == CU_ERR_BUF_TOO_SMALL && calls < max_calls);
    EXPECT(st == CU_OK, "%s win=%zu: finish returned %d after %d calls", f->name, win,
           (int)st, calls);
    EXPECT(pos == f->n && memcmp(out, f->plain, f->n) == 0,
           "%s win=%zu: wrong output (%zu of %zu bytes)", f->name, win, pos, f->n);
    cu_decompress_stream_destroy(s);
    free(out);
}

/* Truncated input must never finish OK, however much output room there is. */
static void check_truncated(const fixture_t* f, size_t keep, size_t slack) {
    cu_decompress_stream_t* s = NULL;
    CHECK(cu_decompress_stream_create(f->algo, &s) == CU_OK);
    size_t cap = f->n + slack;
    uint8_t* out = malloc(cap ? cap : 1);
    CHECK(out);
    size_t pos = 0;
    cu_status_t st = feed(s, f->packed, keep, 4096, out, cap, &pos);
    if (st == CU_OK) {
        int calls = 0;
        do {
            size_t ol = cap - pos;
            st = cu_decompress_stream_finish(s, out + pos, &ol);
            pos += ol;
            calls++;
        } while (st == CU_ERR_BUF_TOO_SMALL && pos < cap && calls < DRAIN_MAX);
    }
    /* Some codecs notice the cut while writing (a bad length), others at
     * finish; none may call it complete. */
    EXPECT(st != CU_OK, "%s: stream cut at %zu of %zu bytes finished OK (slack %zu)", f->name,
           keep, f->packed_len, slack);
    if (slack > 0) {
        EXPECT(st != CU_ERR_BUF_TOO_SMALL,
               "%s: stream cut at %zu of %zu bytes with %zu spare bytes still asks for "
               "more room", f->name, keep, f->packed_len, slack);
    }
    cu_decompress_stream_destroy(s);
    free(out);
}

static void check_limit(const fixture_t* f) {
    for (int over = 0; over < 2; over++) {
        cu_decompress_stream_t* s = NULL;
        CHECK(cu_decompress_stream_create(f->algo, &s) == CU_OK);
        CHECK(cu_decompress_stream_set_max_output(s, over ? f->n - 1 : f->n) == CU_OK);
        uint8_t* out = malloc(f->n);
        CHECK(out);
        size_t pos = 0;
        cu_status_t st = feed(s, f->packed, f->packed_len, 65536, out, f->n, &pos);
        if (st == CU_OK) {
            size_t ol = f->n - pos;
            st = cu_decompress_stream_finish(s, out + pos, &ol);
        }
        if (over) {
            EXPECT(st == CU_ERR_SIZE_LIMIT, "%s: limit n-1 returned %d, expected "
                   "CU_ERR_SIZE_LIMIT", f->name, (int)st);
        } else {
            EXPECT(st == CU_OK, "%s: limit n (exact) returned %d, expected CU_OK", f->name,
                   (int)st);
        }
        cu_decompress_stream_destroy(s);
        free(out);
    }
}

static void check_codec(cu_algorithm_t algo) {
    fixture_t f;
    /* A multiple of every window size used below. */
    const size_t n = 192000;
    if (!fixture_make(&f, algo, n)) {
        printf("  %-8s skipped (no compressor)\n", cu_algorithm_name(algo));
        fixture_free(&f);
        return;
    }
    int before = failures;

    /* 4 KiB, 64 KiB, 1 MiB, and larger than the whole stream, into a buffer of
     * exactly n bytes. */
    const size_t chunks[] = {4096, 65536, 1u << 20, f.packed_len + 1};
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        check_exact(&f, chunks[i], 0, 0);
        check_exact(&f, chunks[i], 0, 1);
    }
    /* 1 byte at a time splits a trailer (checksum, end mark) from the data that
     * filled the buffer; one spare byte of room lets the decoder see the end. */
    check_exact(&f, 1, 1, 0);
    check_exact(&f, 1, 4096, 0);

    const size_t windows[] = {64, 1000, 4000, 64000};
    for (size_t i = 0; i < sizeof(windows) / sizeof(windows[0]); i++) {
        check_windows(&f, windows[i]);
    }

    if (has_truncation(algo)) {
        const size_t cuts[] = {1, 4, f.packed_len / 2, f.packed_len - 1};
        for (size_t i = 0; i < sizeof(cuts) / sizeof(cuts[0]); i++) {
            check_truncated(&f, cuts[i], 0);
            check_truncated(&f, cuts[i], 4096);
        }
    }

    check_limit(&f);

    printf("  %-8s %s\n", f.name, failures == before ? "ok" : "FAILED");
    fixture_free(&f);
}

int main(void) {
    printf("Streaming decompression into exactly-sized output\n");
    for (int a = 0; a <= CU_ALGO_LZ4_RAW; a++) {
        cu_algorithm_t algo = (cu_algorithm_t)a;
        if (algo == CU_ALGO_LZMA || !cu_algorithm_available(algo)) continue;
        check_codec(algo);
    }
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("exact-output tests passed\n");
    return 0;
}
