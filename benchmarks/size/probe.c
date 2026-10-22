/*
 * Binary-size probe: the smallest program that uses one library for one
 * format, compressing and decompressing (or, with -DDECODE_ONLY, only
 * decompressing). size.py links it statically with dead-stripping, strips it,
 * and subtracts an empty probe (-DPROBE_NONE), so the difference is what the
 * library adds to a program. Arguments come from argc so nothing folds away.
 *
 * Select the library with one -DPROBE_<NAME> (see size.py).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t in[256], out[1024], dec[256];

#if defined(PROBE_NONE)
static int probe(size_t n) { return (int)n + in[0] + out[0] + dec[0]; }

/* ---- compress-utils (one codec, chosen with -DCU_PROBE_ALGO) ------------- */
#elif defined(PROBE_CU)
#include "compress_utils.h"
static int probe(size_t n) {
    size_t ol = sizeof(out), dl = sizeof(dec);
#ifndef DECODE_ONLY
    if (cu_compress(CU_PROBE_ALGO, in, n, out, &ol, (int)n) != CU_OK) return 1;
#endif
    return cu_decompress(CU_PROBE_ALGO, out, ol, dec, &dl) != CU_OK;
}

/* ---- zlib API: madler zlib, zlib-ng (zng_), zlib-rs, miniz (mz_) --------- */
#elif defined(PROBE_ZLIB) || defined(PROBE_ZLIB_NG) || defined(PROBE_ZLIB_RS) || \
    defined(PROBE_MINIZ)
#if defined(PROBE_ZLIB_NG)
#include <zlib-ng.h>
#define ZFN(f) zng_##f
#define ZLEN size_t
#elif defined(PROBE_MINIZ)
#include <miniz/miniz.h>
#define ZFN(f) mz_##f
#define ZLEN mz_ulong
#else
#include <zlib.h>
#define ZFN(f) f
#define ZLEN uLongf
#endif
static int probe(size_t n) {
    ZLEN ol = sizeof(out), dl = sizeof(dec);
#ifndef DECODE_ONLY
    if (ZFN(compress2)(out, &ol, in, (ZLEN)n, (int)n) != 0) return 1;
#endif
    return ZFN(uncompress)(dec, &dl, out, ol) != 0;
}

#elif defined(PROBE_LIBDEFLATE)
#include <libdeflate.h>
static int probe(size_t n) {
    size_t ol = sizeof(out), dl = 0;
#ifndef DECODE_ONLY
    struct libdeflate_compressor* c = libdeflate_alloc_compressor((int)n);
    ol = libdeflate_zlib_compress(c, in, n, out, sizeof(out));
    libdeflate_free_compressor(c);
#endif
    struct libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    int r = libdeflate_zlib_decompress(d, out, ol, dec, sizeof(dec), &dl);
    libdeflate_free_decompressor(d);
    return r;
}

#elif defined(PROBE_ISA_L)
#include <isa-l/igzip_lib.h>
static int probe(size_t n) {
    struct isal_zstream s;
    size_t ol = sizeof(out);
#ifndef DECODE_ONLY
    static uint8_t lvl_buf[ISAL_DEF_LVL1_DEFAULT];
    isal_deflate_stateless_init(&s);
    s.level = (uint32_t)n;
    s.level_buf = lvl_buf;
    s.level_buf_size = sizeof(lvl_buf);
    s.gzip_flag = IGZIP_ZLIB;
    s.next_in = in;
    s.avail_in = (uint32_t)n;
    s.next_out = out;
    s.avail_out = sizeof(out);
    s.end_of_stream = 1;
    if (isal_deflate_stateless(&s) != COMP_OK) return 1;
    ol = s.total_out;
#endif
    struct inflate_state st;
    isal_inflate_init(&st);
    st.next_in = out;
    st.avail_in = (uint32_t)ol;
    st.next_out = dec;
    st.avail_out = sizeof(dec);
    st.crc_flag = ISAL_ZLIB;
    return isal_inflate_stateless(&st) != ISAL_DECOMP_OK;
}

/* ---- bzip2 API: libbzip2, libbz2-rs --------------------------------------- */
#elif defined(PROBE_BZ2) || defined(PROBE_LIBBZ2_RS)
#include <bzlib.h>
#if defined(PROBE_LIBBZ2_RS)
void bz_internal_error(int errcode) { exit(errcode); }
#endif
static int probe(size_t n) {
    unsigned int ol = sizeof(out), dl = sizeof(dec);
#ifndef DECODE_ONLY
    if (BZ2_bzBuffToBuffCompress((char*)out, &ol, (char*)in, (unsigned)n, (int)n, 0, 0)) return 1;
#endif
    return BZ2_bzBuffToBuffDecompress((char*)dec, &dl, (char*)out, ol, 0, 0) != 0;
}

#elif defined(PROBE_LZMA)
#include <lzma.h>
static int probe(size_t n) {
    size_t ol = 0, ipos = 0, opos = 0;
    uint64_t memlimit = UINT64_MAX;
#ifndef DECODE_ONLY
    if (lzma_easy_buffer_encode((uint32_t)n, LZMA_CHECK_CRC64, NULL, in, n, out, &ol,
                                sizeof(out)) != LZMA_OK)
        return 1;
#else
    ol = n;
#endif
    return lzma_stream_buffer_decode(&memlimit, 0, NULL, out, &ipos, ol, dec, &opos,
                                     sizeof(dec)) != LZMA_OK;
}

#elif defined(PROBE_LZMA_SDK)
#include "7zCrc.h"
#include "Alloc.h"
#include "Xz.h"
#include "XzCrc64.h"
#include "XzEnc.h"
typedef struct { ISeqInStream vt; size_t left; } BufIn;
static SRes bufin_read(ISeqInStreamPtr p, void* buf, size_t* size) {
    BufIn* b = (BufIn*)(void*)p;
    size_t k = *size < b->left ? *size : b->left;
    memcpy(buf, in, k);
    b->left -= k;
    *size = k;
    return SZ_OK;
}
typedef struct { ISeqOutStream vt; size_t pos; } BufOut;
static size_t bufout_write(ISeqOutStreamPtr p, const void* buf, size_t size) {
    BufOut* b = (BufOut*)(void*)p;
    if (size > sizeof(out) - b->pos) size = sizeof(out) - b->pos;
    memcpy(out + b->pos, buf, size);
    b->pos += size;
    return size;
}
static int probe(size_t n) {
    CrcGenerateTable();
    Crc64GenerateTable();
    SizeT ol = n;
#ifndef DECODE_ONLY
    BufIn bi = {{bufin_read}, n};
    BufOut bo = {{bufout_write}, 0};
    CXzProps props;
    XzProps_Init(&props);
    props.lzma2Props.lzmaProps.level = (int)n;
    if (Xz_Encode(&bo.vt, &bi.vt, &props, NULL) != SZ_OK) return 1;
    ol = bo.pos;
#endif
    CXzUnpacker u;
    XzUnpacker_Construct(&u, &g_Alloc);
    XzUnpacker_Init(&u);
    SizeT dl = sizeof(dec);
    ECoderStatus status;
    SRes r = XzUnpacker_CodeFull(&u, dec, &dl, out, &ol, CODER_FINISH_END, &status);
    XzUnpacker_Free(&u);
    return r != SZ_OK;
}

#elif defined(PROBE_XZ_EMBEDDED)
#ifndef DECODE_ONLY
#error "xz-embedded is a decoder only"
#endif
#include "xz.h"
static int probe(size_t n) {
    xz_crc32_init();
    xz_crc64_init();
    struct xz_dec* s = xz_dec_init(XZ_SINGLE, 0);
    struct xz_buf b = {out, 0, n, dec, 0, sizeof(dec)};
    enum xz_ret r = xz_dec_run(s, &b);
    xz_dec_end(s);
    return r != XZ_STREAM_END;
}

#elif defined(PROBE_ZSTD)
#include "zstd.h"
static int probe(size_t n) {
    size_t ol = n;
#ifndef DECODE_ONLY
    ol = ZSTD_compress(out, sizeof(out), in, n, (int)n);
#endif
    return ZSTD_isError(ZSTD_decompress(dec, sizeof(dec), out, ol));
}

#elif defined(PROBE_LZ4)
#include "lz4frame.h"
static int probe(size_t n) {
    size_t ol = n;
#ifndef DECODE_ONLY
    LZ4F_preferences_t prefs;
    memset(&prefs, 0, sizeof(prefs));
    prefs.compressionLevel = (int)n;
    ol = LZ4F_compressFrame(out, sizeof(out), in, n, &prefs);
#endif
    LZ4F_dctx* d;
    LZ4F_createDecompressionContext(&d, LZ4F_VERSION);
    size_t dl = sizeof(dec), sl = ol;
    size_t r = LZ4F_decompress(d, dec, &dl, out, &sl, NULL);
    LZ4F_freeDecompressionContext(d);
    return LZ4F_isError(r);
}

#elif defined(PROBE_BROTLI)
#include "brotli/decode.h"
#include "brotli/encode.h"
static int probe(size_t n) {
    size_t ol = n, dl = sizeof(dec);
#ifndef DECODE_ONLY
    ol = sizeof(out);
    if (!BrotliEncoderCompress((int)n, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC, n, in, &ol, out))
        return 1;
#endif
    return BrotliDecoderDecompress(ol, out, &dl, dec) != BROTLI_DECODER_RESULT_SUCCESS;
}

#elif defined(PROBE_SNAPPY_C)
#include "snappy.h"
static int probe(size_t n) {
    size_t ol = n;
#ifndef DECODE_ONLY
    struct snappy_env env;
    snappy_init_env(&env);
    snappy_compress(&env, (const char*)in, n, (char*)out, &ol);
    snappy_free_env(&env);
#endif
    return snappy_uncompress((const char*)out, ol, (char*)dec);
}

#elif defined(PROBE_GOOGLE_SNAPPY)
#include <snappy-c.h>
static int probe(size_t n) {
    size_t ol = sizeof(out), dl = sizeof(dec);
#ifndef DECODE_ONLY
    if (snappy_compress((const char*)in, n, (char*)out, &ol) != SNAPPY_OK) return 1;
#else
    ol = n;
#endif
    return snappy_uncompress((const char*)out, ol, (char*)dec, &dl) != SNAPPY_OK;
}

#else
#error "define one PROBE_<NAME> (see size.py)"
#endif

int main(int argc, char** argv) {
    (void)argv;
    return probe((size_t)argc);
}
