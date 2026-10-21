/*
 * fuzz_decompress.c — libFuzzer harness for cu_decompress.
 *
 * Build with clang and -fsanitize=fuzzer,address (or whatever combination
 * of sanitizers you want). The CMake option ENABLE_FUZZ wires this up;
 * see tests/fuzz/CMakeLists.txt.
 *
 * The fuzzer reads (algo_byte, mode_byte, payload) from libFuzzer-generated
 * input and exercises cu_decompress and the streaming decoder. The first
 * byte selects the algorithm:
 *   0 -> zstd, 1 -> brotli, 2 -> zlib, 3 -> bz2, 4 -> lz4, 5 -> xz,
 *   6 -> raw LZO1X, 7 -> gzip, 8 -> raw DEFLATE, 9 -> snappy.
 * The second selects how the stream is driven: bit 0 single-stream mode
 * (CU_DSTREAM_SINGLE), bits 1-2 the output buffer size, bits 3-4 the input
 * chunk size.
 *
 * The harness must not crash, leak, or trigger ASan/UBSan on any input.
 * It is allowed to return any cu_status_t value, but in single-stream mode it
 * aborts if the decoder breaks that mode's contract: eof while output is
 * still draining, unused bytes that aren't the tail of the input fed, or a
 * write after eof that isn't rejected.
 *
 * Build artifact: fuzz_decompress. Run with:
 *   ./fuzz_decompress -max_total_time=60 corpus/
 */

#include "compress_utils.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const cu_algorithm_t ALGOS[] = {
    CU_ALGO_ZSTD, CU_ALGO_BROTLI, CU_ALGO_ZLIB,    CU_ALGO_BZ2,    CU_ALGO_LZ4,
    CU_ALGO_XZ,   CU_ALGO_LZO,    CU_ALGO_GZIP,    CU_ALGO_DEFLATE, CU_ALGO_SNAPPY,
};

static const size_t OUT_CAPS[] = {4096, 1, 7, 64};
static const size_t CHUNKS[] = {1024, 1, 13, 4096};

/* A decoder broke the single-stream contract: make the fuzzer report it. */
#define REQUIRE(cond) \
    do {              \
        if (!(cond)) abort(); \
    } while (0)

#define MAX_OUT (4 * 1024 * 1024)

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;
    cu_algorithm_t algo = ALGOS[data[0] % (sizeof(ALGOS) / sizeof(ALGOS[0]))];
    int single = data[1] & 1;
    size_t out_cap_stream = OUT_CAPS[(data[1] >> 1) & 3];
    size_t chunk_max = CHUNKS[(data[1] >> 3) & 3];
    const uint8_t* payload = data + 2;
    size_t payload_len = size - 2;

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

    /* Also exercise the streaming decompressor. Bound the total output so a
     * small input can't make tiny output buffers spin for millions of calls. */
    cu_decompress_stream_t* ds = NULL;
    if (cu_decompress_stream_create_ex(algo, single ? CU_DSTREAM_SINGLE : 0, &ds) == CU_OK && ds) {
        uint8_t scratch[4096];
        size_t pos = 0, produced = 0;
        int eof = 0;
        while (pos < payload_len && !eof) {
            size_t chunk = payload_len - pos;
            if (chunk > chunk_max) chunk = chunk_max;
            size_t scratch_len = out_cap_stream;
            cu_status_t s = cu_decompress_stream_write(
                ds, payload + pos, chunk, scratch, &scratch_len
            );
            pos += chunk;
            produced += scratch_len;
            /* Drain. */
            while (s == CU_ERR_BUF_TOO_SMALL && produced <= MAX_OUT) {
                REQUIRE(!cu_decompress_stream_eof(ds));
                scratch_len = out_cap_stream;
                s = cu_decompress_stream_write(ds, NULL, 0, scratch, &scratch_len);
                produced += scratch_len;
            }
            if (s != CU_OK) break;
            eof = cu_decompress_stream_eof(ds);
        }
        if (single) {
            const uint8_t* unused = NULL;
            size_t unused_len = 0;
            REQUIRE(cu_decompress_stream_unused(ds, &unused, &unused_len) == CU_OK);
            if (eof) {
                /* Unused is exactly the tail of the input fed so far. */
                REQUIRE(unused_len <= pos);
                REQUIRE(unused_len == 0 || !memcmp(unused, payload + pos - unused_len, unused_len));
                if (pos < payload_len) {
                    size_t scratch_len = out_cap_stream;
                    REQUIRE(cu_decompress_stream_write(ds, payload + pos, payload_len - pos,
                                                       scratch, &scratch_len) ==
                            CU_ERR_STREAM_FINISHED);
                }
            } else {
                REQUIRE(unused_len == 0);
            }
        }
        size_t scratch_len = out_cap_stream;
        cu_status_t s = cu_decompress_stream_finish(ds, scratch, &scratch_len);
        if (eof) REQUIRE(s == CU_OK && scratch_len == 0);
        cu_decompress_stream_destroy(ds);
    }

    return 0;
}
