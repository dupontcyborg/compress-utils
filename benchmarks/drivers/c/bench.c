/*
 * compress-utils C benchmark driver.
 *
 * Thin adapter: wraps the public cu_* one-shot ABI as bench_codec_t entries
 * and hands them to the shared harness (bench_harness.h), which owns timing,
 * statistics, round-trip verification, and NDJSON emission. The algorithm enum
 * rides in each codec's `native_id`.
 *
 * Protocol, env, and --info are documented in benchmarks/README.md.
 */

#include "compress_utils.h"

#include "bench_harness.h"

static size_t cu_bound(const bench_codec_t* c, size_t in_len) {
    return cu_compress_bound(in_len, (cu_algorithm_t)c->native_id);
}

static int cu_do_compress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                          uint8_t* out, size_t* out_len, int level, const char** err) {
    cu_status_t s = cu_compress((cu_algorithm_t)c->native_id, in, in_len, out, out_len, level);
    if (s != CU_OK) { *err = cu_last_error(); return (int)s; }
    return 0;
}

static int cu_do_decompress(const bench_codec_t* c, const uint8_t* in, size_t in_len,
                            uint8_t* out, size_t* out_len, const char** err) {
    cu_status_t s = cu_decompress((cu_algorithm_t)c->native_id, in, in_len, out, out_len);
    if (s != CU_OK) { *err = cu_last_error(); return (int)s; }
    return 0;
}

#define CU_CODEC(NAME, ENUM) \
    { NAME, "compress-utils", (ENUM), cu_bound, cu_do_compress, cu_do_decompress }

static const bench_codec_t CODECS[] = {
    CU_CODEC("zstd", CU_ALGO_ZSTD),
    CU_CODEC("brotli", CU_ALGO_BROTLI),
    CU_CODEC("zlib", CU_ALGO_ZLIB),
    CU_CODEC("bz2", CU_ALGO_BZ2),
    CU_CODEC("lz4", CU_ALGO_LZ4),
    CU_CODEC("xz", CU_ALGO_XZ),
};
static const size_t N_CODECS = sizeof(CODECS) / sizeof(CODECS[0]);

int main(int argc, char** argv) {
    if (argc > 1 && !strcmp(argv[1], "--info")) {
        return bench_info("c", cu_version(), "c");
    }
    return bench_run("c", CODECS, N_CODECS);
}
