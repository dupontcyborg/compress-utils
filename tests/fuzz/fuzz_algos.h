/*
 * fuzz_algos.h — the fuzzer's algorithm selector table, shared by the harness
 * (fuzz_decompress.c) and the seed generator (gen_seeds.c).
 *
 * The first byte of every fuzz input is an index into FUZZ_ALGOS; indexes past
 * the end are ignored. APPEND ONLY: reordering or removing an entry changes
 * what every saved seed, corpus entry and regression input tests.
 */

#ifndef CU_FUZZ_ALGOS_H
#define CU_FUZZ_ALGOS_H

#include "compress_utils.h"

static const cu_algorithm_t FUZZ_ALGOS[] = {
    CU_ALGO_ZSTD,    /*  0 */
    CU_ALGO_BROTLI,  /*  1 */
    CU_ALGO_ZLIB,    /*  2 */
    CU_ALGO_BZ2,     /*  3 */
    CU_ALGO_LZ4,     /*  4 */
    CU_ALGO_XZ,      /*  5 */
    CU_ALGO_LZO,     /*  6 */
    CU_ALGO_GZIP,    /*  7 */
    CU_ALGO_DEFLATE, /*  8 */
    CU_ALGO_SNAPPY,  /*  9 */
    CU_ALGO_LZ4_RAW, /* 10 */
};

#define FUZZ_N_ALGOS (sizeof(FUZZ_ALGOS) / sizeof(FUZZ_ALGOS[0]))

#endif /* CU_FUZZ_ALGOS_H */
