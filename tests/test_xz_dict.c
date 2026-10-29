/*
 * test_xz_dict.c — one-shot xz compression caps the dictionary at the input
 * size (src/algorithms/xz/xz.c, xz_fit_dict_size): the input size rounded up to
 * a power of two, at least 4 KiB, never above the preset's own dictionary.
 * Checks, at sizes around the cap and around the presets' dictionaries:
 *   - the dictionary recorded in the .xz block header is the expected one;
 *   - the output round-trips through cu_decompress and the exact-size
 *     cu_compress_bound buffer is enough;
 *   - a match at the farthest distance the input allows is still found (a cap
 *     below the input would show up as a ratio loss);
 *   - the output is no larger than the streaming encoder's (which keeps the
 *     preset's dictionary) beyond the one-shot header's two size fields;
 *   - the streaming encoder keeps the preset's dictionary (documented behavior);
 *   - the reference `xz` CLI decodes the output, within a decoder memory limit
 *     the preset's full dictionary would exceed. Skipped when `xz` is not on
 *     PATH (and on Windows).
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

static uint64_t rng_state = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 24);
}

static void fill_random(uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)rnd();
}

/* English-like text: skewed vocabulary, repeats at every distance. */
static void fill_text(uint8_t* p, size_t n) {
    enum { VOCAB = 400, MAXW = 9 };
    char words[VOCAB][MAXW + 2];
    for (int w = 0; w < VOCAB; w++) {
        int len = 2 + (int)(rnd() % (MAXW - 1));
        for (int i = 0; i < len; i++) words[w][i] = (char)('a' + rnd() % 26);
        words[w][len] = ' ';
        words[w][len + 1] = 0;
    }
    size_t i = 0;
    while (i < n) {
        int w = (int)((rnd() % VOCAB) * (rnd() % VOCAB) / VOCAB);
        for (const char* c = words[w]; *c && i < n; c++) p[i++] = (uint8_t)*c;
    }
}

/* Dictionary size of the preset a user level maps to (levels 1..10 are presets
 * 0..9): 256 KiB, 1, 2, 4, 4, 8, 8, 16, 32, 64 MiB. */
static uint64_t preset_dict(int level) {
    static const int pow2[10] = {18, 20, 21, 22, 22, 23, 23, 24, 25, 26};
    return (uint64_t)1 << pow2[level - 1];
}

static uint64_t expected_dict(size_t n, int level) {
    uint64_t d = 4096, cap = preset_dict(level);
    while (d < n && d < cap) d <<= 1;
    return d < cap ? d : cap;
}

/* Reads a variable-length integer (7 bits per byte, low first); -1 on overrun. */
static int read_vli(const uint8_t* p, size_t len, size_t* pos) {
    int n = 0;
    for (;;) {
        if (*pos >= len) return -1;
        uint8_t b = p[(*pos)++];
        n++;
        if (!(b & 0x80)) return n;
    }
}

/* Dictionary size from the LZMA2 filter properties in the first block header of
 * a .xz stream (one filter, as we write it); 0 if it cannot be read. */
static uint64_t header_dict(const uint8_t* xz, size_t len) {
    static const uint8_t magic[6] = {0xFD, '7', 'z', 'X', 'Z', 0x00};
    if (len < 12 + 4 || memcmp(xz, magic, 6) != 0) return 0;
    size_t pos = 12;
    size_t header_end = 12 + ((size_t)xz[pos] + 1) * 4;
    if (header_end > len) return 0;
    uint8_t flags = xz[pos + 1];
    pos += 2;
    if (flags & 0x40) { if (read_vli(xz, header_end, &pos) < 0) return 0; }  /* compressed size */
    if (flags & 0x80) { if (read_vli(xz, header_end, &pos) < 0) return 0; }  /* uncompressed size */
    if (pos + 3 > header_end || xz[pos] != 0x21 || xz[pos + 1] != 1) return 0;  /* LZMA2, 1 prop byte */
    uint8_t b = xz[pos + 2];
    if (b > 40) return 0;
    if (b == 40) return 0xFFFFFFFFull;
    return (uint64_t)(2 | (b & 1)) << (b / 2 + 11);
}

static size_t stream_compress(const uint8_t* in, size_t n, int level, uint8_t* out, size_t cap) {
    cu_compress_stream_t* s = NULL;
    if (cu_compress_stream_create(CU_ALGO_XZ, level, &s) != CU_OK) return 0;
    size_t total = 0, got = cap;
    cu_status_t st = cu_compress_stream_write(s, in, n, out, &got);
    total += got;
    if (st == CU_OK) {
        got = cap - total;
        st = cu_compress_stream_finish(s, out + total, &got);
        total += got;
    }
    cu_compress_stream_destroy(s);
    return st == CU_OK ? total : 0;
}

static int have_xz_cli(void) {
#ifdef _WIN32
    return 0;
#else
    return system("xz --version > /dev/null 2>&1") == 0;
#endif
}

#ifndef _WIN32
/* Decodes `xz` with the reference CLI under `memlimit` (e.g. "1MiB"); 1 when it
 * decodes to exactly `want`. */
static int xz_cli_decodes(const uint8_t* xz, size_t xz_len, const uint8_t* want, size_t want_len,
                          const char* memlimit) {
    char tmpl[] = "/tmp/test_xz_dict_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    FILE* f = fdopen(fd, "wb");
    if (!f || fwrite(xz, 1, xz_len, f) != xz_len) { if (f) fclose(f); unlink(tmpl); return -1; }
    fclose(f);
    char cmd[256];
    snprintf(cmd, sizeof cmd, "xz -dc --memlimit-decompress=%s -- %s 2>/dev/null", memlimit, tmpl);
    FILE* p = popen(cmd, "r");
    int ok = 0;
    if (p) {
        uint8_t* got = malloc(want_len + 1);
        size_t n = fread(got, 1, want_len + 1, p);
        ok = pclose(p) == 0 && n == want_len && (want_len == 0 || memcmp(got, want, want_len) == 0);
        free(got);
    }
    unlink(tmpl);
    return ok;
}
#endif

static void check_size(const uint8_t* in, size_t n, int level, int cli, int stored_ok) {
    size_t bound = cu_compress_bound(n, CU_ALGO_XZ);
    uint8_t* out = malloc(bound);
    uint8_t* back = malloc(n + 1);
    uint8_t* sout = malloc(bound + 1024);
    size_t out_len = bound;
    cu_status_t st = cu_compress(CU_ALGO_XZ, in, n, out, &out_len, level);
    EXPECT(st == CU_OK, "level %d n=%zu: compress: %s", level, n, cu_strerror(st));

    size_t back_len = n + 1;
    st = cu_decompress(CU_ALGO_XZ, out, out_len, back, &back_len);
    EXPECT(st == CU_OK && back_len == n && (n == 0 || memcmp(back, in, n) == 0),
           "level %d n=%zu: round trip (%s)", level, n, cu_strerror(st));

    /* An empty input has no block, so no dictionary is recorded. */
    if (n > 0) {
        uint64_t got = header_dict(out, out_len), want = expected_dict(n, level);
        /* Input that does not compress at all is stored; liblzma then records the
         * 4 KiB minimum (a stored block needs no dictionary), whatever the cap. */
        if (stored_ok && got == 4096) want = got;
        EXPECT(got == want, "level %d n=%zu: header dictionary %llu, expected %llu", level, n,
               (unsigned long long)got, (unsigned long long)want);
    }

    /* Streaming keeps the preset's dictionary and the same payload; the one-shot
     * header carries two more size fields (at most ~20 bytes). */
    size_t sl = stream_compress(in, n, level, sout, bound + 1024);
    EXPECT(sl > 0, "level %d n=%zu: stream compress", level, n);
    back_len = n + 1;
    st = cu_decompress(CU_ALGO_XZ, sout, sl, back, &back_len);
    EXPECT(st == CU_OK && back_len == n && (n == 0 || memcmp(back, in, n) == 0),
           "level %d n=%zu: stream round trip (%s)", level, n, cu_strerror(st));
    if (n > 0) {
        EXPECT(header_dict(sout, sl) == preset_dict(level),
               "level %d n=%zu: stream dictionary %llu, expected the preset's %llu", level, n,
               (unsigned long long)header_dict(sout, sl), (unsigned long long)preset_dict(level));
    }
    EXPECT(out_len <= sl + 32, "level %d n=%zu: one-shot %zu bytes vs stream %zu", level, n,
           out_len, sl);

#ifndef _WIN32
    if (cli) {
        /* 1 MiB is below the 8 MiB the default preset would need to decode. */
        const char* limit = expected_dict(n, level) <= (64 << 10) ? "1MiB" : "256MiB";
        int ok = xz_cli_decodes(out, out_len, in, n, limit);
        EXPECT(ok == 1, "level %d n=%zu: xz CLI did not decode within %s", level, n, limit);
    }
#endif
    free(out);
    free(back);
    free(sout);
}

/* [a][a]: a random half repeated once. The farthest match is half bytes back,
 * so the output is only about half the input if the dictionary holds it. */
static void check_far_match(size_t half, int extra, int level) {
    size_t n = 2 * half + (size_t)extra;
    uint8_t* in = malloc(n);
    fill_random(in, half);
    memcpy(in + half, in, half);
    fill_random(in + 2 * half, (size_t)extra);
    size_t cap = cu_compress_bound(n, CU_ALGO_XZ), len = cap;
    uint8_t* out = malloc(cap);
    cu_status_t st = cu_compress(CU_ALGO_XZ, in, n, out, &len, level);
    EXPECT(st == CU_OK, "level %d half=%zu: compress: %s", level, half, cu_strerror(st));
    EXPECT(len < half + (size_t)extra + half / 16 + 200,
           "level %d half=%zu+%d: %zu bytes, the repeat was not matched", level, half, extra, len);
    free(out);
    free(in);
}

int main(void) {
    if (!cu_algorithm_available(CU_ALGO_XZ)) {
        printf("SKIP xz not compiled in\n");
        return 0;
    }
    const int cli = have_xz_cli();
    if (!cli) printf("note: xz CLI not found, skipping the reference-decoder checks\n");

    const size_t max_n = (1u << 20) + 8;  /* one past level 2's 1 MiB dictionary */
    uint8_t* text = malloc(max_n);
    uint8_t* noise = malloc(max_n);
    fill_text(text, max_n);
    fill_random(noise, max_n);

    /* Around the 4 KiB floor and the first powers of two, at every kind of
     * preset (hc3 level 1, hc4 level 3, bt4 levels 6 and 10). */
    static const size_t small[] = {0, 1, 2, 100, 4095, 4096, 4097, 8191, 8192, 8193,
                                   65535, 65536, 65537};
    static const int levels[] = {1, 3, 6, 10};
    for (size_t l = 0; l < sizeof levels / sizeof *levels; l++) {
        for (size_t i = 0; i < sizeof small / sizeof *small; i++) {
            check_size(text, small[i], levels[l], cli, 0);
            check_size(noise, small[i], levels[l], cli, 1);
        }
    }

    /* Around the preset's own dictionary the cap must stop applying: level 1 is
     * 256 KiB, level 2 is 1 MiB. */
    static const size_t big1[] = {262143, 262144, 262145, 300000};
    for (size_t i = 0; i < sizeof big1 / sizeof *big1; i++) check_size(text, big1[i], 1, cli, 0);
    static const size_t big2[] = {(1u << 20) - 1, 1u << 20, (1u << 20) + 1};
    for (size_t i = 0; i < sizeof big2 / sizeof *big2; i++) check_size(text, big2[i], 2, cli, 0);

    /* The farthest match the input allows, at the cap, one byte over, and beyond. */
    static const size_t halves[] = {2048, 4096, 8192, 32768, 65536};
    for (size_t i = 0; i < sizeof halves / sizeof *halves; i++) {
        for (int extra = 0; extra <= 1; extra++) {
            check_far_match(halves[i], extra, 1);
            check_far_match(halves[i], extra, 6);
            check_far_match(halves[i], extra, 10);
        }
    }

    free(text);
    free(noise);
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
