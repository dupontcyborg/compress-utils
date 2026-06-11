/*
 * bench_harness.h — shared C benchmark-driver harness.
 *
 * Both the compress-utils driver (bench.c) and the native-library baseline
 * driver (bench_baseline.c) speak the same benchmark driver protocol (see
 * benchmarks/README.md) and share the same timing, statistics, NDJSON
 * emission, and job loop. The only thing that differs is the set of codecs:
 * each driver supplies an array of bench_codec_t with its own compress /
 * decompress / bound function pointers.
 *
 * A codec carries an `impl` tag ("compress-utils", "libzstd", …) that lands in
 * every result record, so the report tooling can overlay our binding against
 * the native baseline for the same (lang, algo).
 *
 * Modes: each job runs in one-shot or streaming mode. A codec may leave its
 * *_stream pointers NULL (e.g. the native baseline before its streaming
 * support lands); stream-mode jobs for such codecs are skipped, not failed.
 *
 * Header-only: each driver is a single translation unit that includes this and
 * provides main(). Timing wraps only the compress / decompress calls.
 */

#ifndef BENCH_HARNESS_H
#define BENCH_HARNESS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- codec interface ----------------------------------------------------- */

struct bench_codec;

/* compress/decompress return 0 on success (with *out_len set to bytes
 * written) and non-zero on failure, optionally setting *err to a static
 * message. The *_stream variants take a chunk size (the input is fed in
 * chunk-sized pieces, exercising the streaming drain protocol) and may be
 * NULL if the driver doesn't implement streaming for that codec yet.
 * native_id is impl-private scratch (the compress-utils driver stashes its
 * algorithm enum there; the native driver ignores it). */
typedef struct bench_codec {
    const char* name; /* algorithm: "zstd", "brotli", … */
    const char* impl; /* implementation tag for the record */
    int native_id;
    size_t (*bound)(const struct bench_codec*, size_t in_len);
    int (*compress)(const struct bench_codec*, const uint8_t* in, size_t in_len,
                    uint8_t* out, size_t* out_len, int level, const char** err);
    int (*decompress)(const struct bench_codec*, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, const char** err);
    int (*compress_stream)(const struct bench_codec*, const uint8_t* in, size_t in_len,
                           uint8_t* out, size_t* out_len, int level, size_t chunk,
                           const char** err);
    int (*decompress_stream)(const struct bench_codec*, const uint8_t* in, size_t in_len,
                             uint8_t* out, size_t* out_len, size_t chunk, const char** err);
} bench_codec_t;

/* ---- timing -------------------------------------------------------------- */

static uint64_t bench_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int bench_cmp_u64(const void* a, const void* b) {
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

static uint64_t bench_median_sorted(const uint64_t* sorted, size_t n) {
    if (n == 0) return 0;
    if (n % 2) return sorted[n / 2];
    return (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
}

/* Median absolute deviation: median(|x_i - median|). Order-independent. */
static uint64_t bench_mad(const uint64_t* samples, size_t n, uint64_t med) {
    if (n == 0) return 0;
    uint64_t* dev = (uint64_t*)malloc(n * sizeof(uint64_t));
    if (!dev) return 0;
    for (size_t i = 0; i < n; i++) {
        dev[i] = samples[i] > med ? samples[i] - med : med - samples[i];
    }
    qsort(dev, n, sizeof(uint64_t), bench_cmp_u64);
    uint64_t m = bench_median_sorted(dev, n);
    free(dev);
    return m;
}

/* ---- file I/O ------------------------------------------------------------ */

static uint8_t* bench_read_file(const char* path, size_t* out_len) {
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

static void bench_emit_json_string(FILE* out, const char* s) {
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

/* ---- codec lookup -------------------------------------------------------- */

static const bench_codec_t* bench_find(const bench_codec_t* codecs, size_t n,
                                       const char* name) {
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(codecs[i].name, name)) return &codecs[i];
    }
    return NULL;
}

/* ---- one job ------------------------------------------------------------- */

/* Returns 1 = result emitted, 0 = failure, -1 = skipped (stream mode requested
 * but this codec has no streaming functions). */
static int bench_run_job(const char* lang, const bench_codec_t* codec, int level,
                         int is_stream, size_t chunk, const char* path,
                         size_t samples, size_t warmup) {
    if (is_stream && (!codec->compress_stream || !codec->decompress_stream)) {
        return -1;
    }

    size_t in_len = 0;
    uint8_t* in = bench_read_file(path, &in_len);
    if (!in) {
        fprintf(stderr, "bench: cannot read '%s'\n", path);
        return 0;
    }

    size_t bound = codec->bound(codec, in_len);
    uint8_t* comp = (uint8_t*)malloc(bound ? bound : 1);
    uint8_t* dec = (uint8_t*)malloc(in_len ? in_len : 1);
    uint64_t* c_t = (uint64_t*)malloc((samples ? samples : 1) * sizeof(uint64_t));
    uint64_t* d_t = (uint64_t*)malloc((samples ? samples : 1) * sizeof(uint64_t));
    if (!comp || !dec || !c_t || !d_t) {
        fprintf(stderr, "bench: OOM sizing '%s'\n", path);
        free(in); free(comp); free(dec); free(c_t); free(d_t);
        return 0;
    }

    const char* err = NULL;
    size_t comp_len = 0;
    int failed = 0;

    for (size_t i = 0; i < warmup && !failed; i++) {
        comp_len = bound;
        failed = is_stream
            ? codec->compress_stream(codec, in, in_len, comp, &comp_len, level, chunk, &err)
            : codec->compress(codec, in, in_len, comp, &comp_len, level, &err);
    }
    for (size_t i = 0; i < samples && !failed; i++) {
        comp_len = bound;
        uint64_t t0 = bench_now_ns();
        failed = is_stream
            ? codec->compress_stream(codec, in, in_len, comp, &comp_len, level, chunk, &err)
            : codec->compress(codec, in, in_len, comp, &comp_len, level, &err);
        uint64_t t1 = bench_now_ns();
        c_t[i] = t1 - t0;
    }
    if (failed) {
        fprintf(stderr, "bench: compress(%s/%s L%d %s) failed: %s\n",
                codec->name, codec->impl, level, is_stream ? "stream" : "oneshot",
                err ? err : "?");
        free(in); free(comp); free(dec); free(c_t); free(d_t);
        return 0;
    }

    size_t dec_len = 0;
    for (size_t i = 0; i < warmup && !failed; i++) {
        dec_len = in_len;
        failed = is_stream
            ? codec->decompress_stream(codec, comp, comp_len, dec, &dec_len, chunk, &err)
            : codec->decompress(codec, comp, comp_len, dec, &dec_len, &err);
    }
    for (size_t i = 0; i < samples && !failed; i++) {
        dec_len = in_len;
        uint64_t t0 = bench_now_ns();
        failed = is_stream
            ? codec->decompress_stream(codec, comp, comp_len, dec, &dec_len, chunk, &err)
            : codec->decompress(codec, comp, comp_len, dec, &dec_len, &err);
        uint64_t t1 = bench_now_ns();
        d_t[i] = t1 - t0;
    }
    if (failed) {
        fprintf(stderr, "bench: decompress(%s/%s L%d %s) failed: %s\n",
                codec->name, codec->impl, level, is_stream ? "stream" : "oneshot",
                err ? err : "?");
        free(in); free(comp); free(dec); free(c_t); free(d_t);
        return 0;
    }

    int verified = (dec_len == in_len) && (in_len == 0 || memcmp(in, dec, in_len) == 0);

    qsort(c_t, samples, sizeof(uint64_t), bench_cmp_u64);
    qsort(d_t, samples, sizeof(uint64_t), bench_cmp_u64);
    uint64_t c_med = bench_median_sorted(c_t, samples);
    uint64_t d_med = bench_median_sorted(d_t, samples);
    uint64_t c_mad = bench_mad(c_t, samples, c_med);
    uint64_t d_mad = bench_mad(d_t, samples, d_med);

    FILE* o = stdout;
    fputs("{", o);
    fputs("\"lang\":", o); bench_emit_json_string(o, lang); fputs(",", o);
    fputs("\"impl\":", o); bench_emit_json_string(o, codec->impl); fputs(",", o);
    fputs("\"algo\":", o); bench_emit_json_string(o, codec->name); fputs(",", o);
    fprintf(o, "\"level\":%d,", level);
    fputs("\"mode\":", o); bench_emit_json_string(o, is_stream ? "stream" : "oneshot");
    fputs(",", o);
    fprintf(o, "\"chunk_bytes\":%zu,", is_stream ? chunk : (size_t)0);
    fputs("\"input\":", o); bench_emit_json_string(o, path); fputs(",", o);
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

/* ---- driver entry points ------------------------------------------------- */

static size_t bench_env_size(const char* name, size_t fallback) {
    const char* v = getenv(name);
    if (!v || !*v) return fallback;
    long n = strtol(v, NULL, 10);
    return n > 0 ? (size_t)n : fallback;
}

/* Emit a one-line marker so every input job line yields exactly one output
 * line. This keeps the runner's interleaved line-synchronous protocol in
 * step even when a job is skipped (unsupported) or fails. */
static void bench_emit_marker(const char* key) {
    printf("{\"%s\":true}\n", key);
    fflush(stdout);
}

/* Print {"lang","version","driver"} and return 0. `driver` is the runner's
 * registry key for this binary (e.g. "c" or "c-baseline"); the runner uses it
 * to name the results file so distinct drivers don't collide. */
static int bench_info(const char* lang, const char* version, const char* driver) {
    printf("{\"lang\":\"%s\",\"version\":\"%s\",\"driver\":\"%s\"}\n",
           lang, version, driver);
    return 0;
}

/* Read jobs from stdin, run each, emit NDJSON. Returns process exit code.
 *
 * Job line: "<algo> <level> [<mode>] <path>". `mode` is "oneshot" or "stream";
 * it's optional for backward compatibility — a 3-field line is treated as
 * one-shot. `path` may contain spaces. */
static int bench_run(const char* lang, const bench_codec_t* codecs, size_t n_codecs) {
    size_t samples = bench_env_size("BENCH_SAMPLES", 5);
    size_t warmup = bench_env_size("BENCH_WARMUP", 1);
    size_t chunk = bench_env_size("BENCH_CHUNK", 64 * 1024);

    char line[8192];
    int failures = 0;
    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) continue;

        char* algo = line;
        char* sp1 = strchr(line, ' ');
        if (!sp1) { fprintf(stderr, "bench: bad job line: %s\n", line); failures++; continue; }
        *sp1 = '\0';
        char* level_s = sp1 + 1;
        while (*level_s == ' ') level_s++;
        char* sp2 = strchr(level_s, ' ');
        if (!sp2) { fprintf(stderr, "bench: bad job line: %s\n", line); failures++; continue; }
        *sp2 = '\0';
        int level = (int)strtol(level_s, NULL, 10);

        /* Remainder is "[mode ]path". Detect an optional leading mode token. */
        char* rest = sp2 + 1;
        while (*rest == ' ') rest++;
        int is_stream = 0;
        char* path = rest;
        if (!strncmp(rest, "stream ", 7)) {
            is_stream = 1;
            path = rest + 7;
        } else if (!strncmp(rest, "oneshot ", 8)) {
            path = rest + 8;
        }
        while (*path == ' ') path++;

        const bench_codec_t* codec = bench_find(codecs, n_codecs, algo);
        if (!codec) {  /* this driver doesn't provide this algo */
            bench_emit_marker("skipped");
            continue;
        }

        int r = bench_run_job(lang, codec, level, is_stream, chunk, path, samples, warmup);
        if (r == 1) {
            /* result line already emitted by bench_run_job */
        } else if (r == -1) {
            bench_emit_marker("skipped");  /* stream unsupported for this codec */
        } else {
            failures++;
            bench_emit_marker("error");  /* detail already on stderr */
        }
    }
    return failures ? 1 : 0;
}

#endif /* BENCH_HARNESS_H */
