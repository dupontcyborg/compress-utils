/*
 * gen_seeds.c — writes the fuzzer's seed corpus.
 *
 * Usage: ./gen_fuzz_seeds <out_dir> <lzo_fixture_dir>
 *
 * Each seed is a fuzz input as fuzz_decompress.c reads it:
 *   [FUZZ_ALGOS index][mode byte][compressed payload]
 * For every compiled-in codec it writes valid frames of a few sample inputs at
 * two levels, in default and single-stream mode, plus (for codecs with
 * single-stream support) two frames back to back and a frame followed by junk,
 * for the single-stream unused-tail checks. LZO has no encoder, so its seeds
 * come from tests/fixtures/lzo. The fuzzer mutates these into the malformed
 * inputs it actually tests; starting from valid frames gets it past each
 * format's magic bytes, headers and checksums.
 *
 * A mode byte of 0x20 sets a 4 KiB stream limit, which is also the cap cu_decompress_alloc runs under: the 4096-byte sample decodes to exactly the cap and the 20000-byte run is over it.
 */

#include "compress_utils.h"
#include "fuzz_algos.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_MAX 4096
#define RUN_LEN 20000

static const char* g_out_dir;
static int g_written;

static int write_seed(const char* name, uint8_t index, uint8_t mode,
                      const uint8_t* a, size_t a_len,
                      const uint8_t* b, size_t b_len) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s-m%02x", g_out_dir, name, mode);
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "gen_fuzz_seeds: cannot write %s\n", path);
        return 1;
    }
    uint8_t header[2] = {index, mode};
    int failed = fwrite(header, 1, 2, f) != 2 ||
                 (a_len && fwrite(a, 1, a_len, f) != a_len) ||
                 (b_len && fwrite(b, 1, b_len, f) != b_len);
    failed |= fclose(f) != 0;
    if (failed) {
        fprintf(stderr, "gen_fuzz_seeds: cannot write %s\n", path);
        return 1;
    }
    g_written++;
    return 0;
}

/* Compresses in into a malloc'd buffer; NULL if the codec can't. */
static uint8_t* compress_alloc(cu_algorithm_t algo, const uint8_t* in, size_t in_len,
                               int level, size_t* out_len) {
    size_t cap = cu_compress_bound(in_len, algo);
    if (cap == 0) return NULL;
    uint8_t* out = malloc(cap);
    if (!out) return NULL;
    *out_len = cap;
    if (cu_compress(algo, in, in_len, out, out_len, level) != CU_OK) {
        free(out);
        return NULL;
    }
    return out;
}

static uint8_t* read_file(const char* path, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t* buf = malloc(64 * 1024);
    *len = buf ? fread(buf, 1, 64 * 1024, f) : 0;
    int too_big = buf && fgetc(f) != EOF;
    fclose(f);
    if (!buf || too_big) {
        free(buf);
        return NULL;
    }
    return buf;
}

static int lzo_seeds(uint8_t index, const char* fixture_dir) {
    static const char* files[] = {"1.bin", "999.bin"};
    for (size_t i = 0; i < 2; i++) {
        char path[1024], name[64];
        snprintf(path, sizeof(path), "%s/%s", fixture_dir, files[i]);
        size_t len;
        uint8_t* data = read_file(path, &len);
        if (!data) {
            fprintf(stderr, "gen_fuzz_seeds: cannot read %s\n", path);
            return 1;
        }
        snprintf(name, sizeof(name), "lzo-fixture-%s", files[i]);
        int failed = write_seed(name, index, 0x00, data, len, NULL, 0);
        free(data);
        if (failed) return 1;
    }
    /* The smallest valid block: just the end marker. */
    static const uint8_t empty[] = {17, 0, 0};
    return write_seed("lzo-empty", index, 0x00, empty, sizeof(empty), NULL, 0);
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <out_dir> <lzo_fixture_dir>\n", argv[0]);
        return 2;
    }
    g_out_dir = argv[1];

    /* Sample inputs: empty, one byte, repetitive text, and mixed data (text,
     * pseudo-random bytes, a long run, and a repeat of the random block at a
     * distance) so the frames carry literals, matches and runs. */
    static uint8_t text[1024], mixed[SAMPLE_MAX], run[RUN_LEN];
    static const char phrase[] = "The quick brown fox jumps over the lazy dog. ";
    for (size_t i = 0; i < sizeof(text); i++) text[i] = (uint8_t)phrase[i % (sizeof(phrase) - 1)];
    memcpy(mixed, text, 1024);
    uint32_t rng = 12345;
    for (size_t i = 1024; i < 2048; i++) {
        rng = rng * 1664525u + 1013904223u;
        mixed[i] = (uint8_t)(rng >> 24);
    }
    memset(mixed + 2048, 'x', 1024);
    memcpy(mixed + 3072, mixed + 1024, 1024);
    memset(run, 'a', sizeof(run));

    const struct {
        const char* name;
        const uint8_t* data;
        size_t len;
    } samples[] = {
        {"empty", (const uint8_t*)"", 0},
        {"byte", (const uint8_t*)"a", 1},
        {"text", text, sizeof(text)},
        {"mixed", mixed, sizeof(mixed)},
        {"run", run, sizeof(run)},
    };
    static const int levels[] = {1, 10};
    static const uint8_t junk[] = "trailing junk after the frame";

    for (uint8_t index = 0; index < FUZZ_N_ALGOS; index++) {
        cu_algorithm_t algo = FUZZ_ALGOS[index];
        if (!cu_algorithm_available(algo)) continue;
        const char* algo_name = cu_algorithm_name(algo);
        if (algo == CU_ALGO_LZO) {
            if (lzo_seeds(index, argv[2])) return 1;
            continue;
        }

        cu_decompress_stream_t* probe = NULL;
        int has_single = cu_decompress_stream_create_ex(algo, CU_DSTREAM_SINGLE, &probe) == CU_OK;
        cu_decompress_stream_destroy(probe);

        for (size_t s = 0; s < sizeof(samples) / sizeof(samples[0]); s++) {
            for (size_t l = 0; l < 2; l++) {
                size_t len;
                uint8_t* frame = compress_alloc(algo, samples[s].data, samples[s].len, levels[l], &len);
                if (!frame) {
                    fprintf(stderr, "gen_fuzz_seeds: %s failed to compress %s\n", algo_name, samples[s].name);
                    return 1;
                }
                char name[128];
                snprintf(name, sizeof(name), "%s-%s-l%d", algo_name, samples[s].name, levels[l]);
                int failed = write_seed(name, index, 0x00, frame, len, NULL, 0) ||
                             write_seed(name, index, 0x01, frame, len, NULL, 0) ||
                             write_seed(name, index, 0x20, frame, len, NULL, 0);
                free(frame);
                if (failed) return 1;
            }
        }

        if (!has_single) continue;
        size_t text_len, byte_len;
        uint8_t* text_frame = compress_alloc(algo, text, sizeof(text), 1, &text_len);
        uint8_t* byte_frame = compress_alloc(algo, (const uint8_t*)"a", 1, 1, &byte_len);
        int failed = !text_frame || !byte_frame;
        char name[128];
        for (uint8_t mode = 0; mode < 2 && !failed; mode++) {
            snprintf(name, sizeof(name), "%s-two-frames", algo_name);
            failed |= write_seed(name, index, mode, text_frame, text_len, byte_frame, byte_len);
            snprintf(name, sizeof(name), "%s-frame-junk", algo_name);
            failed |= write_seed(name, index, mode, text_frame, text_len, junk, sizeof(junk) - 1);
        }
        free(text_frame);
        free(byte_frame);
        if (failed) {
            fprintf(stderr, "gen_fuzz_seeds: %s concatenated seeds failed\n", algo_name);
            return 1;
        }
    }

    printf("gen_fuzz_seeds: wrote %d seeds to %s\n", g_written, g_out_dir);
    return 0;
}
