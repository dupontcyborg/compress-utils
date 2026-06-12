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

/* ---- streaming --------------------------------------------------------------
 * Feed input in `chunk`-sized pieces through each library's native streaming
 * API, writing all output into the (bound-sized) `out` buffer. Mirrors the
 * cu_*_stream_* paths so a cu-vs-native stream comparison is apples-to-apples.
 * Streaming doesn't know the total size up front, so frames omit content size
 * (which can make the stream ratio differ slightly from one-shot — expected).
 */

/* zstd */
static int z_cstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                     uint8_t* out, size_t* out_len, int level, size_t chunk, const char** err) {
    (void)c; if (chunk == 0) chunk = 64 * 1024;
    ZSTD_CCtx* z = ZSTD_createCCtx();
    if (!z) { *err = "ZSTD_createCCtx"; return 1; }
    ZSTD_CCtx_setParameter(z, ZSTD_c_compressionLevel, zstd_level(level));
    ZSTD_outBuffer ob = {out, *out_len, 0};
    int rc = 0;
    for (size_t off = 0; off < in_len && !rc; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        ZSTD_inBuffer ib = {in + off, n, 0};
        while (ib.pos < ib.size) {
            size_t r = ZSTD_compressStream2(z, &ob, &ib, ZSTD_e_continue);
            if (ZSTD_isError(r)) { *err = ZSTD_getErrorName(r); rc = 1; break; }
        }
    }
    if (!rc) {
        ZSTD_inBuffer ib = {NULL, 0, 0};
        size_t rem;
        do {
            rem = ZSTD_compressStream2(z, &ob, &ib, ZSTD_e_end);
            if (ZSTD_isError(rem)) { *err = ZSTD_getErrorName(rem); rc = 1; break; }
        } while (rem != 0);
    }
    ZSTD_freeCCtx(z);
    if (!rc) *out_len = ob.pos;
    return rc;
}

static int z_dstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                     uint8_t* out, size_t* out_len, size_t chunk, const char** err) {
    (void)c; if (chunk == 0) chunk = 64 * 1024;
    ZSTD_DCtx* z = ZSTD_createDCtx();
    if (!z) { *err = "ZSTD_createDCtx"; return 1; }
    ZSTD_outBuffer ob = {out, *out_len, 0};
    int rc = 0;
    for (size_t off = 0; off < in_len && !rc; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        ZSTD_inBuffer ib = {in + off, n, 0};
        while (ib.pos < ib.size) {
            size_t r = ZSTD_decompressStream(z, &ob, &ib);
            if (ZSTD_isError(r)) { *err = ZSTD_getErrorName(r); rc = 1; break; }
        }
    }
    ZSTD_freeDCtx(z);
    if (!rc) *out_len = ob.pos;
    return rc;
}

/* brotli */
static int br_cstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, int level, size_t chunk, const char** err) {
    (void)c; if (chunk == 0) chunk = 64 * 1024;
    BrotliEncoderState* e = BrotliEncoderCreateInstance(NULL, NULL, NULL);
    if (!e) { *err = "BrotliEncoderCreateInstance"; return 1; }
    BrotliEncoderSetParameter(e, BROTLI_PARAM_QUALITY, brotli_level(level));
    BrotliEncoderSetParameter(e, BROTLI_PARAM_LGWIN, BROTLI_DEFAULT_WINDOW);
    size_t cap = *out_len, avail_out = cap;
    uint8_t* next_out = out;
    int rc = 0;
    for (size_t off = 0; off < in_len && !rc; off += chunk) {
        size_t avail_in = in_len - off < chunk ? in_len - off : chunk;
        const uint8_t* next_in = in + off;
        while (avail_in > 0) {
            if (!BrotliEncoderCompressStream(e, BROTLI_OPERATION_PROCESS, &avail_in, &next_in,
                                             &avail_out, &next_out, NULL)) {
                *err = "brotli process"; rc = 1; break;
            }
        }
    }
    if (!rc) {
        size_t avail_in = 0;
        const uint8_t* next_in = NULL;
        while (!BrotliEncoderIsFinished(e)) {
            if (!BrotliEncoderCompressStream(e, BROTLI_OPERATION_FINISH, &avail_in, &next_in,
                                             &avail_out, &next_out, NULL)) {
                *err = "brotli finish"; rc = 1; break;
            }
        }
    }
    BrotliEncoderDestroyInstance(e);
    if (!rc) *out_len = cap - avail_out;
    return rc;
}

static int br_dstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, size_t chunk, const char** err) {
    (void)c; if (chunk == 0) chunk = 64 * 1024;
    BrotliDecoderState* dec = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (!dec) { *err = "BrotliDecoderCreateInstance"; return 1; }
    size_t cap = *out_len, avail_out = cap;
    uint8_t* next_out = out;
    int rc = 0, done = 0;
    for (size_t off = 0; off < in_len && !rc && !done; off += chunk) {
        size_t avail_in = in_len - off < chunk ? in_len - off : chunk;
        const uint8_t* next_in = in + off;
        for (;;) {
            BrotliDecoderResult r = BrotliDecoderDecompressStream(
                dec, &avail_in, &next_in, &avail_out, &next_out, NULL);
            if (r == BROTLI_DECODER_RESULT_SUCCESS) { done = 1; break; }
            if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) break;
            *err = "brotli decode"; rc = 1; break;
        }
    }
    BrotliDecoderDestroyInstance(dec);
    if (!rc) *out_len = cap - avail_out;
    return rc;
}

/* zlib */
static int zl_cstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, int level, size_t chunk, const char** err) {
    (void)c; if (chunk == 0) chunk = 64 * 1024;
    z_stream s; memset(&s, 0, sizeof(s));
    if (deflateInit(&s, clamp_level(level, 1, 9)) != Z_OK) { *err = "deflateInit"; return 1; }
    size_t cap = *out_len;
    s.next_out = out; s.avail_out = (uInt)cap;
    int rc = 0;
    for (size_t off = 0; off < in_len && !rc; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        s.next_in = (Bytef*)(uintptr_t)(in + off); s.avail_in = (uInt)n;
        while (s.avail_in > 0) {
            if (deflate(&s, Z_NO_FLUSH) != Z_OK) { *err = "deflate"; rc = 1; break; }
        }
    }
    if (!rc) {
        int r;
        do { r = deflate(&s, Z_FINISH); } while (r == Z_OK);
        if (r != Z_STREAM_END) { *err = "deflate finish"; rc = 1; }
    }
    if (!rc) *out_len = cap - s.avail_out;
    deflateEnd(&s);
    return rc;
}

static int zl_dstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, size_t chunk, const char** err) {
    (void)c; if (chunk == 0) chunk = 64 * 1024;
    z_stream s; memset(&s, 0, sizeof(s));
    if (inflateInit(&s) != Z_OK) { *err = "inflateInit"; return 1; }
    size_t cap = *out_len;
    s.next_out = out; s.avail_out = (uInt)cap;
    int rc = 0, done = 0;
    for (size_t off = 0; off < in_len && !rc && !done; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        s.next_in = (Bytef*)(uintptr_t)(in + off); s.avail_in = (uInt)n;
        while (s.avail_in > 0) {
            int r = inflate(&s, Z_NO_FLUSH);
            if (r == Z_STREAM_END) { done = 1; break; }
            if (r != Z_OK) { *err = "inflate"; rc = 1; break; }
        }
    }
    if (!rc) *out_len = cap - s.avail_out;
    inflateEnd(&s);
    return rc;
}

/* bz2 */
static int bz_cstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, int level, size_t chunk, const char** err) {
    (void)c; if (chunk == 0) chunk = 64 * 1024;
    bz_stream s; memset(&s, 0, sizeof(s));
    if (BZ2_bzCompressInit(&s, clamp_level(level, 1, 9), 0, 0) != BZ_OK) {
        *err = "bzCompressInit"; return 1;
    }
    size_t cap = *out_len;
    s.next_out = (char*)out; s.avail_out = (unsigned int)cap;
    int rc = 0;
    for (size_t off = 0; off < in_len && !rc; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        s.next_in = (char*)(uintptr_t)(in + off); s.avail_in = (unsigned int)n;
        while (s.avail_in > 0) {
            if (BZ2_bzCompress(&s, BZ_RUN) != BZ_RUN_OK) { *err = "bzCompress"; rc = 1; break; }
        }
    }
    if (!rc) {
        int r;
        do { r = BZ2_bzCompress(&s, BZ_FINISH); } while (r == BZ_FINISH_OK);
        if (r != BZ_STREAM_END) { *err = "bz finish"; rc = 1; }
    }
    if (!rc) *out_len = cap - s.avail_out;
    BZ2_bzCompressEnd(&s);
    return rc;
}

static int bz_dstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, size_t chunk, const char** err) {
    (void)c; if (chunk == 0) chunk = 64 * 1024;
    bz_stream s; memset(&s, 0, sizeof(s));
    if (BZ2_bzDecompressInit(&s, 0, 0) != BZ_OK) { *err = "bzDecompressInit"; return 1; }
    size_t cap = *out_len;
    s.next_out = (char*)out; s.avail_out = (unsigned int)cap;
    int rc = 0, done = 0;
    for (size_t off = 0; off < in_len && !rc && !done; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        s.next_in = (char*)(uintptr_t)(in + off); s.avail_in = (unsigned int)n;
        while (s.avail_in > 0) {
            int r = BZ2_bzDecompress(&s);
            if (r == BZ_STREAM_END) { done = 1; break; }
            if (r != BZ_OK) { *err = "bzDecompress"; rc = 1; break; }
        }
    }
    if (!rc) *out_len = cap - s.avail_out;
    BZ2_bzDecompressEnd(&s);
    return rc;
}

/* lz4 (frame) */
static int l4_cstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, int level, size_t chunk, const char** err) {
    (void)c; if (chunk == 0) chunk = 64 * 1024;
    LZ4F_cctx* cc = NULL;
    if (LZ4F_isError(LZ4F_createCompressionContext(&cc, LZ4F_VERSION))) {
        *err = "lz4 cctx"; return 1;
    }
    LZ4F_preferences_t p;
    lz4_prefs(&p, level, 0);  /* streaming: content size unknown */
    size_t cap = *out_len, pos = 0;
    int rc = 0;
    size_t r = LZ4F_compressBegin(cc, out, cap, &p);
    if (LZ4F_isError(r)) { *err = LZ4F_getErrorName(r); rc = 1; } else { pos = r; }
    for (size_t off = 0; off < in_len && !rc; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        r = LZ4F_compressUpdate(cc, out + pos, cap - pos, in + off, n, NULL);
        if (LZ4F_isError(r)) { *err = LZ4F_getErrorName(r); rc = 1; break; }
        pos += r;
    }
    if (!rc) {
        r = LZ4F_compressEnd(cc, out + pos, cap - pos, NULL);
        if (LZ4F_isError(r)) { *err = LZ4F_getErrorName(r); rc = 1; } else { pos += r; }
    }
    LZ4F_freeCompressionContext(cc);
    if (!rc) *out_len = pos;
    return rc;
}

static int l4_dstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, size_t chunk, const char** err) {
    (void)c; if (chunk == 0) chunk = 64 * 1024;
    LZ4F_dctx* d = NULL;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&d, LZ4F_VERSION))) {
        *err = "lz4 dctx"; return 1;
    }
    size_t cap = *out_len, opos = 0, ipos = 0;
    int rc = 0;
    while (ipos < in_len) {
        size_t src = in_len - ipos < chunk ? in_len - ipos : chunk;
        size_t dst = cap - opos;
        size_t hint = LZ4F_decompress(d, out + opos, &dst, in + ipos, &src, NULL);
        if (LZ4F_isError(hint)) { *err = LZ4F_getErrorName(hint); rc = 1; break; }
        opos += dst; ipos += src;
        if (hint == 0) break;  /* frame complete */
        if (src == 0 && dst == 0) { *err = "lz4 stalled"; rc = 1; break; }
    }
    LZ4F_freeDecompressionContext(d);
    if (!rc) *out_len = opos;
    return rc;
}

/* xz */
static int xz_cstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, int level, size_t chunk, const char** err) {
    (void)c; if (chunk == 0) chunk = 64 * 1024;
    lzma_stream s = LZMA_STREAM_INIT;
    if (lzma_easy_encoder(&s, (uint32_t)clamp_level(level - 1, 0, 9), LZMA_CHECK_CRC64) != LZMA_OK) {
        *err = "lzma encoder"; return 1;
    }
    size_t cap = *out_len;
    s.next_out = out; s.avail_out = cap;
    int rc = 0;
    for (size_t off = 0; off < in_len && !rc; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        s.next_in = in + off; s.avail_in = n;
        while (s.avail_in > 0) {
            if (lzma_code(&s, LZMA_RUN) != LZMA_OK) { *err = "lzma run"; rc = 1; break; }
        }
    }
    if (!rc) {
        lzma_ret r;
        do { r = lzma_code(&s, LZMA_FINISH); } while (r == LZMA_OK);
        if (r != LZMA_STREAM_END) { *err = "lzma finish"; rc = 1; }
    }
    if (!rc) *out_len = cap - s.avail_out;
    lzma_end(&s);
    return rc;
}

static int xz_dstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, size_t chunk, const char** err) {
    (void)c; if (chunk == 0) chunk = 64 * 1024;
    lzma_stream s = LZMA_STREAM_INIT;
    if (lzma_stream_decoder(&s, UINT64_MAX, 0) != LZMA_OK) { *err = "lzma decoder"; return 1; }
    size_t cap = *out_len;
    s.next_out = out; s.avail_out = cap;
    int rc = 0, done = 0;
    for (size_t off = 0; off < in_len && !rc && !done; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        s.next_in = in + off; s.avail_in = n;
        while (s.avail_in > 0) {
            lzma_ret r = lzma_code(&s, LZMA_RUN);
            if (r == LZMA_STREAM_END) { done = 1; break; }
            if (r != LZMA_OK) { *err = "lzma decode"; rc = 1; break; }
        }
    }
    if (!rc) *out_len = cap - s.avail_out;
    lzma_end(&s);
    return rc;
}

/* ---- registry ------------------------------------------------------------ */

static const bench_codec_t CODECS[] = {
    {"zstd", "libzstd", 0, z_bound, z_compress, z_decompress, z_cstream, z_dstream},
    {"brotli", "libbrotli", 0, br_bound, br_compress, br_decompress, br_cstream, br_dstream},
    {"zlib", "zlib", 0, zl_bound, zl_compress, zl_decompress, zl_cstream, zl_dstream},
    {"bz2", "libbz2", 0, bz_bound, bz_compress, bz_decompress, bz_cstream, bz_dstream},
    {"lz4", "liblz4", 0, l4_bound, l4_compress, l4_decompress, l4_cstream, l4_dstream},
    {"xz", "liblzma", 0, xz_bound, xz_compress, xz_decompress, xz_cstream, xz_dstream},
};
static const size_t N_CODECS = sizeof(CODECS) / sizeof(CODECS[0]);

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--info")) {
        return bench_info("c", "baseline", "c-baseline");
    }
    return bench_run("c", CODECS, N_CODECS);
}
