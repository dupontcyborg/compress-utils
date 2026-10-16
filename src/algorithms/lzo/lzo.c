/* Raw LZO1X adapter; the MIT-licensed decoder lives in third_party/lzokay. */
#define CU_RAW_STREAM_LZO_ONLY /* leave out the LZ4 fast path */
#include "../../raw_block_stream.h"
#include "algorithm_registry.h"
#include "lzokay.h"

#include <stdint.h>

static cu_status_t lzo_status(lzokay_status_t status) {
    switch (status) {
        case LZOKAY_OK:
            return CU_OK;
        case LZOKAY_TRUNCATED:
            cu_set_last_error("truncated LZO1X input");
            return CU_ERR_TRUNCATED;
        case LZOKAY_OUTPUT_FULL:
            cu_set_last_error("LZO1X output exceeds capacity");
            return CU_ERR_BUF_TOO_SMALL;
        case LZOKAY_SIZE_LIMIT:
            cu_set_last_error("LZO1X length exceeds size limit");
            return CU_ERR_SIZE_LIMIT;
        default:
            cu_set_last_error("invalid LZO1X block");
            return CU_ERR_DECOMPRESSION;
    }
}

static cu_status_t lzo_decompress(const uint8_t* input, size_t length, uint8_t* output,
                                  size_t* output_length) {
    size_t limit = cu_get_max_decompressed_size();
    size_t capacity = *output_length;
    if (limit && capacity > limit) capacity = limit;
    size_t produced = 0;
    cu_status_t status = lzo_status(lzokay_decode(input, length, output, capacity, &produced));
    if (status == CU_ERR_BUF_TOO_SMALL && limit && *output_length >= limit)
        status = CU_ERR_SIZE_LIMIT;
    if (status == CU_OK) *output_length = produced;
    return status;
}
static cu_status_t lzo_size_hint(const uint8_t* input, size_t length, size_t* output_size) {
    size_t limit = cu_get_max_decompressed_size();
    cu_status_t status =
        lzo_status(lzokay_decode(input, length, NULL, limit ? limit : SIZE_MAX, output_size));
    return status == CU_ERR_BUF_TOO_SMALL ? CU_ERR_SIZE_LIMIT : status;
}
static cu_status_t lzo_stream_create(void** state) { return raw_stream_create(1, state); }
const cu_algorithm_vtbl_t cu_lzo_vtbl = {
    .name = "lzo",
    .decompress = lzo_decompress,
    .decompress_size_hint = lzo_size_hint,
    .decompress_stream_create = lzo_stream_create,
    .decompress_stream_write = raw_stream_write,
    .decompress_stream_finish = raw_stream_finish,
    .decompress_stream_destroy = raw_stream_destroy,
};
