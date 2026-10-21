/*
 * test_error_paths.c — what a caller sees when something is off, for every
 * codec: undersized output buffers, empty and truncated input, the
 * decompressed-size cap, writes and flushes while a stream is finishing or
 * after it finished, data after the end of a stream, finishing a stream that
 * never got its input, invalid levels and algorithms, and the status and
 * error-string helpers. The round-trip suites cover the happy paths.
 *
 * Exits with 0 on success, nonzero with a message on failure.
 */

#include "compress_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do {                                       \
    if (!(cond)) {                                                  \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
        fprintf(stderr, __VA_ARGS__);                               \
        fprintf(stderr, "  cu_last_error: %s\n", cu_last_error());  \
        return 1;                                                   \
    }                                                               \
} while (0)

#define CHECK_STATUS(call, want) do {                               \
    cu_status_t _s = (call);                                        \
    CHECK(_s == (want), "%s: %s -> %s, want %s\n", name, #call,     \
          cu_strerror(_s), cu_strerror(want));                      \
} while (0)

/* Codecs with framing (a format that marks its own end). */
static const cu_algorithm_t FRAMED[] = {
    CU_ALGO_ZSTD, CU_ALGO_BROTLI, CU_ALGO_ZLIB,
    CU_ALGO_BZ2,  CU_ALGO_LZ4,    CU_ALGO_XZ,
    CU_ALGO_SNAPPY, CU_ALGO_GZIP, CU_ALGO_DEFLATE,
};
#define N_FRAMED (sizeof(FRAMED) / sizeof(FRAMED[0]))

#define DEFAULT_CAP ((size_t)1 << 30)  /* cu_set_max_decompressed_size's default */
#define SAMPLE_LEN (64 * 1024)
static uint8_t g_sample[SAMPLE_LEN];

/* Text with pseudo-random runs: compressible, but not to almost nothing. */
static void make_sample(void) {
    static const char phrase[] = "error paths deserve tests too. ";
    unsigned rng = 7;
    for (size_t i = 0; i < SAMPLE_LEN; i++) {
        rng = rng * 1664525u + 1013904223u;
        g_sample[i] = (i / 512) % 3 == 2 ? (uint8_t)(rng >> 24)
                                         : (uint8_t)phrase[i % (sizeof(phrase) - 1)];
    }
}

/* One-shot compresses the sample into a malloc'd frame. */
static uint8_t* compress_sample(cu_algorithm_t algo, size_t* frame_len) {
    size_t cap = cu_compress_bound(SAMPLE_LEN, algo);
    uint8_t* frame = malloc(cap);
    *frame_len = cap;
    if (frame && cu_compress(algo, g_sample, SAMPLE_LEN, frame, frame_len, 5) != CU_OK) {
        free(frame);
        return NULL;
    }
    return frame;
}

/* Stream-compresses data into a malloc'd frame. Unlike cu_compress, the
 * encoder doesn't know the total size up front, so formats that can record
 * it (zstd, LZ4) leave it out: a different decode path. */
static uint8_t* compress_stream(cu_algorithm_t algo, const uint8_t* data, size_t len,
                                size_t* frame_len) {
    size_t cap = cu_compress_bound(len, algo) + 1024, pos = 0, n;
    uint8_t* frame = malloc(cap);
    cu_compress_stream_t* cs = NULL;
    cu_status_t s = frame ? cu_compress_stream_create(algo, 5, &cs) : CU_ERR_OOM;
    if (s == CU_OK && len > 0) {
        n = cap;
        s = cu_compress_stream_write(cs, data, len, frame, &n);
        pos = n;
    }
    while (s == CU_OK || s == CU_ERR_BUF_TOO_SMALL) {
        n = cap - pos;
        s = cu_compress_stream_finish(cs, frame + pos, &n);
        pos += n;
        if (s == CU_OK) break;
    }
    cu_compress_stream_destroy(cs);
    if (s != CU_OK) {
        free(frame);
        return NULL;
    }
    *frame_len = pos;
    return frame;
}

/* One-shot compress into a 1-byte buffer reports the size it needs. */
static int test_compress_too_small(cu_algorithm_t algo) {
    const char* name = cu_algorithm_name(algo);
    uint8_t out[1];
    size_t out_len = sizeof(out);
    CHECK_STATUS(cu_compress(algo, g_sample, SAMPLE_LEN, out, &out_len, 5), CU_ERR_BUF_TOO_SMALL);
    CHECK(out_len == cu_compress_bound(SAMPLE_LEN, algo),
          "%s: required size %zu, want the bound %zu\n", name, out_len,
          cu_compress_bound(SAMPLE_LEN, algo));
    return 0;
}

/* One-shot decode of empty, truncated, oversized and non-compressed input,
 * and into a buffer that's too small. Takes ownership of frame. */
static int test_decompress_edges(cu_algorithm_t algo, uint8_t* frame, size_t frame_len) {
    const char* name = cu_algorithm_name(algo);
    CHECK(frame, "%s: compress failed\n", name);
    uint8_t* out = malloc(SAMPLE_LEN);
    CHECK(out, "out of memory\n");
    int failed = 1;

    size_t out_len = SAMPLE_LEN;
    size_t hint = 0;
    if (cu_decompress(algo, NULL, 0, out, &out_len) != CU_ERR_TRUNCATED) {
        fprintf(stderr, "FAIL %s: empty input must be CU_ERR_TRUNCATED\n", name);
        goto done;
    }
    if (cu_decompress_size_hint(algo, NULL, 0, &hint) == CU_OK) {
        fprintf(stderr, "FAIL %s: size hint claimed a size for empty input\n", name);
        goto done;
    }
    static const uint8_t not_compressed[16] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    };
    if (cu_decompress_size_hint(algo, not_compressed, sizeof(not_compressed), &hint) == CU_OK) {
        fprintf(stderr, "FAIL %s: size hint claimed a size for non-compressed input\n", name);
        goto done;
    }

    out_len = SAMPLE_LEN;
    cu_status_t s = cu_decompress(algo, frame, frame_len / 2, out, &out_len);
    if (s != CU_ERR_TRUNCATED && s != CU_ERR_DECOMPRESSION) {
        fprintf(stderr, "FAIL %s: half a frame -> %s, want truncated or corrupt\n",
                name, cu_strerror(s));
        goto done;
    }

    /* Too small an output buffer: formats that record their size say how
     * much is needed; the others can't know it. */
    int sized = cu_decompress_size_hint(algo, frame, frame_len, &hint) == CU_OK;
    uint8_t small[16];
    out_len = sizeof(small);
    s = cu_decompress(algo, frame, frame_len, small, &out_len);
    if (sized ? s != CU_ERR_BUF_TOO_SMALL || out_len != SAMPLE_LEN : s != CU_ERR_SIZE_UNKNOWN) {
        fprintf(stderr, "FAIL %s: 16-byte buffer -> %s (out_len %zu), want %s\n", name,
                cu_strerror(s), out_len, sized ? "BUF_TOO_SMALL with the size" : "SIZE_UNKNOWN");
        goto done;
    }

    /* The decompressed-size cap applies whatever the buffer size. */
    cu_set_max_decompressed_size(1024);
    out_len = SAMPLE_LEN;
    s = cu_decompress(algo, frame, frame_len, out, &out_len);
    cu_set_max_decompressed_size(DEFAULT_CAP);
    if (s != CU_ERR_SIZE_LIMIT) {
        fprintf(stderr, "FAIL %s: over the cap -> %s, want SIZE_LIMIT\n", name, cu_strerror(s));
        goto done;
    }

    out_len = SAMPLE_LEN;
    if (cu_decompress(algo, frame, frame_len, out, &out_len) != CU_OK || out_len != SAMPLE_LEN ||
        memcmp(out, g_sample, SAMPLE_LEN) != 0) {
        fprintf(stderr, "FAIL %s: round trip after the error cases\n", name);
        goto done;
    }
    failed = 0;
done:
    free(out);
    free(frame);
    return failed;
}

/* A compress stream that started finishing takes no more input or flushes,
 * and one that finished takes nothing at all. */
static int test_compress_stream_after_finish(cu_algorithm_t algo) {
    const char* name = cu_algorithm_name(algo);
    cu_compress_stream_t* cs = NULL;
    CHECK_STATUS(cu_compress_stream_create(algo, 5, &cs), CU_OK);

    static uint8_t out[256 * 1024];
    size_t out_len = sizeof(out);
    CHECK_STATUS(cu_compress_stream_write(cs, g_sample, SAMPLE_LEN, out, &out_len), CU_OK);

    /* Finish into one byte: it can't all fit, so finishing is in progress. */
    uint8_t one[1];
    size_t one_len = sizeof(one);
    CHECK_STATUS(cu_compress_stream_finish(cs, one, &one_len), CU_ERR_BUF_TOO_SMALL);
    out_len = sizeof(out);
    CHECK_STATUS(cu_compress_stream_write(cs, g_sample, 16, out, &out_len), CU_ERR_STREAM_STATE);
    out_len = sizeof(out);
    cu_status_t f = cu_compress_stream_flush(cs, out, &out_len);
    CHECK(f == CU_ERR_STREAM_STATE || f == CU_ERR_UNSUPPORTED_ALGO,
          "%s: flush while finishing -> %s\n", name, cu_strerror(f));

    cu_status_t s;
    do {
        out_len = sizeof(out);
        s = cu_compress_stream_finish(cs, out, &out_len);
    } while (s == CU_ERR_BUF_TOO_SMALL);
    CHECK(s == CU_OK, "%s: finish drain -> %s\n", name, cu_strerror(s));

    out_len = sizeof(out);
    CHECK_STATUS(cu_compress_stream_write(cs, g_sample, 16, out, &out_len), CU_ERR_STREAM_FINISHED);
    out_len = sizeof(out);
    CHECK_STATUS(cu_compress_stream_flush(cs, out, &out_len), CU_ERR_STREAM_FINISHED);
    cu_compress_stream_destroy(cs);
    return 0;
}

/* A write that returned BUF_TOO_SMALL leaves input pending in the encoder.
 * Calling flush (where supported) or finish next, without draining with empty
 * writes first, must still emit all of it. */
static int test_finish_with_pending(cu_algorithm_t algo, int flush_first) {
    const char* name = cu_algorithm_name(algo);
    /* 1 MiB: more than any encoder takes into its own buffers in one write. */
    enum { BIG = 16 * SAMPLE_LEN };
    static uint8_t big[BIG], out[BIG];
    for (size_t i = 0; i < BIG; i += SAMPLE_LEN) memcpy(big + i, g_sample, SAMPLE_LEN);
    size_t cap = cu_compress_bound(BIG, algo) + 1024, pos = 0, n;
    uint8_t* frame = malloc(cap);
    CHECK(frame, "out of memory\n");
    cu_compress_stream_t* cs = NULL;
    cu_status_t s = cu_compress_stream_create(algo, 5, &cs);
    if (s == CU_OK) {
        n = 64;
        s = cu_compress_stream_write(cs, big, BIG, frame, &n);
        pos = n;
        if (s == CU_ERR_BUF_TOO_SMALL) s = CU_OK;  /* pending; brotli takes it all */
    }
    if (s == CU_OK && flush_first) {
        do {
            n = 64 < cap - pos ? 64 : cap - pos;
            s = cu_compress_stream_flush(cs, frame + pos, &n);
            pos += n;
        } while (s == CU_ERR_BUF_TOO_SMALL);
    }
    while (s == CU_OK || s == CU_ERR_BUF_TOO_SMALL) {
        n = cap - pos;
        s = cu_compress_stream_finish(cs, frame + pos, &n);
        pos += n;
        if (s == CU_OK) break;
    }
    cu_compress_stream_destroy(cs);
    size_t out_len = sizeof(out);
    cu_status_t d = s == CU_OK ? cu_decompress(algo, frame, pos, out, &out_len) : s;
    free(frame);
    CHECK(s == CU_OK, "%s: %s with input pending -> %s\n", name,
          flush_first ? "flush then finish" : "finish", cu_strerror(s));
    CHECK(d == CU_OK && out_len == BIG && memcmp(out, big, BIG) == 0,
          "%s: %s with input pending lost data (%s, %zu bytes)\n", name,
          flush_first ? "flush then finish" : "finish", cu_strerror(d), out_len);
    return 0;
}

/* A compress stream finished without any input makes a valid empty frame. */
static int test_empty_stream(cu_algorithm_t algo) {
    const char* name = cu_algorithm_name(algo);
    size_t frame_len;
    uint8_t* frame = compress_stream(algo, NULL, 0, &frame_len);
    CHECK(frame, "%s: empty stream failed: %s\n", name, cu_last_error());
    uint8_t out[16];
    size_t out_len = sizeof(out);
    cu_status_t s = cu_decompress(algo, frame, frame_len, out, &out_len);
    free(frame);
    CHECK(s == CU_OK && out_len == 0, "%s: empty frame -> %s, %zu bytes\n", name,
          cu_strerror(s), out_len);
    return 0;
}

/* Default-mode decompress streams: data after the end of the stream, a write
 * after finish, and finishing a stream that never got its input. */
static int test_decompress_stream_ends(cu_algorithm_t algo) {
    const char* name = cu_algorithm_name(algo);
    size_t frame_len;
    uint8_t* frame = compress_sample(algo, &frame_len);
    CHECK(frame, "%s: compress failed\n", name);
    static uint8_t out[SAMPLE_LEN * 2];
    static const uint8_t junk[] = "this is not part of any compressed stream";
    int failed = 1;

    /* Snappy has no incremental decoder: it buffers the block and decodes it
     * at finish, so it only sees what follows the block then. */
    const int buffered = algo == CU_ALGO_SNAPPY;
    cu_decompress_stream_t* ds = NULL;
    size_t out_len;
    cu_status_t s;
    if (cu_decompress_stream_create(algo, &ds) != CU_OK) goto done;
    out_len = sizeof(out);
    s = cu_decompress_stream_write(ds, frame, frame_len, out, &out_len);
    if (s != CU_OK || out_len != (buffered ? 0 : SAMPLE_LEN)) {
        fprintf(stderr, "FAIL %s: decode the frame -> %s, %zu bytes\n", name, cu_strerror(s), out_len);
        goto done;
    }
    /* Raw DEFLATE ignores what follows its stream (it usually belongs to an
     * enclosing container); every other format rejects it. */
    cu_status_t want = algo == CU_ALGO_DEFLATE ? CU_OK : CU_ERR_DECOMPRESSION;
    out_len = sizeof(out);
    s = cu_decompress_stream_write(ds, junk, sizeof(junk), out, &out_len);
    if (s != (buffered ? CU_OK : want)) {
        fprintf(stderr, "FAIL %s: data after the end -> %s, want %s\n", name, cu_strerror(s),
                cu_strerror(buffered ? CU_OK : want));
        goto done;
    }
    out_len = sizeof(out);
    s = cu_decompress_stream_finish(ds, out, &out_len);
    if (s != want) {
        fprintf(stderr, "FAIL %s: finish after that -> %s, want %s\n", name, cu_strerror(s),
                cu_strerror(want));
        goto done;
    }
    if (want == CU_OK) {
        out_len = sizeof(out);
        s = cu_decompress_stream_write(ds, frame, 1, out, &out_len);
        if (s != CU_ERR_STREAM_FINISHED) {
            fprintf(stderr, "FAIL %s: write after finish -> %s\n", name, cu_strerror(s));
            goto done;
        }
    }
    cu_decompress_stream_destroy(ds);

    /* A finished stream takes no more input. */
    ds = NULL;
    if (cu_decompress_stream_create(algo, &ds) != CU_OK) goto done;
    out_len = sizeof(out);
    if (cu_decompress_stream_write(ds, frame, frame_len, out, &out_len) != CU_OK) goto done;
    out_len = sizeof(out);
    if (cu_decompress_stream_finish(ds, out, &out_len) != CU_OK ||
        out_len != (buffered ? SAMPLE_LEN : 0)) {
        fprintf(stderr, "FAIL %s: finish after the whole frame\n", name);
        goto done;
    }
    out_len = sizeof(out);
    s = cu_decompress_stream_write(ds, frame, frame_len, out, &out_len);
    if (s != CU_ERR_STREAM_FINISHED) {
        fprintf(stderr, "FAIL %s: write after finish -> %s\n", name, cu_strerror(s));
        goto done;
    }
    cu_decompress_stream_destroy(ds);

    /* Finishing before any input arrived is truncation. */
    ds = NULL;
    if (cu_decompress_stream_create(algo, &ds) != CU_OK) goto done;
    out_len = sizeof(out);
    s = cu_decompress_stream_finish(ds, out, &out_len);
    if (s != CU_ERR_TRUNCATED) {
        fprintf(stderr, "FAIL %s: finish with no input -> %s\n", name, cu_strerror(s));
        goto done;
    }
    failed = 0;
done:
    if (failed && !ds) fprintf(stderr, "FAIL %s: stream setup failed: %s\n", name, cu_last_error());
    cu_decompress_stream_destroy(ds);
    free(frame);
    return failed;
}

/* Raw LZ4 blocks don't record their size: the hint can't give one, and a
 * one-shot compress into too small a buffer reports the bound. */
static int test_raw_lz4(void) {
    const char* name = "lz4_raw";
    if (!cu_algorithm_available(CU_ALGO_LZ4_RAW)) return 0;
    if (test_compress_too_small(CU_ALGO_LZ4_RAW)) return 1;
    size_t frame_len;
    uint8_t* frame = compress_sample(CU_ALGO_LZ4_RAW, &frame_len);
    CHECK(frame, "%s: compress failed\n", name);
    size_t hint = 0;
    cu_status_t s = cu_decompress_size_hint(CU_ALGO_LZ4_RAW, frame, frame_len, &hint);
    free(frame);
    CHECK(s == CU_ERR_SIZE_UNKNOWN, "%s: size hint -> %s\n", name, cu_strerror(s));
    return 0;
}

/* LZO (decode only): a hand-built block of 200 literals, then the end marker.
 * Checks the one-shot size cap and buffer limit, and that a stream rejects
 * data after the end marker, in the same write or a later one. Raw LZ4: a
 * stream won't take new input before the previous write is drained. */
static int test_raw_streams(void) {
    const char* name = "raw";
    static uint8_t block[1 + 200 + 3];
    block[0] = 17 + 200;  /* first byte > 17: a literal run of (byte - 17) */
    for (int i = 0; i < 200; i++) block[1 + i] = (uint8_t)('a' + i % 26);
    block[201] = 17;  /* end marker */
    uint8_t out[4096];
    size_t out_len;

    if (cu_algorithm_available(CU_ALGO_LZO)) {
        name = "lzo";
        out_len = sizeof(out);
        CHECK_STATUS(cu_decompress(CU_ALGO_LZO, block, sizeof(block), out, &out_len), CU_OK);
        CHECK(out_len == 200 && memcmp(out, block + 1, 200) == 0, "lzo: literal block\n");
        out_len = 50;
        CHECK_STATUS(cu_decompress(CU_ALGO_LZO, block, sizeof(block), out, &out_len),
                     CU_ERR_BUF_TOO_SMALL);
        cu_set_max_decompressed_size(100);
        out_len = sizeof(out);
        cu_status_t s = cu_decompress(CU_ALGO_LZO, block, sizeof(block), out, &out_len);
        cu_set_max_decompressed_size(DEFAULT_CAP);
        CHECK(s == CU_ERR_SIZE_LIMIT, "lzo: over the cap -> %s\n", cu_strerror(s));

        uint8_t joined[sizeof(block) + 4];
        memcpy(joined, block, sizeof(block));
        memcpy(joined + sizeof(block), "junk", 4);
        cu_decompress_stream_t* ds = NULL;
        CHECK_STATUS(cu_decompress_stream_create(CU_ALGO_LZO, &ds), CU_OK);
        out_len = sizeof(out);
        CHECK_STATUS(cu_decompress_stream_write(ds, joined, sizeof(joined), out, &out_len),
                     CU_ERR_DECOMPRESSION);
        cu_decompress_stream_destroy(ds);
        CHECK_STATUS(cu_decompress_stream_create(CU_ALGO_LZO, &ds), CU_OK);
        out_len = sizeof(out);
        CHECK_STATUS(cu_decompress_stream_write(ds, block, sizeof(block), out, &out_len), CU_OK);
        CHECK(out_len == 200, "lzo: stream output %zu\n", out_len);
        out_len = sizeof(out);
        CHECK_STATUS(cu_decompress_stream_write(ds, (const uint8_t*)"junk", 4, out, &out_len),
                     CU_ERR_DECOMPRESSION);
        cu_decompress_stream_destroy(ds);
    }

    if (cu_algorithm_available(CU_ALGO_LZ4_RAW)) {
        name = "lz4_raw";
        size_t frame_len;
        uint8_t* frame = compress_sample(CU_ALGO_LZ4_RAW, &frame_len);
        CHECK(frame, "lz4_raw: compress failed\n");
        cu_decompress_stream_t* ds = NULL;
        cu_status_t made = cu_decompress_stream_create(CU_ALGO_LZ4_RAW, &ds);
        uint8_t one[1];
        size_t one_len = sizeof(one);
        cu_status_t first = made == CU_OK
            ? cu_decompress_stream_write(ds, frame, frame_len, one, &one_len) : made;
        out_len = sizeof(out);
        cu_status_t second = first == CU_ERR_BUF_TOO_SMALL
            ? cu_decompress_stream_write(ds, frame, frame_len, out, &out_len) : first;
        cu_decompress_stream_destroy(ds);
        free(frame);
        CHECK(first == CU_ERR_BUF_TOO_SMALL && second == CU_ERR_STREAM_STATE,
              "lz4_raw: write before draining -> %s then %s\n", cu_strerror(first),
              cu_strerror(second));
    }
    return 0;
}

/* Invalid levels and algorithms, and the status and error-string helpers. */
static int test_api_errors(void) {
    const char* name = "api";
    const cu_algorithm_t unknown = (cu_algorithm_t)99, negative = (cu_algorithm_t)-1;
    uint8_t out[64];
    size_t out_len = sizeof(out), hint;

    CHECK(cu_algorithm_name(unknown) == NULL && cu_algorithm_name(negative) == NULL,
          "unknown algorithms have no name\n");
    CHECK(!cu_algorithm_available(unknown) && !cu_algorithm_available(negative),
          "unknown algorithms aren't available\n");
    CHECK(cu_compress_bound(100, unknown) == 0, "no bound for an unknown algorithm\n");
    CHECK_STATUS(cu_compress(unknown, g_sample, 10, out, &out_len, 5), CU_ERR_UNSUPPORTED_ALGO);
    CHECK(strstr(cu_last_error(), "99") != NULL, "the error names the algorithm: %s\n",
          cu_last_error());
    out_len = sizeof(out);
    CHECK_STATUS(cu_decompress(unknown, g_sample, 10, out, &out_len), CU_ERR_UNSUPPORTED_ALGO);
    CHECK_STATUS(cu_decompress_size_hint(negative, g_sample, 10, &hint), CU_ERR_UNSUPPORTED_ALGO);
    cu_compress_stream_t* cs = NULL;
    cu_decompress_stream_t* ds = NULL;
    CHECK_STATUS(cu_compress_stream_create(unknown, 5, &cs), CU_ERR_UNSUPPORTED_ALGO);
    CHECK_STATUS(cu_decompress_stream_create(unknown, &ds), CU_ERR_UNSUPPORTED_ALGO);
    CHECK(!cs && !ds, "no stream on failure\n");

    for (int level = -1; level <= 11; level += 12) {
        out_len = sizeof(out);
        CHECK_STATUS(cu_compress(CU_ALGO_ZSTD, g_sample, 10, out, &out_len, level),
                     CU_ERR_INVALID_LEVEL);
        CHECK_STATUS(cu_compress_stream_create(CU_ALGO_ZSTD, level, &cs), CU_ERR_INVALID_LEVEL);
        CHECK(cs == NULL, "no stream on failure\n");
    }
    CHECK(strstr(cu_last_error(), "level") != NULL, "the error explains: %s\n", cu_last_error());
    cu_clear_last_error();
    CHECK(cu_last_error()[0] == '\0', "cleared: %s\n", cu_last_error());

    /* Every status has its own message; unknown values get a generic one. */
    for (int a = CU_OK; a <= CU_ERR_INTERNAL; a++) {
        const char* msg = cu_strerror((cu_status_t)a);
        CHECK(msg && *msg && strcmp(msg, "unknown error") != 0, "status %d has no message\n", a);
        for (int b = CU_OK; b < a; b++) {
            CHECK(strcmp(msg, cu_strerror((cu_status_t)b)) != 0,
                  "statuses %d and %d share a message\n", a, b);
        }
    }
    CHECK(strcmp(cu_strerror((cu_status_t)999), "unknown error") == 0, "unknown status\n");
    return 0;
}

int main(void) {
    make_sample();
    if (test_api_errors()) return 1;
    for (size_t i = 0; i < N_FRAMED; i++) {
        cu_algorithm_t algo = FRAMED[i];
        if (!cu_algorithm_available(algo)) continue;
        size_t len;
        uint8_t* frame = compress_sample(algo, &len);
        if (test_compress_too_small(algo))           return 1;
        if (test_decompress_edges(algo, frame, len)) return 1;
        frame = compress_stream(algo, g_sample, SAMPLE_LEN, &len);
        if (test_decompress_edges(algo, frame, len)) return 1;
        if (test_empty_stream(algo))                 return 1;
        if (test_finish_with_pending(algo, 0))       return 1;
        if (algo != CU_ALGO_SNAPPY && algo != CU_ALGO_BZ2 &&  /* can't flush */
            test_finish_with_pending(algo, 1))       return 1;
        if (test_compress_stream_after_finish(algo)) return 1;
        if (test_decompress_stream_ends(algo))       return 1;
        printf("%-8s ok\n", cu_algorithm_name(algo));
    }
    if (test_raw_lz4())     return 1;
    if (test_raw_streams()) return 1;
    printf("OK\n");
    return 0;
}
