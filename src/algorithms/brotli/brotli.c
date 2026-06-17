/*
 * brotli.c — Brotli vtable for compress-utils.
 *
 * Brotli's wire format never carries the decompressed size, so:
 *   - cu_decompress_size_hint returns CU_ERR_SIZE_UNKNOWN.
 *   - one-shot decompress uses BrotliDecoderDecompressStream into the
 *     caller's buffer and returns SIZE_UNKNOWN if it doesn't fit.
 *
 * Streaming uses BrotliEncoderCompressStream / BrotliDecoderDecompressStream
 * with the standard stashed-tail protocol.
 */

#include "algorithm_registry.h"
#include "compress_utils.h"

#include "brotli/decode.h"
#include "brotli/encode.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int brotli_native_level(int user_level) {
    /* Brotli quality 0..11. User 1..10 -> 1..11. */
    if (user_level < 1) return 1;
    if (user_level > 11) return 11;
    return user_level + (user_level == 10 ? 1 : 0);
    /* user 10 -> 11 (best), others map 1:1 to their numeric value. */
}

/* ============================================================================
 * One-shot
 * ============================================================================ */

static size_t brotli_compress_bound(size_t in_len) {
    /* BrotliEncoderMaxCompressedSize gives the right bound. */
    size_t b = BrotliEncoderMaxCompressedSize(in_len);
    if (b == 0) {
        /* Brotli returns 0 for inputs too large; fall back to a generous
         * estimate. */
        return in_len + (in_len >> 2) + 256;
    }
    return b;
}

static cu_status_t brotli_compress(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len,
    int level
) {
    size_t cap = *out_len;
    size_t needed = brotli_compress_bound(in_len);
    if (cap < needed) {
        *out_len = needed;
        return CU_ERR_BUF_TOO_SMALL;
    }
    size_t encoded = cap;
    BROTLI_BOOL ok = BrotliEncoderCompress(
        brotli_native_level(level),
        BROTLI_DEFAULT_WINDOW,
        BROTLI_DEFAULT_MODE,
        in_len, in,
        &encoded, out
    );
    if (!ok) {
        cu_set_last_error("brotli: compression failed");
        return CU_ERR_COMPRESSION;
    }
    *out_len = encoded;
    return CU_OK;
}

static cu_status_t brotli_decompress(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    if (in_len == 0) {
        cu_set_last_error("brotli: empty input");
        return CU_ERR_TRUNCATED;
    }
    BrotliDecoderState* st = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (!st) { cu_set_last_error("brotli: oom"); return CU_ERR_OOM; }

    const uint8_t* next_in = in;
    size_t avail_in = in_len;
    uint8_t* next_out = out;
    size_t avail_out = *out_len;
    size_t cap_limit = cu_get_max_decompressed_size();

    BrotliDecoderResult r = BrotliDecoderDecompressStream(
        st, &avail_in, &next_in, &avail_out, &next_out, NULL
    );
    cu_status_t ret;
    if (r == BROTLI_DECODER_RESULT_SUCCESS) {
        size_t produced = (size_t)(next_out - out);
        if (cap_limit > 0 && produced > cap_limit) {
            cu_set_last_errorf("brotli: decompressed size %zu exceeds cap %zu", produced, cap_limit);
            ret = CU_ERR_SIZE_LIMIT;
        } else {
            *out_len = produced;
            ret = CU_OK;
        }
    } else if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
        cu_set_last_error("brotli: output buffer too small (size not encoded in stream)");
        ret = CU_ERR_SIZE_UNKNOWN;
    } else if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
        cu_set_last_error("brotli: truncated input");
        ret = CU_ERR_TRUNCATED;
    } else {
        BrotliDecoderErrorCode e = BrotliDecoderGetErrorCode(st);
        cu_set_last_errorf("brotli: %s", BrotliDecoderErrorString(e));
        ret = CU_ERR_DECOMPRESSION;
    }
    BrotliDecoderDestroyInstance(st);
    return ret;
}

static cu_status_t brotli_decompress_size_hint(
    const uint8_t* in, size_t in_len, size_t* out_size
) {
    (void)in; (void)in_len; (void)out_size;
    return CU_ERR_SIZE_UNKNOWN;
}

/* ============================================================================
 * Streaming compression
 * ============================================================================ */

typedef struct {
    BrotliEncoderState* enc;
    uint8_t* pending;
    size_t   pending_len;
    size_t   pending_cap;
    int      finishing;
    int      finished;
} brotli_cstream_state_t;

static cu_status_t cstream_pending_append(brotli_cstream_state_t* st,
                                          const uint8_t* src, size_t n) {
    if (n == 0) return CU_OK;
    size_t need = st->pending_len + n;
    if (need > st->pending_cap) {
        size_t new_cap = st->pending_cap ? st->pending_cap * 2 : 16 * 1024;
        while (new_cap < need) new_cap *= 2;
        uint8_t* p = realloc(st->pending, new_cap);
        if (!p) { cu_set_last_error("brotli: oom"); return CU_ERR_OOM; }
        st->pending = p;
        st->pending_cap = new_cap;
    }
    memcpy(st->pending + st->pending_len, src, n);
    st->pending_len += n;
    return CU_OK;
}

static cu_status_t brotli_cstream_create(int level, void** out_state) {
    brotli_cstream_state_t* st = calloc(1, sizeof(*st));
    if (!st) { cu_set_last_error("brotli: oom"); return CU_ERR_OOM; }
    st->enc = BrotliEncoderCreateInstance(NULL, NULL, NULL);
    if (!st->enc) {
        free(st);
        cu_set_last_error("brotli: BrotliEncoderCreateInstance failed");
        return CU_ERR_OOM;
    }
    BrotliEncoderSetParameter(st->enc, BROTLI_PARAM_QUALITY, brotli_native_level(level));
    *out_state = st;
    return CU_OK;
}

static cu_status_t cstream_pump(brotli_cstream_state_t* st, BrotliEncoderOperation op,
                                uint8_t* out, size_t* out_len) {
    size_t cap = *out_len;
    size_t written = 0;
    const uint8_t* next_in = st->pending;
    size_t avail_in = st->pending_len;

    for (;;) {
        uint8_t* next_out = out + written;
        size_t avail_out = cap - written;
        BROTLI_BOOL ok = BrotliEncoderCompressStream(
            st->enc, op, &avail_in, &next_in, &avail_out, &next_out, NULL
        );
        if (!ok) {
            cu_set_last_error("brotli: encode error");
            return CU_ERR_COMPRESSION;
        }
        written = cap - avail_out;
        if (op == BROTLI_OPERATION_FINISH && BrotliEncoderIsFinished(st->enc)) {
            st->finished = 1;
            /* Compact pending. */
            size_t consumed = st->pending_len - avail_in;
            if (consumed > 0 && avail_in > 0) {
                memmove(st->pending, st->pending + consumed, avail_in);
            }
            st->pending_len = avail_in;
            *out_len = written;
            return CU_OK;
        }
        if (avail_out == 0 && (avail_in > 0 || BrotliEncoderHasMoreOutput(st->enc))) {
            /* Buffer full and more to do. */
            size_t consumed = st->pending_len - avail_in;
            if (consumed > 0 && avail_in > 0) {
                memmove(st->pending, st->pending + consumed, avail_in);
            }
            st->pending_len = avail_in;
            *out_len = written;
            return CU_ERR_BUF_TOO_SMALL;
        }
        if (avail_in == 0 && !BrotliEncoderHasMoreOutput(st->enc)) {
            st->pending_len = 0;
            *out_len = written;
            return CU_OK;
        }
        /* Otherwise loop and write more. */
    }
}

static cu_status_t brotli_cstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    brotli_cstream_state_t* st = (brotli_cstream_state_t*)state;
    if (st->finishing) {
        cu_set_last_error("brotli: write after finish");
        return CU_ERR_STREAM_STATE;
    }
    cu_status_t s = cstream_pending_append(st, in, in_len);
    if (s != CU_OK) return s;
    return cstream_pump(st, BROTLI_OPERATION_PROCESS, out, out_len);
}

static cu_status_t brotli_cstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    brotli_cstream_state_t* st = (brotli_cstream_state_t*)state;
    st->finishing = 1;
    if (st->finished) { *out_len = 0; return CU_OK; }
    return cstream_pump(st, BROTLI_OPERATION_FINISH, out, out_len);
}

static void brotli_cstream_destroy(void* state) {
    brotli_cstream_state_t* st = (brotli_cstream_state_t*)state;
    if (!st) return;
    if (st->enc) BrotliEncoderDestroyInstance(st->enc);
    free(st->pending);
    free(st);
}

/* ============================================================================
 * Streaming decompression
 * ============================================================================ */

typedef struct {
    BrotliDecoderState* dec;
    uint8_t* pending;
    size_t   pending_len;
    size_t   pending_cap;
    int      finished;
} brotli_dstream_state_t;

static cu_status_t dstream_pending_append(brotli_dstream_state_t* st,
                                          const uint8_t* src, size_t n) {
    if (n == 0) return CU_OK;
    size_t need = st->pending_len + n;
    if (need > st->pending_cap) {
        size_t new_cap = st->pending_cap ? st->pending_cap * 2 : 16 * 1024;
        while (new_cap < need) new_cap *= 2;
        uint8_t* p = realloc(st->pending, new_cap);
        if (!p) { cu_set_last_error("brotli: oom"); return CU_ERR_OOM; }
        st->pending = p;
        st->pending_cap = new_cap;
    }
    memcpy(st->pending + st->pending_len, src, n);
    st->pending_len += n;
    return CU_OK;
}

static cu_status_t brotli_dstream_create(void** out_state) {
    brotli_dstream_state_t* st = calloc(1, sizeof(*st));
    if (!st) { cu_set_last_error("brotli: oom"); return CU_ERR_OOM; }
    st->dec = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    if (!st->dec) {
        free(st);
        cu_set_last_error("brotli: BrotliDecoderCreateInstance failed");
        return CU_ERR_OOM;
    }
    *out_state = st;
    return CU_OK;
}

static cu_status_t dstream_pump(brotli_dstream_state_t* st,
                                uint8_t* out, size_t* out_len) {
    size_t cap = *out_len;
    size_t written = 0;
    const uint8_t* next_in = st->pending;
    size_t avail_in = st->pending_len;

    for (;;) {
        uint8_t* next_out = out + written;
        size_t avail_out = cap - written;
        BrotliDecoderResult r = BrotliDecoderDecompressStream(
            st->dec, &avail_in, &next_in, &avail_out, &next_out, NULL
        );
        written = cap - avail_out;
        if (r == BROTLI_DECODER_RESULT_SUCCESS) {
            st->finished = 1;
            st->pending_len = avail_in;  /* should be 0; if not, trailing data error */
            if (avail_in > 0) {
                cu_set_last_error("brotli: trailing data after end of stream");
                return CU_ERR_DECOMPRESSION;
            }
            *out_len = written;
            return CU_OK;
        }
        if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT) {
            /* All pending consumed. */
            st->pending_len = 0;
            *out_len = written;
            return CU_OK;
        }
        if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT) {
            /* Buffer full; shift pending tail. */
            size_t consumed = st->pending_len - avail_in;
            if (consumed > 0 && avail_in > 0) {
                memmove(st->pending, st->pending + consumed, avail_in);
            }
            st->pending_len = avail_in;
            *out_len = written;
            return CU_ERR_BUF_TOO_SMALL;
        }
        /* ERROR */
        BrotliDecoderErrorCode e = BrotliDecoderGetErrorCode(st->dec);
        cu_set_last_errorf("brotli: %s", BrotliDecoderErrorString(e));
        return CU_ERR_DECOMPRESSION;
    }
}

static cu_status_t brotli_dstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    brotli_dstream_state_t* st = (brotli_dstream_state_t*)state;
    if (st->finished && in_len > 0) {
        cu_set_last_error("brotli: data after end of stream");
        return CU_ERR_DECOMPRESSION;
    }
    cu_status_t s = dstream_pending_append(st, in, in_len);
    if (s != CU_OK) return s;
    return dstream_pump(st, out, out_len);
}

static cu_status_t brotli_dstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    brotli_dstream_state_t* st = (brotli_dstream_state_t*)state;
    cu_status_t s = dstream_pump(st, out, out_len);
    if (s != CU_OK) return s;
    if (!st->finished) {
        cu_set_last_error("brotli: truncated stream at finish");
        return CU_ERR_TRUNCATED;
    }
    return CU_OK;
}

static void brotli_dstream_destroy(void* state) {
    brotli_dstream_state_t* st = (brotli_dstream_state_t*)state;
    if (!st) return;
    if (st->dec) BrotliDecoderDestroyInstance(st->dec);
    free(st->pending);
    free(st);
}

/* ============================================================================
 * Vtable
 * ============================================================================ */

const cu_algorithm_vtbl_t cu_brotli_vtbl = {
    .name                      = "brotli",
    /* Direction split (#7): see zstd.c for the CU_OMIT_* rationale. */
#ifndef CU_OMIT_COMPRESS
    .compress_bound            = brotli_compress_bound,
    .compress                  = brotli_compress,
    .compress_stream_create    = brotli_cstream_create,
    .compress_stream_write     = brotli_cstream_write,
    .compress_stream_finish    = brotli_cstream_finish,
    .compress_stream_destroy   = brotli_cstream_destroy,
#endif
#ifndef CU_OMIT_DECOMPRESS
    .decompress                = brotli_decompress,
    .decompress_size_hint      = brotli_decompress_size_hint,
    .decompress_stream_create  = brotli_dstream_create,
    .decompress_stream_write   = brotli_dstream_write,
    .decompress_stream_finish  = brotli_dstream_finish,
    .decompress_stream_destroy = brotli_dstream_destroy,
#endif
};
