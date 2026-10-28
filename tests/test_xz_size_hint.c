/*
 * test_xz_size_hint.c — cu_decompress_size_hint for xz (src/algorithms/xz/xz.c,
 * xz_decompress_size_hint): the uncompressed size comes from the Stream Footer
 * and Index, and only for an input that is exactly one Stream. Checks:
 *   - one-shot and streaming output of every size reports the exact size, and
 *     an output buffer of exactly that size decodes it;
 *   - the global cap applies to the declared size;
 *   - concatenated Streams, Stream Padding, trailing bytes, truncation and
 *     too-short input are SIZE_UNKNOWN (so a caller streams), and the one-shot
 *     decoder still reads the concatenated and padded forms;
 *   - a many-Block Stream written by the reference `xz` CLI sums its Blocks.
 *     Skipped when `xz` is not on PATH (and on Windows).
 *
 * Exits with 0 on success, nonzero with a message on failure.
 */

#include "compress_utils.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#endif

static int failures = 0;

#define EXPECT(cond, ...)                                                          \
    do {                                                                           \
        if (!(cond)) {                                                             \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                   \
            fprintf(stderr, __VA_ARGS__);                                          \
            fprintf(stderr, "\n");                                                 \
            failures++;                                                            \
            return;                                                                \
        }                                                                          \
    } while (0)

/* Skewed bytes with repeats, so the output is neither tiny nor stored. */
static void fill_text(uint8_t* p, size_t n, uint32_t seed) {
    uint32_t x = seed;
    for (size_t i = 0; i < n; i++) {
        x = x * 1664525u + 1013904223u;
        p[i] = (uint8_t)("etaoin shrdlu\n"[(x >> 24) % 14] + ((x >> 20) % 7 == 0 ? (x >> 8) % 5 : 0));
    }
}

static uint8_t* compress_oneshot(const uint8_t* in, size_t n, int level, size_t* out_len) {
    size_t cap = cu_compress_bound(n, CU_ALGO_XZ);
    uint8_t* out = malloc(cap);
    *out_len = cap;
    if (!out || cu_compress(CU_ALGO_XZ, in, n, out, out_len, level) != CU_OK) { free(out); return NULL; }
    return out;
}

static uint8_t* compress_streaming(const uint8_t* in, size_t n, size_t* out_len) {
    cu_compress_stream_t* cs = NULL;
    if (cu_compress_stream_create(CU_ALGO_XZ, 6, &cs) != CU_OK) return NULL;
    size_t cap = cu_compress_bound(n, CU_ALGO_XZ) + 4096, pos = 0;
    uint8_t* out = malloc(cap);
    size_t ol = cap;
    cu_status_t s = out ? cu_compress_stream_write(cs, in, n, out, &ol) : CU_ERR_OOM;
    pos = ol;
    if (s == CU_OK) { ol = cap - pos; s = cu_compress_stream_finish(cs, out + pos, &ol); pos += ol; }
    cu_compress_stream_destroy(cs);
    if (s != CU_OK) { free(out); return NULL; }
    *out_len = pos;
    return out;
}

/* Decodes into a buffer of exactly `want_len` bytes and compares. */
static int decodes_exactly(const uint8_t* xz, size_t xz_len, const uint8_t* want, size_t want_len) {
    uint8_t* out = malloc(want_len ? want_len : 1);
    size_t ol = want_len;
    int ok = out && cu_decompress(CU_ALGO_XZ, xz, xz_len, out, &ol) == CU_OK &&
             ol == want_len && (want_len == 0 || memcmp(out, want, want_len) == 0);
    free(out);
    return ok;
}

static cu_status_t hint(const uint8_t* xz, size_t xz_len, size_t* size) {
    *size = 12345;
    return cu_decompress_size_hint(CU_ALGO_XZ, xz, xz_len, size);
}

static void test_exact_sizes(void) {
    static const size_t sizes[] = {0, 1, 100, 4096, 70000, 1u << 20};
    for (size_t k = 0; k < sizeof sizes / sizeof *sizes; k++) {
        size_t n = sizes[k];
        uint8_t* in = malloc(n ? n : 1);
        EXPECT(in, "oom");
        fill_text(in, n, (uint32_t)k + 1);
        for (int level = 1; level <= 10; level += 5) {
            size_t xz_len = 0, size = 0;
            uint8_t* xz = compress_oneshot(in, n, level, &xz_len);
            EXPECT(xz, "n=%zu level %d: compress failed", n, level);
            cu_status_t s = hint(xz, xz_len, &size);
            EXPECT(s == CU_OK && size == n, "n=%zu level %d: hint status %d size %zu", n, level, (int)s, size);
            EXPECT(decodes_exactly(xz, xz_len, in, n), "n=%zu level %d: exact-size decode failed", n, level);
            free(xz);
        }
        size_t xz_len = 0, size = 0;
        uint8_t* xz = compress_streaming(in, n, &xz_len);
        EXPECT(xz, "n=%zu: streaming compress failed", n);
        cu_status_t s = hint(xz, xz_len, &size);
        EXPECT(s == CU_OK && size == n, "n=%zu streamed: hint status %d size %zu", n, (int)s, size);
        EXPECT(decodes_exactly(xz, xz_len, in, n), "n=%zu streamed: exact-size decode failed", n);
        free(xz);
        free(in);
    }
}

static void test_cap(void) {
    enum { N = 100000 };
    uint8_t* in = malloc(N);
    EXPECT(in, "oom");
    fill_text(in, N, 7);
    size_t xz_len = 0, size = 0;
    uint8_t* xz = compress_oneshot(in, N, 6, &xz_len);
    EXPECT(xz, "compress failed");
    size_t old_cap = cu_get_max_decompressed_size();
    cu_set_max_decompressed_size(N - 1);
    cu_status_t s = hint(xz, xz_len, &size);
    cu_set_max_decompressed_size(N);
    cu_status_t at = hint(xz, xz_len, &size);
    cu_set_max_decompressed_size(old_cap);
    EXPECT(s == CU_ERR_SIZE_LIMIT, "cap below the size: status %d", (int)s);
    EXPECT(at == CU_OK && size == N, "cap at the size: status %d size %zu", (int)at, size);
    free(xz);
    free(in);
}

static void test_not_one_stream(void) {
    enum { NA = 50000, NB = 30000 };
    uint8_t* a = malloc(NA);
    uint8_t* b = malloc(NB);
    EXPECT(a && b, "oom");
    fill_text(a, NA, 11);
    fill_text(b, NB, 12);
    size_t la = 0, lb = 0, size = 0;
    uint8_t* xa = compress_oneshot(a, NA, 6, &la);
    uint8_t* xb = compress_oneshot(b, NB, 6, &lb);
    EXPECT(xa && xb, "compress failed");

    uint8_t* cat = malloc(la + lb + 8);
    uint8_t* want = malloc(NA + NB);
    EXPECT(cat && want, "oom");
    memcpy(want, a, NA);
    memcpy(want + NA, b, NB);

    /* Two Streams: the footer only describes the last one. */
    memcpy(cat, xa, la);
    memcpy(cat + la, xb, lb);
    EXPECT(hint(cat, la + lb, &size) == CU_ERR_SIZE_UNKNOWN, "concatenated streams: hint not SIZE_UNKNOWN");
    EXPECT(decodes_exactly(cat, la + lb, want, NA + NB), "concatenated streams: decode failed");

    /* Stream Padding after the footer (a multiple of four zero bytes). */
    memcpy(cat, xa, la);
    memset(cat + la, 0, 4);
    EXPECT(hint(cat, la + 4, &size) == CU_ERR_SIZE_UNKNOWN, "stream padding: hint not SIZE_UNKNOWN");
    EXPECT(decodes_exactly(cat, la + 4, a, NA), "stream padding: decode failed");

    /* Trailing bytes that are not padding, a missing last byte, a short input. */
    memcpy(cat, xa, la);
    cat[la] = 0x5a;
    EXPECT(hint(cat, la + 1, &size) == CU_ERR_SIZE_UNKNOWN, "trailing byte: hint not SIZE_UNKNOWN");
    EXPECT(hint(xa, la - 1, &size) == CU_ERR_SIZE_UNKNOWN, "truncated: hint not SIZE_UNKNOWN");
    EXPECT(hint(xa, 11, &size) == CU_ERR_SIZE_UNKNOWN, "short input: hint not SIZE_UNKNOWN");
    EXPECT(hint(xa, 0, &size) == CU_ERR_SIZE_UNKNOWN, "empty input: hint not SIZE_UNKNOWN");

    /* A footer with a damaged Backward Size (its CRC no longer matches). */
    memcpy(cat, xa, la);
    for (size_t i = 0; i < 4; i++) cat[la - 8 + i] ^= 0xff;  /* Backward Size */
    EXPECT(hint(cat, la, &size) == CU_ERR_SIZE_UNKNOWN, "corrupt footer: hint not SIZE_UNKNOWN");

    free(want);
    free(cat);
    free(xa);
    free(xb);
    free(a);
    free(b);
}

#ifndef _WIN32
/* A Stream of many Blocks, written by the reference CLI. 1 when it ran. */
static int xz_cli_many_blocks(const uint8_t* in, size_t n, uint8_t** out, size_t* out_len) {
    char tmpl[] = "/tmp/test_xz_size_hint_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return 0;
    FILE* f = fdopen(fd, "wb");
    if (!f || fwrite(in, 1, n, f) != n) { if (f) fclose(f); unlink(tmpl); return 0; }
    fclose(f);
    char cmd[256];
    snprintf(cmd, sizeof cmd, "xz -c --block-size=65536 -- %s 2>/dev/null", tmpl);
    FILE* p = popen(cmd, "r");
    if (!p) { unlink(tmpl); return 0; }
    size_t cap = n + 65536, len = 0, got;
    uint8_t* buf = malloc(cap);
    while (buf && (got = fread(buf + len, 1, cap - len, p)) > 0) len += got;
    int status = pclose(p);
    unlink(tmpl);
    if (!buf || status != 0 || len == 0) { free(buf); return 0; }
    *out = buf;
    *out_len = len;
    return 1;
}
#endif

static void test_many_blocks(void) {
#ifndef _WIN32
    if (system("xz --version > /dev/null 2>&1") != 0) {
        printf("note: xz CLI not found, skipping the many-block check\n");
        return;
    }
    enum { N = 1000000 };
    uint8_t* in = malloc(N);
    EXPECT(in, "oom");
    fill_text(in, N, 21);
    uint8_t* xz = NULL;
    size_t xz_len = 0, size = 0;
    EXPECT(xz_cli_many_blocks(in, N, &xz, &xz_len), "xz CLI failed");
    cu_status_t s = hint(xz, xz_len, &size);
    EXPECT(s == CU_OK && size == N, "many blocks: hint status %d size %zu", (int)s, size);
    EXPECT(decodes_exactly(xz, xz_len, in, N), "many blocks: exact-size decode failed");
    free(xz);
    free(in);
#endif
}

int main(void) {
    if (!cu_algorithm_available(CU_ALGO_XZ)) {
        printf("SKIP xz not compiled in\n");
        return 0;
    }
    test_exact_sizes();
    test_cap();
    test_not_one_stream();
    test_many_blocks();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
