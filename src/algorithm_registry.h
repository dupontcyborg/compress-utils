/*
 * algorithm_registry.h — internal interface for per-algorithm dispatch.
 *
 * Each algorithm implements a cu_algorithm_vtbl_t and exports a single
 * pointer to it (e.g., cu_zstd_vtbl). registry.c collects these pointers
 * into a fixed-index array keyed by cu_algorithm_t.
 *
 * This header is internal — consumers must not include it.
 */

#ifndef CU_ALGORITHM_REGISTRY_H
#define CU_ALGORITHM_REGISTRY_H

#include "compress_utils.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Algorithm vtable. Streaming functions operate on an opaque algorithm-
 * specific state pointer, owned by the cu_compress_stream / cu_decompress_stream
 * wrapper structs in compress_utils.c.
 */
typedef struct cu_algorithm_vtbl {
    const char* name;

    /* One-shot. */
    size_t      (*compress_bound)(size_t in_len);
    cu_status_t (*compress)(const uint8_t* in, size_t in_len,
                            uint8_t* out, size_t* out_len, int level);
    cu_status_t (*decompress)(const uint8_t* in, size_t in_len,
                              uint8_t* out, size_t* out_len);
    cu_status_t (*decompress_size_hint)(const uint8_t* in, size_t in_len,
                                        size_t* out_size);

    /* Streaming compression. State is owned by the caller; vtable
     * functions allocate it in `create` and free it in `destroy`. */
    cu_status_t (*compress_stream_create)(int level, void** out_state);
    cu_status_t (*compress_stream_write)(void* state,
                                         const uint8_t* in, size_t in_len,
                                         uint8_t* out, size_t* out_len);
    cu_status_t (*compress_stream_finish)(void* state,
                                          uint8_t* out, size_t* out_len);
    /* Optional: emit everything needed to decode the input so far, keeping
     * the frame open. NULL for codecs that can't (Snappy, bzip2). */
    cu_status_t (*compress_stream_flush)(void* state,
                                         uint8_t* out, size_t* out_len);
    void        (*compress_stream_destroy)(void* state);

    /* Streaming decompression. */
    cu_status_t (*decompress_stream_create)(void** out_state);
    cu_status_t (*decompress_stream_write)(void* state,
                                           const uint8_t* in, size_t in_len,
                                           uint8_t* out, size_t* out_len);
    cu_status_t (*decompress_stream_finish)(void* state,
                                            uint8_t* out, size_t* out_len);
    void        (*decompress_stream_destroy)(void* state);

    /* Optional single-stream decoding (CU_DSTREAM_SINGLE); NULL for codecs
     * without it (raw LZ4, LZO). create_single makes a state, driven by the
     * write/finish/destroy entries above, that stops at the end of the first
     * frame and keeps the input after it. stream_end reports whether that end
     * has been decoded and, if so, the kept input (unused/unused_len may be
     * NULL when only the flag is wanted). Once ended, write is only called
     * with no input, to drain output still held back, and must return CU_OK
     * once nothing is; finish before the end returns CU_ERR_TRUNCATED. */
    cu_status_t (*decompress_stream_create_single)(void** out_state);
    int         (*decompress_stream_end)(const void* state,
                                         const uint8_t** unused, size_t* unused_len);
} cu_algorithm_vtbl_t;

/*
 * Returns the vtable for an algorithm, or NULL if the algorithm value is
 * out of range or was not compiled into this build (INCLUDE_<ALGO> off).
 */
const cu_algorithm_vtbl_t* cu_registry_lookup(cu_algorithm_t algo);

/* Internal error-message setter used by algorithm implementations. */
void cu_set_last_error(const char* msg);
void cu_set_last_errorf(const char* fmt, ...);

/* Internal cap used by one-shot decompression. */
size_t cu_get_max_decompressed_size(void);

#ifdef __cplusplus
}
#endif

#endif  /* CU_ALGORITHM_REGISTRY_H */
