/*
 * Alternative-library driver: one C compression library that isn't
 * compress-utils or the codecs it vendors, benchmarked through the same
 * harness and protocol as bench.c. The runner compiles this file once per
 * library with -DALT_<NAME> and links only that library, so libraries that
 * export the same symbols (madler zlib and zlib-rs) never meet in one binary.
 * The libraries are built by benchmarks/deps/deps.py.
 *
 * Each driver gets native levels (the runner translates through
 * benchmarks/levels.json) and reports its own level range through --info, for
 * sweeps. Calls are each library's documented one-shot API, plus its streaming
 * API where it has one (fed and drained as c-baseline streams); a library whose
 * one-shot API takes a reusable context or work buffer (libdeflate, ISA-L)
 * gets one per level, reused across calls, as its documentation intends.
 * Records carry impl=<library>.
 */

#include "bench_harness.h"

#ifndef ALT_VERSION
#define ALT_VERSION "?"
#endif

/* ========================================================================= */
#if defined(ALT_ZLIB_NG)
#include <zlib-ng.h>
#define ZPFX zng_
#define ZSTREAM zng_stream
#define ZLEN size_t
#include "bench_zapi.h"
#define IMPL "zlib-ng"
static const bench_codec_t CODECS[] = {
    ZA_ZLIB(IMPL, 0, 0), ZA_GZIP(IMPL, 0, 0), ZA_DEFLATE(IMPL, 0, 0),
};

/* ========================================================================= */
#elif defined(ALT_ZLIB_RS)
#include <zlib.h>
#define ZPFX
#define ZSTREAM z_stream
#define ZLEN uLongf
#include "bench_zapi.h"
#define IMPL "zlib-rs"
static const bench_codec_t CODECS[] = {
    ZA_ZLIB(IMPL, 0, 0), ZA_GZIP(IMPL, 0, 0), ZA_DEFLATE(IMPL, 0, 0),
};

/* ========================================================================= */
#elif defined(ALT_MINIZ)
/* miniz has no gzip wrapper. Its level 10 is MZ_UBER_COMPRESSION. */
#include <miniz/miniz.h>
#define ZPFX mz_
#define ZSTREAM mz_stream
#define ZLEN mz_ulong
#include "bench_zapi.h"
#define IMPL "miniz"
static const bench_codec_t CODECS[] = {
    ZA_ZLIB(IMPL, 1, 10), ZA_DEFLATE(IMPL, 1, 10),
};

/* ========================================================================= */
#elif defined(ALT_LIBDEFLATE) || defined(ALT_LIBDEFLATE_PERCALL)
/* One-shot only. No format has a size hint (gzip's ISIZE is a guess, not used), so every
 * one starts at the default capacity and double on LIBDEFLATE_INSUFFICIENT_SPACE.
 * A compressor per level and one decompressor, reused. Levels
 * 1..12 (0 = store). The per-call variant allocates and frees them on every
 * call instead, as compress-utils sets up its codec on every call, to separate
 * libdeflate's codec speed from the setup it saves by reuse. */
#include <libdeflate.h>
#if defined(ALT_LIBDEFLATE_PERCALL)
#define IMPL "libdeflate-percall"
#else
#define IMPL "libdeflate"
#endif

static struct libdeflate_compressor* ld_comp[13];
static struct libdeflate_decompressor* ld_dec;

static struct libdeflate_compressor* ld_compressor(int level) {
    if (level < 0 || level > 12) return NULL;
    if (!ld_comp[level]) ld_comp[level] = libdeflate_alloc_compressor(level);
    return ld_comp[level];
}

#if defined(ALT_LIBDEFLATE_PERCALL)
#define LD_DONE_COMPRESS(level)                       \
    (libdeflate_free_compressor(ld_comp[level]), ld_comp[level] = NULL)
#define LD_DONE_DECOMPRESS() (libdeflate_free_decompressor(ld_dec), ld_dec = NULL)
#else
#define LD_DONE_COMPRESS(level) ((void)0)
#define LD_DONE_DECOMPRESS() ((void)0)
#endif

static size_t ld_bound(const bench_codec_t* c, size_t n) {
    (void)c;
    return libdeflate_gzip_compress_bound(NULL, n);  /* the largest of the three */
}

#define LD_FORMAT(NAME)                                                                       \
    static int ld_##NAME##_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len, \
                                    uint8_t* out, size_t* out_len, int level,                 \
                                    const char** err) {                                       \
        (void)c;                                                                              \
        struct libdeflate_compressor* z = ld_compressor(level);                               \
        if (!z) { *err = "libdeflate_alloc_compressor"; return 1; }                           \
        size_t n = libdeflate_##NAME##_compress(z, in, in_len, out, *out_len);                \
        LD_DONE_COMPRESS(level);                                                              \
        if (!n) { *err = "libdeflate_" #NAME "_compress"; return 1; }                         \
        *out_len = n;                                                                         \
        return 0;                                                                             \
    }                                                                                         \
    static int ld_##NAME##_decompress(const bench_codec_t* c, const uint8_t* in,              \
                                      size_t in_len, uint8_t* out, size_t* out_len,           \
                                      const char** err) {                                     \
        (void)c;                                                                              \
        if (!ld_dec) ld_dec = libdeflate_alloc_decompressor();                                \
        size_t actual = 0;                                                                    \
        enum libdeflate_result r =                                                            \
            libdeflate_##NAME##_decompress(ld_dec, in, in_len, out, *out_len, &actual);       \
        LD_DONE_DECOMPRESS();                                                                 \
        if (r == LIBDEFLATE_INSUFFICIENT_SPACE) return BENCH_NEED_MORE;                       \
        if (r != LIBDEFLATE_SUCCESS) {                                                        \
            *err = "libdeflate_" #NAME "_decompress";                                         \
            return 1;                                                                         \
        }                                                                                     \
        *out_len = actual;                                                                    \
        return 0;                                                                             \
    }
LD_FORMAT(zlib)
LD_FORMAT(gzip)
LD_FORMAT(deflate)

static const bench_codec_t CODECS[] = {
    {"zlib", IMPL, 0, ld_bound, ld_zlib_compress, ld_zlib_decompress, NULL, NULL, 1, 12},
    {"gzip", IMPL, 0, ld_bound, ld_gzip_compress, ld_gzip_decompress, NULL, NULL, 1, 12},
    {"deflate", IMPL, 0, ld_bound, ld_deflate_compress, ld_deflate_decompress, NULL, NULL, 1, 12},
};

/* ========================================================================= */
#elif defined(ALT_LIBZSTD_CTX)
/* The libzstd we vendor, with the same settings as c-baseline and the wrapper
 * (level, pledged size, ZSTD_compress2), but one ZSTD_CCtx and one ZSTD_DCtx
 * created once and reused, as zstd recommends for many small inputs. The gap to
 * c-baseline is the setup a reusable context would save compress-utils. */
#include "zstd.h"
#include "zstd_errors.h"
#define IMPL "libzstd-ctx"

static ZSTD_CCtx* zc;
static ZSTD_DCtx* zd;

static size_t zx_bound(const bench_codec_t* c, size_t n) {
    (void)c;
    return ZSTD_compressBound(n);
}

static int zx_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                       size_t* out_len, int level, const char** err) {
    (void)c;
    if (!zc) zc = ZSTD_createCCtx();
    ZSTD_CCtx_reset(zc, ZSTD_reset_session_only);
    ZSTD_CCtx_setParameter(zc, ZSTD_c_compressionLevel, level);
    ZSTD_CCtx_setPledgedSrcSize(zc, in_len);
    size_t r = ZSTD_compress2(zc, out, *out_len, in, in_len);
    if (ZSTD_isError(r)) { *err = ZSTD_getErrorName(r); return 1; }
    *out_len = r;
    return 0;
}

static int zx_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                         size_t* out_len, const char** err) {
    (void)c;
    if (!zd) zd = ZSTD_createDCtx();
    size_t r = ZSTD_decompressDCtx(zd, out, *out_len, in, in_len);
    if (ZSTD_isError(r)) {
        if (ZSTD_getErrorCode(r) == ZSTD_error_dstSize_tooSmall) return BENCH_NEED_MORE;
        *err = ZSTD_getErrorName(r);
        return 1;
    }
    *out_len = r;
    return 0;
}

/* The frame header's content size; unknown or an error counts as none. */
static int zx_hint(const bench_codec_t* c, const uint8_t* in, size_t in_len, size_t* out_size) {
    (void)c;
    unsigned long long n = ZSTD_getFrameContentSize(in, in_len);
    if (n == ZSTD_CONTENTSIZE_UNKNOWN || n == ZSTD_CONTENTSIZE_ERROR) return 1;
    *out_size = (size_t)n;
    return 0;
}

static const bench_codec_t CODECS[] = {
    {"zstd", IMPL, 0, zx_bound, zx_compress, zx_decompress, NULL, NULL, 0, 0, zx_hint},
};

/* ========================================================================= */
#elif defined(ALT_LIBLZMA_FITDICT)
/* The liblzma we vendor, as c-baseline and the wrapper call it (preset, CRC64,
 * one-shot buffer API), except the dictionary is capped at the input size
 * rounded up to a power of two (at least 4 KiB), as 7-Zip does. Input never
 * reaches past the dictionary either way, so ratio is unchanged; only the
 * header's dictionary field, and the memory both sides set up, shrink. */
#include <lzma.h>
#define IMPL "liblzma-fitdict"

static size_t lf_bound(const bench_codec_t* c, size_t n) {
    (void)c;
    return lzma_stream_buffer_bound(n);
}

static int lf_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                       size_t* out_len, int level, const char** err) {
    (void)c;
    lzma_options_lzma opt;
    if (lzma_lzma_preset(&opt, (uint32_t)level)) { *err = "lzma_lzma_preset"; return 1; }
    uint32_t dict = LZMA_DICT_SIZE_MIN;
    while (dict < in_len && dict < opt.dict_size) dict <<= 1;
    if (dict < opt.dict_size) opt.dict_size = dict;
    lzma_filter filters[] = {{LZMA_FILTER_LZMA2, &opt}, {LZMA_VLI_UNKNOWN, NULL}};
    size_t out_pos = 0;
    if (lzma_stream_buffer_encode(filters, LZMA_CHECK_CRC64, NULL, in, in_len, out, &out_pos,
                                  *out_len) != LZMA_OK) {
        *err = "lzma_stream_buffer_encode";
        return 1;
    }
    *out_len = out_pos;
    return 0;
}

static int lf_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                         size_t* out_len, const char** err) {
    (void)c;
    uint64_t memlimit = UINT64_MAX;
    size_t in_pos = 0, out_pos = 0;
    lzma_ret r = lzma_stream_buffer_decode(&memlimit, 0, NULL, in, &in_pos, in_len, out, &out_pos,
                                           *out_len);
    /* LZMA_BUF_ERROR means the output was too small (the API resets out_pos on
     * error, so it can't tell that from truncated input; ours never is). */
    if (r == LZMA_BUF_ERROR) return BENCH_NEED_MORE;
    if (r != LZMA_OK) { *err = "lzma_stream_buffer_decode"; return 1; }
    *out_len = out_pos;
    return 0;
}

static const bench_codec_t CODECS[] = {
    {"xz", IMPL, 0, lf_bound, lf_compress, lf_decompress, NULL, NULL, 0, 0},
};

/* ========================================================================= */
#elif defined(ALT_ISA_L)
/* igzip's stateless (one-shot) API and its stateful (streaming) one, levels
 * 0..3, with the level work buffer allocated once per level and reused. */
#include <isa-l/igzip_lib.h>
#define IMPL "isa-l"

static uint8_t* isal_buf[4];
static const uint32_t isal_buf_size[4] = {ISAL_DEF_LVL0_DEFAULT, ISAL_DEF_LVL1_DEFAULT,
                                          ISAL_DEF_LVL2_DEFAULT, ISAL_DEF_LVL3_DEFAULT};

/* ISA-L has no bound function; DEFLATE's stored blocks cost 5 bytes per 64 KiB. */
static size_t isal_bound(const bench_codec_t* c, size_t n) {
    (void)c;
    return n + n / 16 + 1024;
}

static int isal_compress_fmt(const uint8_t* in, size_t in_len, uint8_t* out, size_t* out_len,
                             int level, uint16_t gzip_flag, const char** err) {
    if (level < 0 || level > 3) { *err = "isa-l level out of range"; return 1; }
    if (isal_buf_size[level] && !isal_buf[level]) isal_buf[level] = malloc(isal_buf_size[level]);
    struct isal_zstream s;
    isal_deflate_stateless_init(&s);
    s.level = (uint32_t)level;
    s.level_buf = isal_buf[level];
    s.level_buf_size = isal_buf_size[level];
    s.gzip_flag = gzip_flag;
    s.next_in = (uint8_t*)in;
    s.avail_in = (uint32_t)in_len;
    s.next_out = out;
    s.avail_out = (uint32_t)*out_len;
    s.end_of_stream = 1;
    s.flush = NO_FLUSH;
    if (isal_deflate_stateless(&s) != COMP_OK) { *err = "isal_deflate_stateless"; return 1; }
    *out_len = s.total_out;
    return 0;
}

static int isal_decompress_fmt(const uint8_t* in, size_t in_len, uint8_t* out, size_t* out_len,
                               uint32_t crc_flag, const char** err) {
    struct inflate_state st;
    isal_inflate_init(&st);
    st.next_in = (uint8_t*)in;
    st.avail_in = (uint32_t)in_len;
    st.next_out = out;
    st.avail_out = (uint32_t)*out_len;
    st.crc_flag = crc_flag;
    int r = isal_inflate_stateless(&st);
    if (r == ISAL_OUT_OVERFLOW) return BENCH_NEED_MORE;
    if (r != ISAL_DECOMP_OK) { *err = "isal_inflate_stateless"; return 1; }
    *out_len = st.total_out;
    return 0;
}

/* Stateful streaming: chunks in with NO_FLUSH, end_of_stream on the last. */
static int isal_cstream_fmt(const uint8_t* in, size_t in_len, uint8_t* out, size_t* out_len,
                            int level, uint16_t gzip_flag, size_t chunk, const char** err) {
    if (level < 0 || level > 3) { *err = "isa-l level out of range"; return 1; }
    if (chunk == 0) chunk = 64 * 1024;
    if (isal_buf_size[level] && !isal_buf[level]) isal_buf[level] = malloc(isal_buf_size[level]);
    struct isal_zstream s;
    isal_deflate_init(&s);
    s.level = (uint32_t)level;
    s.level_buf = isal_buf[level];
    s.level_buf_size = isal_buf_size[level];
    s.gzip_flag = gzip_flag;
    s.flush = NO_FLUSH;
    s.next_out = out;
    s.avail_out = (uint32_t)*out_len;
    size_t off = 0;
    do {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        s.next_in = (uint8_t*)in + off;
        s.avail_in = (uint32_t)n;
        off += n;
        s.end_of_stream = off == in_len;
        do {
            if (isal_deflate(&s) != COMP_OK) { *err = "isal_deflate"; return 1; }
        } while (s.avail_in > 0 || (s.end_of_stream && s.internal_state.state != ZSTATE_END));
    } while (off < in_len);
    *out_len = s.total_out;
    return 0;
}

static int isal_dstream_fmt(const uint8_t* in, size_t in_len, uint8_t* out, size_t* out_len,
                            uint32_t crc_flag, size_t chunk, const char** err) {
    if (chunk == 0) chunk = 64 * 1024;
    struct inflate_state st;
    isal_inflate_init(&st);
    st.crc_flag = crc_flag;
    size_t win = *out_len, total = 0;
    for (size_t off = 0; off < in_len && st.block_state != ISAL_BLOCK_FINISH; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        st.next_in = (uint8_t*)in + off;
        st.avail_in = (uint32_t)n;
        do {
            st.next_out = out;
            st.avail_out = (uint32_t)win;
            if (isal_inflate(&st) != ISAL_DECOMP_OK) { *err = "isal_inflate"; return 1; }
            bench_sink_write(out, win - st.avail_out);
            total += win - st.avail_out;
        } while ((st.avail_in > 0 || st.avail_out == 0) && st.block_state != ISAL_BLOCK_FINISH);
    }
    if (st.block_state != ISAL_BLOCK_FINISH) { *err = "isal_inflate: truncated"; return 1; }
    *out_len = total;
    return 0;
}

#define ISAL_FORMAT(NAME, GZFLAG, CRCFLAG)                                                     \
    static int isal_##NAME##_compress(const bench_codec_t* c, const uint8_t* in,               \
                                      size_t in_len, uint8_t* out, size_t* out_len, int level, \
                                      const char** err) {                                      \
        (void)c;                                                                               \
        return isal_compress_fmt(in, in_len, out, out_len, level, GZFLAG, err);                \
    }                                                                                          \
    static int isal_##NAME##_decompress(const bench_codec_t* c, const uint8_t* in,             \
                                        size_t in_len, uint8_t* out, size_t* out_len,          \
                                        const char** err) {                                    \
        (void)c;                                                                               \
        return isal_decompress_fmt(in, in_len, out, out_len, CRCFLAG, err);                    \
    }                                                                                          \
    static int isal_##NAME##_cstream(const bench_codec_t* c, const uint8_t* in,               \
                                     size_t in_len, uint8_t* out, size_t* out_len, int level,  \
                                     size_t chunk, const char** err) {                         \
        (void)c;                                                                               \
        return isal_cstream_fmt(in, in_len, out, out_len, level, GZFLAG, chunk, err);          \
    }                                                                                          \
    static int isal_##NAME##_dstream(const bench_codec_t* c, const uint8_t* in,               \
                                     size_t in_len, uint8_t* out, size_t* out_len,             \
                                     size_t chunk, const char** err) {                         \
        (void)c;                                                                               \
        return isal_dstream_fmt(in, in_len, out, out_len, CRCFLAG, chunk, err);                \
    }
ISAL_FORMAT(zlib, IGZIP_ZLIB, ISAL_ZLIB)
ISAL_FORMAT(gzip, IGZIP_GZIP, ISAL_GZIP)
ISAL_FORMAT(deflate, IGZIP_DEFLATE, ISAL_DEFLATE)

static const bench_codec_t CODECS[] = {
    {"zlib", IMPL, 0, isal_bound, isal_zlib_compress, isal_zlib_decompress, isal_zlib_cstream,
     isal_zlib_dstream, 0, 3},
    {"gzip", IMPL, 0, isal_bound, isal_gzip_compress, isal_gzip_decompress, isal_gzip_cstream,
     isal_gzip_dstream, 0, 3},
    {"deflate", IMPL, 0, isal_bound, isal_deflate_compress, isal_deflate_decompress,
     isal_deflate_cstream, isal_deflate_dstream, 0, 3},
};

/* ========================================================================= */
#elif defined(ALT_LIBBZ2_RS)
/* libbzip2's buffer API, the same calls (and workFactor 0) as the wrapper. */
#include <bzlib.h>
#define IMPL "libbz2-rs"

/* Built without its stdio feature (see deps.py), the library leaves this hook
 * to the application, as libbzip2 does under BZ_NO_STDIO. */
void bz_internal_error(int errcode) {
    fprintf(stderr, "libbz2-rs internal error %d\n", errcode);
    abort();
}

static size_t bz_bound(const bench_codec_t* c, size_t n) {
    (void)c;
    return n + n / 100 + 600;
}

static int bz_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                       size_t* out_len, int level, const char** err) {
    (void)c;
    unsigned int dl = (unsigned int)*out_len;
    if (BZ2_bzBuffToBuffCompress((char*)out, &dl, (char*)in, (unsigned int)in_len, level, 0,
                                 0) != BZ_OK) {
        *err = "BZ2_bzBuffToBuffCompress";
        return 1;
    }
    *out_len = dl;
    return 0;
}

static int bz_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                         size_t* out_len, const char** err) {
    (void)c;
    unsigned int dl = (unsigned int)*out_len;
    int r = BZ2_bzBuffToBuffDecompress((char*)out, &dl, (char*)in, (unsigned int)in_len, 0, 0);
    if (r == BZ_OUTBUFF_FULL) return BENCH_NEED_MORE;
    if (r != BZ_OK) { *err = "BZ2_bzBuffToBuffDecompress"; return 1; }
    *out_len = dl;
    return 0;
}

/* Streaming, as c-baseline streams libbzip2. */
static int bz_cstream(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                      size_t* out_len, int level, size_t chunk, const char** err) {
    (void)c;
    if (chunk == 0) chunk = 64 * 1024;
    bz_stream s;
    memset(&s, 0, sizeof(s));
    if (BZ2_bzCompressInit(&s, level, 0, 0) != BZ_OK) { *err = "BZ2_bzCompressInit"; return 1; }
    size_t cap = *out_len;
    s.next_out = (char*)out;
    s.avail_out = (unsigned int)cap;
    int rc = 0;
    for (size_t off = 0; off < in_len && !rc; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        s.next_in = (char*)(uintptr_t)(in + off);
        s.avail_in = (unsigned int)n;
        while (s.avail_in > 0) {
            if (BZ2_bzCompress(&s, BZ_RUN) != BZ_RUN_OK) { *err = "BZ2_bzCompress"; rc = 1; break; }
        }
    }
    if (!rc) {
        int r;
        do { r = BZ2_bzCompress(&s, BZ_FINISH); } while (r == BZ_FINISH_OK);
        if (r != BZ_STREAM_END) { *err = "BZ2_bzCompress finish"; rc = 1; }
    }
    if (!rc) *out_len = cap - s.avail_out;
    BZ2_bzCompressEnd(&s);
    return rc;
}

static int bz_dstream(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                      size_t* out_len, size_t chunk, const char** err) {
    (void)c;
    if (chunk == 0) chunk = 64 * 1024;
    bz_stream s;
    memset(&s, 0, sizeof(s));
    if (BZ2_bzDecompressInit(&s, 0, 0) != BZ_OK) { *err = "BZ2_bzDecompressInit"; return 1; }
    size_t win = *out_len, total = 0;
    int rc = 0, done = 0;
    for (size_t off = 0; off < in_len && !rc && !done; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        s.next_in = (char*)(uintptr_t)(in + off);
        s.avail_in = (unsigned int)n;
        do {
            s.next_out = (char*)out;
            s.avail_out = (unsigned int)win;
            int r = BZ2_bzDecompress(&s);
            bench_sink_write(out, win - s.avail_out);
            total += win - s.avail_out;
            if (r == BZ_STREAM_END) { done = 1; break; }
            if (r != BZ_OK) { *err = "BZ2_bzDecompress"; rc = 1; break; }
        } while (s.avail_in > 0 || s.avail_out == 0);
    }
    if (!rc && !done) { *err = "BZ2_bzDecompress: truncated"; rc = 1; }
    if (!rc) *out_len = total;
    BZ2_bzDecompressEnd(&s);
    return rc;
}

static const bench_codec_t CODECS[] = {
    {"bz2", IMPL, 0, bz_bound, bz_compress, bz_decompress, bz_cstream, bz_dstream, 0, 0},
};

/* ========================================================================= */
#elif defined(ALT_LZMA_SDK)
/* 7-Zip's xz encoder and decoder, built single-threaded (Z7_ST), CRC64 check.
 * Its levels 0..9 are LZMA SDK levels, which pick different dictionary sizes
 * than xz's presets. One-shot, the encoder is told the input size (reduceSize),
 * as 7-Zip does, so small inputs get a small dictionary; streaming, it isn't. */
#include "7zCrc.h"
#include "Alloc.h"
#include "Xz.h"
#include "XzCrc64.h"
#include "XzEnc.h"
#define IMPL "lzma-sdk"

typedef struct {
    ISeqInStream vt;
    const uint8_t* p;
    size_t left;
    size_t chunk; /* streaming: hand out at most this much per read; 0 = no limit */
} BufIn;

static SRes bufin_read(ISeqInStreamPtr pp, void* buf, size_t* size) {
    BufIn* b = (BufIn*)(void*)pp;
    size_t n = *size < b->left ? *size : b->left;
    if (b->chunk && n > b->chunk) n = b->chunk;
    memcpy(buf, b->p, n);
    b->p += n;
    b->left -= n;
    *size = n;
    return SZ_OK;
}

typedef struct {
    ISeqOutStream vt;
    uint8_t* p;
    size_t left, written;
} BufOut;

static size_t bufout_write(ISeqOutStreamPtr pp, const void* buf, size_t size) {
    BufOut* b = (BufOut*)(void*)pp;
    size_t n = size < b->left ? size : b->left;
    memcpy(b->p + b->written, buf, n);
    b->written += n;
    b->left -= n;
    return n;
}

static size_t xz_bound(const bench_codec_t* c, size_t n) {
    (void)c;
    return n + n / 16 + 4096;
}

/* chunk == 0: one-shot, told the input size (reduceSize). Otherwise streaming:
 * the encoder pulls at most `chunk` bytes per read and doesn't know the size,
 * so it keeps the preset's dictionary, as a streaming liblzma encoder does. */
static int xz_encode(const uint8_t* in, size_t in_len, uint8_t* out, size_t* out_len, int level,
                     size_t chunk, const char** err) {
    BufIn bin = {{bufin_read}, in, in_len, chunk};
    BufOut bout = {{bufout_write}, out, *out_len, 0};
    CXzProps props;
    XzProps_Init(&props);
    props.lzma2Props.lzmaProps.level = level;
    props.checkId = XZ_CHECK_CRC64;
    props.numTotalThreads = 1;
    if (!chunk) props.reduceSize = in_len;
    if (Xz_Encode(&bout.vt, &bin.vt, &props, NULL) != SZ_OK) { *err = "Xz_Encode"; return 1; }
    *out_len = bout.written;
    return 0;
}

static int xz_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                       size_t* out_len, int level, const char** err) {
    (void)c;
    return xz_encode(in, in_len, out, out_len, level, 0, err);
}

static int xz_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                         size_t* out_len, const char** err) {
    (void)c;
    CXzUnpacker u;
    XzUnpacker_Construct(&u, &g_Alloc);
    XzUnpacker_Init(&u);
    SizeT dl = *out_len, sl = in_len;
    ECoderStatus status;
    SRes r = XzUnpacker_CodeFull(&u, out, &dl, in, &sl, CODER_FINISH_END, &status);
    int ok = r == SZ_OK && XzUnpacker_IsStreamWasFinished(&u);
    XzUnpacker_Free(&u);
    /* No error code says "output too small": with FINISH_END, reaching the limit
     * before the stream ends is reported as a data error. */
    if (!ok && dl == *out_len) return BENCH_NEED_MORE;
    if (!ok) { *err = "XzUnpacker_CodeFull"; return 1; }
    *out_len = dl;
    return 0;
}

static int xz_cstream(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                      size_t* out_len, int level, size_t chunk, const char** err) {
    (void)c;
    return xz_encode(in, in_len, out, out_len, level, chunk ? chunk : 64 * 1024, err);
}

/* Streaming decode: XzUnpacker_Code over chunk-sized input pieces, into the
 * bounded window, each produced run going to the sink. FINISH_ANY: with more
 * output to come, FINISH_END would treat reaching the window's end as an error. */
static int xz_dstream(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                      size_t* out_len, size_t chunk, const char** err) {
    (void)c;
    if (chunk == 0) chunk = 64 * 1024;
    CXzUnpacker u;
    XzUnpacker_Construct(&u, &g_Alloc);
    XzUnpacker_Init(&u);
    size_t win = *out_len, total = 0;
    int rc = 0;
    for (size_t off = 0; off < in_len && !rc; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        /* A call can stop early (at a block or stream boundary, or a full
         * window): keep going until the chunk is consumed and the window wasn't
         * filled, or nothing moves. */
        size_t used = 0;
        SizeT dl;
        do {
            SizeT sl = n - used;
            dl = win;
            ECoderStatus status;
            if (XzUnpacker_Code(&u, out, &dl, in + off + used, &sl, off + n == in_len,
                                CODER_FINISH_ANY, &status) != SZ_OK ||
                (sl == 0 && dl == 0 && used < n)) {
                *err = "XzUnpacker_Code";
                rc = 1;
            }
            bench_sink_write(out, dl);
            total += dl;
            used += sl;
        } while (!rc && (used < n || dl == win));
    }
    if (!rc && !XzUnpacker_IsStreamWasFinished(&u)) { *err = "xz: truncated"; rc = 1; }
    XzUnpacker_Free(&u);
    if (!rc) *out_len = total;
    return rc;
}

static const bench_codec_t CODECS[] = {
    {"xz", IMPL, 0, xz_bound, xz_compress, xz_decompress, xz_cstream, xz_dstream, 0, 9},
};
#define ALT_INIT() (CrcGenerateTable(), Crc64GenerateTable())

/* ========================================================================= */
#elif defined(ALT_GOOGLE_SNAPPY)
/* Google's C++ snappy through its C API. No levels. */
#include <snappy-c.h>
#define IMPL "google-snappy"

static size_t sn_bound(const bench_codec_t* c, size_t n) {
    (void)c;
    return snappy_max_compressed_length(n);
}

static int sn_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                       size_t* out_len, int level, const char** err) {
    (void)c;
    (void)level;
    if (snappy_compress((const char*)in, in_len, (char*)out, out_len) != SNAPPY_OK) {
        *err = "snappy_compress";
        return 1;
    }
    return 0;
}

static int sn_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                         size_t* out_len, const char** err) {
    (void)c;
    snappy_status r = snappy_uncompress((const char*)in, in_len, (char*)out, out_len);
    if (r == SNAPPY_BUFFER_TOO_SMALL) return BENCH_NEED_MORE;
    if (r != SNAPPY_OK) { *err = "snappy_uncompress"; return 1; }
    return 0;
}

/* The length varint at the start of the stream. */
static int sn_hint(const bench_codec_t* c, const uint8_t* in, size_t in_len, size_t* out_size) {
    (void)c;
    return snappy_uncompressed_length((const char*)in, in_len, out_size) != SNAPPY_OK;
}

static const bench_codec_t CODECS[] = {
    {"snappy", IMPL, 0, sn_bound, sn_compress, sn_decompress, NULL, NULL, 0, 0, sn_hint},
};

/* ========================================================================= */
#else
#error "define one ALT_<NAME> (see benchmarks/runner.py)"
#endif

#ifndef ALT_INIT
#define ALT_INIT() ((void)0)
#endif

static const size_t N_CODECS = sizeof(CODECS) / sizeof(CODECS[0]);

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--info")) {
        return bench_info_codecs("c", ALT_VERSION, "c-" IMPL, IMPL, CODECS, N_CODECS);
    }
    ALT_INIT();
    return bench_run("c", CODECS, N_CODECS);
}
