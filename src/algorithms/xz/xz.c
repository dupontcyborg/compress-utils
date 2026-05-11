/*
 * xz.c — XZ/LZMA vtable for compress-utils.
 *
 * XZ streams encode the uncompressed size in their stream footer/index,
 * so a size hint is theoretically possible. Parsing the footer requires
 * scanning to the end of the stream, which is non-trivial; for now
 * size_hint returns CU_ERR_SIZE_UNKNOWN and one-shot decompress uses an
 * incremental decoder. (Footer-parsing size_hint is a future enhancement
 * — tracked in TODO.md.)
 *
 * Streaming uses lzma_easy_encoder / lzma_auto_decoder with the standard
 * stashed-tail protocol.
 */

#include "algorithm_registry.h"
#include "compress_utils.h"

#include "xz/lzma.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static uint32_t xz_native_level(int user_level) {
    /* XZ preset 0..9. User 1..10 -> 0..9. */
    if (user_level < 1) return 0;
    if (user_level > 10) return 9;
    return (uint32_t)(user_level - 1);
}

static cu_status_t map_lzma_error(lzma_ret r, cu_status_t fallback) {
    if (r == LZMA_OK || r == LZMA_STREAM_END) return CU_OK;
    cu_set_last_errorf("xz: lzma_ret %d", (int)r);
    if (r == LZMA_MEM_ERROR) return CU_ERR_OOM;
    if (r == LZMA_DATA_ERROR) return CU_ERR_DECOMPRESSION;
    if (r == LZMA_BUF_ERROR) return CU_ERR_BUF_TOO_SMALL;
    return fallback;
}

/* ============================================================================
 * One-shot
 * ============================================================================ */

static size_t xz_compress_bound(size_t in_len) {
    /* lzma_stream_buffer_bound gives the worst-case bound for a complete
     * stream encoding of in_len bytes. */
    return lzma_stream_buffer_bound(in_len);
}

static cu_status_t xz_compress(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len,
    int level
) {
    size_t cap = *out_len;
    size_t needed = xz_compress_bound(in_len);
    if (cap < needed) {
        *out_len = needed;
        return CU_ERR_BUF_TOO_SMALL;
    }
    size_t out_pos = 0;
    lzma_ret r = lzma_easy_buffer_encode(
        xz_native_level(level), LZMA_CHECK_CRC64, NULL,
        in, in_len,
        out, &out_pos, cap
    );
    if (r != LZMA_OK) {
        return map_lzma_error(r, CU_ERR_COMPRESSION);
    }
    *out_len = out_pos;
    return CU_OK;
}

static cu_status_t xz_decompress(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    if (in_len == 0) {
        cu_set_last_error("xz: empty input");
        return CU_ERR_TRUNCATED;
    }
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_auto_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK) {
        cu_set_last_error("xz: lzma_auto_decoder init failed");
        return CU_ERR_OOM;
    }
    strm.next_in = in;
    strm.avail_in = in_len;
    strm.next_out = out;
    strm.avail_out = *out_len;

    size_t cap_limit = cu_get_max_decompressed_size();
    lzma_ret r = lzma_code(&strm, LZMA_FINISH);
    cu_status_t ret;
    if (r == LZMA_STREAM_END) {
        if (cap_limit > 0 && strm.total_out > cap_limit) {
            cu_set_last_errorf("xz: decompressed %llu exceeds cap %zu",
                               (unsigned long long)strm.total_out, cap_limit);
            ret = CU_ERR_SIZE_LIMIT;
        } else {
            *out_len = (size_t)strm.total_out;
            ret = CU_OK;
        }
    } else if (r == LZMA_OK && strm.avail_out == 0) {
        cu_set_last_error("xz: output buffer too small (size not probed)");
        ret = CU_ERR_SIZE_UNKNOWN;
    } else if (r == LZMA_BUF_ERROR) {
        cu_set_last_error("xz: truncated input or output too small");
        ret = CU_ERR_TRUNCATED;
    } else {
        ret = map_lzma_error(r, CU_ERR_DECOMPRESSION);
    }
    lzma_end(&strm);
    return ret;
}

static cu_status_t xz_decompress_size_hint(
    const uint8_t* in, size_t in_len, size_t* out_size
) {
    (void)in; (void)in_len; (void)out_size;
    /* TODO: parse the xz stream footer/index for the uncompressed size.
     * For now, callers route to streaming for size-unknown inputs. */
    return CU_ERR_SIZE_UNKNOWN;
}

/* ============================================================================
 * Streaming (shared infrastructure)
 * ============================================================================ */

typedef struct {
    lzma_stream strm;
    int      strm_inited;
    uint8_t* pending;
    size_t   pending_len;
    size_t   pending_cap;
    int      finishing;
    int      stream_end;
} xz_stream_state_t;

static cu_status_t pending_append(xz_stream_state_t* st, const uint8_t* src, size_t n) {
    if (n == 0) return CU_OK;
    size_t need = st->pending_len + n;
    if (need > st->pending_cap) {
        size_t new_cap = st->pending_cap ? st->pending_cap * 2 : 16 * 1024;
        while (new_cap < need) new_cap *= 2;
        uint8_t* p = realloc(st->pending, new_cap);
        if (!p) { cu_set_last_error("xz: oom"); return CU_ERR_OOM; }
        st->pending = p;
        st->pending_cap = new_cap;
    }
    memcpy(st->pending + st->pending_len, src, n);
    st->pending_len += n;
    return CU_OK;
}

static cu_status_t stream_pump(xz_stream_state_t* st, lzma_action action,
                               uint8_t* out, size_t* out_len) {
    size_t cap = *out_len;
    size_t written = 0;

    st->strm.next_in = st->pending;
    st->strm.avail_in = st->pending_len;
    for (;;) {
        st->strm.next_out = out + written;
        st->strm.avail_out = cap - written;
        size_t before = st->strm.avail_out;
        lzma_ret r = lzma_code(&st->strm, action);
        written += before - st->strm.avail_out;

        if (r == LZMA_STREAM_END) {
            st->stream_end = 1;
            size_t consumed = st->pending_len - st->strm.avail_in;
            if (consumed > 0 && st->strm.avail_in > 0) {
                memmove(st->pending, st->pending + consumed, st->strm.avail_in);
            }
            st->pending_len = st->strm.avail_in;
            *out_len = written;
            return CU_OK;
        }
        if (r == LZMA_OK) {
            if (st->strm.avail_out == 0 && (st->strm.avail_in > 0 || action != LZMA_RUN)) {
                /* Out full and more to write. */
                size_t consumed = st->pending_len - st->strm.avail_in;
                if (consumed > 0 && st->strm.avail_in > 0) {
                    memmove(st->pending, st->pending + consumed, st->strm.avail_in);
                }
                st->pending_len = st->strm.avail_in;
                *out_len = written;
                return CU_ERR_BUF_TOO_SMALL;
            }
            if (action == LZMA_RUN && st->strm.avail_in == 0) {
                st->pending_len = 0;
                *out_len = written;
                return CU_OK;
            }
            continue;
        }
        return map_lzma_error(r, action == LZMA_RUN ? CU_ERR_COMPRESSION : CU_ERR_DECOMPRESSION);
    }
}

/* ============================================================================
 * Streaming compression
 * ============================================================================ */

static cu_status_t xz_cstream_create(int level, void** out_state) {
    xz_stream_state_t* st = calloc(1, sizeof(*st));
    if (!st) { cu_set_last_error("xz: oom"); return CU_ERR_OOM; }
    lzma_stream init = LZMA_STREAM_INIT;
    st->strm = init;
    lzma_ret r = lzma_easy_encoder(&st->strm, xz_native_level(level), LZMA_CHECK_CRC64);
    if (r != LZMA_OK) {
        cu_status_t s = map_lzma_error(r, CU_ERR_COMPRESSION);
        free(st);
        return s;
    }
    st->strm_inited = 1;
    *out_state = st;
    return CU_OK;
}

static cu_status_t xz_cstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    xz_stream_state_t* st = (xz_stream_state_t*)state;
    if (st->finishing) {
        cu_set_last_error("xz: write after finish");
        return CU_ERR_STREAM_STATE;
    }
    cu_status_t s = pending_append(st, in, in_len);
    if (s != CU_OK) return s;
    return stream_pump(st, LZMA_RUN, out, out_len);
}

static cu_status_t xz_cstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    xz_stream_state_t* st = (xz_stream_state_t*)state;
    st->finishing = 1;
    if (st->stream_end) { *out_len = 0; return CU_OK; }
    return stream_pump(st, LZMA_FINISH, out, out_len);
}

static void xz_cstream_destroy(void* state) {
    xz_stream_state_t* st = (xz_stream_state_t*)state;
    if (!st) return;
    if (st->strm_inited) lzma_end(&st->strm);
    free(st->pending);
    free(st);
}

/* ============================================================================
 * Streaming decompression
 * ============================================================================ */

static cu_status_t xz_dstream_create(void** out_state) {
    xz_stream_state_t* st = calloc(1, sizeof(*st));
    if (!st) { cu_set_last_error("xz: oom"); return CU_ERR_OOM; }
    lzma_stream init = LZMA_STREAM_INIT;
    st->strm = init;
    lzma_ret r = lzma_auto_decoder(&st->strm, UINT64_MAX, LZMA_CONCATENATED);
    if (r != LZMA_OK) {
        cu_status_t s = map_lzma_error(r, CU_ERR_DECOMPRESSION);
        free(st);
        return s;
    }
    st->strm_inited = 1;
    *out_state = st;
    return CU_OK;
}

static cu_status_t xz_dstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    xz_stream_state_t* st = (xz_stream_state_t*)state;
    if (st->stream_end && in_len > 0) {
        cu_set_last_error("xz: data after end of stream");
        return CU_ERR_DECOMPRESSION;
    }
    cu_status_t s = pending_append(st, in, in_len);
    if (s != CU_OK) return s;
    return stream_pump(st, LZMA_RUN, out, out_len);
}

static cu_status_t xz_dstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    xz_stream_state_t* st = (xz_stream_state_t*)state;
    if (st->stream_end && st->pending_len == 0) {
        *out_len = 0;
        return CU_OK;
    }
    cu_status_t s = stream_pump(st, LZMA_FINISH, out, out_len);
    if (s != CU_OK) return s;
    if (!st->stream_end) {
        cu_set_last_error("xz: truncated stream at finish");
        return CU_ERR_TRUNCATED;
    }
    return CU_OK;
}

static void xz_dstream_destroy(void* state) {
    xz_stream_state_t* st = (xz_stream_state_t*)state;
    if (!st) return;
    if (st->strm_inited) lzma_end(&st->strm);
    free(st->pending);
    free(st);
}

/* ============================================================================
 * Vtable
 * ============================================================================ */

const cu_algorithm_vtbl_t cu_xz_vtbl = {
    .name                      = "xz",
    .compress_bound            = xz_compress_bound,
    .compress                  = xz_compress,
    .decompress                = xz_decompress,
    .decompress_size_hint      = xz_decompress_size_hint,
    .compress_stream_create    = xz_cstream_create,
    .compress_stream_write     = xz_cstream_write,
    .compress_stream_finish    = xz_cstream_finish,
    .compress_stream_destroy   = xz_cstream_destroy,
    .decompress_stream_create  = xz_dstream_create,
    .decompress_stream_write   = xz_dstream_write,
    .decompress_stream_finish  = xz_dstream_finish,
    .decompress_stream_destroy = xz_dstream_destroy,
};
