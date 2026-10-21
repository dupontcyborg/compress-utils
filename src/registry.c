/*
 * registry.c — algorithm vtable lookup.
 *
 * Each algorithm exports a `cu_<algo>_vtbl` symbol from its own .c file,
 * conditionally compiled based on the INCLUDE_<ALGO> CMake options. This
 * file routes a cu_algorithm_t enum value to the right vtable.
 *
 * A table indexed by enum value, with designated initializers: disabled
 * algorithms simply have no entry, so their slot is NULL. (Not a switch:
 * coverage tools count the #ifdef lines between a switch's `return` cases as
 * code that never runs.)
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
#ifdef INCLUDE_DEFLATE
extern const cu_algorithm_vtbl_t cu_deflate_vtbl;
#endif
#ifdef INCLUDE_BZ2
extern const cu_algorithm_vtbl_t cu_bz2_vtbl;
#endif
#ifdef INCLUDE_LZ4
extern const cu_algorithm_vtbl_t cu_lz4_vtbl;
#endif
#ifdef INCLUDE_LZ4_RAW
extern const cu_algorithm_vtbl_t cu_lz4_raw_vtbl;
#endif
#ifdef INCLUDE_XZ
extern const cu_algorithm_vtbl_t cu_xz_vtbl;
#endif
#ifdef INCLUDE_SNAPPY
extern const cu_algorithm_vtbl_t cu_snappy_vtbl;
#endif
#ifdef INCLUDE_LZO
extern const cu_algorithm_vtbl_t cu_lzo_vtbl;
#endif
#ifdef INCLUDE_GZIP
extern const cu_algorithm_vtbl_t cu_gzip_vtbl;
#endif

static const cu_algorithm_vtbl_t* const REGISTRY[CU_ALGO_LZO + 1] = {
#ifdef INCLUDE_ZSTD
    [CU_ALGO_ZSTD]    = &cu_zstd_vtbl,
#endif
#ifdef INCLUDE_BROTLI
    [CU_ALGO_BROTLI]  = &cu_brotli_vtbl,
#endif
#ifdef INCLUDE_ZLIB
    [CU_ALGO_ZLIB]    = &cu_zlib_vtbl,
#endif
#ifdef INCLUDE_DEFLATE
    [CU_ALGO_DEFLATE] = &cu_deflate_vtbl,
#endif
#ifdef INCLUDE_BZ2
    [CU_ALGO_BZ2]     = &cu_bz2_vtbl,
#endif
#ifdef INCLUDE_LZ4
    [CU_ALGO_LZ4]     = &cu_lz4_vtbl,
#endif
#ifdef INCLUDE_LZ4_RAW
    [CU_ALGO_LZ4_RAW] = &cu_lz4_raw_vtbl,
#endif
#ifdef INCLUDE_XZ
    [CU_ALGO_XZ]      = &cu_xz_vtbl,
    [CU_ALGO_LZMA]    = &cu_xz_vtbl,  /* alias */
#endif
#ifdef INCLUDE_SNAPPY
    [CU_ALGO_SNAPPY]  = &cu_snappy_vtbl,
#endif
#ifdef INCLUDE_GZIP
    [CU_ALGO_GZIP]    = &cu_gzip_vtbl,
#endif
#ifdef INCLUDE_LZO
    [CU_ALGO_LZO]     = &cu_lzo_vtbl,
#endif
};

const cu_algorithm_vtbl_t* cu_registry_lookup(cu_algorithm_t algo) {
    /* Unsigned compare: out-of-range and negative values are both unknown. */
    if ((unsigned)algo >= sizeof(REGISTRY) / sizeof(REGISTRY[0])) return NULL;
    return REGISTRY[algo];
}
