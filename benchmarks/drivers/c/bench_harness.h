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
 * Modes: each job runs in one-shot, streaming or message-set mode (one one-shot
 * call per message of a message set, timed over the whole set). A codec may
 * leave its *_stream pointers NULL; stream-mode jobs for such codecs are
 * skipped (a skip marker keeps the runner's line-synchronous protocol in
 * step), not failed.
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

/* The output buffer of a one-shot decompress. `cap` is the capacity the decoder
 * may use now; `alloc` is the physical size, kept between calls (so malloc is
 * not timed after the first pass) and always >= cap. */
typedef struct { uint8_t* p; size_t cap; size_t alloc; } bench_dbuf_t;

/* compress/decompress return 0 on success (with *out_len set to bytes
 * written) and non-zero on failure, optionally setting *err to a static
 * message. decompress gets *out_len = the capacity of `out` and must return
 * BENCH_NEED_MORE (not failure) when the buffer was too small and nothing else
 * went wrong; bench_decompress_auto then retries with a larger one.
 * decompress_stream is different: `out` is a bounded window (*out_len = its
 * size) that the codec decodes into repeatedly, handing every produced run to
 * bench_sink_write(); on return *out_len = total bytes produced. The *_stream
 * variants take a chunk size (the input is fed in chunk-sized pieces,
 * exercising the streaming drain protocol) and may be NULL if the driver
 * doesn't implement streaming for that codec yet.
 * decompress_grow is optional (the last field), for libraries with a streaming
 * API: a real caller that doesn't know the size loops, growing the buffer and
 * continuing the decode. It decodes into buf->p (buf->cap bytes now); when the
 * library needs more output space it calls bench_dbuf_grow (doubles the
 * capacity, keeps the bytes produced, counts a retry) and continues, with no
 * restart. bench_decompress_auto uses it when there is no size hint, or when a
 * hint turned out too small; codecs without it restart from scratch.
 * size_hint is optional: it returns 0 and the decompressed size when the
 * compressed bytes record it (frame header, …), non-zero otherwise.
 * native_id is impl-private scratch (the compress-utils driver stashes its
 * algorithm enum there; the native driver ignores it). */
#define BENCH_NEED_MORE 2
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
    /* The library's own level range for this format, reported by --info so the
     * runner can sweep it. Both 0 (the default when omitted): the range in
     * benchmarks/levels.json applies. */
    int level_min, level_max;
    int (*size_hint)(const struct bench_codec*, const uint8_t* in, size_t in_len,
                     size_t* out_size);
    int (*decompress_grow)(const struct bench_codec*, const uint8_t* in, size_t in_len,
                           bench_dbuf_t* buf, size_t* out_len, const char** err);
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

/* ---- decompress sizing --------------------------------------------------- */

/* Decompress is never told the original size. The buffer starts at what the
 * codec learns from the compressed bytes (size_hint), else 3 * in_len (at least
 * 1 byte), and doubles when the library needs more room. Whole-buffer-only
 * libraries restart from scratch on a miss; libraries with a streaming API
 * (decompress_grow) continue where they stopped. Either way the cost is
 * intentionally part of the timed call. The physical buffer is the caller's and
 * is kept between calls (grown only when needed), so malloc is not timed after
 * the first pass; growth after that only moves the logical capacity. */

static uint64_t bench_retries; /* times the starting capacity was too small so far (restarts and growth steps) */
static uint64_t bench_misses;  /* decompress calls that had no usable size hint so far */
static uint64_t bench_last_misses; /* misses during the last timed pass */

#define BENCH_HINT_MAX ((size_t)1 << 30)
#define BENCH_WIN_MIN ((size_t)64 * 1024) /* smallest streaming output window */

/* Sets the usable capacity to `cap`, reallocating (bytes already there are kept) only
 * if the physical buffer is smaller. */
static int bench_dbuf_reserve(bench_dbuf_t* buf, size_t cap, const char** err) {
    if (buf->alloc < cap || !buf->p) {
        uint8_t* p = (uint8_t*)realloc(buf->p, cap);
        if (!p) { *err = "out of memory"; return 1; }
        buf->p = p;
        buf->alloc = cap;
    }
    buf->cap = cap;
    return 0;
}

/* Doubles the capacity (the 4 GiB give-up applies) and counts a retry. */
static int bench_dbuf_grow(bench_dbuf_t* buf, const char** err) {
    if (buf->cap > ((size_t)1 << 31)) { *err = "output exceeds 4 GiB"; return 1; }
    bench_retries++;
    return bench_dbuf_reserve(buf, buf->cap * 2, err);
}

/* The starting capacity is this many times the compressed size (default 3); BENCH_START_MULT
 * overrides it to see how much a result depends on the guess. */
static size_t bench_start_mult(void) {
    static size_t m;
    if (!m) {
        const char* v = getenv("BENCH_START_MULT");
        long n = v ? strtol(v, NULL, 10) : 0;
        m = n > 0 ? (size_t)n : 3;
    }
    return m;
}

static int bench_decompress_auto(const bench_codec_t* codec, const uint8_t* in, size_t in_len,
                                 bench_dbuf_t* buf, size_t* out_len, const char** err) {
    size_t cap = 0;
    int hinted = codec->size_hint && !codec->size_hint(codec, in, in_len, &cap) &&
                 cap <= BENCH_HINT_MAX;
    if (!hinted) {
        cap = bench_start_mult() * in_len;
        bench_misses++;
    }
    if (!cap) cap = 1;
    if (bench_dbuf_reserve(buf, cap, err)) return 1;
    if (!hinted && codec->decompress_grow) {
        return codec->decompress_grow(codec, in, in_len, buf, out_len, err);
    }
    for (;;) {
        size_t n = buf->cap;
        int r = codec->decompress(codec, in, in_len, buf->p, &n, err);
        if (!r) { *out_len = n; return 0; }
        if (r != BENCH_NEED_MORE) return r;
        if (bench_dbuf_grow(buf, err)) return 1;
        /* A hint that was too small: carry on with the growing decode. */
        if (codec->decompress_grow) {
            return codec->decompress_grow(codec, in, in_len, buf, out_len, err);
        }
    }
}


/* Streaming decompress hands every produced run to the sink instead of keeping
 * it. A timed pass only counts (it must not touch the bytes); the verification
 * pass compares each run against the original. */
typedef struct { const uint8_t* ref; size_t ref_len, pos; int ok, verify; } bench_sink_t;
static bench_sink_t bench_sink;

static inline void bench_sink_write(const uint8_t* p, size_t n) {
    if (bench_sink.verify) {
        if (n > bench_sink.ref_len - bench_sink.pos || memcmp(p, bench_sink.ref + bench_sink.pos, n)) {
            bench_sink.ok = 0;
        }
    }
    bench_sink.pos += n;
}

/* ---- result record ------------------------------------------------------ */

/* Sorts the sample arrays and prints one NDJSON result. `messages` > 0 marks a
 * message-set record: the times are for the whole set, the byte counts its
 * totals. `retries` is the times the starting capacity was too small (restarts or growth steps) in one timed decompress pass. */
static void bench_emit_result(const char* lang, const bench_codec_t* codec, int level,
                              const char* mode, size_t chunk, const char* path,
                              size_t in_bytes, size_t out_bytes, size_t messages,
                              uint64_t* c_t, uint64_t* d_t, size_t samples, size_t warmup,
                              int verified, uint64_t retries) {
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
    fputs("\"mode\":", o); bench_emit_json_string(o, mode); fputs(",", o);
    fprintf(o, "\"chunk_bytes\":%zu,", chunk);
    fputs("\"input\":", o); bench_emit_json_string(o, path); fputs(",", o);
    fprintf(o, "\"input_bytes\":%zu,", in_bytes);
    fprintf(o, "\"output_bytes\":%zu,", out_bytes);
    if (messages) {
        fprintf(o, "\"messages\":%zu,", messages);
    }
    /* Streaming never uses the hint (no total size up front). */
    /* "codec" only when the hint was usable on every call of the last timed pass: a codec can
     * have a hint function and still get nothing from it (zlib, brotli, a truncated frame). */
    fputs(codec->size_hint && strcmp(mode, "stream") && bench_last_misses == 0
              ? "\"decompress_size_hint\":\"codec\","
              : "\"decompress_size_hint\":\"none\",", o);
    fprintf(o, "\"decompress_retries\":%llu,", (unsigned long long)retries);
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
    /* Streaming decodes into a bounded window; one-shot into dbuf (see
     * bench_decompress_auto). */
    size_t win_len = chunk > BENCH_WIN_MIN ? chunk : BENCH_WIN_MIN;
    uint8_t* win = is_stream ? (uint8_t*)malloc(win_len) : NULL;
    bench_dbuf_t dbuf = {NULL, 0, 0};
    uint64_t* c_t = (uint64_t*)malloc((samples ? samples : 1) * sizeof(uint64_t));
    uint64_t* d_t = (uint64_t*)malloc((samples ? samples : 1) * sizeof(uint64_t));
    if (!comp || (is_stream && !win) || !c_t || !d_t) {
        fprintf(stderr, "bench: OOM sizing '%s'\n", path);
        free(in); free(comp); free(win); free(c_t); free(d_t);
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
        free(in); free(comp); free(win); free(c_t); free(d_t);
        return 0;
    }

    size_t dec_len = 0;
    uint64_t retries = 0;
    bench_sink = (bench_sink_t){in, in_len, 0, 1, 0};
    for (size_t i = 0; i < warmup && !failed; i++) {
        dec_len = win_len;
        bench_sink.pos = 0;
        failed = is_stream
            ? codec->decompress_stream(codec, comp, comp_len, win, &dec_len, chunk, &err)
            : bench_decompress_auto(codec, comp, comp_len, &dbuf, &dec_len, &err);
    }
    for (size_t i = 0; i < samples && !failed; i++) {
        dec_len = win_len;
        bench_sink.pos = 0;
        uint64_t r0 = bench_retries, m0 = bench_misses;
        uint64_t t0 = bench_now_ns();
        failed = is_stream
            ? codec->decompress_stream(codec, comp, comp_len, win, &dec_len, chunk, &err)
            : bench_decompress_auto(codec, comp, comp_len, &dbuf, &dec_len, &err);
        uint64_t t1 = bench_now_ns();
        d_t[i] = t1 - t0;
        retries = bench_retries - r0;
        bench_last_misses = bench_misses - m0;
    }
    int verified = 0;
    if (!failed && is_stream) {
        /* One extra untimed decode, comparing each produced run to the input. */
        dec_len = win_len;
        bench_sink = (bench_sink_t){in, in_len, 0, 1, 1};
        failed = codec->decompress_stream(codec, comp, comp_len, win, &dec_len, chunk, &err);
        verified = !failed && bench_sink.ok && bench_sink.pos == in_len && dec_len == in_len;
    } else if (!failed) {
        verified = (dec_len == in_len) && (in_len == 0 || memcmp(in, dbuf.p, in_len) == 0);
    }
    if (failed) {
        fprintf(stderr, "bench: decompress(%s/%s L%d %s) failed: %s\n",
                codec->name, codec->impl, level, is_stream ? "stream" : "oneshot",
                err ? err : "?");
        free(in); free(comp); free(win); free(dbuf.p); free(c_t); free(d_t);
        return 0;
    }

    bench_emit_result(lang, codec, level, is_stream ? "stream" : "oneshot",
                      is_stream ? chunk : 0, path, in_len, comp_len, 0, c_t, d_t,
                      samples, warmup, verified, retries);

    free(in); free(comp); free(win); free(dbuf.p); free(c_t); free(d_t);
    return 1;
}

/* ---- message sets -------------------------------------------------------- */

/* Reads a message set (benchmarks/corpus/msgset.py): records of a u32
 * little-endian length followed by that many bytes. Returns the file buffer
 * and sets *n, *offs and *lens (n entries each, malloc'd); NULL if the file
 * can't be read or is malformed. */
static uint8_t* bench_read_msgset(const char* path, size_t* n, size_t** offs, size_t** lens) {
    size_t len = 0;
    uint8_t* buf = bench_read_file(path, &len);
    if (!buf) return NULL;
    size_t count = 0;
    for (size_t pos = 0; pos < len; count++) {
        if (len - pos < 4) { free(buf); return NULL; }
        size_t m = (size_t)buf[pos] | (size_t)buf[pos + 1] << 8 |
                   (size_t)buf[pos + 2] << 16 | (size_t)buf[pos + 3] << 24;
        if (m > len - pos - 4) { free(buf); return NULL; }
        pos += 4 + m;
    }
    *offs = (size_t*)malloc((count ? count : 1) * sizeof(size_t));
    *lens = (size_t*)malloc((count ? count : 1) * sizeof(size_t));
    if (!*offs || !*lens) { free(buf); free(*offs); free(*lens); return NULL; }
    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        (*lens)[i] = (size_t)buf[pos] | (size_t)buf[pos + 1] << 8 |
                     (size_t)buf[pos + 2] << 16 | (size_t)buf[pos + 3] << 24;
        (*offs)[i] = pos + 4;
        pos += 4 + (*lens)[i];
    }
    *n = count;
    return buf;
}

/* Times compressing every message of a set with its own one-shot call, then
 * decompressing each (sized by bench_decompress_auto, one physical buffer kept
 * across messages). Returns 1 = result
 * emitted, 0 = failure. Verification runs after timing, untimed. */
static int bench_run_msgset_job(const char* lang, const bench_codec_t* codec, int level,
                                const char* path, size_t samples, size_t warmup) {
    size_t n = 0, *offs = NULL, *lens = NULL;
    uint8_t* buf = bench_read_msgset(path, &n, &offs, &lens);
    if (!buf) {
        fprintf(stderr, "bench: cannot read message set '%s'\n", path);
        return 0;
    }

    /* Each message gets its own bound-sized slot in one compressed arena. */
    size_t in_total = 0, arena = 0;
    size_t* coffs = (size_t*)malloc((n ? n : 1) * sizeof(size_t));
    size_t* caps = (size_t*)malloc((n ? n : 1) * sizeof(size_t));
    size_t* clens = (size_t*)malloc((n ? n : 1) * sizeof(size_t));
    for (size_t i = 0; coffs && caps && i < n; i++) {
        coffs[i] = arena;
        caps[i] = codec->bound(codec, lens[i]);
        arena += caps[i];
        in_total += lens[i];
    }
    uint8_t* comp = (uint8_t*)malloc(arena ? arena : 1);
    bench_dbuf_t dbuf = {NULL, 0, 0};
    uint64_t* c_t = (uint64_t*)malloc((samples ? samples : 1) * sizeof(uint64_t));
    uint64_t* d_t = (uint64_t*)malloc((samples ? samples : 1) * sizeof(uint64_t));
    int ok = coffs && caps && clens && comp && c_t && d_t;
    if (!ok) fprintf(stderr, "bench: OOM sizing '%s'\n", path);

    const char* err = NULL;
    for (size_t r = 0; ok && r < warmup + samples; r++) {
        uint64_t t0 = bench_now_ns();
        for (size_t i = 0; ok && i < n; i++) {
            clens[i] = caps[i];
            ok = !codec->compress(codec, buf + offs[i], lens[i], comp + coffs[i], &clens[i],
                                  level, &err);
        }
        uint64_t t1 = bench_now_ns();
        if (r >= warmup) c_t[r - warmup] = t1 - t0;
    }
    if (!ok && c_t) {
        fprintf(stderr, "bench: compress(%s/%s L%d msgset) failed: %s\n",
                codec->name, codec->impl, level, err ? err : "?");
    }
    int compressed = ok;
    uint64_t retries = 0;
    for (size_t r = 0; ok && r < warmup + samples; r++) {
        uint64_t r0 = bench_retries, m0 = bench_misses;
        uint64_t t0 = bench_now_ns();
        for (size_t i = 0; ok && i < n; i++) {
            size_t dec_len = 0;
            ok = !bench_decompress_auto(codec, comp + coffs[i], clens[i], &dbuf, &dec_len, &err);
        }
        uint64_t t1 = bench_now_ns();
        if (r >= warmup) {
            d_t[r - warmup] = t1 - t0;
            retries = bench_retries - r0;
            bench_last_misses = bench_misses - m0;
        }
    }
    if (!ok && compressed) {
        fprintf(stderr, "bench: decompress(%s/%s L%d msgset) failed: %s\n",
                codec->name, codec->impl, level, err ? err : "?");
    }

    if (ok) {
        int verified = 1;
        size_t out_total = 0;
        for (size_t i = 0; i < n; i++) {
            size_t dec_len = 0;
            verified = verified &&
                       !bench_decompress_auto(codec, comp + coffs[i], clens[i], &dbuf, &dec_len,
                                              &err) &&
                       dec_len == lens[i] && (!dec_len || !memcmp(dbuf.p, buf + offs[i], dec_len));
            out_total += clens[i];
        }
        bench_emit_result(lang, codec, level, "msgset", 0, path, in_total, out_total, n,
                          c_t, d_t, samples, warmup, verified, retries);
    }

    free(buf); free(offs); free(lens); free(coffs); free(caps); free(clens);
    free(comp); free(dbuf.p); free(c_t); free(d_t);
    return ok;
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

/* ---- memory jobs ---------------------------------------------------------- */

#ifdef __linux__
#include <malloc.h>

/* Reads VmRSS and VmHWM (kB) from /proc/self/status. Returns 0 on success. */
static int bench_mem_read(size_t* rss_kb, size_t* hwm_kb) {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return 1;
    char l[256];
    int got = 0;
    while (fgets(l, sizeof(l), f)) {
        if (!strncmp(l, "VmRSS:", 6) && sscanf(l + 6, "%zu", rss_kb) == 1) got |= 1;
        else if (!strncmp(l, "VmHWM:", 6) && sscanf(l + 6, "%zu", hwm_kb) == 1) got |= 2;
    }
    fclose(f);
    return got != 3;
}

/* Resets VmHWM to the current RSS (writing 5 to clear_refs). Returns 0 on success. */
static int bench_mem_reset_hwm(void) {
    FILE* f = fopen("/proc/self/clear_refs", "w");
    if (!f) return 1;
    fputs("5", f);
    return fclose(f) != 0;
}

/* One measured phase: RSS before, reset the high-water mark, run `op`, peak after. Sets
 * *base_kb and *delta_kb (peak - base, at least 0). Returns 0 on success. */
#define BENCH_MEM_PHASE(base_kb, delta_kb, failed, op)                                   \
    do {                                                                                 \
        size_t rss_ = 0, hwm_ = 0, junk_ = 0;                                            \
        if (bench_mem_read(&rss_, &junk_) || bench_mem_reset_hwm()) { (failed) = 2; break; } \
        (failed) = (op);                                                                 \
        if (bench_mem_read(&junk_, &hwm_)) { (failed) = 2; break; }                      \
        (base_kb) = rss_;                                                                \
        (delta_kb) = hwm_ > rss_ ? hwm_ - rss_ : 0;                                      \
    } while (0)

static size_t bench_kb_sub(size_t kb, size_t bytes) {
    size_t b = (bytes + 1023) / 1024;
    return kb > b ? kb - b : 0;
}
#endif

/* Peak memory of one compress and one decompress of a file, in a process that has done
 * nothing else (memory.py starts a fresh driver per job). The compressed and decompressed
 * buffers are malloc'd but never touched before the call, so they cost RSS only as the
 * codec writes them; each phase measures the peak RSS above the RSS before it, and the
 * overhead columns subtract the bytes the call had to write. RSS counts touched pages
 * only. Returns 1 = result emitted, 0 = failure, -1 = skipped (no streaming functions). */
static int bench_run_mem_job(const char* lang, const bench_codec_t* codec, int level,
                             int is_stream, size_t chunk, const char* path) {
#ifndef __linux__
    (void)lang; (void)codec; (void)level; (void)is_stream; (void)chunk; (void)path;
    bench_emit_marker("mem_unsupported");
    return 1;
#else
    if (is_stream && (!codec->compress_stream || !codec->decompress_stream)) return -1;
    size_t in_len = 0;
    uint8_t* in = bench_read_file(path, &in_len);
    if (!in) {
        fprintf(stderr, "bench: cannot read '%s'\n", path);
        return 0;
    }
    size_t bound = codec->bound(codec, in_len);
    size_t win_len = chunk > BENCH_WIN_MIN ? chunk : BENCH_WIN_MIN;
    uint8_t* comp = (uint8_t*)malloc(bound ? bound : 1);
    uint8_t* dec = (uint8_t*)malloc(is_stream ? win_len : (in_len ? in_len : 1));
    if (!comp || !dec) {
        fprintf(stderr, "bench: OOM sizing '%s'\n", path);
        free(in); free(comp); free(dec);
        return 0;
    }

    const char* err = NULL;
    size_t comp_len = bound, dec_len = is_stream ? win_len : in_len;
    size_t base_kb = 0, c_kb = 0, d_kb = 0, junk_kb = 0;
    int failed = 0;
    bench_sink = (bench_sink_t){in, in_len, 0, 1, 0};
#ifdef __GLIBC__
    malloc_trim(0);
#endif
    BENCH_MEM_PHASE(base_kb, c_kb, failed,
                    is_stream
                        ? codec->compress_stream(codec, in, in_len, comp, &comp_len, level, chunk, &err)
                        : codec->compress(codec, in, in_len, comp, &comp_len, level, &err));
#ifdef __GLIBC__
    malloc_trim(0);  /* heap the compressor freed must not hide what the decompressor needs */
#endif
    if (!failed) {
        BENCH_MEM_PHASE(junk_kb, d_kb, failed,
                        is_stream
                            ? codec->decompress_stream(codec, comp, comp_len, dec, &dec_len, chunk, &err)
                            : codec->decompress(codec, comp, comp_len, dec, &dec_len, &err));
    }
    if (failed) {
        fprintf(stderr, "bench: mem(%s/%s L%d %s) failed: %s\n", codec->name, codec->impl, level,
                is_stream ? "stream" : "oneshot",
                failed == 2 ? "cannot read or reset /proc/self (clear_refs)" : err ? err : "?");
        free(in); free(comp); free(dec);
        return 0;
    }

    int verified;
    if (is_stream) {
        dec_len = win_len;
        bench_sink = (bench_sink_t){in, in_len, 0, 1, 1};
        verified = !codec->decompress_stream(codec, comp, comp_len, dec, &dec_len, chunk, &err) &&
                   bench_sink.ok && bench_sink.pos == in_len && dec_len == in_len;
    } else {
        verified = dec_len == in_len && (in_len == 0 || memcmp(in, dec, in_len) == 0);
    }

    /* Overhead = peak above the RSS before the call, minus the output the call wrote: the
     * compressed bytes (both modes); the decompressed bytes (one-shot), or the one window
     * (streaming, which reuses it). */
    FILE* o = stdout;
    fputs("{\"lang\":", o); bench_emit_json_string(o, lang);
    fputs(",\"impl\":", o); bench_emit_json_string(o, codec->impl);
    fputs(",\"algo\":", o); bench_emit_json_string(o, codec->name);
    fprintf(o, ",\"level\":%d,\"mode\":\"%s\",", level, is_stream ? "memstream" : "mem");
    fprintf(o, "\"chunk_bytes\":%zu,", is_stream ? chunk : (size_t)0);
    fputs("\"input\":", o); bench_emit_json_string(o, path);
    fprintf(o, ",\"input_bytes\":%zu,\"output_bytes\":%zu,", in_len, comp_len);
    fprintf(o, "\"mem_compress_kb\":%zu,\"mem_compress_overhead_kb\":%zu,", c_kb,
            bench_kb_sub(c_kb, comp_len));
    fprintf(o, "\"mem_decompress_kb\":%zu,\"mem_decompress_overhead_kb\":%zu,", d_kb,
            bench_kb_sub(d_kb, is_stream ? win_len : in_len));
    fprintf(o, "\"baseline_kb\":%zu,\"verified\":%s}\n", base_kb, verified ? "true" : "false");
    fflush(o);

    free(in); free(comp); free(dec);
    return 1;
#endif
}

/* Print {"lang","version","driver"} and return 0. `driver` is the runner's
 * registry key for this binary (e.g. "c" or "c-baseline"); the runner uses it
 * to name the results file so distinct drivers don't collide. */
static int bench_info(const char* lang, const char* version, const char* driver) {
    printf("{\"lang\":\"%s\",\"version\":\"%s\",\"driver\":\"%s\"}\n",
           lang, version, driver);
    return 0;
}

/* bench_info plus "impl" and "levels": {algo: [min, max]} for the codecs that
 * declare their own level range. */
static int bench_info_codecs(const char* lang, const char* version, const char* driver,
                             const char* impl, const bench_codec_t* codecs, size_t n) {
    printf("{\"lang\":\"%s\",\"version\":\"%s\",\"driver\":\"%s\",\"impl\":\"%s\","
           "\"levels\":{", lang, version, driver, impl);
    int first = 1;
    for (size_t i = 0; i < n; i++) {
        if (!codecs[i].level_min && !codecs[i].level_max) continue;
        printf("%s\"%s\":[%d,%d]", first ? "" : ",", codecs[i].name, codecs[i].level_min,
               codecs[i].level_max);
        first = 0;
    }
    printf("}}\n");
    return 0;
}

/* Read jobs from stdin, run each, emit NDJSON. Returns process exit code.
 *
 * Job line: "<algo> <level> [<mode>] <path>". `mode` is "oneshot", "stream" or
 * "msgset" (path is a message set), or "mem" / "memstream" (one memory measurement, see
 * bench_run_mem_job; memory.py sends one job per fresh process); it's optional for backward compatibility —
 * a 3-field line is treated as one-shot. `path` may contain spaces. */
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
        int is_stream = 0, is_msgset = 0, is_mem = 0;
        char* path = rest;
        if (!strncmp(rest, "memstream ", 10)) {
            is_mem = is_stream = 1;
            path = rest + 10;
        } else if (!strncmp(rest, "mem ", 4)) {
            is_mem = 1;
            path = rest + 4;
        } else if (!strncmp(rest, "stream ", 7)) {
            is_stream = 1;
            path = rest + 7;
        } else if (!strncmp(rest, "msgset ", 7)) {
            is_msgset = 1;
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

        int r = is_mem ? bench_run_mem_job(lang, codec, level, is_stream, chunk, path)
              : is_msgset ? bench_run_msgset_job(lang, codec, level, path, samples, warmup)
                          : bench_run_job(lang, codec, level, is_stream, chunk, path, samples,
                                          warmup);
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
