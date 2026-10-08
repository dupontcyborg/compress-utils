/* Raw RFC 1951 DEFLATE, sharing the zlib backend with windowBits -15. */
#include "../zlib/deflate_backend.h"

/* windowBits-specific wrappers (the rest of the vtable is shared verbatim). */
static size_t raw_deflate_compress_bound(size_t in_len) {
    return dfl_compress_bound(in_len, (-15));
}
static cu_status_t raw_deflate_compress(const uint8_t* in, size_t in_len,
                                 uint8_t* out, size_t* out_len, int level) {
    return dfl_compress(in, in_len, out, out_len, level, (-15));
}
static cu_status_t raw_deflate_decompress(const uint8_t* in, size_t in_len,
                                   uint8_t* out, size_t* out_len) {
    return dfl_decompress(in, in_len, out, out_len, (-15));
}
static cu_status_t raw_deflate_cstream_create(int level, void** out_state) {
    return dfl_cstream_create(level, (-15), out_state);
}
static cu_status_t raw_deflate_dstream_create(void** out_state) {
    return dfl_dstream_create((-15), out_state);
}

const cu_algorithm_vtbl_t cu_deflate_vtbl = {
    .name                      = "deflate",
    /* Direction split (#7): see zstd.c for the CU_OMIT_* rationale. */
#ifndef CU_OMIT_COMPRESS
    .compress_bound            = raw_deflate_compress_bound,
    .compress                  = raw_deflate_compress,
    .compress_stream_create    = raw_deflate_cstream_create,
    .compress_stream_write     = dfl_cstream_write,
    .compress_stream_finish    = dfl_cstream_finish,
    .compress_stream_destroy   = dfl_stream_destroy,
#endif
#ifndef CU_OMIT_DECOMPRESS
    .decompress                = raw_deflate_decompress,
    .decompress_size_hint      = dfl_decompress_size_hint,
    .decompress_stream_create  = raw_deflate_dstream_create,
    .decompress_stream_write   = dfl_dstream_write,
    .decompress_stream_finish  = dfl_dstream_finish,
    .decompress_stream_destroy = dfl_stream_destroy,
#endif
};
