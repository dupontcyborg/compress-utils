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
 * sweeps. Calls are each library's documented one-shot API; a library whose
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
/* One-shot only. A compressor per level and one decompressor, reused. Levels
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
    if (ZSTD_isError(r)) { *err = ZSTD_getErrorName(r); return 1; }
    *out_len = r;
    return 0;
}

static const bench_codec_t CODECS[] = {
    {"zstd", IMPL, 0, zx_bound, zx_compress, zx_decompress, NULL, NULL, 0, 0},
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
    if (lzma_stream_buffer_decode(&memlimit, 0, NULL, in, &in_pos, in_len, out, &out_pos,
                                  *out_len) != LZMA_OK) {
        *err = "lzma_stream_buffer_decode";
        return 1;
    }
    *out_len = out_pos;
    return 0;
}

static const bench_codec_t CODECS[] = {
    {"xz", IMPL, 0, lf_bound, lf_compress, lf_decompress, NULL, NULL, 0, 0},
};

/* ========================================================================= */
#elif defined(ALT_ISA_L)
/* igzip's stateless (one-shot) API, levels 0..3, with its level work buffer
 * allocated once per level and reused. */
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
    if (isal_inflate_stateless(&st) != ISAL_DECOMP_OK) { *err = "isal_inflate_stateless"; return 1; }
    *out_len = st.total_out;
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
    }
ISAL_FORMAT(zlib, IGZIP_ZLIB, ISAL_ZLIB)
ISAL_FORMAT(gzip, IGZIP_GZIP, ISAL_GZIP)
ISAL_FORMAT(deflate, IGZIP_DEFLATE, ISAL_DEFLATE)

static const bench_codec_t CODECS[] = {
    {"zlib", IMPL, 0, isal_bound, isal_zlib_compress, isal_zlib_decompress, NULL, NULL, 0, 3},
    {"gzip", IMPL, 0, isal_bound, isal_gzip_compress, isal_gzip_decompress, NULL, NULL, 0, 3},
    {"deflate", IMPL, 0, isal_bound, isal_deflate_compress, isal_deflate_decompress, NULL, NULL,
     0, 3},
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
    if (BZ2_bzBuffToBuffDecompress((char*)out, &dl, (char*)in, (unsigned int)in_len, 0, 0) !=
        BZ_OK) {
        *err = "BZ2_bzBuffToBuffDecompress";
        return 1;
    }
    *out_len = dl;
    return 0;
}

static const bench_codec_t CODECS[] = {
    {"bz2", IMPL, 0, bz_bound, bz_compress, bz_decompress, NULL, NULL, 0, 0},
};

/* ========================================================================= */
#elif defined(ALT_LZMA_SDK)
/* 7-Zip's xz encoder and decoder, built single-threaded (Z7_ST), CRC64 check.
 * Its levels 0..9 are LZMA SDK levels, which pick different dictionary sizes
 * than xz's presets. The encoder is told the input size (reduceSize), as 7-Zip
 * does, so small inputs get a small dictionary. */
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
} BufIn;

static SRes bufin_read(ISeqInStreamPtr pp, void* buf, size_t* size) {
    BufIn* b = (BufIn*)(void*)pp;
    size_t n = *size < b->left ? *size : b->left;
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

static int xz_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                       size_t* out_len, int level, const char** err) {
    (void)c;
    BufIn bin = {{bufin_read}, in, in_len};
    BufOut bout = {{bufout_write}, out, *out_len, 0};
    CXzProps props;
    XzProps_Init(&props);
    props.lzma2Props.lzmaProps.level = level;
    props.checkId = XZ_CHECK_CRC64;
    props.numTotalThreads = 1;
    props.reduceSize = in_len;
    if (Xz_Encode(&bout.vt, &bin.vt, &props, NULL) != SZ_OK) { *err = "Xz_Encode"; return 1; }
    *out_len = bout.written;
    return 0;
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
    if (!ok) { *err = "XzUnpacker_CodeFull"; return 1; }
    *out_len = dl;
    return 0;
}

static const bench_codec_t CODECS[] = {
    {"xz", IMPL, 0, xz_bound, xz_compress, xz_decompress, NULL, NULL, 0, 9},
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
    if (snappy_uncompress((const char*)in, in_len, (char*)out, out_len) != SNAPPY_OK) {
        *err = "snappy_uncompress";
        return 1;
    }
    return 0;
}

static const bench_codec_t CODECS[] = {
    {"snappy", IMPL, 0, sn_bound, sn_compress, sn_decompress, NULL, NULL, 0, 0},
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
