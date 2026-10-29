/*
 * Native-library baseline driver.
 *
 * Same language (C), same harness, same protocol as bench.c — but each codec
 * calls the upstream library directly instead of going through the cu_* ABI.
 * This isolates compress-utils' wrapper overhead: the same native level (the
 * runner translates each job's level through benchmarks/levels.json) and frame
 * settings (mirrored from src/algorithms/<algo>/<algo>.c), so any ratio
 * difference is a bug and any speed difference is dispatch cost.
 *
 * Records carry impl="lib<algo>". Run alongside the compress-utils driver and
 * report.py overlays them per algorithm.
 *
 * Mirrored frame settings (keep in sync with the wrappers):
 *   zstd   ZSTD_compress2 + pledgedSrcSize
 *   brotli BrotliEncoderCompress, default window/mode
 *   zlib   compress2 / uncompress
 *   gzip, deflate  deflateInit2 windowBits 31 / -15 (bench_zapi.h), one-shot only
 *   bz2    BZ2_bzBuffToBuff{Compress,Decompress}, wf=0
 *   lz4    LZ4F frame, contentSize + checksum + linked
 *   xz     lzma_easy_buffer_encode, CRC64
 *   snappy our first-party core (src/algorithms/snappy/core) called directly, one-shot only
 */

#include <stdint.h>

#include "brotli/decode.h"
#include "brotli/encode.h"
#include "bzlib.h"
#include "lz4frame.h"
#include "lzma.h"
#include "snappy_core.h"
#include "zlib.h"
#include "zstd.h"
#include "zstd_errors.h"

#include "bench_harness.h"

#define ZPFX
#define ZSTREAM z_stream
#define ZLEN uLongf
#include "bench_zapi.h"

/* ---- zstd ---------------------------------------------------------------- */

static size_t z_bound(const bench_codec_t* c, size_t n) { (void)c; return ZSTD_compressBound(n); }

static int z_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, int level, const char** err) {
    (void)c;
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    if (!cctx) { *err = "ZSTD_createCCtx failed"; return 1; }
    ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level);
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
    if (ZSTD_isError(r)) {
        if (ZSTD_getErrorCode(r) == ZSTD_error_dstSize_tooSmall) return BENCH_NEED_MORE;
        *err = ZSTD_getErrorName(r);
        return 1;
    }
    *out_len = r;
    return 0;
}

/* The frame header's content size; unknown (streamed frames) or an error counts as none. */
static int z_hint(const bench_codec_t* c, const uint8_t* in, size_t in_len, size_t* out_size) {
    (void)c;
    unsigned long long n = ZSTD_getFrameContentSize(in, in_len);
    if (n == ZSTD_CONTENTSIZE_UNKNOWN || n == ZSTD_CONTENTSIZE_ERROR) return 1;
    *out_size = (size_t)n;
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
    BROTLI_BOOL ok = BrotliEncoderCompress(level, BROTLI_DEFAULT_WINDOW,
                                           BROTLI_DEFAULT_MODE, in_len, in, &enc, out);
    if (!ok) { *err = "BrotliEncoderCompress failed"; return 1; }
    *out_len = enc;
    return 0;
}

static int br_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                         uint8_t* out, size_t* out_len, const char** err) {
    (void)c;
    /* BrotliDecoderDecompress can't tell "too small" from "corrupt", so drive the
     * streaming decoder over the whole input into the fixed buffer, as callers do. */
    BrotliDecoderState* d = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (!d) { *err = "BrotliDecoderCreateInstance"; return 1; }
    size_t avail_in = in_len, avail_out = *out_len;
    const uint8_t* next_in = in;
    uint8_t* next_out = out;
    BrotliDecoderResult r = BrotliDecoderDecompressStream(d, &avail_in, &next_in, &avail_out,
                                                          &next_out, NULL);
    BrotliDecoderDestroyInstance(d);
    if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) return BENCH_NEED_MORE;
    if (r != BROTLI_DECODER_RESULT_SUCCESS) { *err = "BrotliDecoderDecompressStream failed"; return 1; }
    *out_len = *out_len - avail_out;
    return 0;
}

/* ---- zlib ---------------------------------------------------------------- */

static size_t zl_bound(const bench_codec_t* c, size_t n) { (void)c; return compressBound((uLong)n); }

static int zl_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                       uint8_t* out, size_t* out_len, int level, const char** err) {
    (void)c;
    uLongf cap = (uLongf)*out_len;
    int r = compress2(out, &cap, in, (uLong)in_len, level);
    if (r != Z_OK) { *err = "zlib compress2 failed"; return 1; }
    *out_len = cap;
    return 0;
}

static int zl_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                         uint8_t* out, size_t* out_len, const char** err) {
    (void)c;
    uLongf cap = (uLongf)*out_len;
    int r = uncompress(out, &cap, in, (uLong)in_len);
    if (r == Z_BUF_ERROR) return BENCH_NEED_MORE;
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
                                     (unsigned int)in_len, level, 0, 0);
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
    if (r == BZ_OUTBUFF_FULL) return BENCH_NEED_MORE;
    if (r != BZ_OK) { *err = "BZ2 decompress failed"; return 1; }
    *out_len = cap;
    return 0;
}

/* ---- lz4 (frame format) -------------------------------------------------- */

static void lz4_prefs(LZ4F_preferences_t* p, int level, size_t content_size) {
    memset(p, 0, sizeof(*p));
    p->compressionLevel = level;
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
    for (;;) {
        size_t dst = out_cap - out_pos;
        size_t src = in_len - in_pos;
        size_t hint = LZ4F_decompress(dctx, out + out_pos, &dst, in + in_pos, &src, NULL);
        if (LZ4F_isError(hint)) { *err = LZ4F_getErrorName(hint); rc = 1; break; }
        out_pos += dst;
        in_pos += src;
        if (hint == 0) break;        /* frame complete */
        if (dst == 0 && src == 0) {  /* no progress: out of room, or out of input */
            if (out_pos == out_cap) rc = BENCH_NEED_MORE;
            else { *err = "LZ4F truncated"; rc = 1; }
            break;
        }
    }
    LZ4F_freeDecompressionContext(dctx);
    if (!rc) *out_len = out_pos;
    return rc;
}

/* The frame header's content size flag; the library reads it from the header. */
static int l4_hint(const bench_codec_t* c, const uint8_t* in, size_t in_len, size_t* out_size) {
    (void)c;
    LZ4F_dctx* dctx = NULL;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION))) return 1;
    LZ4F_frameInfo_t info;
    size_t src = in_len;
    int rc = 1;
    if (!LZ4F_isError(LZ4F_getFrameInfo(dctx, &info, in, &src)) && info.contentSize) {
        *out_size = (size_t)info.contentSize;
        rc = 0;
    }
    LZ4F_freeDecompressionContext(dctx);
    return rc;
}

/* ---- xz ------------------------------------------------------------------ */

static size_t xz_bound(const bench_codec_t* c, size_t n) { (void)c; return lzma_stream_buffer_bound(n); }

static int xz_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                       uint8_t* out, size_t* out_len, int level, const char** err) {
    (void)c;
    uint32_t preset = (uint32_t)level;
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
    /* LZMA_BUF_ERROR means the output was too small (the API resets out_pos on
     * error, so it can't tell that from truncated input; ours never is). */
    if (r == LZMA_BUF_ERROR) return BENCH_NEED_MORE;
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
    ZSTD_CCtx_setParameter(z, ZSTD_c_compressionLevel, level);
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
    size_t win = *out_len, total = 0;
    int rc = 0, done = 0;
    for (size_t off = 0; off < in_len && !rc && !done; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        ZSTD_inBuffer ib = {in + off, n, 0};
        ZSTD_outBuffer ob = {out, win, 0};
        do {  /* until the chunk is consumed and the window wasn't filled */
            ob.pos = 0;
            size_t r = ZSTD_decompressStream(z, &ob, &ib);
            if (ZSTD_isError(r)) { *err = ZSTD_getErrorName(r); rc = 1; break; }
            bench_sink_write(out, ob.pos);
            total += ob.pos;
            if (r == 0) { done = 1; break; }  /* frame complete */
        } while (ib.pos < ib.size || ob.pos == ob.size);
    }
    ZSTD_freeDCtx(z);
    if (!rc) *out_len = total;
    return rc;
}

/* brotli */
static int br_cstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, int level, size_t chunk, const char** err) {
    (void)c; if (chunk == 0) chunk = 64 * 1024;
    BrotliEncoderState* e = BrotliEncoderCreateInstance(NULL, NULL, NULL);
    if (!e) { *err = "BrotliEncoderCreateInstance"; return 1; }
    BrotliEncoderSetParameter(e, BROTLI_PARAM_QUALITY, level);
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
    size_t win = *out_len, total = 0;
    int rc = 0, done = 0;
    for (size_t off = 0; off < in_len && !rc && !done; off += chunk) {
        size_t avail_in = in_len - off < chunk ? in_len - off : chunk;
        const uint8_t* next_in = in + off;
        for (;;) {
            size_t avail_out = win;
            uint8_t* next_out = out;
            BrotliDecoderResult r = BrotliDecoderDecompressStream(
                dec, &avail_in, &next_in, &avail_out, &next_out, NULL);
            bench_sink_write(out, win - avail_out);
            total += win - avail_out;
            if (r == BROTLI_DECODER_RESULT_SUCCESS) { done = 1; break; }
            if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) break;
            if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) continue;  /* drain */
            *err = "brotli decode"; rc = 1; break;
        }
    }
    BrotliDecoderDestroyInstance(dec);
    if (!rc) *out_len = total;
    return rc;
}

/* zlib */
static int zl_cstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, int level, size_t chunk, const char** err) {
    (void)c; if (chunk == 0) chunk = 64 * 1024;
    z_stream s; memset(&s, 0, sizeof(s));
    if (deflateInit(&s, level) != Z_OK) { *err = "deflateInit"; return 1; }
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
    size_t win = *out_len, total = 0;
    int rc = 0, done = 0;
    for (size_t off = 0; off < in_len && !rc && !done; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        s.next_in = (Bytef*)(uintptr_t)(in + off); s.avail_in = (uInt)n;
        do {
            s.next_out = out; s.avail_out = (uInt)win;
            int r = inflate(&s, Z_NO_FLUSH);
            bench_sink_write(out, win - s.avail_out);
            total += win - s.avail_out;
            if (r == Z_STREAM_END) { done = 1; break; }
            if (r != Z_OK && !(r == Z_BUF_ERROR && s.avail_in == 0)) { *err = "inflate"; rc = 1; break; }
        } while (s.avail_in > 0 || s.avail_out == 0);
    }
    if (!rc) *out_len = total;
    inflateEnd(&s);
    return rc;
}

/* bz2 */
static int bz_cstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, int level, size_t chunk, const char** err) {
    (void)c; if (chunk == 0) chunk = 64 * 1024;
    bz_stream s; memset(&s, 0, sizeof(s));
    if (BZ2_bzCompressInit(&s, level, 0, 0) != BZ_OK) {
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
    size_t win = *out_len, total = 0;
    int rc = 0, done = 0;
    for (size_t off = 0; off < in_len && !rc && !done; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        s.next_in = (char*)(uintptr_t)(in + off); s.avail_in = (unsigned int)n;
        do {
            s.next_out = (char*)out; s.avail_out = (unsigned int)win;
            int r = BZ2_bzDecompress(&s);
            bench_sink_write(out, win - s.avail_out);
            total += win - s.avail_out;
            if (r == BZ_STREAM_END) { done = 1; break; }
            if (r != BZ_OK) { *err = "bzDecompress"; rc = 1; break; }
        } while (s.avail_in > 0 || s.avail_out == 0);
    }
    if (!rc) *out_len = total;
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
    size_t win = *out_len, total = 0, ipos = 0;
    int rc = 0;
    for (;;) {
        size_t src = in_len - ipos < chunk ? in_len - ipos : chunk;
        size_t dst = win;
        size_t hint = LZ4F_decompress(d, out, &dst, in + ipos, &src, NULL);
        if (LZ4F_isError(hint)) { *err = LZ4F_getErrorName(hint); rc = 1; break; }
        bench_sink_write(out, dst);
        total += dst; ipos += src;
        if (hint == 0) break;  /* frame complete */
        if (src == 0 && dst == 0) { *err = "lz4 stalled"; rc = 1; break; }
    }
    LZ4F_freeDecompressionContext(d);
    if (!rc) *out_len = total;
    return rc;
}

/* xz */
static int xz_cstream(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                      uint8_t* out, size_t* out_len, int level, size_t chunk, const char** err) {
    (void)c; if (chunk == 0) chunk = 64 * 1024;
    lzma_stream s = LZMA_STREAM_INIT;
    if (lzma_easy_encoder(&s, (uint32_t)level, LZMA_CHECK_CRC64) != LZMA_OK) {
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
    size_t win = *out_len, total = 0;
    int rc = 0, done = 0;
    for (size_t off = 0; off < in_len && !rc && !done; off += chunk) {
        size_t n = in_len - off < chunk ? in_len - off : chunk;
        s.next_in = in + off; s.avail_in = n;
        do {
            s.next_out = out; s.avail_out = win;
            lzma_ret r = lzma_code(&s, LZMA_RUN);
            bench_sink_write(out, win - s.avail_out);
            total += win - s.avail_out;
            if (r == LZMA_STREAM_END) { done = 1; break; }
            if (r != LZMA_OK && !(r == LZMA_BUF_ERROR && s.avail_in == 0)) {
                *err = "lzma decode"; rc = 1; break;
            }
        } while (s.avail_in > 0 || s.avail_out == 0);
    }
    if (!rc) *out_len = total;
    lzma_end(&s);
    return rc;
}

/* ---- snappy (one-shot, as src/algorithms/snappy/snappy.c) --------------- */

static size_t sn_bound(const bench_codec_t* c, size_t n) {
    (void)c;
    return snappy_core_max_compressed_length(n);
}

static int sn_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                       size_t* out_len, int level, const char** err) {
    (void)c;
    (void)level;
    size_t n = 0;
    if (snappy_core_encode(in, in_len, out, *out_len, &n) != SNAPPY_CORE_OK) {
        *err = "snappy_core_encode";
        return 1;
    }
    *out_len = n;
    return 0;
}

static int sn_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len, uint8_t* out,
                         size_t* out_len, const char** err) {
    (void)c;
    size_t needed = 0;
    if (snappy_core_uncompressed_length(in, in_len, &needed) != SNAPPY_CORE_OK) {
        *err = "snappy_core_uncompressed_length";
        return 1;
    }
    if (needed > *out_len) return BENCH_NEED_MORE;
    size_t n = 0;
    if (snappy_core_decode(in, in_len, out, needed, &n) != SNAPPY_CORE_OK) {
        *err = "snappy_core_decode";
        return 1;
    }
    *out_len = n;
    return 0;
}

/* The length varint at the start of the stream. */
static int sn_hint(const bench_codec_t* c, const uint8_t* in, size_t in_len, size_t* out_size) {
    (void)c;
    return snappy_core_uncompressed_length(in, in_len, out_size) != SNAPPY_CORE_OK;
}

/* ---- registry ------------------------------------------------------------ */

static const bench_codec_t CODECS[] = {
    {"zstd", "libzstd", 0, z_bound, z_compress, z_decompress, z_cstream, z_dstream, 0, 0, z_hint},
    {"brotli", "libbrotli", 0, br_bound, br_compress, br_decompress, br_cstream, br_dstream},
    {"zlib", "zlib", 0, zl_bound, zl_compress, zl_decompress, zl_cstream, zl_dstream},
    {"bz2", "libbz2", 0, bz_bound, bz_compress, bz_decompress, bz_cstream, bz_dstream},
    {"lz4", "liblz4", 0, l4_bound, l4_compress, l4_decompress, l4_cstream, l4_dstream, 0, 0, l4_hint},
    {"xz", "liblzma", 0, xz_bound, xz_compress, xz_decompress, xz_cstream, xz_dstream},
    ZA_GZIP("zlib", 0, 0),
    ZA_DEFLATE("zlib", 0, 0),
    {"snappy", "snappy-core", 0, sn_bound, sn_compress, sn_decompress, NULL, NULL, 0, 0, sn_hint},
};
static const size_t N_CODECS = sizeof(CODECS) / sizeof(CODECS[0]);

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--info")) {
        return bench_info("c", "baseline", "c-baseline");
    }
    return bench_run("c", CODECS, N_CODECS);
}
