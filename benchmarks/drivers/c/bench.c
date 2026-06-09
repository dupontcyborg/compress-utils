/*
 * C benchmark driver for compress-utils.
 *
 * Implements the language-agnostic benchmark driver protocol (see
 * benchmarks/README.md). Every driver — C, Python, WASM, future Rust/Go —
 * speaks the same protocol so the runner and report tooling stay uniform:
 *
 *   stdin   one job per line:  "<algo> <level> <abs_input_path>"
 *   stdout  one NDJSON result object per job line, in input order
 *   env     BENCH_SAMPLES (default 5), BENCH_WARMUP (default 1)
 *
 * Invoke with a single "--info" argument to print driver metadata
 * ({"lang","version"}) and exit; the runner calls this once per run.
 *
 * Timing: CLOCK_MONOTONIC around the one-shot cu_compress / cu_decompress
 * calls only. Buffer allocation and file I/O are excluded. We report the
 * median, MAD (median absolute deviation), and min in nanoseconds over the
 * sampled iterations; throughput is derived downstream from uncompressed
 * bytes ÷ median time. Every job round-trips and byte-compares once so a
 * silently-wrong codec can't post a fast time.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "compress_utils.h"

/* ---- timing -------------------------------------------------------------- */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

/* Median of a sorted array. */
static uint64_t median_sorted(const uint64_t* sorted, size_t n) {
    if (n == 0) return 0;
    if (n % 2) return sorted[n / 2];
    return (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
}

/* Median absolute deviation: median(|x_i - median(x)|). */
static uint64_t mad(const uint64_t* samples, size_t n, uint64_t med) {
    if (n == 0) return 0;
    uint64_t* dev = (uint64_t*)malloc(n * sizeof(uint64_t));
    if (!dev) return 0;
    for (size_t i = 0; i < n; i++) {
        dev[i] = samples[i] > med ? samples[i] - med : med - samples[i];
    }
    qsort(dev, n, sizeof(uint64_t), cmp_u64);
    uint64_t m = median_sorted(dev, n);
    free(dev);
    return m;
}

/* ---- algorithm mapping --------------------------------------------------- */

static int algo_from_name(const char* name, cu_algorithm_t* out) {
    if (!strcmp(name, "zstd"))   { *out = CU_ALGO_ZSTD;   return 1; }
    if (!strcmp(name, "brotli")) { *out = CU_ALGO_BROTLI; return 1; }
    if (!strcmp(name, "zlib"))   { *out = CU_ALGO_ZLIB;   return 1; }
    if (!strcmp(name, "bz2"))    { *out = CU_ALGO_BZ2;    return 1; }
    if (!strcmp(name, "lz4"))    { *out = CU_ALGO_LZ4;    return 1; }
    if (!strcmp(name, "xz"))     { *out = CU_ALGO_XZ;     return 1; }
    if (!strcmp(name, "lzma"))   { *out = CU_ALGO_LZMA;   return 1; }
    return 0;
}

/* ---- file I/O ------------------------------------------------------------ */

static uint8_t* read_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    *out_len = (size_t)sz;
    return buf;
}

/* JSON-escape a path for safe NDJSON emission. */
static void emit_json_string(FILE* out, const char* s) {
    fputc('"', out);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', out); fputc(c, out); }
        else if (c == '\n') { fputs("\\n", out); }
        else if (c == '\t') { fputs("\\t", out); }
        else if (c < 0x20) { fprintf(out, "\\u%04x", c); }
        else { fputc(c, out); }
    }
    fputc('"', out);
}

/* ---- one job ------------------------------------------------------------- */

static int run_job(const char* algo_name, int level, const char* path,
                   size_t samples, size_t warmup) {
    cu_algorithm_t algo;
    if (!algo_from_name(algo_name, &algo)) {
        fprintf(stderr, "bench: unknown algorithm '%s'\n", algo_name);
        return 0;
    }
    if (!cu_algorithm_available(algo)) {
        fprintf(stderr, "bench: algorithm '%s' not compiled in\n", algo_name);
        return 0;
    }

    size_t in_len = 0;
    uint8_t* in = read_file(path, &in_len);
    if (!in) {
        fprintf(stderr, "bench: cannot read '%s'\n", path);
        return 0;
    }

    size_t bound = cu_compress_bound(in_len, algo);
    uint8_t* comp = (uint8_t*)malloc(bound ? bound : 1);
    uint8_t* dec = (uint8_t*)malloc(in_len ? in_len : 1);
    uint64_t* c_t = (uint64_t*)malloc((samples ? samples : 1) * sizeof(uint64_t));
    uint64_t* d_t = (uint64_t*)malloc((samples ? samples : 1) * sizeof(uint64_t));
    if (!comp || !dec || !c_t || !d_t) {
        fprintf(stderr, "bench: OOM sizing '%s'\n", path);
        free(in); free(comp); free(dec); free(c_t); free(d_t);
        return 0;
    }

    size_t comp_len = 0;
    cu_status_t st = CU_OK;

    /* Compress: warmup then sample. */
    for (size_t i = 0; i < warmup; i++) {
        comp_len = bound;
        st = cu_compress(algo, in, in_len, comp, &comp_len, level);
        if (st != CU_OK) break;
    }
    for (size_t i = 0; st == CU_OK && i < samples; i++) {
        comp_len = bound;
        uint64_t t0 = now_ns();
        st = cu_compress(algo, in, in_len, comp, &comp_len, level);
        uint64_t t1 = now_ns();
        c_t[i] = t1 - t0;
    }
    if (st != CU_OK) {
        fprintf(stderr, "bench: compress(%s L%d) failed: %s\n",
                algo_name, level, cu_last_error());
        free(in); free(comp); free(dec); free(c_t); free(d_t);
        return 0;
    }

    /* Decompress: warmup then sample. */
    size_t dec_len = 0;
    for (size_t i = 0; i < warmup; i++) {
        dec_len = in_len;
        st = cu_decompress(algo, comp, comp_len, dec, &dec_len);
        if (st != CU_OK) break;
    }
    for (size_t i = 0; st == CU_OK && i < samples; i++) {
        dec_len = in_len;
        uint64_t t0 = now_ns();
        st = cu_decompress(algo, comp, comp_len, dec, &dec_len);
        uint64_t t1 = now_ns();
        d_t[i] = t1 - t0;
    }
    if (st != CU_OK) {
        fprintf(stderr, "bench: decompress(%s L%d) failed: %s\n",
                algo_name, level, cu_last_error());
        free(in); free(comp); free(dec); free(c_t); free(d_t);
        return 0;
    }

    int verified = (dec_len == in_len) && (in_len == 0 || memcmp(in, dec, in_len) == 0);

    qsort(c_t, samples, sizeof(uint64_t), cmp_u64);
    qsort(d_t, samples, sizeof(uint64_t), cmp_u64);
    uint64_t c_med = median_sorted(c_t, samples);
    uint64_t d_med = median_sorted(d_t, samples);

    /* Recompute MAD against the unsorted-equivalent median (sorted copy is
     * fine — MAD is order-independent). */
    uint64_t c_mad = mad(c_t, samples, c_med);
    uint64_t d_mad = mad(d_t, samples, d_med);

    FILE* o = stdout;
    fputs("{", o);
    fputs("\"lang\":\"c\",", o);
    fputs("\"algo\":", o); emit_json_string(o, algo_name); fputs(",", o);
    fprintf(o, "\"level\":%d,", level);
    fputs("\"input\":", o); emit_json_string(o, path); fputs(",", o);
    fprintf(o, "\"input_bytes\":%zu,", in_len);
    fprintf(o, "\"output_bytes\":%zu,", comp_len);
    fprintf(o, "\"compress_ns_median\":%llu,", (unsigned long long)c_med);
    fprintf(o, "\"compress_ns_mad\":%llu,", (unsigned long long)c_mad);
    fprintf(o, "\"compress_ns_min\":%llu,", (unsigned long long)c_t[0]);
    fprintf(o, "\"decompress_ns_median\":%llu,", (unsigned long long)d_med);
    fprintf(o, "\"decompress_ns_mad\":%llu,", (unsigned long long)d_mad);
    fprintf(o, "\"decompress_ns_min\":%llu,", (unsigned long long)d_t[0]);
    fprintf(o, "\"samples\":%zu,", samples);
    fprintf(o, "\"warmup\":%zu,", warmup);
    fprintf(o, "\"verified\":%s", verified ? "true" : "false");
    fputs("}\n", o);
    fflush(o);

    free(in); free(comp); free(dec); free(c_t); free(d_t);
    return 1;
}

/* ---- driver loop --------------------------------------------------------- */

static size_t env_size(const char* name, size_t fallback) {
    const char* v = getenv(name);
    if (!v || !*v) return fallback;
    long n = strtol(v, NULL, 10);
    return n > 0 ? (size_t)n : fallback;
}

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--info")) {
        printf("{\"lang\":\"c\",\"version\":\"%s\"}\n", cu_version());
        return 0;
    }

    size_t samples = env_size("BENCH_SAMPLES", 5);
    size_t warmup = env_size("BENCH_WARMUP", 1);

    char line[8192];
    int failures = 0;
    while (fgets(line, sizeof(line), stdin)) {
        /* Strip trailing newline. */
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        /* Parse "<algo> <level> <path>". Path may contain spaces, so split
         * on the first two whitespace runs only. */
        char* algo = line;
        char* sp1 = strchr(line, ' ');
        if (!sp1) { fprintf(stderr, "bench: bad job line: %s\n", line); failures++; continue; }
        *sp1 = '\0';
        char* level_s = sp1 + 1;
        while (*level_s == ' ') level_s++;
        char* sp2 = strchr(level_s, ' ');
        if (!sp2) { fprintf(stderr, "bench: bad job line: %s\n", line); failures++; continue; }
        *sp2 = '\0';
        char* path = sp2 + 1;
        while (*path == ' ') path++;

        int level = (int)strtol(level_s, NULL, 10);
        if (!run_job(algo, level, path, samples, warmup)) failures++;
    }

    return failures ? 1 : 0;
}
