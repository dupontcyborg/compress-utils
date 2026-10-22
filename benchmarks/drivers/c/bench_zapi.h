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
    if (ZAPI(uncompress)(out, &dl, in, (ZLEN)in_len) != Z_OK) { *err = "uncompress"; return 1; }
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
    ZAPI(inflateEnd)(&s);
    if (r != Z_STREAM_END) { *err = "inflate"; return 1; }
    return 0;
}

#define ZA_FORMAT(NAME, WBITS)                                                               \
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
ZA_FORMAT(gzip, 31)
ZA_FORMAT(deflate, -15)

/* Codec entries: one-shot only (no streaming functions), with a level range
 * (0, 0 = levels.json's). */
#define ZA_ZLIB(IMPL, LO, HI) \
    {"zlib", IMPL, 0, za_bound, za_zlib_compress, za_zlib_decompress, NULL, NULL, LO, HI}
#define ZA_GZIP(IMPL, LO, HI) \
    {"gzip", IMPL, 0, za_bound, za_gzip_compress, za_gzip_decompress, NULL, NULL, LO, HI}
#define ZA_DEFLATE(IMPL, LO, HI) \
    {"deflate", IMPL, 0, za_bound, za_deflate_compress, za_deflate_decompress, NULL, NULL, LO, HI}

#endif /* BENCH_ZAPI_H */
