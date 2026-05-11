/*
 * fuzz_decompress.c — libFuzzer harness for cu_decompress.
 *
 * Build with clang and -fsanitize=fuzzer,address (or whatever combination
 * of sanitizers you want). The CMake option ENABLE_FUZZ wires this up;
 * see tests/fuzz/CMakeLists.txt.
 *
 * The fuzzer reads (algo_byte, payload) pairs from libFuzzer-generated
 * input and exercises cu_decompress for every algorithm. The first byte
 * of every input selects the algorithm:
 *   0 -> zstd, 1 -> brotli, 2 -> zlib, 3 -> bz2, 4 -> lz4, 5 -> xz.
 *
 * The harness must not crash, leak, or trigger ASan/UBSan on any input.
 * It is allowed to return any cu_status_t value.
 *
 * Build artifact: fuzz_decompress. Run with:
 *   ./fuzz_decompress -max_total_time=60 corpus/
 */

#include "compress_utils.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

static const cu_algorithm_t ALGOS[] = {
    CU_ALGO_ZSTD, CU_ALGO_BROTLI, CU_ALGO_ZLIB,
    CU_ALGO_BZ2,  CU_ALGO_LZ4,    CU_ALGO_XZ,
};

#define MAX_OUT (4 * 1024 * 1024)

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 1) return 0;
    cu_algorithm_t algo = ALGOS[data[0] % (sizeof(ALGOS) / sizeof(ALGOS[0]))];
    const uint8_t* payload = data + 1;
    size_t payload_len = size - 1;

    if (!cu_algorithm_available(algo)) return 0;

    /* Cap allocations so fuzzer can't make us OOM. */
    cu_set_max_decompressed_size(MAX_OUT);

    /* Probe first — exercises size_hint paths. */
    size_t hint = 0;
    (void)cu_decompress_size_hint(algo, payload, payload_len, &hint);

    /* Try one-shot decompress into a moderate buffer. */
    size_t out_cap = 64 * 1024;
    if (hint > 0 && hint <= MAX_OUT) out_cap = hint;
    uint8_t* out = malloc(out_cap);
    if (!out) return 0;
    size_t out_len = out_cap;
    (void)cu_decompress(algo, payload, payload_len, out, &out_len);
    free(out);

    /* Also exercise the streaming decompressor. */
    cu_decompress_stream_t* ds = NULL;
    if (cu_decompress_stream_create(algo, &ds) == CU_OK && ds) {
        uint8_t scratch[4096];
        size_t pos = 0;
        while (pos < payload_len) {
            size_t chunk = payload_len - pos;
            if (chunk > 1024) chunk = 1024;
            size_t scratch_len = sizeof(scratch);
            cu_status_t s = cu_decompress_stream_write(
                ds, payload + pos, chunk, scratch, &scratch_len
            );
            pos += chunk;
            if (s == CU_ERR_BUF_TOO_SMALL) {
                /* Drain. */
                for (;;) {
                    scratch_len = sizeof(scratch);
                    s = cu_decompress_stream_write(ds, NULL, 0, scratch, &scratch_len);
                    if (s != CU_ERR_BUF_TOO_SMALL) break;
                }
            }
            if (s != CU_OK) break;
        }
        size_t scratch_len = sizeof(scratch);
        (void)cu_decompress_stream_finish(ds, scratch, &scratch_len);
        cu_decompress_stream_destroy(ds);
    }

    return 0;
}
