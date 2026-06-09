/*
 * Native-library baseline driver.
 *
 * Same language (C), same harness, same protocol as bench.c — but each codec
 * calls the upstream library directly instead of going through the cu_* ABI.
 * This isolates compress-utils' wrapper overhead: identical level mapping and
 * frame settings (mirrored from src/algorithms/<algo>/<algo>.c), so any
 * ratio difference is a bug and any speed difference is dispatch cost.
 *
 * Records carry impl="lib<algo>". Run alongside the compress-utils driver and
 * report.py overlays them per algorithm.
 *
 * Mirrored mappings (keep in sync with src/utils/levels.h + the wrappers):
 *   zstd   user*22/10 (≥1)        ZSTD_compress2 + pledgedSrcSize
 *   brotli user(+1 if 10), 1..11  BrotliEncoderCompress, default window/mode
 *   zlib   clamp 1..9             compress2 / uncompress
 *   bz2    clamp 1..9             BZ2_bzBuffToBuff{Compress,Decompress}, wf=0
 *   lz4    fast/HC split          LZ4F frame, contentSize + checksum + linked
 *   xz     clamp(user-1,0..9)     lzma_easy_buffer_encode, CRC64
 */

#include <stdint.h>

#include "brotli/decode.h"
#include "brotli/encode.h"
#include "bz2/bzlib.h"
#include "lz4/lz4frame.h"
#include "xz/lzma.h"
#include "zlib/zlib.h"
#include "zstd/zstd.h"

#include "bench_harness.h"

/* ---- level mappings (mirrored from the compress-utils wrappers) ---------- */

static int clamp_level(int user, int lo, int hi) {
    if (user < lo) return lo;
    if (user > hi) return hi;
    return user;
}

static int zstd_level(int u) {
    int n = (u * 22) / 10;
    return n < 1 ? 1 : (n > 22 ? 22 : n);
}
static int brotli_level(int u) {
    if (u < 1) return 1;
    if (u > 11) return 11;
    return u + (u == 10 ? 1 : 0);
}
static int lz4_level(int u) {
    if (u <= 3) return u - 1;
    return 4 + ((u - 4) * 8) / 6;
}

/* ---- zstd ---------------------------------------------------------------- */

static size_t z_bound(const bench_codec_t* c, size_t n) { (void)c; return ZSTD_compressBound(n); }

static int z_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, int level, const char** err) {
    (void)c;
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    if (!cctx) { *err = "ZSTD_createCCtx failed"; return 1; }
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, zstd_level(level));
    ZSTD_CCtx_setPledgedSrcSize(cctx, in_len);
    size_t r = ZSTD_compress2(cctx, out, *out_len, in, in_len);
    ZSTD_freeCCtx(cctx);
    if (ZSTD_isError(r)) { *err = ZSTD_getErrorName(r); return 1; }
    *out_len = r;
    return 0;
}

static int z_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                        uint8_t* out, size_t* out_len, const char** err) {
    (void)c;
    size_t r = ZSTD_decompress(out, *out_len, in, in_len);
    if (ZSTD_isError(r)) { *err = ZSTD_getErrorName(r); return 1; }
    *out_len = r;
    return 0;
}

/* ---- brotli -------------------------------------------------------------- */

static size_t br_bound(const bench_codec_t* c, size_t n) {
    (void)c;
    size_t b = BrotliEncoderMaxCompressedSize(n);
    return b ? b : n + (n >> 2) + 256;
}

static int br_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                       uint8_t* out, size_t* out_len, int level, const char** err) {
    (void)c;
    size_t enc = *out_len;
    BROTLI_BOOL ok = BrotliEncoderCompress(brotli_level(level), BROTLI_DEFAULT_WINDOW,
                                           BROTLI_DEFAULT_MODE, in_len, in, &enc, out);
    if (!ok) { *err = "BrotliEncoderCompress failed"; return 1; }
    *out_len = enc;
    return 0;
}

static int br_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                         uint8_t* out, size_t* out_len, const char** err) {
    (void)c;
    size_t dec = *out_len;
    BrotliDecoderResult r = BrotliDecoderDecompress(in_len, in, &dec, out);
    if (r != BROTLI_DECODER_RESULT_SUCCESS) { *err = "BrotliDecoderDecompress failed"; return 1; }
    *out_len = dec;
    return 0;
}

/* ---- zlib ---------------------------------------------------------------- */

static size_t zl_bound(const bench_codec_t* c, size_t n) { (void)c; return compressBound((uLong)n); }

static int zl_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                       uint8_t* out, size_t* out_len, int level, const char** err) {
    (void)c;
    uLongf cap = (uLongf)*out_len;
    int r = compress2(out, &cap, in, (uLong)in_len, clamp_level(level, 1, 9));
    if (r != Z_OK) { *err = "zlib compress2 failed"; return 1; }
    *out_len = cap;
    return 0;
}

static int zl_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                         uint8_t* out, size_t* out_len, const char** err) {
    (void)c;
    uLongf cap = (uLongf)*out_len;
    int r = uncompress(out, &cap, in, (uLong)in_len);
    if (r != Z_OK) { *err = "zlib uncompress failed"; return 1; }
    *out_len = cap;
    return 0;
}

/* ---- bz2 ----------------------------------------------------------------- */

static size_t bz_bound(const bench_codec_t* c, size_t n) { (void)c; return n + n / 100 + 600; }

static int bz_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                       uint8_t* out, size_t* out_len, int level, const char** err) {
    (void)c;
    unsigned int cap = (unsigned int)*out_len;
    int r = BZ2_bzBuffToBuffCompress((char*)out, &cap, (char*)(uintptr_t)in,
                                     (unsigned int)in_len, clamp_level(level, 1, 9), 0, 0);
    if (r != BZ_OK) { *err = "BZ2 compress failed"; return 1; }
    *out_len = cap;
    return 0;
}

static int bz_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                         uint8_t* out, size_t* out_len, const char** err) {
    (void)c;
    unsigned int cap = (unsigned int)*out_len;
    int r = BZ2_bzBuffToBuffDecompress((char*)out, &cap, (char*)(uintptr_t)in,
                                       (unsigned int)in_len, 0, 0);
    if (r != BZ_OK) { *err = "BZ2 decompress failed"; return 1; }
    *out_len = cap;
    return 0;
}

/* ---- lz4 (frame format) -------------------------------------------------- */

static void lz4_prefs(LZ4F_preferences_t* p, int level, size_t content_size) {
    memset(p, 0, sizeof(*p));
    p->compressionLevel = lz4_level(level);
    p->frameInfo.contentSize = content_size;
    p->frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    p->frameInfo.blockMode = LZ4F_blockLinked;
}

static size_t l4_bound(const bench_codec_t* c, size_t n) {
    (void)c;
    LZ4F_preferences_t p;
    memset(&p, 0, sizeof(p));
    p.frameInfo.contentSize = n;
    p.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    return LZ4F_compressFrameBound(n, &p);
}

static int l4_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                       uint8_t* out, size_t* out_len, int level, const char** err) {
    (void)c;
    LZ4F_preferences_t p;
    lz4_prefs(&p, level, in_len);
    size_t r = LZ4F_compressFrame(out, *out_len, in, in_len, &p);
    if (LZ4F_isError(r)) { *err = LZ4F_getErrorName(r); return 1; }
    *out_len = r;
    return 0;
}

static int l4_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                         uint8_t* out, size_t* out_len, const char** err) {
    (void)c;
    LZ4F_dctx* dctx = NULL;
    size_t r = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    if (LZ4F_isError(r)) { *err = LZ4F_getErrorName(r); return 1; }

    size_t out_cap = *out_len, out_pos = 0, in_pos = 0;
    int rc = 0;
    while (in_pos < in_len) {
        size_t dst = out_cap - out_pos;
        size_t src = in_len - in_pos;
        size_t hint = LZ4F_decompress(dctx, out + out_pos, &dst, in + in_pos, &src, NULL);
        if (LZ4F_isError(hint)) { *err = LZ4F_getErrorName(hint); rc = 1; break; }
        out_pos += dst;
        in_pos += src;
        if (hint == 0) break;        /* frame complete */
        if (dst == 0 && src == 0) { *err = "LZ4F stalled"; rc = 1; break; }
    }
    LZ4F_freeDecompressionContext(dctx);
    if (!rc) *out_len = out_pos;
    return rc;
}

/* ---- xz ------------------------------------------------------------------ */

static size_t xz_bound(const bench_codec_t* c, size_t n) { (void)c; return lzma_stream_buffer_bound(n); }

static int xz_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                       uint8_t* out, size_t* out_len, int level, const char** err) {
    (void)c;
    uint32_t preset = (uint32_t)clamp_level(level - 1, 0, 9);
    size_t out_pos = 0;
    lzma_ret r = lzma_easy_buffer_encode(preset, LZMA_CHECK_CRC64, NULL,
                                         in, in_len, out, &out_pos, *out_len);
    if (r != LZMA_OK) { *err = "lzma encode failed"; return 1; }
    *out_len = out_pos;
    return 0;
}

static int xz_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                         uint8_t* out, size_t* out_len, const char** err) {
    (void)c;
    uint64_t memlimit = UINT64_MAX;
    size_t in_pos = 0, out_pos = 0;
    lzma_ret r = lzma_stream_buffer_decode(&memlimit, 0, NULL, in, &in_pos, in_len,
                                           out, &out_pos, *out_len);
    if (r != LZMA_OK) { *err = "lzma decode failed"; return 1; }
    *out_len = out_pos;
    return 0;
}

/* ---- registry ------------------------------------------------------------ */

static const bench_codec_t CODECS[] = {
    {"zstd", "libzstd", 0, z_bound, z_compress, z_decompress},
    {"brotli", "libbrotli", 0, br_bound, br_compress, br_decompress},
    {"zlib", "zlib", 0, zl_bound, zl_compress, zl_decompress},
    {"bz2", "libbz2", 0, bz_bound, bz_compress, bz_decompress},
    {"lz4", "liblz4", 0, l4_bound, l4_compress, l4_decompress},
    {"xz", "liblzma", 0, xz_bound, xz_compress, xz_decompress},
};
static const size_t N_CODECS = sizeof(CODECS) / sizeof(CODECS[0]);

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--info")) {
        return bench_info("c", "baseline", "c-baseline");
    }
    return bench_run("c", CODECS, N_CODECS);
}
