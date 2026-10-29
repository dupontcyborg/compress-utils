/*
 * compress-utils C benchmark driver.
 *
 * Thin adapter: wraps the public cu_* one-shot ABI as bench_codec_t entries
 * and hands them to the shared harness (bench_harness.h), which owns timing,
 * statistics, round-trip verification, and NDJSON emission. The algorithm enum
 * rides in each codec's `native_id`.
 *
 * Protocol, env, and --info are documented in benchmarks/README.md.
 */

#include "compress_utils.h"

#include "bench_harness.h"

static size_t cu_bound(const bench_codec_t* c, size_t in_len) {
    return cu_compress_bound(in_len, (cu_algorithm_t)c->native_id);
}

static int cu_do_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                          uint8_t* out, size_t* out_len, int level, const char** err) {
    cu_status_t s = cu_compress((cu_algorithm_t)c->native_id, in, in_len, out, out_len, level);
    if (s != CU_OK) { *err = cu_last_error(); return (int)s; }
    return 0;
}

static int cu_do_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                            uint8_t* out, size_t* out_len, const char** err) {
    cu_status_t s = cu_decompress((cu_algorithm_t)c->native_id, in, in_len, out, out_len);
    if (s == CU_ERR_BUF_TOO_SMALL || s == CU_ERR_SIZE_UNKNOWN) return BENCH_NEED_MORE;
    if (s != CU_OK) { *err = cu_last_error(); return 1; }
    return 0;
}

/* Safety bound on drain iterations — a correct codec drains in a few passes
 * into a bound-sized buffer; this only fires if the C side gets stuck neither
 * emitting nor finishing (mirrors the JS dispatcher's DRAIN_MAX_ITERATIONS). */
#define CU_DRAIN_MAX (1 << 20)

/* Streaming compress: feed input in `chunk`-sized pieces through the cu_*
 * stream ABI, honoring the "fill buffer, BUF_TOO_SMALL, drain with (NULL,0)"
 * protocol. `out` is bound-sized so it always has room. */
static int cu_do_compress_stream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                                 uint8_t* out, size_t* out_len, int level, size_t chunk,
                                 const char** err) {
    if (chunk == 0) chunk = 64 * 1024;
    cu_compress_stream_t* s = NULL;
    cu_status_t st = cu_compress_stream_create((cu_algorithm_t)c->native_id, level, &s);
    if (st != CU_OK) { *err = cu_last_error(); return (int)st; }

    size_t cap = *out_len, pos = 0;
    for (size_t off = 0; off < in_len; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        size_t ol = cap - pos;
        st = cu_compress_stream_write(s, in + off, n, out + pos, &ol);
        pos += ol;
        for (int guard = 0; st == CU_ERR_BUF_TOO_SMALL && guard < CU_DRAIN_MAX; guard++) {
            ol = cap - pos;
            st = cu_compress_stream_write(s, NULL, 0, out + pos, &ol);
            pos += ol;
        }
        if (st != CU_OK) { *err = cu_last_error(); cu_compress_stream_destroy(s); return (int)st; }
    }
    for (int guard = 0; guard < CU_DRAIN_MAX; guard++) {
        size_t ol = cap - pos;
        st = cu_compress_stream_finish(s, out + pos, &ol);
        pos += ol;
        if (st == CU_OK) break;
        if (st != CU_ERR_BUF_TOO_SMALL) {
            *err = cu_last_error();
            cu_compress_stream_destroy(s);
            return (int)st;
        }
    }
    if (st != CU_OK) {
        *err = "finish still returned BUF_TOO_SMALL after CU_DRAIN_MAX calls";
        cu_compress_stream_destroy(s);
        return (int)st;
    }
    cu_compress_stream_destroy(s);
    *out_len = pos;
    return 0;
}

/* Streaming decompress: feed the compressed buffer in `chunk`-sized pieces,
 * decoding into the bounded window `out` (*out_len = its size) and handing each
 * produced run to the sink. */
static int cu_do_decompress_stream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                                   uint8_t* out, size_t* out_len, size_t chunk,
                                   const char** err) {
    if (chunk == 0) chunk = 64 * 1024;
    cu_decompress_stream_t* s = NULL;
    cu_status_t st = cu_decompress_stream_create((cu_algorithm_t)c->native_id, &s);
    if (st != CU_OK) { *err = cu_last_error(); return (int)st; }

    size_t win = *out_len, total = 0;
    for (size_t off = 0; off < in_len; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        size_t ol = win;
        st = cu_decompress_stream_write(s, in + off, n, out, &ol);
        bench_sink_write(out, ol);
        total += ol;
        for (int guard = 0; st == CU_ERR_BUF_TOO_SMALL && guard < CU_DRAIN_MAX; guard++) {
            ol = win;
            st = cu_decompress_stream_write(s, NULL, 0, out, &ol);
            bench_sink_write(out, ol);
            total += ol;
        }
        if (st != CU_OK) { *err = cu_last_error(); cu_decompress_stream_destroy(s); return (int)st; }
    }
    for (int guard = 0; guard < CU_DRAIN_MAX; guard++) {
        size_t ol = win;
        st = cu_decompress_stream_finish(s, out, &ol);
        bench_sink_write(out, ol);
        total += ol;
        if (st == CU_OK) break;
        if (st != CU_ERR_BUF_TOO_SMALL) {
            *err = cu_last_error();
            cu_decompress_stream_destroy(s);
            return (int)st;
        }
    }
    if (st != CU_OK) {
        *err = "finish still returned BUF_TOO_SMALL after CU_DRAIN_MAX calls";
        cu_decompress_stream_destroy(s);
        return (int)st;
    }
    cu_decompress_stream_destroy(s);
    *out_len = total;
    return 0;
}

/* The size the compressed bytes record, as the library reports it (the harness
 * calls this, not cu_do_decompress). */
static int cu_do_size_hint(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                           size_t* out_size) {
    return cu_decompress_size_hint((cu_algorithm_t)c->native_id, in, in_len, out_size) != CU_OK;
}

/* Unknown-size decompress as a streaming caller does it: the stream API into a buffer that
 * grows and keeps decoding, the input fed in 256 KiB chunks, honouring the BUF_TOO_SMALL drain
 * protocol. This is a stand-in for the core helper cu_decompress_alloc: replace it with a call
 * to cu_decompress_alloc once that lands in the main tree. */
#define CU_ALLOC_CHUNK ((size_t)256 * 1024)

/* One write/finish call into the buffer's free space, draining with (NULL, 0) and growing the
 * buffer whenever it reports BUF_TOO_SMALL, until it returns something else. */
static cu_status_t cu_alloc_standin_step(cu_decompress_stream_t* s, const uint8_t* in, size_t n,
                                         int finish, bench_dbuf_t* buf, size_t* pos,
                                         const char** err) {
    cu_status_t st = CU_OK;
    for (int guard = 0; guard < CU_DRAIN_MAX; guard++) {
        size_t ol = buf->cap - *pos;
        st = finish ? cu_decompress_stream_finish(s, buf->p + *pos, &ol)
                    : cu_decompress_stream_write(s, in, n, buf->p + *pos, &ol);
        *pos += ol;
        if (st != CU_ERR_BUF_TOO_SMALL) return st;
        if ((*pos == buf->cap || ol == 0) && bench_dbuf_grow(buf, err)) return CU_ERR_BUF_TOO_SMALL;
        in = NULL;
        n = 0;
    }
    *err = "BUF_TOO_SMALL after CU_DRAIN_MAX calls";
    return CU_ERR_BUF_TOO_SMALL;
}

static int cu_do_decompress_alloc_standin(const bench_codec_t* c, const uint8_t* in,
                                          size_t in_len, bench_dbuf_t* buf, size_t* out_len,
                                          const char** err) {
    cu_decompress_stream_t* s = NULL;
    cu_status_t st = cu_decompress_stream_create((cu_algorithm_t)c->native_id, &s);
    if (st != CU_OK) { *err = cu_last_error(); return (int)st; }
    size_t pos = 0;
    const char* gerr = NULL;
    for (size_t off = 0; off < in_len && st == CU_OK; off += CU_ALLOC_CHUNK) {
        size_t n = in_len - off < CU_ALLOC_CHUNK ? in_len - off : CU_ALLOC_CHUNK;
        st = cu_alloc_standin_step(s, in + off, n, 0, buf, &pos, &gerr);
    }
    if (st == CU_OK) st = cu_alloc_standin_step(s, NULL, 0, 1, buf, &pos, &gerr);
    cu_decompress_stream_destroy(s);
    if (st != CU_OK) { *err = gerr ? gerr : cu_last_error(); return (int)st; }
    *out_len = pos;
    return 0;
}

#define CU_CODEC(NAME, ENUM)                                              \
    { NAME, "compress-utils", (ENUM), cu_bound, cu_do_compress,           \
      cu_do_decompress, cu_do_compress_stream, cu_do_decompress_stream, 0, 0, cu_do_size_hint,    \
      cu_do_decompress_alloc_standin }

static const bench_codec_t CODECS[] = {
    CU_CODEC("zstd", CU_ALGO_ZSTD),
    CU_CODEC("brotli", CU_ALGO_BROTLI),
    CU_CODEC("zlib", CU_ALGO_ZLIB),
    CU_CODEC("bz2", CU_ALGO_BZ2),
    CU_CODEC("lz4", CU_ALGO_LZ4),
    CU_CODEC("xz", CU_ALGO_XZ),
    CU_CODEC("snappy", CU_ALGO_SNAPPY),
    CU_CODEC("gzip", CU_ALGO_GZIP),
    CU_CODEC("deflate", CU_ALGO_DEFLATE),
};
static const size_t N_CODECS = sizeof(CODECS) / sizeof(CODECS[0]);

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--info")) {
        return bench_info("c", cu_version(), "c");
    }
    return bench_run("c", CODECS, N_CODECS);
}
