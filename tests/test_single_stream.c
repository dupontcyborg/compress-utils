/*
 * Single-stream decoding (CU_DSTREAM_SINGLE): the decoder stops at the end of
 * the first frame/member/stream, reports eof once all its output is out, and
 * hands back exactly the input received after the end. Checked for every
 * codec that supports it, with the frame followed by trailing bytes fed in
 * one write, split at and around the boundary, and one byte at a time, through
 * output buffers from 1 byte to 64 KiB. Also: back-to-back frames, zstd/LZ4
 * skippable frames, gzip/xz padding, writes after the end, truncation, the
 * Snappy tag scanner's limits, and the codecs that don't support the mode.
 */
#include "compress_utils.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value)                                                                \
    do {                                                                            \
        if (!(value)) {                                                             \
            fprintf(stderr, "single-stream check failed: %s:%d: %s (%s)\n",        \
                    __FILE__, __LINE__, #value, cu_last_error());                   \
            exit(1);                                                                \
        }                                                                           \
    } while (0)

static int failures = 0;

#define FAIL(...)                                                                   \
    do {                                                                            \
        fprintf(stderr, __VA_ARGS__);                                               \
        fputc('\n', stderr);                                                        \
        failures++;                                                                 \
    } while (0)

/* What follows a zpickle message: the next message's header, then data. */
static const uint8_t TRAILER[] = {'Z', 'P', 'K', 'L', 1, 1, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                  0,   0,   0,   0,   0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

/* Payload contents: 0 pseudorandom, 1 text-like (compressible). */
static void make(int kind, uint8_t* p, size_t n, unsigned seed) {
    unsigned x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        p[i] = kind == 0 ? (uint8_t)(x >> 16) : (uint8_t)("single stream "[i % 14] + (i / 1013) % 5);
    }
}

typedef struct {
    uint8_t* data;
    size_t len;
} buf_t;

static buf_t compress_frame(cu_algorithm_t algo, const uint8_t* plain, size_t n) {
    buf_t b;
    b.len = cu_compress_bound(n, algo);
    b.data = malloc(b.len ? b.len : 1);
    CHECK(b.data);
    CHECK(cu_compress(algo, plain, n, b.data, &b.len, 5) == CU_OK);
    return b;
}

static buf_t concat(const uint8_t* a, size_t a_len, const uint8_t* b, size_t b_len) {
    buf_t r;
    r.len = a_len + b_len;
    r.data = malloc(r.len ? r.len : 1);
    CHECK(r.data);
    if (a_len) memcpy(r.data, a, a_len);
    if (b_len) memcpy(r.data + a_len, b, b_len);
    return r;
}

static cu_decompress_stream_t* open_single(cu_algorithm_t algo) {
    cu_decompress_stream_t* ds = NULL;
    CHECK(cu_decompress_stream_create_ex(algo, CU_DSTREAM_SINGLE, &ds) == CU_OK);
    CHECK(ds && !cu_decompress_stream_eof(ds));
    return ds;
}

/* Result of feeding a single-mode decoder until eof (or the input ran out). */
typedef struct {
    uint8_t* out;
    size_t out_len;
    size_t fed;        /* input bytes passed to write before eof */
    int eof;
    cu_status_t error; /* first status other than OK/BUF_TOO_SMALL */
} decoded_t;

/* Writes one piece, draining through `cap`-byte buffers until CU_OK. */
static cu_status_t write_piece(cu_decompress_stream_t* ds, const uint8_t* p, size_t len, size_t cap,
                               decoded_t* r, size_t max) {
    static uint8_t buf[65536];
    cu_status_t st;
    do {
        size_t got = cap;
        st = cu_decompress_stream_write(ds, p, len, buf, &got);
        if (st != CU_OK && st != CU_ERR_BUF_TOO_SMALL) return st;
        CHECK(got <= cap && r->out_len + got <= max);
        memcpy(r->out + r->out_len, buf, got);
        r->out_len += got;
        /* While output is still draining, eof stays 0. */
        if (st == CU_ERR_BUF_TOO_SMALL) CHECK(!cu_decompress_stream_eof(ds));
        p = NULL;
        len = 0;
    } while (st == CU_ERR_BUF_TOO_SMALL);
    return CU_OK;
}

/* Feeds `in` in pieces: `step` bytes at a time (0 = one piece), or split in
 * two at `split` when step is 0 and split < n. Stops feeding at eof, as a
 * caller reading a file or socket would. */
static decoded_t feed(cu_decompress_stream_t* ds, const uint8_t* in, size_t n, size_t step,
                      size_t split, size_t cap, size_t max) {
    decoded_t r;
    memset(&r, 0, sizeof(r));
    r.out = malloc(max ? max : 1);
    CHECK(r.out);
    size_t off = 0;
    while (off < n && !cu_decompress_stream_eof(ds)) {
        size_t len = n - off;
        if (step) len = step < len ? step : len;
        else if (split < n && off < split) len = split - off;
        cu_status_t st = write_piece(ds, in + off, len, cap, &r, max);
        if (st != CU_OK) {
            r.error = st;
            return r;
        }
        off += len;
        r.fed = off;
    }
    r.eof = cu_decompress_stream_eof(ds);
    return r;
}

/* The decoder stopped exactly at `frame_len`: output is `plain`, eof is set,
 * and unused is exactly the fed input past the end. */
static int check_end(cu_decompress_stream_t* ds, const decoded_t* r, const uint8_t* in,
                     size_t frame_len, const uint8_t* plain, size_t plain_len) {
    const uint8_t* unused = NULL;
    size_t unused_len = 0;
    CHECK(cu_decompress_stream_unused(ds, &unused, &unused_len) == CU_OK);
    if (r->error != CU_OK) return 0;
    if (!r->eof || r->out_len != plain_len || (plain_len && memcmp(r->out, plain, plain_len)))
        return 0;
    if (r->fed < frame_len || unused_len != r->fed - frame_len) return 0;
    return unused_len == 0 || !memcmp(unused, in + frame_len, unused_len);
}

/* After eof: data is rejected without being consumed, while empty writes,
 * finish, and unused keep working. */
static void check_after_eof(cu_decompress_stream_t* ds) {
    const uint8_t* before = NULL;
    size_t before_len = 0;
    CHECK(cu_decompress_stream_unused(ds, &before, &before_len) == CU_OK);
    uint8_t* saved = malloc(before_len ? before_len : 1);
    CHECK(saved);
    if (before_len) memcpy(saved, before, before_len);

    uint8_t out[16];
    size_t got = sizeof(out);
    CHECK(cu_decompress_stream_write(ds, TRAILER, 4, out, &got) == CU_ERR_STREAM_FINISHED);
    got = sizeof(out);
    CHECK(cu_decompress_stream_write(ds, NULL, 0, out, &got) == CU_OK && got == 0);
    got = sizeof(out);
    CHECK(cu_decompress_stream_finish(ds, out, &got) == CU_OK && got == 0);
    got = sizeof(out);
    CHECK(cu_decompress_stream_finish(ds, out, &got) == CU_OK && got == 0);
    got = sizeof(out);
    CHECK(cu_decompress_stream_write(ds, TRAILER, 4, out, &got) == CU_ERR_STREAM_FINISHED);
    CHECK(cu_decompress_stream_eof(ds));

    const uint8_t* after = NULL;
    size_t after_len = 0;
    CHECK(cu_decompress_stream_unused(ds, &after, &after_len) == CU_OK);
    CHECK(after_len == before_len && (before_len == 0 || !memcmp(after, saved, before_len)));
    free(saved);
}

/* One frame followed by TRAILER, fed whole, split at/around the boundary, and
 * byte by byte (small payloads), through each output buffer size. */
static void check_trailing(cu_algorithm_t algo, const uint8_t* plain, size_t plain_len) {
    buf_t frame = compress_frame(algo, plain, plain_len);
    buf_t in = concat(frame.data, frame.len, TRAILER, sizeof(TRAILER));
    const size_t caps[] = {1, 7, 4096, 65536};
    for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++) {
        size_t cap = caps[c];
        /* Tiny output buffers on the large payload only repeat the smaller
         * payloads' cases at many times the cost. */
        if (cap < 4096 && plain_len > 100000) continue;
        struct {
            const char* name;
            size_t step, split;
        } modes[] = {
            {"whole", 0, SIZE_MAX},
            {"split at end", 0, frame.len},
            {"split end-1", 0, frame.len - 1},
            {"split end+1", 0, frame.len + 1},
            {"byte by byte", 1, SIZE_MAX},
        };
        for (size_t m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
            if (modes[m].step == 1 && plain_len > 4096) continue;
            cu_decompress_stream_t* ds = open_single(algo);
            decoded_t r = feed(ds, in.data, in.len, modes[m].step, modes[m].split, cap,
                               plain_len + 64);
            if (!check_end(ds, &r, in.data, frame.len, plain, plain_len)) {
                FAIL("%s: %zu-byte payload + trailer, %s, out %zu: eof %d, %zu/%zu bytes out, "
                     "fed %zu of frame %zu, status %d", cu_algorithm_name(algo), plain_len,
                     modes[m].name, cap, r.eof, r.out_len, plain_len, r.fed, frame.len,
                     (int)r.error);
            } else {
                check_after_eof(ds);
            }
            free(r.out);
            cu_decompress_stream_destroy(ds);
        }
    }

    /* Draining with finish instead of empty writes, from 1-byte buffers: it
     * reaches eof without reporting truncation. */
    if (plain_len <= 100000) {
        cu_decompress_stream_t* ds = open_single(algo);
        static uint8_t buf[64];
        uint8_t* out = malloc(plain_len + 64);
        CHECK(out);
        size_t out_len = 0, got = 1;
        cu_status_t st = cu_decompress_stream_write(ds, in.data, in.len, buf, &got);
        CHECK(st == CU_OK || st == CU_ERR_BUF_TOO_SMALL);
        memcpy(out + out_len, buf, got);
        out_len += got;
        while (st == CU_ERR_BUF_TOO_SMALL) {
            got = 1;
            st = cu_decompress_stream_finish(ds, buf, &got);
            CHECK(st == CU_OK || st == CU_ERR_BUF_TOO_SMALL);
            CHECK(out_len + got <= plain_len + 64);
            memcpy(out + out_len, buf, got);
            out_len += got;
        }
        const uint8_t* unused = NULL;
        size_t unused_len = 0;
        CHECK(cu_decompress_stream_unused(ds, &unused, &unused_len) == CU_OK);
        if (!cu_decompress_stream_eof(ds) || out_len != plain_len ||
            (plain_len && memcmp(out, plain, plain_len)) || unused_len != sizeof(TRAILER) ||
            memcmp(unused, TRAILER, sizeof(TRAILER))) {
            FAIL("%s: %zu-byte payload, finish-drain: eof %d, %zu/%zu bytes, unused %zu",
                 cu_algorithm_name(algo), plain_len, cu_decompress_stream_eof(ds), out_len,
                 plain_len, unused_len);
        }
        free(out);
        cu_decompress_stream_destroy(ds);
    }

    /* Truncated: the frame minus its last byte never reaches eof, and finish
     * reports truncation. */
    if (frame.len > 1) {
        cu_decompress_stream_t* ds = open_single(algo);
        decoded_t r = feed(ds, frame.data, frame.len - 1, 0, SIZE_MAX, 65536, plain_len + 64);
        uint8_t out[64];
        size_t got = sizeof(out);
        cu_status_t st = r.error != CU_OK ? r.error : cu_decompress_stream_finish(ds, out, &got);
        const uint8_t* unused = NULL;
        size_t unused_len = 1;
        CHECK(cu_decompress_stream_unused(ds, &unused, &unused_len) == CU_OK);
        if (r.eof || cu_decompress_stream_eof(ds) || unused_len != 0 ||
            (st != CU_ERR_TRUNCATED && st != CU_ERR_DECOMPRESSION)) {
            FAIL("%s: %zu-byte payload truncated by one byte: eof %d, status %d",
                 cu_algorithm_name(algo), plain_len, cu_decompress_stream_eof(ds), (int)st);
        }
        free(r.out);
        cu_decompress_stream_destroy(ds);
    }
    free(in.data);
    free(frame.data);
}

/* Two frames back to back: decoding stops after the first, whose unused is the
 * whole second frame, which then decodes on its own. */
static void check_two_frames(cu_algorithm_t algo) {
    static uint8_t p1[5000], p2[3000];
    make(1, p1, sizeof(p1), 11);
    make(0, p2, sizeof(p2), 12);
    buf_t f1 = compress_frame(algo, p1, sizeof(p1)), f2 = compress_frame(algo, p2, sizeof(p2));
    buf_t in = concat(f1.data, f1.len, f2.data, f2.len);

    cu_decompress_stream_t* ds = open_single(algo);
    decoded_t r = feed(ds, in.data, in.len, 0, SIZE_MAX, 4096, sizeof(p1) + 64);
    if (!check_end(ds, &r, in.data, f1.len, p1, sizeof(p1))) {
        FAIL("%s: two frames: first didn't end cleanly (eof %d, %zu bytes out, status %d)",
             cu_algorithm_name(algo), r.eof, r.out_len, (int)r.error);
    } else {
        const uint8_t* unused = NULL;
        size_t unused_len = 0;
        CHECK(cu_decompress_stream_unused(ds, &unused, &unused_len) == CU_OK);
        CHECK(unused_len == f2.len);
        cu_decompress_stream_t* ds2 = open_single(algo);
        decoded_t r2 = feed(ds2, unused, unused_len, 0, SIZE_MAX, 4096, sizeof(p2) + 64);
        if (!check_end(ds2, &r2, unused, f2.len, p2, sizeof(p2)))
            FAIL("%s: two frames: second didn't decode from unused", cu_algorithm_name(algo));
        free(r2.out);
        cu_decompress_stream_destroy(ds2);
    }
    free(r.out);
    cu_decompress_stream_destroy(ds);

    /* The default mode still decodes both as one stream. */
    if (algo == CU_ALGO_ZSTD || algo == CU_ALGO_LZ4 || algo == CU_ALGO_GZIP || algo == CU_ALGO_XZ) {
        cu_decompress_stream_t* dd = NULL;
        CHECK(cu_decompress_stream_create(algo, &dd) == CU_OK);
        decoded_t d = feed(dd, in.data, in.len, 0, SIZE_MAX, 4096, sizeof(p1) + sizeof(p2) + 64);
        static uint8_t tail[64];
        size_t got = sizeof(tail);
        CHECK(d.error == CU_OK && !d.eof);
        CHECK(cu_decompress_stream_finish(dd, tail, &got) == CU_OK && got == 0);
        CHECK(d.out_len == sizeof(p1) + sizeof(p2) && !memcmp(d.out, p1, sizeof(p1)) &&
              !memcmp(d.out + sizeof(p1), p2, sizeof(p2)));
        free(d.out);
        cu_decompress_stream_destroy(dd);
    }
    free(in.data);
    free(f1.data);
    free(f2.data);
}

/* zstd/LZ4 skippable frame: magic 0x184D2A5?, LE32 size, contents. */
static size_t skippable(uint8_t* p, uint8_t nibble, size_t size) {
    uint32_t magic = 0x184D2A50u | nibble;
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(magic >> (8 * i));
    for (int i = 0; i < 4; i++) p[4 + i] = (uint8_t)(size >> (8 * i));
    memset(p + 8, 0x5A, size);
    return 8 + size;
}

/* Skippable frames before the data frame are skipped; those after it are
 * unused. A stream of skippable frames alone is truncated. */
static void check_skippable(cu_algorithm_t algo) {
    static uint8_t plain[2000], in[8192];
    make(1, plain, sizeof(plain), 21);
    buf_t frame = compress_frame(algo, plain, sizeof(plain));
    size_t n = skippable(in, 0, 13);
    n += skippable(in + n, 0xF, 0);
    size_t start = n;  /* the skippable frames alone */
    memcpy(in + n, frame.data, frame.len);
    n += frame.len;
    size_t end = n;
    n += skippable(in + n, 3, 7);
    memcpy(in + n, TRAILER, sizeof(TRAILER));
    n += sizeof(TRAILER);

    const size_t steps[] = {0, 1, 5};
    for (size_t s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
        cu_decompress_stream_t* ds = open_single(algo);
        decoded_t r = feed(ds, in, n, steps[s], SIZE_MAX, 7, sizeof(plain) + 64);
        if (!check_end(ds, &r, in, end, plain, sizeof(plain)))
            FAIL("%s: skippable frames around the data frame (step %zu): eof %d, %zu bytes, "
                 "fed %zu, end %zu, status %d", cu_algorithm_name(algo), steps[s], r.eof,
                 r.out_len, r.fed, end, (int)r.error);
        free(r.out);
        cu_decompress_stream_destroy(ds);
    }

    /* Only skippable frames: no data frame, so finish reports truncation. */
    {
        cu_decompress_stream_t* ds = open_single(algo);
        decoded_t r = feed(ds, in, start, 0, SIZE_MAX, 64, 64);
        uint8_t out[64];
        size_t got = sizeof(out);
        CHECK(r.error == CU_OK && !r.eof && r.out_len == 0);
        CHECK(cu_decompress_stream_finish(ds, out, &got) == CU_ERR_TRUNCATED);
        CHECK(!cu_decompress_stream_eof(ds));
        free(r.out);
        cu_decompress_stream_destroy(ds);
    }
    free(frame.data);
}

/* gzip zero padding and xz stream padding after the end are unused. */
static void check_padding(cu_algorithm_t algo) {
    static uint8_t plain[3000];
    make(1, plain, sizeof(plain), 31);
    buf_t frame = compress_frame(algo, plain, sizeof(plain));
    static const uint8_t pad[8] = {0};
    buf_t padded = concat(frame.data, frame.len, pad, sizeof(pad));
    buf_t in = concat(padded.data, padded.len, TRAILER, sizeof(TRAILER));
    cu_decompress_stream_t* ds = open_single(algo);
    decoded_t r = feed(ds, in.data, in.len, 0, SIZE_MAX, 4096, sizeof(plain) + 64);
    if (!check_end(ds, &r, in.data, frame.len, plain, sizeof(plain)))
        FAIL("%s: padding after the end isn't unused", cu_algorithm_name(algo));
    free(r.out);
    cu_decompress_stream_destroy(ds);
    free(in.data);
    free(padded.data);
    free(frame.data);
}

/* Snappy's tag scan: malformed lengths fail without waiting for more input,
 * the size cap is captured at creation, and an empty block ends right after
 * its length. */
static void check_snappy_scan(void) {
    struct {
        const char* name;
        uint8_t bytes[16];
        size_t len;
    } bad[] = {
        {"overlong varint", {0x80, 0x80, 0x80, 0x80, 0x80, 0x01}, 6},
        {"length over 32 bits", {0xFF, 0xFF, 0xFF, 0xFF, 0x7F}, 5},
        /* declared 10, then a literal of 20 (tag (20-1)<<2) whose bytes never arrive */
        {"oversized literal", {10, 19 << 2}, 2},
        /* declared 10, literal of 60 via a 1-byte extra length */
        {"oversized long literal", {10, 60 << 2, 59}, 3},
        /* declared 5, literal 'a', then a copy (2-byte offset) of 10 */
        {"copy past declared length", {5, 0 << 2, 'a', (9 << 2) | 2, 1, 0}, 6},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        cu_decompress_stream_t* ds = open_single(CU_ALGO_SNAPPY);
        uint8_t out[64];
        size_t got = sizeof(out);
        cu_status_t st = cu_decompress_stream_write(ds, bad[i].bytes, bad[i].len, out, &got);
        if (st != CU_ERR_DECOMPRESSION || cu_decompress_stream_eof(ds))
            FAIL("snappy scan: %s: status %d", bad[i].name, (int)st);
        cu_decompress_stream_destroy(ds);
    }

    /* A declared length over the cap fails before the block arrives. */
    {
        cu_set_max_decompressed_size(1000);
        cu_decompress_stream_t* ds = open_single(CU_ALGO_SNAPPY);
        cu_set_max_decompressed_size((size_t)1 << 30);
        const uint8_t header[] = {0xD0, 0x0F};  /* 2000 */
        uint8_t out[8];
        size_t got = sizeof(out);
        CHECK(cu_decompress_stream_write(ds, header, sizeof(header), out, &got) == CU_ERR_SIZE_LIMIT);
        cu_decompress_stream_destroy(ds);
    }
    /* The cap is the one at creation: lowering it later doesn't affect the
     * stream. */
    {
        static uint8_t plain[3000];
        make(1, plain, sizeof(plain), 41);
        buf_t frame = compress_frame(CU_ALGO_SNAPPY, plain, sizeof(plain));
        cu_decompress_stream_t* ds = open_single(CU_ALGO_SNAPPY);
        cu_set_max_decompressed_size(10);
        decoded_t r = feed(ds, frame.data, frame.len, 0, SIZE_MAX, 4096, sizeof(plain) + 64);
        cu_set_max_decompressed_size((size_t)1 << 30);
        CHECK(check_end(ds, &r, frame.data, frame.len, plain, sizeof(plain)));
        free(r.out);
        free(frame.data);
        cu_decompress_stream_destroy(ds);
    }
    /* Empty block: length 0, then the next message. */
    {
        buf_t in = concat((const uint8_t*)"\0", 1, TRAILER, sizeof(TRAILER));
        cu_decompress_stream_t* ds = open_single(CU_ALGO_SNAPPY);
        decoded_t r = feed(ds, in.data, in.len, 0, SIZE_MAX, 64, 64);
        CHECK(check_end(ds, &r, in.data, 1, NULL, 0));
        free(r.out);
        free(in.data);
        cu_decompress_stream_destroy(ds);
    }
    /* While the block's output is still draining, data is rejected without
     * being consumed. */
    {
        static uint8_t plain[300];
        make(1, plain, sizeof(plain), 43);
        buf_t frame = compress_frame(CU_ALGO_SNAPPY, plain, sizeof(plain));
        buf_t in = concat(frame.data, frame.len, TRAILER, sizeof(TRAILER));
        cu_decompress_stream_t* ds = open_single(CU_ALGO_SNAPPY);
        uint8_t out[16];
        size_t got = 16;
        CHECK(cu_decompress_stream_write(ds, in.data, in.len, out, &got) == CU_ERR_BUF_TOO_SMALL);
        CHECK(got == 16 && !memcmp(out, plain, 16) && !cu_decompress_stream_eof(ds));
        got = 16;
        CHECK(cu_decompress_stream_write(ds, TRAILER, 4, out, &got) == CU_ERR_STREAM_STATE);
        const uint8_t* unused = NULL;
        size_t unused_len = 1;
        CHECK(cu_decompress_stream_unused(ds, &unused, &unused_len) == CU_OK && unused_len == 0);
        size_t total = 16;
        cu_status_t st;
        do {
            got = 16;
            st = cu_decompress_stream_write(ds, NULL, 0, out, &got);
            CHECK(st == CU_OK || st == CU_ERR_BUF_TOO_SMALL);
            CHECK(!memcmp(out, plain + total, got));
            total += got;
        } while (st == CU_ERR_BUF_TOO_SMALL);
        CHECK(total == sizeof(plain) && cu_decompress_stream_eof(ds));
        CHECK(cu_decompress_stream_unused(ds, &unused, &unused_len) == CU_OK);
        CHECK(unused_len == sizeof(TRAILER) && !memcmp(unused, TRAILER, sizeof(TRAILER)));
        free(in.data);
        free(frame.data);
        cu_decompress_stream_destroy(ds);
    }
}

/* A decoding error is final, in single and default mode (flags): later writes
 * and finish repeat it without re-entering the codec, valid input can't revive
 * the stream, and it never reports eof. */
static void check_failed(cu_algorithm_t algo, uint32_t flags) {
    static uint8_t plain[2000];
    make(1, plain, sizeof(plain), 61);
    buf_t frame = compress_frame(algo, plain, sizeof(plain));
    /* Garbage where the frame's header should be. */
    buf_t bad = concat(frame.data, frame.len, NULL, 0);
    for (size_t i = 0; i < bad.len && i < 16; i++) bad.data[i] ^= 0xA5;

    cu_decompress_stream_t* ds = NULL;
    CHECK(cu_decompress_stream_create_ex(algo, flags, &ds) == CU_OK && ds);
    static uint8_t out[4096];
    size_t got = sizeof(out);
    cu_status_t first = cu_decompress_stream_write(ds, bad.data, bad.len, out, &got);
    if (first == CU_OK || first == CU_ERR_BUF_TOO_SMALL) {
        /* Not rejected at write (e.g. a Snappy length that still parses):
         * finish is where it fails. */
        got = sizeof(out);
        first = cu_decompress_stream_finish(ds, out, &got);
    }
    if (first == CU_OK || first == CU_ERR_BUF_TOO_SMALL) {
        FAIL("%s (flags %u): corrupted frame decoded without error", cu_algorithm_name(algo),
             (unsigned)flags);
    } else {
        got = sizeof(out);
        cu_status_t again = cu_decompress_stream_write(ds, frame.data, frame.len, out, &got);
        size_t got2 = sizeof(out);
        cu_status_t drain = cu_decompress_stream_write(ds, NULL, 0, out, &got2);
        size_t got3 = sizeof(out);
        cu_status_t fin = cu_decompress_stream_finish(ds, out, &got3);
        const uint8_t* unused = NULL;
        size_t unused_len = 1;
        CHECK(cu_decompress_stream_unused(ds, &unused, &unused_len) == CU_OK);
        if (again != first || drain != first || fin != first || cu_decompress_stream_eof(ds) ||
            unused_len != 0) {
            FAIL("%s (flags %u): after error %d: write %d, drain %d, finish %d, eof %d, unused %zu",
                 cu_algorithm_name(algo), (unsigned)flags, (int)first, (int)again, (int)drain,
                 (int)fin, cu_decompress_stream_eof(ds), unused_len);
        }
    }
    cu_decompress_stream_destroy(ds);

    /* finish before the end is final too: the rest of the frame can't follow. */
    CHECK(cu_decompress_stream_create_ex(algo, flags, &ds) == CU_OK && ds);
    decoded_t r = feed(ds, frame.data, frame.len / 2, 0, SIZE_MAX, 4096, sizeof(plain) + 64);
    got = sizeof(out);
    CHECK(r.error == CU_OK);
    cu_status_t cut = cu_decompress_stream_finish(ds, out, &got);
    /* Default-mode Snappy decodes its one block at finish, so a cut block
     * reads as corrupt there; single mode always reports truncation. */
    CHECK(flags ? cut == CU_ERR_TRUNCATED : cut != CU_OK && cut != CU_ERR_BUF_TOO_SMALL);
    got = sizeof(out);
    CHECK(cu_decompress_stream_write(ds, frame.data + frame.len / 2, frame.len - frame.len / 2,
                                     out, &got) == cut);
    CHECK(!cu_decompress_stream_eof(ds));
    free(r.out);
    cu_decompress_stream_destroy(ds);
    free(bad.data);
    free(frame.data);
}

/* Flags and the codecs without single mode. */
static void check_api(void) {
    cu_decompress_stream_t* ds = NULL;
    CHECK(cu_decompress_stream_create_ex(CU_ALGO_ZLIB, 0x2, &ds) == CU_ERR_INVALID_ARG && !ds);
    CHECK(cu_decompress_stream_create_ex(CU_ALGO_ZLIB, CU_DSTREAM_SINGLE | 0x80, &ds) ==
          CU_ERR_INVALID_ARG);
    CHECK(cu_decompress_stream_create_ex(CU_ALGO_ZLIB, CU_DSTREAM_SINGLE, NULL) == CU_ERR_INVALID_ARG);
    const cu_algorithm_t unsupported[] = {CU_ALGO_LZ4_RAW, CU_ALGO_LZO};
    for (size_t i = 0; i < 2; i++) {
        if (!cu_algorithm_available(unsupported[i])) continue;
        CHECK(cu_decompress_stream_create_ex(unsupported[i], CU_DSTREAM_SINGLE, &ds) ==
              CU_ERR_UNSUPPORTED_ALGO && !ds);
        CHECK(cu_decompress_stream_create_ex(unsupported[i], 0, &ds) == CU_OK);
        cu_decompress_stream_destroy(ds);
    }

    /* Default-mode streams never report eof or unused input. */
    CHECK(cu_decompress_stream_create_ex(CU_ALGO_ZLIB, 0, &ds) == CU_OK);
    static uint8_t plain[100];
    make(1, plain, sizeof(plain), 51);
    buf_t frame = compress_frame(CU_ALGO_ZLIB, plain, sizeof(plain));
    decoded_t r = feed(ds, frame.data, frame.len, 0, SIZE_MAX, 4096, sizeof(plain));
    CHECK(r.error == CU_OK && !r.eof && r.out_len == sizeof(plain));
    const uint8_t* unused = (const uint8_t*)"x";
    size_t unused_len = 1;
    CHECK(cu_decompress_stream_unused(ds, &unused, &unused_len) == CU_OK);
    CHECK(unused == NULL && unused_len == 0);
    CHECK(cu_decompress_stream_unused(ds, NULL, &unused_len) == CU_ERR_INVALID_ARG);
    CHECK(cu_decompress_stream_unused(NULL, &unused, &unused_len) == CU_ERR_INVALID_ARG);
    CHECK(cu_decompress_stream_eof(NULL) == 0);
    free(r.out);
    free(frame.data);
    cu_decompress_stream_destroy(ds);
}

int main(void) {
    const cu_algorithm_t algos[] = {CU_ALGO_ZSTD, CU_ALGO_LZ4,    CU_ALGO_ZLIB,
                                    CU_ALGO_GZIP, CU_ALGO_DEFLATE, CU_ALGO_BROTLI,
                                    CU_ALGO_BZ2,  CU_ALGO_XZ,     CU_ALGO_SNAPPY};
    const size_t sizes[] = {0, 1, 1000, 100000, 1536 * 1024};
    check_api();
    for (size_t a = 0; a < sizeof(algos) / sizeof(algos[0]); a++) {
        cu_algorithm_t algo = algos[a];
        if (!cu_algorithm_available(algo)) continue;
        int before = failures;
        for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
            uint8_t* plain = malloc(sizes[s] ? sizes[s] : 1);
            CHECK(plain);
            make(s % 2, plain, sizes[s], (unsigned)s + 1);
            check_trailing(algo, plain, sizes[s]);
            free(plain);
        }
        check_two_frames(algo);
        check_failed(algo, CU_DSTREAM_SINGLE);
        check_failed(algo, 0);
        if (algo == CU_ALGO_ZSTD || algo == CU_ALGO_LZ4) check_skippable(algo);
        if (algo == CU_ALGO_GZIP || algo == CU_ALGO_XZ) check_padding(algo);
        if (algo == CU_ALGO_SNAPPY) check_snappy_scan();
        printf("%-8s %s\n", cu_algorithm_name(algo), failures == before ? "ok" : "FAILED");
    }
    if (failures) {
        fprintf(stderr, "%d single-stream check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
