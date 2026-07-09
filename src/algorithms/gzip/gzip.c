/*
 * gzip.c — gzip vtable for compress-utils.
 *
 * gzip is the same DEFLATE codec as zlib, differing only in the wrapper the
 * zlib library writes: gzip = RFC 1952 (10-byte header + CRC-32 + ISIZE
 * trailer), selected with windowBits 31 (15 + 16). All logic lives in
 * ../zlib/deflate_backend.h, shared verbatim with zlib.c; this file just binds
 * the gzip windowBits into the vtable slots that need it.
 *
 * Output is a standard gzip stream — interoperable with the `gzip` CLI, `.gz`
 * files, Python's gzip/zlib(wbits=31), and any RFC 1952 reader. Like zlib, the
 * stream carries no cheap decompressed-size probe (gzip's ISIZE is mod 2^32
 * and needs the whole stream), so size_hint reports unknown.
 */

#include "../zlib/deflate_backend.h"

/* windowBits-specific wrappers (the rest of the vtable is shared verbatim). */
static size_t gzip_compress_bound(size_t in_len) {
    return dfl_compress_bound(in_len, CU_DFL_GZIP_WBITS);
}
static cu_status_t gzip_compress(const uint8_t* in, size_t in_len,
                                 uint8_t* out, size_t* out_len, int level) {
    return dfl_compress(in, in_len, out, out_len, level, CU_DFL_GZIP_WBITS);
}
static cu_status_t gzip_decompress(const uint8_t* in, size_t in_len,
                                   uint8_t* out, size_t* out_len) {
    return dfl_decompress(in, in_len, out, out_len, CU_DFL_GZIP_WBITS);
}
static cu_status_t gzip_cstream_create(int level, void** out_state) {
    return dfl_cstream_create(level, CU_DFL_GZIP_WBITS, out_state);
}
static cu_status_t gzip_dstream_create(void** out_state) {
    return dfl_dstream_create(CU_DFL_GZIP_WBITS, out_state);
}

const cu_algorithm_vtbl_t cu_gzip_vtbl = {
    .name                      = "gzip",
    /* Direction split (#7): see zstd.c for the CU_OMIT_* rationale. */
#ifndef CU_OMIT_COMPRESS
    .compress_bound            = gzip_compress_bound,
    .compress                  = gzip_compress,
    .compress_stream_create    = gzip_cstream_create,
    .compress_stream_write     = dfl_cstream_write,
    .compress_stream_finish    = dfl_cstream_finish,
    .compress_stream_destroy   = dfl_stream_destroy,
#endif
#ifndef CU_OMIT_DECOMPRESS
    .decompress                = gzip_decompress,
    .decompress_size_hint      = dfl_decompress_size_hint,
    .decompress_stream_create  = gzip_dstream_create,
    .decompress_stream_write   = dfl_dstream_write,
    .decompress_stream_finish  = dfl_dstream_finish,
    .decompress_stream_destroy = dfl_stream_destroy,
#endif
};
