/*
 * test_deflate_bound.c — cu_compress_bound is a true upper bound for the DEFLATE
 * family (zlib, gzip, raw deflate) at every level, on input that exercises each
 * block type: runs, text, incompressible bytes (stored blocks) and a mix. The
 * one-shot compressors are handed exactly cu_compress_bound bytes of output
 * space and must fit, then round-trip. Guards the deflate backend's own
 * worst-case output against the bound the wrappers advertise.
 *
 * Exits with 0 on success, nonzero with a message on failure.
 */

#include "compress_utils.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const cu_algorithm_t ALGOS[] = {CU_ALGO_ZLIB, CU_ALGO_GZIP, CU_ALGO_DEFLATE};
#define N_ALGOS (sizeof(ALGOS) / sizeof(ALGOS[0]))

enum { K_ZEROS, K_TEXT, K_RANDOM, K_MIXED, N_KINDS };
static const char* const KIND_NAME[N_KINDS] = {"zeros", "text", "random", "mixed"};

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint8_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint8_t)(rng_state >> 24);
}

static void fill(uint8_t* p, size_t n, int kind) {
    static const char* const words[] = {"the ", "quick ", "brown ", "fox ", "jumps ",
                                        "over ", "lazy ", "dog ", "and ", "runs "};
    size_t i = 0;
    switch (kind) {
    case K_ZEROS: memset(p, 0, n); break;
    case K_TEXT:
        while (i < n) {
            const char* w = words[rnd() % 10];
            for (; *w && i < n; w++) p[i++] = (uint8_t)*w;
        }
        break;
    case K_RANDOM:
        for (i = 0; i < n; i++) p[i] = rnd();
        break;
    default: /* mixed: alternating random bursts and runs */
        while (i < n) {
            size_t len = 1 + (size_t)(rnd() % 200), j;
            int run = rnd() & 1;
            uint8_t v = rnd();
            for (j = 0; j < len && i < n; j++, i++) p[i] = run ? v : rnd();
        }
    }
}

int main(void) {
    static const size_t SIZES[] = {
        0, 1, 2, 3, 4, 5, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65, 100, 255, 256, 257,
        1000, 1023, 1024, 1025, 4095, 4096, 4097, 16383, 16384, 16385, 32767, 32768,
        32769, 65535, 65536, 65537, 131071, 131072, 131073, 300000, 1u << 20};
    const size_t N_SIZES = sizeof(SIZES) / sizeof(SIZES[0]);
    const size_t max_n = 1u << 20;
    uint8_t* in = malloc(max_n);
    uint8_t* dec = malloc(max_n + 1);
    size_t checked = 0;

    for (size_t a = 0; a < N_ALGOS; a++) {
        cu_algorithm_t algo = ALGOS[a];
        if (!cu_algorithm_available(algo)) continue;
        const char* name = cu_algorithm_name(algo);
        for (int kind = 0; kind < N_KINDS; kind++) {
            for (size_t s = 0; s < N_SIZES; s++) {
                size_t n = SIZES[s];
                fill(in, n, kind);
                size_t bound = cu_compress_bound(n, algo);
                uint8_t* out = malloc(bound ? bound : 1);
                for (int level = 1; level <= 10; level++) {
                    /* Big inputs at a subset of levels keeps the run short. */
                    if (n >= 131071 && level != 1 && level != 6 && level != 10) continue;
                    size_t out_len = bound;
                    cu_status_t st = cu_compress(algo, in, n, out, &out_len, level);
                    if (st != CU_OK || out_len > bound) {
                        fprintf(stderr,
                                "FAIL %s %s n=%zu level=%d: status=%s out_len=%zu bound=%zu (%s)\n",
                                name, KIND_NAME[kind], n, level, cu_strerror(st), out_len,
                                bound, cu_last_error());
                        return 1;
                    }
                    size_t dec_len = n + 1;
                    st = cu_decompress(algo, out, out_len, dec, &dec_len);
                    if (st != CU_OK || dec_len != n || (n && memcmp(dec, in, n) != 0)) {
                        fprintf(stderr, "FAIL %s %s n=%zu level=%d: round-trip %s (got %zu bytes)\n",
                                name, KIND_NAME[kind], n, level, cu_strerror(st), dec_len);
                        return 1;
                    }
                    checked++;
                }
                free(out);
            }
        }
    }
    free(in);
    free(dec);
    printf("deflate bound holds: %zu compress/decompress round-trips\n", checked);
    return 0;
}
