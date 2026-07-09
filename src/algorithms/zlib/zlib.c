/*
 * zlib.c — zlib vtable for compress-utils.
 *
 * zlib and gzip are the same DEFLATE codec differing only in the wrapper the
 * zlib library writes (zlib = RFC 1950, windowBits 15). All the actual logic
 * lives in deflate_backend.h, shared with src/algorithms/gzip/gzip.c; this
 * file just binds the zlib windowBits into the vtable slots that need it.
 */

#include "deflate_backend.h"

/* windowBits-specific wrappers (the rest of the vtable is shared verbatim). */
static size_t zlib_compress_bound(size_t in_len) {
    return dfl_compress_bound(in_len, CU_DFL_ZLIB_WBITS);
}
static cu_status_t zlib_compress(const uint8_t* in, size_t in_len,
                                 uint8_t* out, size_t* out_len, int level) {
    return dfl_compress(in, in_len, out, out_len, level, CU_DFL_ZLIB_WBITS);
}
static cu_status_t zlib_decompress(const uint8_t* in, size_t in_len,
                                   uint8_t* out, size_t* out_len) {
    return dfl_decompress(in, in_len, out, out_len, CU_DFL_ZLIB_WBITS);
}
static cu_status_t zlib_cstream_create(int level, void** out_state) {
    return dfl_cstream_create(level, CU_DFL_ZLIB_WBITS, out_state);
}
static cu_status_t zlib_dstream_create(void** out_state) {
    return dfl_dstream_create(CU_DFL_ZLIB_WBITS, out_state);
}

const cu_algorithm_vtbl_t cu_zlib_vtbl = {
    .name                      = "zlib",
    /* Direction split (#7): see zstd.c for the CU_OMIT_* rationale. */
#ifndef CU_OMIT_COMPRESS
    .compress_bound            = zlib_compress_bound,
    .compress                  = zlib_compress,
    .compress_stream_create    = zlib_cstream_create,
    .compress_stream_write     = dfl_cstream_write,
    .compress_stream_finish    = dfl_cstream_finish,
    .compress_stream_destroy   = dfl_stream_destroy,
#endif
#ifndef CU_OMIT_DECOMPRESS
    .decompress                = zlib_decompress,
    .decompress_size_hint      = dfl_decompress_size_hint,
    .decompress_stream_create  = zlib_dstream_create,
    .decompress_stream_write   = dfl_dstream_write,
    .decompress_stream_finish  = dfl_dstream_finish,
    .decompress_stream_destroy = dfl_stream_destroy,
#endif
};
