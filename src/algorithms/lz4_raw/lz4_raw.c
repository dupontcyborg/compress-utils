/* Raw LZ4 blocks, without framing or a private size prefix.
 * The caller supplies output capacity. Blocks cannot be streamed without
 * application framing, so stream creation deliberately returns unsupported.
 */
#include "algorithm_registry.h"
#include <lz4.h>
#include <limits.h>

static size_t raw_bound(size_t length) {
    return length <= LZ4_MAX_INPUT_SIZE ? (size_t)LZ4_compressBound((int)length) : 0;
}
static cu_status_t raw_compress(const uint8_t* input, size_t length,
                                uint8_t* output, size_t* capacity, int level) {
    (void)level; /* LZ4's fast block encoder has no compression level. */
    size_t bound = raw_bound(length);
    if (!bound || *capacity > INT_MAX) return CU_ERR_INVALID_ARG;
    if (*capacity < bound) { *capacity = bound; return CU_ERR_BUF_TOO_SMALL; }
    int written = LZ4_compress_default((const char*)input, (char*)output,
                                      (int)length, (int)*capacity);
    if (!written) return CU_ERR_COMPRESSION;
    *capacity = (size_t)written;
    return CU_OK;
}
static cu_status_t raw_decompress(const uint8_t* input, size_t length,
                                  uint8_t* output, size_t* capacity) {
    if (length > INT_MAX || *capacity > INT_MAX) return CU_ERR_INVALID_ARG;
    size_t limit = cu_get_max_decompressed_size();
    if (limit && *capacity > limit) return CU_ERR_SIZE_LIMIT;
    int written = LZ4_decompress_safe((const char*)input, (char*)output,
                                     (int)length, (int)*capacity);
    if (written < 0) return CU_ERR_DECOMPRESSION;
    *capacity = (size_t)written;
    return CU_OK;
}
static cu_status_t raw_size(const uint8_t* input, size_t length, size_t* size) {
    (void)input; (void)length; (void)size;
    return CU_ERR_SIZE_UNKNOWN;
}
const cu_algorithm_vtbl_t cu_lz4_raw_vtbl = {
    .name = "lz4_raw",
#ifndef CU_OMIT_COMPRESS
    .compress_bound = raw_bound,
    .compress = raw_compress,
#endif
#ifndef CU_OMIT_DECOMPRESS
    .decompress = raw_decompress,
    .decompress_size_hint = raw_size,
#endif
};
