/*
 * test_deflate_levels.c — the 1..10 level scale of the DEFLATE family (zlib,
 * gzip, raw deflate) behaves as documented in src/algorithms/zlib/
 * deflate_backend.h: level 1 (zlib-ng's quick strategy) gives clearly the
 * largest output, level 7 beats level 2, and levels 8, 9 and 10 are the same
 * setting. Output is the same on every CPU and SIMD path, so the checks
 * are deterministic. Skips any codec that isn't compiled in.
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

static uint64_t rng_state = 0x2545F4914F6CDD1Dull;
static uint32_t rnd(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 24);
}

/* English-like text: words drawn with a skewed distribution from a random
 * vocabulary, so there are repeats at every distance but no long exact runs. */
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
        uint32_t r = rnd();
        int w = (int)((r % VOCAB) * (rnd() % VOCAB) / VOCAB);  /* skewed to low ids */
        for (const char* c = words[w]; *c && i < n; c++) p[i++] = (uint8_t)*c;
    }
}

int main(void) {
    const size_t n = 512 * 1024;
    uint8_t* in = malloc(n);
    fill_text(in, n);
    const size_t cap = cu_compress_bound(n, CU_ALGO_ZLIB) + 1024;
    uint8_t* out[11];
    for (int l = 1; l <= 10; l++) out[l] = malloc(cap);

    for (size_t a = 0; a < N_ALGOS; a++) {
        cu_algorithm_t algo = ALGOS[a];
        if (!cu_algorithm_available(algo)) continue;
        const char* name = cu_algorithm_name(algo);
        size_t len[11];
        for (int l = 1; l <= 10; l++) {
            len[l] = cap;
            cu_status_t st = cu_compress(algo, in, n, out[l], &len[l], l);
            if (st != CU_OK) {
                fprintf(stderr, "FAIL %s level %d: %s\n", name, l, cu_strerror(st));
                return 1;
            }
        }
        /* Level 1 is the "quick" strategy: clearly the largest output. Beyond
         * that the sizes are only checked end to end (a step may swap a little
         * ratio for speed), not step by step. */
        for (int l = 2; l <= 10; l++) {
            if (len[1] < len[l] + len[l] / 10) {
                fprintf(stderr, "FAIL %s: level 1 (%zu bytes) is not clearly larger than level %d (%zu bytes)\n",
                        name, len[1], l, len[l]);
                return 1;
            }
        }
        if (len[7] > len[2]) {
            fprintf(stderr, "FAIL %s: level 7 (%zu bytes) larger than level 2 (%zu bytes)\n",
                    name, len[7], len[2]);
            return 1;
        }
        for (int l = 9; l <= 10; l++) {
            if (len[l] != len[8] || memcmp(out[l], out[8], len[8]) != 0) {
                fprintf(stderr, "FAIL %s: level %d differs from level 8 (documented as one setting)\n",
                        name, l);
                return 1;
            }
        }
        printf("%-8s level sizes:", name);
        for (int l = 1; l <= 10; l++) printf(" %zu", len[l]);
        printf("\n");
    }
    for (int l = 1; l <= 10; l++) free(out[l]);
    free(in);
    printf("deflate level scale ok\n");
    return 0;
}
