/*
 * bench_zapi.h — zlib, gzip and raw DEFLATE codecs over any library with
 * zlib's API, for the C drivers.
 *
 * madler zlib, zlib-ng's native API (zng_ names), zlib-rs and miniz (mz_
 * names) all speak the zlib API with different prefixes and length types.
 * Define these before including:
 *
 *   ZPFX      the function prefix (empty for zlib), e.g. zng_
 *   ZSTREAM   the stream type, e.g. z_stream
 *   ZLEN      compress2/uncompress's length type, e.g. uLongf
 *
 * zlib uses the documented one-shot compress2/uncompress. gzip (windowBits
 * 31) and raw DEFLATE (-15) use deflateInit2/inflateInit2 with zlib's
 * defaults (memLevel 8, default strategy) and a single Z_FINISH call, the same
 * settings src/algorithms/zlib/deflate_backend.h uses.
 *
 * Streaming (all three formats) feeds the input in chunk-sized pieces with
 * Z_NO_FLUSH, then finishes with Z_FINISH, into one bound-sized output buffer,
 * as c-baseline streams madler zlib. Streaming decompress drains a bounded
 * window instead, into the harness's sink.
 *
 * Decompress returns BENCH_NEED_MORE when the output buffer is too small.
 * zlib, gzip and raw deflate are all unknown-size: gzip's ISIZE is only the last member's size mod 2^32, a guess rather than a recorded size, so it is not used.
 */

#ifndef BENCH_ZAPI_H
#define BENCH_ZAPI_H

#include "bench_harness.h"

#define ZAPI_CAT2(a, b) a##b
#define ZAPI_CAT(a, b) ZAPI_CAT2(a, b)
#define ZAPI(fn) ZAPI_CAT(ZPFX, fn)

/* compressBound covers the zlib wrapper; the gzip one is 12 bytes longer. */
static size_t za_bound(const bench_codec_t* c, size_t n) {
    (void)c;
    return (size_t)ZAPI(compressBound)((ZLEN)n) + 32;
}

static int za_zlib_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                            uint8_t* out, size_t* out_len, int level, const char** err) {
    (void)c;
    ZLEN dl = (ZLEN)*out_len;
    if (ZAPI(compress2)(out, &dl, in, (ZLEN)in_len, level) != Z_OK) { *err = "compress2"; return 1; }
    *out_len = (size_t)dl;
    return 0;
}

static int za_zlib_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                              uint8_t* out, size_t* out_len, const char** err) {
    (void)c;
    ZLEN dl = (ZLEN)*out_len;
    int r = ZAPI(uncompress)(out, &dl, in, (ZLEN)in_len);
    if (r == Z_BUF_ERROR) return BENCH_NEED_MORE;
    if (r != Z_OK) { *err = "uncompress"; return 1; }
    *out_len = (size_t)dl;
    return 0;
}

static int za_deflate(const uint8_t* in, size_t in_len, uint8_t* out, size_t* out_len,
                      int level, int wbits, const char** err) {
    ZSTREAM s;
    memset(&s, 0, sizeof(s));
    if (ZAPI(deflateInit2)(&s, level, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        *err = "deflateInit2";
        return 1;
    }
    s.next_in = (void*)in;
    s.avail_in = (unsigned)in_len;
    s.next_out = out;
    s.avail_out = (unsigned)*out_len;
    int r = ZAPI(deflate)(&s, Z_FINISH);
    *out_len = (size_t)s.total_out;
    ZAPI(deflateEnd)(&s);
    if (r != Z_STREAM_END) { *err = "deflate"; return 1; }
    return 0;
}

static int za_inflate(const uint8_t* in, size_t in_len, uint8_t* out, size_t* out_len,
                      int wbits, const char** err) {
    ZSTREAM s;
    memset(&s, 0, sizeof(s));
    if (ZAPI(inflateInit2)(&s, wbits) != Z_OK) { *err = "inflateInit2"; return 1; }
    s.next_in = (void*)in;
    s.avail_in = (unsigned)in_len;
    s.next_out = out;
    s.avail_out = (unsigned)*out_len;
    int r = ZAPI(inflate)(&s, Z_FINISH);
    *out_len = (size_t)s.total_out;
    int full = s.avail_out == 0;
    ZAPI(inflateEnd)(&s);
    if (r != Z_STREAM_END) {
        if (full) return BENCH_NEED_MORE;  /* no room left (Z_BUF_ERROR or Z_OK, by library) */
        *err = "inflate";
        return 1;
    }
    return 0;
}

static int za_cstream(const uint8_t* in, size_t in_len, uint8_t* out, size_t* out_len,
                      int level, int wbits, size_t chunk, const char** err) {
    if (chunk == 0) chunk = 64 * 1024;
    ZSTREAM s;
    memset(&s, 0, sizeof(s));
    if (ZAPI(deflateInit2)(&s, level, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        *err = "deflateInit2";
        return 1;
    }
    size_t cap = *out_len;
    s.next_out = out;
    s.avail_out = (unsigned)cap;
    int rc = 0;
    for (size_t off = 0; off < in_len && !rc; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        s.next_in = (void*)(uintptr_t)(in + off);
        s.avail_in = (unsigned)n;
        while (s.avail_in > 0) {
            if (ZAPI(deflate)(&s, Z_NO_FLUSH) != Z_OK) { *err = "deflate"; rc = 1; break; }
        }
    }
    if (!rc) {
        int r;
        do { r = ZAPI(deflate)(&s, Z_FINISH); } while (r == Z_OK);
        if (r != Z_STREAM_END) { *err = "deflate finish"; rc = 1; }
    }
    if (!rc) *out_len = cap - s.avail_out;
    ZAPI(deflateEnd)(&s);
    return rc;
}

/* Decodes into the bounded window `out` (*out_len = its size), handing each
 * produced run to the sink. Z_BUF_ERROR with nothing left to read just means
 * the chunk is used up. */
static int za_dstream(const uint8_t* in, size_t in_len, uint8_t* out, size_t* out_len,
                      int wbits, size_t chunk, const char** err) {
    if (chunk == 0) chunk = 64 * 1024;
    ZSTREAM s;
    memset(&s, 0, sizeof(s));
    if (ZAPI(inflateInit2)(&s, wbits) != Z_OK) { *err = "inflateInit2"; return 1; }
    size_t win = *out_len, total = 0;
    int rc = 0, done = 0;
    for (size_t off = 0; off < in_len && !rc && !done; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        s.next_in = (void*)(uintptr_t)(in + off);
        s.avail_in = (unsigned)n;
        do {
            s.next_out = out;
            s.avail_out = (unsigned)win;
            int r = ZAPI(inflate)(&s, Z_NO_FLUSH);
            size_t got = win - s.avail_out;
            bench_sink_write(out, got);
            total += got;
            if (r == Z_STREAM_END) { done = 1; break; }
            if (r != Z_OK && !(r == Z_BUF_ERROR && s.avail_in == 0)) {
                *err = "inflate";
                rc = 1;
                break;
            }
        } while (s.avail_in > 0 || s.avail_out == 0);
    }
    if (!rc && !done) { *err = "inflate: truncated"; rc = 1; }
    if (!rc) *out_len = total;
    ZAPI(inflateEnd)(&s);
    return rc;
}

/* One-shot through deflateInit2/inflateInit2 (gzip, raw deflate). */
#define ZA_ONESHOT(NAME, WBITS)                                                               \
    static int za_##NAME##_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len, \
                                    uint8_t* out, size_t* out_len, int level,                 \
                                    const char** err) {                                       \
        (void)c;                                                                              \
        return za_deflate(in, in_len, out, out_len, level, WBITS, err);                       \
    }                                                                                         \
    static int za_##NAME##_decompress(const bench_codec_t* c, const uint8_t* in,              \
                                      size_t in_len, uint8_t* out, size_t* out_len,           \
                                      const char** err) {                                     \
        (void)c;                                                                              \
        return za_inflate(in, in_len, out, out_len, WBITS, err);                              \
    }
#define ZA_STREAMS(NAME, WBITS)                                                               \
    static int za_##NAME##_cstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,  \
                                   uint8_t* out, size_t* out_len, int level, size_t chunk,    \
                                   const char** err) {                                        \
        (void)c;                                                                              \
        return za_cstream(in, in_len, out, out_len, level, WBITS, chunk, err);                \
    }                                                                                         \
    static int za_##NAME##_dstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,  \
                                   uint8_t* out, size_t* out_len, size_t chunk,               \
                                   const char** err) {                                        \
        (void)c;                                                                              \
        return za_dstream(in, in_len, out, out_len, WBITS, chunk, err);                       \
    }
ZA_ONESHOT(gzip, 31)
ZA_ONESHOT(deflate, -15)
ZA_STREAMS(zlib, 15)
ZA_STREAMS(gzip, 31)
ZA_STREAMS(deflate, -15)

/* Codec entries, one-shot and streaming, with a level range (0, 0 =
 * levels.json's). */
#define ZA_ZLIB(IMPL, LO, HI)                                                                  \
    {"zlib", IMPL, 0, za_bound, za_zlib_compress, za_zlib_decompress, za_zlib_cstream,         \
     za_zlib_dstream, LO, HI}
#define ZA_GZIP(IMPL, LO, HI)                                                                  \
    {"gzip", IMPL, 0, za_bound, za_gzip_compress, za_gzip_decompress, za_gzip_cstream,         \
     za_gzip_dstream, LO, HI}
#define ZA_DEFLATE(IMPL, LO, HI)                                                               \
    {"deflate", IMPL, 0, za_bound, za_deflate_compress, za_deflate_decompress,                 \
     za_deflate_cstream, za_deflate_dstream, LO, HI}

#endif /* BENCH_ZAPI_H */
