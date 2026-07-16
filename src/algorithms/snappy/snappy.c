/*
 * snappy.c — Snappy vtable for compress-utils.
 *
 * Wire format: the raw Snappy block format (a varint of the uncompressed
 * length followed by the compressed stream), as produced by
 * snappy_compress() / consumed by snappy_uncompress(). This is the format
 * used by `python -m snappy` (raw mode), Java's Snappy.compress(), and the
 * reference C++ snappy::Compress(). It is NOT the Snappy *framing* format
 * (the `.sz` stream format used by CLIs / streaming libraries).
 *
 * Implementation: the pure-C port andikleen/snappy-c (third_party/snappy),
 * NOT google/snappy (C++). This keeps the whole release library pure C — no
 * libc++/libstdc++ runtime dependency in any binding. google/snappy is retained
 * at third_party/snappy-oracle purely as the differential-test oracle; it never
 * ships. The wire format is identical (verified byte-for-byte + both-direction
 * interop in the CTest differential test).
 *
 * Raw-block-only is a deliberate decision (2026-07-08): it's the format
 * Snappy's ecosystem embeds (Parquet pages, Kafka messages, Cassandra, RPC),
 * and it carries the uncompressed length so cu_decompress_size_hint works.
 * The framing format is intentionally NOT supported here — the raw-block API
 * is all we wrap, so framing would have to be hand-written (chunking +
 * masked CRC-32C) and would be a *separate* codec (CU_ALGO_SNAPPY_FRAMED),
 * not a mode of this one. It's a non-breaking addition if `.sz` interop is
 * ever needed; see TODO.md (Algorithms) and docs/adding-an-algorithm.md.
 *
 * Size hint: the raw block format leads with a varint of the uncompressed
 * length, so cu_decompress_size_hint always succeeds (via
 * snappy_uncompressed_length).
 *
 * Levels: Snappy has a single compression mode — there is no level knob.
 * The `level` argument is accepted (for API uniformity) and ignored.
 *
 * Streaming: the raw block format is not incrementally encodable/decodable
 * (the whole input is needed to emit the block, and the whole block to
 * decode it). To keep the streaming API's output bit-identical to the
 * one-shot output — a property the cross-API round-trip tests require — the
 * stream implementations buffer all input, then run the one-shot codec on
 * finish and drain the result through the caller's buffer. Consequently the
 * streaming path holds the full input (and output) in memory; it does not
 * bound memory the way the natively-streaming codecs (zstd, zlib, ...) do.
 */

#include "algorithm_registry.h"
#include "compress_utils.h"

#include <snappy.h>  /* andikleen/snappy-c: snappy_env + raw-block compress/uncompress */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* andikleen's API returns 0 on success, negative on failure (no rich error
 * codes / buffer-too-small — we size buffers ourselves before every call). */
static cu_status_t snappy_err(int rc, cu_status_t fallback, const char* what) {
    if (rc == 0) return CU_OK;
    cu_set_last_errorf("snappy: %s failed (rc=%d)", what, rc);
    return fallback;
}

/* ============================================================================
 * One-shot
 * ============================================================================ */

static size_t snappy_compress_bound(size_t in_len) {
    return snappy_max_compressed_length(in_len);
}

static cu_status_t snappy_compress_oneshot(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len,
    int level
) {
    (void)level;  /* Snappy has no compression levels. */
    size_t needed = snappy_max_compressed_length(in_len);
    if (*out_len < needed) {
        *out_len = needed;
        return CU_ERR_BUF_TOO_SMALL;
    }
    struct snappy_env env;
    if (snappy_init_env(&env) != 0) {
        cu_set_last_error("snappy: env alloc failed");
        return CU_ERR_OOM;
    }
    size_t clen = *out_len;
    int rc = snappy_compress(&env, (const char*)(in ? in : (const uint8_t*)""),
                             in_len, (char*)out, &clen);
    snappy_free_env(&env);
    if (rc != 0) return snappy_err(rc, CU_ERR_COMPRESSION, "compress");
    *out_len = clen;
    return CU_OK;
}

static cu_status_t snappy_decompress_oneshot(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    if (in_len == 0) {
        cu_set_last_error("snappy: empty input");
        return CU_ERR_TRUNCATED;
    }
    size_t needed = 0;
    if (!snappy_uncompressed_length((const char*)in, in_len, &needed)) {
        cu_set_last_error("snappy: invalid header");
        return CU_ERR_DECOMPRESSION;
    }

    size_t cap_limit = cu_get_max_decompressed_size();
    if (cap_limit > 0 && needed > cap_limit) {
        cu_set_last_errorf("snappy: decompressed size %zu exceeds cap %zu", needed, cap_limit);
        return CU_ERR_SIZE_LIMIT;
    }
    if (*out_len < needed) {
        *out_len = needed;
        return CU_ERR_BUF_TOO_SMALL;
    }
    /* andikleen's snappy_uncompress writes exactly `needed` bytes (read from the
     * header); it takes no capacity, so `out` must be sized to `needed` — it is. */
    int rc = snappy_uncompress((const char*)in, in_len, (char*)out);
    if (rc != 0) return snappy_err(rc, CU_ERR_DECOMPRESSION, "uncompress");
    *out_len = needed;
    return CU_OK;
}

static cu_status_t snappy_decompress_size_hint(
    const uint8_t* in, size_t in_len, size_t* out_size
) {
    if (in_len == 0) return CU_ERR_TRUNCATED;
    size_t sz = 0;
    if (!snappy_uncompressed_length((const char*)in, in_len, &sz)) {
        cu_set_last_error("snappy: invalid header");
        return CU_ERR_DECOMPRESSION;
    }
    *out_size = sz;
    return CU_OK;
}

/* ============================================================================
 * Streaming — buffer-all-then-run (see file header for why)
 * ============================================================================ */

typedef struct {
    /* Accumulated input. */
    uint8_t* in_buf;
    size_t   in_len;
    size_t   in_cap;
    /* Produced output waiting to be drained into the caller's buffer. */
    uint8_t* out_buf;
    size_t   out_head;
    size_t   out_tail;
    int      produced;   /* codec has run; out_buf is populated */
} snappy_stream_state_t;

static cu_status_t sn_in_append(snappy_stream_state_t* st, const uint8_t* src, size_t n) {
    if (n == 0) return CU_OK;
    size_t need = st->in_len + n;
    if (need > st->in_cap) {
        size_t new_cap = st->in_cap ? st->in_cap * 2 : 64 * 1024;
        while (new_cap < need) new_cap *= 2;
        uint8_t* p = realloc(st->in_buf, new_cap);
        if (!p) { cu_set_last_error("snappy: oom"); return CU_ERR_OOM; }
        st->in_buf = p;
        st->in_cap = new_cap;
    }
    memcpy(st->in_buf + st->in_len, src, n);
    st->in_len += n;
    return CU_OK;
}

/* Copy from out_buf into the caller's buffer. Returns 1 if bytes remain. */
static int sn_out_drain(snappy_stream_state_t* st,
                        uint8_t* out, size_t cap, size_t* written) {
    size_t available = st->out_tail - st->out_head;
    size_t room = cap - *written;
    size_t n = available < room ? available : room;
    if (n > 0) {
        memcpy(out + *written, st->out_buf + st->out_head, n);
        *written += n;
        st->out_head += n;
    }
    return st->out_head < st->out_tail;
}

static void snappy_stream_destroy(void* state) {
    snappy_stream_state_t* st = (snappy_stream_state_t*)state;
    if (!st) return;
    free(st->in_buf);
    free(st->out_buf);
    free(st);
}

/* ---- Streaming compression ---- */

static cu_status_t snappy_cstream_create(int level, void** out_state) {
    (void)level;  /* Snappy has no compression levels. */
    snappy_stream_state_t* st = calloc(1, sizeof(*st));
    if (!st) { cu_set_last_error("snappy: oom"); return CU_ERR_OOM; }
    *out_state = st;
    return CU_OK;
}

static cu_status_t snappy_cstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    (void)out;
    snappy_stream_state_t* st = (snappy_stream_state_t*)state;
    if (st->produced) {
        cu_set_last_error("snappy: write after finish");
        return CU_ERR_STREAM_STATE;
    }
    cu_status_t s = sn_in_append(st, in, in_len);
    *out_len = 0;  /* raw-block Snappy produces no output until finish */
    return s;
}

static cu_status_t snappy_cstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    snappy_stream_state_t* st = (snappy_stream_state_t*)state;
    size_t cap = *out_len;
    size_t written = 0;

    if (!st->produced) {
        size_t bound = snappy_max_compressed_length(st->in_len);
        st->out_buf = malloc(bound ? bound : 1);
        if (!st->out_buf) { cu_set_last_error("snappy: oom"); return CU_ERR_OOM; }
        struct snappy_env env;
        if (snappy_init_env(&env) != 0) {
            cu_set_last_error("snappy: env alloc failed");
            return CU_ERR_OOM;
        }
        size_t clen = bound;
        int rc = snappy_compress(
            &env, (const char*)(st->in_buf ? st->in_buf : (const uint8_t*)""),
            st->in_len, (char*)st->out_buf, &clen);
        snappy_free_env(&env);
        if (rc != 0) return snappy_err(rc, CU_ERR_COMPRESSION, "compress");
        st->out_tail = clen;
        st->produced = 1;
    }

    int more = sn_out_drain(st, out, cap, &written);
    *out_len = written;
    return more ? CU_ERR_BUF_TOO_SMALL : CU_OK;
}

/* ---- Streaming decompression ---- */

static cu_status_t snappy_dstream_create(void** out_state) {
    snappy_stream_state_t* st = calloc(1, sizeof(*st));
    if (!st) { cu_set_last_error("snappy: oom"); return CU_ERR_OOM; }
    *out_state = st;
    return CU_OK;
}

static cu_status_t snappy_dstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    (void)out;
    snappy_stream_state_t* st = (snappy_stream_state_t*)state;
    if (st->produced) {
        cu_set_last_error("snappy: data after end of stream");
        return CU_ERR_DECOMPRESSION;
    }
    cu_status_t s = sn_in_append(st, in, in_len);
    *out_len = 0;  /* whole block needed before we can decode */
    return s;
}

static cu_status_t snappy_dstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    snappy_stream_state_t* st = (snappy_stream_state_t*)state;
    size_t cap = *out_len;
    size_t written = 0;

    if (!st->produced) {
        if (st->in_len == 0) {
            cu_set_last_error("snappy: truncated stream at finish");
            return CU_ERR_TRUNCATED;
        }
        size_t usize = 0;
        if (!snappy_uncompressed_length((const char*)st->in_buf, st->in_len, &usize)) {
            cu_set_last_error("snappy: invalid header");
            return CU_ERR_DECOMPRESSION;
        }
        st->out_buf = malloc(usize ? usize : 1);
        if (!st->out_buf) { cu_set_last_error("snappy: oom"); return CU_ERR_OOM; }
        int rc = snappy_uncompress((const char*)st->in_buf, st->in_len,
                                   (char*)st->out_buf);
        if (rc != 0) return snappy_err(rc, CU_ERR_DECOMPRESSION, "uncompress");
        st->out_tail = usize;
        st->produced = 1;
    }

    int more = sn_out_drain(st, out, cap, &written);
    *out_len = written;
    return more ? CU_ERR_BUF_TOO_SMALL : CU_OK;
}

/* ============================================================================
 * Vtable
 * ============================================================================ */

const cu_algorithm_vtbl_t cu_snappy_vtbl = {
    .name                      = "snappy",
    /* Direction split (#7): see zstd.c for the CU_OMIT_* rationale. */
#ifndef CU_OMIT_COMPRESS
    .compress_bound            = snappy_compress_bound,
    .compress                  = snappy_compress_oneshot,
    .compress_stream_create    = snappy_cstream_create,
    .compress_stream_write     = snappy_cstream_write,
    .compress_stream_finish    = snappy_cstream_finish,
    .compress_stream_destroy   = snappy_stream_destroy,
#endif
#ifndef CU_OMIT_DECOMPRESS
    .decompress                = snappy_decompress_oneshot,
    .decompress_size_hint      = snappy_decompress_size_hint,
    .decompress_stream_create  = snappy_dstream_create,
    .decompress_stream_write   = snappy_dstream_write,
    .decompress_stream_finish  = snappy_dstream_finish,
    .decompress_stream_destroy = snappy_stream_destroy,
#endif
};
