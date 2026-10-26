/*
 * fuzz_roundtrip.c — libFuzzer harness for the compress side: whatever bytes
 * the fuzzer picks must compress and decompress back to themselves.
 *
 * The decoder harness (fuzz_decompress.c) only reaches an encoder through the
 * seed corpus; this one drives the encoders directly, with the input bytes as
 * the data. Layout: (algo_byte, level_byte, mode_byte, data...). The first
 * byte indexes FUZZ_ALGOS (fuzz_algos.h; out-of-range and decode-only codecs
 * are skipped), the second picks level 1-10, and the third how the stream is
 * driven: bits 0-1 the output buffer size, bits 2-3 the write chunk size,
 * bit 4 a nonterminal flush after every few writes.
 *
 * Each input is compressed twice: one-shot into a buffer of exactly
 * cu_compress_bound bytes, and through the streaming encoder (small buffers,
 * drain protocol, optional flushes). Both outputs must decompress to the
 * input. The harness aborts on a failed round trip, an output over the
 * advertised bound, or an encoder that stalls in a drain loop.
 *
 * Build artifact: fuzz_roundtrip (see tests/fuzz/CMakeLists.txt). Run with:
 *   ./fuzz_roundtrip -max_total_time=60 -max_len=65536 corpus/ seeds/
 * Any file works as a seed: the bytes after the third are plain data.
 */

#include "compress_utils.h"
#include "fuzz_algos.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const size_t OUT_CAPS[] = {4096, 1, 7, 64};
static const size_t CHUNKS[] = {1024, 1, 13, 4096};

#define MAX_DATA (256 * 1024)
#define MAX_STALLS 1000

/* A codec broke its contract: make the fuzzer report it. */
#define REQUIRE(cond) \
    do {              \
        if (!(cond)) abort(); \
    } while (0)

typedef struct {
    uint8_t* p;
    size_t len, cap;
} buf_t;

static void buf_add(buf_t* b, const uint8_t* src, size_t n) {
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n) cap *= 2;
        b->p = realloc(b->p, cap);
        REQUIRE(b->p);
        b->cap = cap;
    }
    if (n) memcpy(b->p + b->len, src, n);
    b->len += n;
}

/* Run a stream call to completion: drain with fresh buffers until CU_OK. */
typedef cu_status_t (*drain_fn)(cu_compress_stream_t*, const uint8_t*, size_t, uint8_t*, size_t*);

static cu_status_t call_write(cu_compress_stream_t* s, const uint8_t* in, size_t n,
                              uint8_t* out, size_t* out_len) {
    return cu_compress_stream_write(s, in, n, out, out_len);
}
static cu_status_t call_flush(cu_compress_stream_t* s, const uint8_t* in, size_t n,
                              uint8_t* out, size_t* out_len) {
    (void)in; (void)n;
    return cu_compress_stream_flush(s, out, out_len);
}
static cu_status_t call_finish(cu_compress_stream_t* s, const uint8_t* in, size_t n,
                               uint8_t* out, size_t* out_len) {
    (void)in; (void)n;
    return cu_compress_stream_finish(s, out, out_len);
}

/* Returns the final status; appends everything written to dst. */
static cu_status_t drain(drain_fn fn, cu_compress_stream_t* s, const uint8_t* in, size_t n,
                         uint8_t* scratch, size_t scratch_cap, buf_t* dst) {
    int stalls = 0;
    for (;;) {
        size_t out_len = scratch_cap;
        cu_status_t st = fn(s, in, n, scratch, &out_len);
        REQUIRE(out_len <= scratch_cap);
        /* Only OK and BUF_TOO_SMALL report bytes written; an error leaves the
         * capacity in out_len. */
        if (st == CU_OK || st == CU_ERR_BUF_TOO_SMALL) buf_add(dst, scratch, out_len);
        in = NULL;
        n = 0;
        if (st != CU_ERR_BUF_TOO_SMALL) return st;
        stalls = out_len ? 0 : stalls + 1;
        REQUIRE(stalls < MAX_STALLS);
    }
}

static void check_decodes_to(cu_algorithm_t algo, const uint8_t* comp, size_t comp_len,
                             const uint8_t* want, size_t want_len) {
    uint8_t* dec = malloc(want_len + 1);
    REQUIRE(dec);
    size_t dec_len = want_len + 1;
    cu_status_t st = cu_decompress(algo, comp, comp_len, dec, &dec_len);
    REQUIRE(st == CU_OK);
    REQUIRE(dec_len == want_len);
    REQUIRE(want_len == 0 || memcmp(dec, want, want_len) == 0);
    free(dec);
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 3 || data[0] >= FUZZ_N_ALGOS) return 0;
    cu_algorithm_t algo = FUZZ_ALGOS[data[0]];
    int level = 1 + data[1] % 10;
    size_t out_cap = OUT_CAPS[data[2] & 3];
    size_t chunk = CHUNKS[(data[2] >> 2) & 3];
    int flush = (data[2] >> 4) & 1;
    const uint8_t* payload = data + 3;
    size_t payload_len = size - 3;
    if (payload_len > MAX_DATA) payload_len = MAX_DATA;

    if (algo == CU_ALGO_LZO || !cu_algorithm_available(algo)) return 0;

    /* One-shot, into exactly the advertised bound. */
    size_t bound = cu_compress_bound(payload_len, algo);
    uint8_t* comp = malloc(bound ? bound : 1);
    REQUIRE(comp);
    size_t comp_len = bound;
    REQUIRE(cu_compress(algo, payload, payload_len, comp, &comp_len, level) == CU_OK);
    REQUIRE(comp_len <= bound);
    check_decodes_to(algo, comp, comp_len, payload, payload_len);
    free(comp);

    /* Streaming, with small buffers, the drain protocol and optional flushes. */
    cu_compress_stream_t* s = NULL;
    cu_status_t created = cu_compress_stream_create(algo, level, &s);
    if (created == CU_ERR_UNSUPPORTED_ALGO) return 0; /* raw LZ4 block: one-shot only */
    REQUIRE(created == CU_OK);
    uint8_t* scratch = malloc(out_cap);
    REQUIRE(scratch);
    buf_t out = {0};
    int can_flush = flush;
    int writes = 0;
    for (size_t pos = 0; pos < payload_len;) {
        size_t n = payload_len - pos < chunk ? payload_len - pos : chunk;
        REQUIRE(drain(call_write, s, payload + pos, n, scratch, out_cap, &out) == CU_OK);
        pos += n;
        if (can_flush && ++writes % 3 == 0) {
            cu_status_t st = drain(call_flush, s, NULL, 0, scratch, out_cap, &out);
            if (st == CU_ERR_UNSUPPORTED_ALGO) can_flush = 0; /* snappy, bzip2 */
            else REQUIRE(st == CU_OK);
        }
    }
    REQUIRE(drain(call_finish, s, NULL, 0, scratch, out_cap, &out) == CU_OK);
    cu_compress_stream_destroy(s);
    free(scratch);
    check_decodes_to(algo, out.p, out.len, payload, payload_len);
    free(out.p);
    return 0;
}
