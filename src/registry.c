/*
 * registry.c — algorithm vtable lookup.
 *
 * Each algorithm exports a `cu_<algo>_vtbl` symbol from its own .c file,
 * conditionally compiled based on the INCLUDE_<ALGO> CMake options. This
 * file routes a cu_algorithm_t enum value to the right vtable.
 *
 * A switch statement (rather than an array indexed by enum value)
 * handles holes from disabled algorithms cleanly: unavailable algorithms
 * simply have no case and the default returns NULL.
 */

#include "algorithm_registry.h"
#include "compress_utils.h"

#ifdef INCLUDE_ZSTD
extern const cu_algorithm_vtbl_t cu_zstd_vtbl;
#endif
#ifdef INCLUDE_BROTLI
extern const cu_algorithm_vtbl_t cu_brotli_vtbl;
#endif
#ifdef INCLUDE_ZLIB
extern const cu_algorithm_vtbl_t cu_zlib_vtbl;
#endif
#ifdef INCLUDE_BZ2
extern const cu_algorithm_vtbl_t cu_bz2_vtbl;
#endif
#ifdef INCLUDE_LZ4
extern const cu_algorithm_vtbl_t cu_lz4_vtbl;
#endif
#ifdef INCLUDE_XZ
extern const cu_algorithm_vtbl_t cu_xz_vtbl;
#endif
#ifdef INCLUDE_SNAPPY
extern const cu_algorithm_vtbl_t cu_snappy_vtbl;
#endif

const cu_algorithm_vtbl_t* cu_registry_lookup(cu_algorithm_t algo) {
    switch (algo) {
#ifdef INCLUDE_ZSTD
        case CU_ALGO_ZSTD:   return &cu_zstd_vtbl;
#endif
#ifdef INCLUDE_BROTLI
        case CU_ALGO_BROTLI: return &cu_brotli_vtbl;
#endif
#ifdef INCLUDE_ZLIB
        case CU_ALGO_ZLIB:   return &cu_zlib_vtbl;
#endif
#ifdef INCLUDE_BZ2
        case CU_ALGO_BZ2:    return &cu_bz2_vtbl;
#endif
#ifdef INCLUDE_LZ4
        case CU_ALGO_LZ4:    return &cu_lz4_vtbl;
#endif
#ifdef INCLUDE_XZ
        case CU_ALGO_XZ:     return &cu_xz_vtbl;
        case CU_ALGO_LZMA:   return &cu_xz_vtbl;  /* alias */
#endif
#ifdef INCLUDE_SNAPPY
        case CU_ALGO_SNAPPY: return &cu_snappy_vtbl;
#endif
        default:             return NULL;
    }
}
