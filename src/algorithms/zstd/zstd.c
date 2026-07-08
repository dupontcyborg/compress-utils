/*
 * zstd.c — ZSTD vtable implementation for compress-utils.
 *
 * Conforms to the cu_algorithm_vtbl_t shape declared in
 * algorithm_registry.h.
 *
 * One-shot:
 *   - compress writes a complete ZSTD frame including pledgedSrcSize, so
 *     cu_decompress_size_hint can always probe and so streaming-produced
 *     frames can be one-shot decompressed (we set pledgedSrcSize from
 *     in_len).
 *
 *   - decompress consults ZSTD_getFrameContentSize. If the frame has a
 *     known content size, fails fast if out is too small. If the size is
 *     unknown (e.g. frame from a different producer without
 *     pledgedSrcSize), falls back to ZSTD_decompressStream into the
 *     caller's buffer; returns CU_ERR_SIZE_UNKNOWN if that exhausts the
 *     buffer.
 *
 * Streaming:
 *   - Write/finish honor the public ABI's "fill buffer, return
 *     BUF_TOO_SMALL with unconsumed state preserved" protocol. State
 *     buffers the unconsumed tail of `in` between calls so the caller
 *     can drain with (in=NULL, in_len=0).
 */

#include "algorithm_registry.h"
#include "compress_utils.h"
#include "utils/levels.h"

#include "zstd/zstd.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ZSTD: user 1..10 → ZSTD native 1..22. */
static int zstd_native_level(int user_level) {
    return cu_scale_level(user_level, 22);
}

static cu_status_t map_zstd_error(size_t code, cu_status_t fallback) {
    if (!ZSTD_isError(code)) return CU_OK;
    cu_set_last_errorf("zstd: %s", ZSTD_getErrorName(code));
    return fallback;
}

/* ============================================================================
 * One-shot
 * ============================================================================ */

static size_t zstd_compress_bound(size_t in_len) {
    return ZSTD_compressBound(in_len);
}

static cu_status_t zstd_compress(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len,
    int level
) {
    size_t cap = *out_len;
    size_t needed = ZSTD_compressBound(in_len);
    if (cap < needed) {
        *out_len = needed;
        return CU_ERR_BUF_TOO_SMALL;
    }

    /* Use a one-shot CCtx so we can set pledgedSrcSize; this writes the
     * decompressed size into the frame header, which makes the inverse
     * (one-shot decompress, size hint probe) straightforward. */
    ZSTD_CCtx* cctx = ZSTD_createCCtx();
    if (!cctx) {
        cu_set_last_error("zstd: ZSTD_createCCtx failed");
        return CU_ERR_OOM;
    }

    size_t r;
    r = ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, zstd_native_level(level));
    if (ZSTD_isError(r)) {
        cu_status_t s = map_zstd_error(r, CU_ERR_COMPRESSION);
        ZSTD_freeCCtx(cctx);
        return s;
    }
    r = ZSTD_CCtx_setPledgedSrcSize(cctx, in_len);
    if (ZSTD_isError(r)) {
        cu_status_t s = map_zstd_error(r, CU_ERR_COMPRESSION);
        ZSTD_freeCCtx(cctx);
        return s;
    }

    r = ZSTD_compress2(cctx, out, cap, in, in_len);
    ZSTD_freeCCtx(cctx);
    if (ZSTD_isError(r)) {
        return map_zstd_error(r, CU_ERR_COMPRESSION);
    }
    *out_len = r;
    return CU_OK;
}

static cu_status_t zstd_decompress(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    if (in_len == 0) {
        cu_set_last_error("zstd: empty input");
        return CU_ERR_TRUNCATED;
    }

    unsigned long long content_size = ZSTD_getFrameContentSize(in, in_len);

    if (content_size == ZSTD_CONTENTSIZE_ERROR) {
        cu_set_last_error("zstd: not a valid zstd frame");
        return CU_ERR_DECOMPRESSION;
    }

    size_t cap = *out_len;
    size_t cap_limit = cu_get_max_decompressed_size();

    if (content_size != ZSTD_CONTENTSIZE_UNKNOWN) {
        if (cap_limit > 0 && content_size > cap_limit) {
            cu_set_last_errorf("zstd: declared size %llu exceeds cap %zu",
                               content_size, cap_limit);
            return CU_ERR_SIZE_LIMIT;
        }
        if (cap < (size_t)content_size) {
            *out_len = (size_t)content_size;
            return CU_ERR_BUF_TOO_SMALL;
        }
        size_t r = ZSTD_decompress(out, cap, in, in_len);
        if (ZSTD_isError(r)) {
            return map_zstd_error(r, CU_ERR_DECOMPRESSION);
        }
        *out_len = r;
        return CU_OK;
    }

    /* Unknown size: stream into the caller's buffer. If it fits, great;
     * if not, surface CU_ERR_SIZE_UNKNOWN so the caller knows to switch
     * to the streaming API (we cannot tell them how much to allocate). */
    ZSTD_DStream* ds = ZSTD_createDStream();
    if (!ds) {
        cu_set_last_error("zstd: ZSTD_createDStream failed");
        return CU_ERR_OOM;
    }
    size_t r = ZSTD_initDStream(ds);
    if (ZSTD_isError(r)) {
        cu_status_t s = map_zstd_error(r, CU_ERR_DECOMPRESSION);
        ZSTD_freeDStream(ds);
        return s;
    }

    ZSTD_inBuffer in_buf  = { in,  in_len, 0 };
    ZSTD_outBuffer out_buf = { out, cap,   0 };
    int frame_done = 0;
    while (in_buf.pos < in_buf.size) {
        size_t ret = ZSTD_decompressStream(ds, &out_buf, &in_buf);
        if (ZSTD_isError(ret)) {
            cu_status_t s = map_zstd_error(ret, CU_ERR_DECOMPRESSION);
            ZSTD_freeDStream(ds);
            return s;
        }
        if (ret == 0) { frame_done = 1; break; }
        if (out_buf.pos == out_buf.size && in_buf.pos < in_buf.size) {
            /* Buffer exhausted, input remains. We cannot grow. */
            ZSTD_freeDStream(ds);
            cu_set_last_error("zstd: output buffer exhausted and size unknown; use streaming");
            return CU_ERR_SIZE_UNKNOWN;
        }
        if (cap_limit > 0 && out_buf.pos > cap_limit) {
            ZSTD_freeDStream(ds);
            cu_set_last_errorf("zstd: decompressed output exceeded cap %zu", cap_limit);
            return CU_ERR_SIZE_LIMIT;
        }
    }
    ZSTD_freeDStream(ds);
    if (!frame_done) {
        cu_set_last_error("zstd: truncated frame");
        return CU_ERR_TRUNCATED;
    }
    *out_len = out_buf.pos;
    return CU_OK;
}

static cu_status_t zstd_decompress_size_hint(
    const uint8_t* in, size_t in_len, size_t* out_size
) {
    if (in_len == 0) {
        cu_set_last_error("zstd: empty input");
        return CU_ERR_TRUNCATED;
    }
    /* ZSTD_getFrameContentSize gracefully handles short/invalid input;
     * we don't need the static-only ZSTD_FRAMEHEADERSIZE_MIN macro. */
    unsigned long long n = ZSTD_getFrameContentSize(in, in_len);
    if (n == ZSTD_CONTENTSIZE_ERROR) {
        cu_set_last_error("zstd: not a valid zstd frame");
        return CU_ERR_DECOMPRESSION;
    }
    if (n == ZSTD_CONTENTSIZE_UNKNOWN) {
        return CU_ERR_SIZE_UNKNOWN;
    }
    if (n > (size_t)-1) {
        cu_set_last_error("zstd: declared size exceeds size_t");
        return CU_ERR_SIZE_LIMIT;
    }
    *out_size = (size_t)n;
    return CU_OK;
}

/* ============================================================================
 * Streaming
 *
 * The state struct carries the codec stream plus a small pending-input
 * buffer that holds the tail of the caller's `in` when the output filled
 * mid-write. On the next call (which the caller should make with
 * in=NULL, in_len=0), we drain pending_in before consuming new input.
 *
 * This is the fix for the bug in the prior C++ streaming: writes that
 * overflowed the output buffer used to silently drop the unconsumed input.
 * ============================================================================ */

typedef struct {
    ZSTD_CStream* cs;
    /* Pending unconsumed bytes from the previous write call. Heap-allocated;
     * may be NULL when empty. */
    uint8_t* pending;
    size_t   pending_len;
    size_t   pending_cap;
    int      finishing;  /* set once cu_compress_stream_finish() begins draining */
} zstd_cstream_state_t;

static cu_status_t zstd_cstream_create(int level, void** out_state) {
    zstd_cstream_state_t* st = calloc(1, sizeof(*st));
    if (!st) {
        cu_set_last_error("zstd: out of memory");
        return CU_ERR_OOM;
    }
    st->cs = ZSTD_createCStream();
    if (!st->cs) {
        free(st);
        cu_set_last_error("zstd: ZSTD_createCStream failed");
        return CU_ERR_OOM;
    }
    size_t r = ZSTD_CCtx_setParameter(st->cs, ZSTD_c_compressionLevel, zstd_native_level(level));
    if (ZSTD_isError(r)) {
        cu_status_t s = map_zstd_error(r, CU_ERR_COMPRESSION);
        ZSTD_freeCStream(st->cs);
        free(st);
        return s;
    }
    *out_state = st;
    return CU_OK;
}

/*
 * Append `n` bytes of `src` to the pending buffer, growing as needed.
 */
static cu_status_t pending_append(zstd_cstream_state_t* st, const uint8_t* src, size_t n) {
    /* Avoid the trivial case (no-op append shouldn't trigger realloc). */
    if (n == 0) return CU_OK;
    size_t need = st->pending_len + n;
    if (need > st->pending_cap) {
        size_t new_cap = st->pending_cap ? st->pending_cap * 2 : 16 * 1024;
        while (new_cap < need) new_cap *= 2;
        uint8_t* p = realloc(st->pending, new_cap);
        if (!p) {
            cu_set_last_error("zstd: out of memory in pending buffer");
            return CU_ERR_OOM;
        }
        st->pending = p;
        st->pending_cap = new_cap;
    }
    memcpy(st->pending + st->pending_len, src, n);
    st->pending_len += n;
    return CU_OK;
}

static cu_status_t zstd_cstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    zstd_cstream_state_t* st = (zstd_cstream_state_t*)state;
    if (st->finishing) {
        cu_set_last_error("zstd: write after finish started");
        return CU_ERR_STREAM_STATE;
    }
    size_t cap = *out_len;
    ZSTD_outBuffer ob = { out, cap, 0 };

    /* Drain any previously-buffered tail first. */
    if (st->pending_len > 0) {
        ZSTD_inBuffer ib = { st->pending, st->pending_len, 0 };
        while (ib.pos < ib.size) {
            size_t r = ZSTD_compressStream2(st->cs, &ob, &ib, ZSTD_e_continue);
            if (ZSTD_isError(r)) return map_zstd_error(r, CU_ERR_COMPRESSION);
            if (ob.pos == ob.size && ib.pos < ib.size) {
                /* Output filled; shift remaining tail to front and surface. */
                size_t consumed = ib.pos;
                memmove(st->pending, st->pending + consumed, ib.size - consumed);
                st->pending_len = ib.size - consumed;
                /* Also stash all of new input since we never started it. */
                cu_status_t s = pending_append(st, in, in_len);
                if (s != CU_OK) return s;
                *out_len = ob.pos;
                return CU_ERR_BUF_TOO_SMALL;
            }
        }
        st->pending_len = 0;
    }

    /* Feed new input. */
    ZSTD_inBuffer ib = { in, in_len, 0 };
    while (ib.pos < ib.size) {
        size_t r = ZSTD_compressStream2(st->cs, &ob, &ib, ZSTD_e_continue);
        if (ZSTD_isError(r)) return map_zstd_error(r, CU_ERR_COMPRESSION);
        if (ob.pos == ob.size && ib.pos < ib.size) {
            /* Output filled; stash unconsumed tail. */
            cu_status_t s = pending_append(st, in + ib.pos, in_len - ib.pos);
            if (s != CU_OK) return s;
            *out_len = ob.pos;
            return CU_ERR_BUF_TOO_SMALL;
        }
    }

    *out_len = ob.pos;
    return CU_OK;
}

static cu_status_t zstd_cstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    zstd_cstream_state_t* st = (zstd_cstream_state_t*)state;
    st->finishing = 1;
    size_t cap = *out_len;
    ZSTD_outBuffer ob = { out, cap, 0 };

    /* Drain any pending tail first. */
    if (st->pending_len > 0) {
        ZSTD_inBuffer ib = { st->pending, st->pending_len, 0 };
        while (ib.pos < ib.size) {
            size_t r = ZSTD_compressStream2(st->cs, &ob, &ib, ZSTD_e_continue);
            if (ZSTD_isError(r)) return map_zstd_error(r, CU_ERR_COMPRESSION);
            if (ob.pos == ob.size && ib.pos < ib.size) {
                size_t consumed = ib.pos;
                memmove(st->pending, st->pending + consumed, ib.size - consumed);
                st->pending_len = ib.size - consumed;
                *out_len = ob.pos;
                return CU_ERR_BUF_TOO_SMALL;
            }
        }
        st->pending_len = 0;
    }

    /* End the frame, looping until ZSTD reports nothing remaining. */
    ZSTD_inBuffer ib = { NULL, 0, 0 };
    for (;;) {
        size_t r = ZSTD_compressStream2(st->cs, &ob, &ib, ZSTD_e_end);
        if (ZSTD_isError(r)) return map_zstd_error(r, CU_ERR_COMPRESSION);
        if (r == 0) {
            *out_len = ob.pos;
            return CU_OK;
        }
        if (ob.pos == ob.size) {
            /* More to write, no room. */
            *out_len = ob.pos;
            return CU_ERR_BUF_TOO_SMALL;
        }
        /* r > 0 with room remaining: loop and write more. */
    }
}

static void zstd_cstream_destroy(void* state) {
    zstd_cstream_state_t* st = (zstd_cstream_state_t*)state;
    if (!st) return;
    if (st->cs) ZSTD_freeCStream(st->cs);
    free(st->pending);
    free(st);
}

/* ----- Decompression stream ----- */

typedef struct {
    ZSTD_DStream* ds;
    uint8_t* pending;
    size_t   pending_len;
    size_t   pending_cap;
    int      frame_done;
} zstd_dstream_state_t;

static cu_status_t zstd_dstream_create(void** out_state) {
    zstd_dstream_state_t* st = calloc(1, sizeof(*st));
    if (!st) {
        cu_set_last_error("zstd: out of memory");
        return CU_ERR_OOM;
    }
    st->ds = ZSTD_createDStream();
    if (!st->ds) {
        free(st);
        cu_set_last_error("zstd: ZSTD_createDStream failed");
        return CU_ERR_OOM;
    }
    size_t r = ZSTD_initDStream(st->ds);
    if (ZSTD_isError(r)) {
        cu_status_t s = map_zstd_error(r, CU_ERR_DECOMPRESSION);
        ZSTD_freeDStream(st->ds);
        free(st);
        return s;
    }
    *out_state = st;
    return CU_OK;
}

static cu_status_t dstream_pending_append(zstd_dstream_state_t* st, const uint8_t* src, size_t n) {
    if (n == 0) return CU_OK;
    size_t need = st->pending_len + n;
    if (need > st->pending_cap) {
        size_t new_cap = st->pending_cap ? st->pending_cap * 2 : 16 * 1024;
        while (new_cap < need) new_cap *= 2;
        uint8_t* p = realloc(st->pending, new_cap);
        if (!p) {
            cu_set_last_error("zstd: out of memory in decompression pending buffer");
            return CU_ERR_OOM;
        }
        st->pending = p;
        st->pending_cap = new_cap;
    }
    memcpy(st->pending + st->pending_len, src, n);
    st->pending_len += n;
    return CU_OK;
}

static cu_status_t zstd_dstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    zstd_dstream_state_t* st = (zstd_dstream_state_t*)state;
    if (st->frame_done) {
        cu_set_last_error("zstd: write after frame end");
        return CU_ERR_STREAM_FINISHED;
    }
    size_t cap = *out_len;
    ZSTD_outBuffer ob = { out, cap, 0 };

    /* Drain pending input first. */
    if (st->pending_len > 0) {
        ZSTD_inBuffer ib = { st->pending, st->pending_len, 0 };
        while (ib.pos < ib.size) {
            size_t r = ZSTD_decompressStream(st->ds, &ob, &ib);
            if (ZSTD_isError(r)) return map_zstd_error(r, CU_ERR_DECOMPRESSION);
            if (r == 0) { st->frame_done = 1; break; }
            if (ob.pos == ob.size && ib.pos < ib.size) {
                size_t consumed = ib.pos;
                memmove(st->pending, st->pending + consumed, ib.size - consumed);
                st->pending_len = ib.size - consumed;
                cu_status_t s = dstream_pending_append(st, in, in_len);
                if (s != CU_OK) return s;
                *out_len = ob.pos;
                return CU_ERR_BUF_TOO_SMALL;
            }
        }
        st->pending_len = 0;
    }

    if (st->frame_done) {
        if (in_len > 0) {
            cu_set_last_error("zstd: trailing data after end of frame");
            return CU_ERR_DECOMPRESSION;
        }
        *out_len = ob.pos;
        return CU_OK;
    }

    ZSTD_inBuffer ib = { in, in_len, 0 };
    while (ib.pos < ib.size) {
        size_t r = ZSTD_decompressStream(st->ds, &ob, &ib);
        if (ZSTD_isError(r)) return map_zstd_error(r, CU_ERR_DECOMPRESSION);
        if (r == 0) { st->frame_done = 1; break; }
        if (ob.pos == ob.size && ib.pos < ib.size) {
            cu_status_t s = dstream_pending_append(st, in + ib.pos, in_len - ib.pos);
            if (s != CU_OK) return s;
            *out_len = ob.pos;
            return CU_ERR_BUF_TOO_SMALL;
        }
    }

    *out_len = ob.pos;
    return CU_OK;
}

static cu_status_t zstd_dstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    zstd_dstream_state_t* st = (zstd_dstream_state_t*)state;
    /* If pending input remains, try to drain it (frame may complete mid-pending). */
    if (st->pending_len > 0) {
        cu_status_t s = zstd_dstream_write(state, NULL, 0, out, out_len);
        if (s != CU_OK) return s;
    } else {
        *out_len = 0;
    }
    if (!st->frame_done) {
        cu_set_last_error("zstd: truncated frame at finish");
        return CU_ERR_TRUNCATED;
    }
    return CU_OK;
}

static void zstd_dstream_destroy(void* state) {
    zstd_dstream_state_t* st = (zstd_dstream_state_t*)state;
    if (!st) return;
    if (st->ds) ZSTD_freeDStream(st->ds);
    free(st->pending);
    free(st);
}

/* ============================================================================
 * Vtable
 * ============================================================================ */

/*
 * Direction split (WASM size opt #7): CU_OMIT_COMPRESS / CU_OMIT_DECOMPRESS
 * leave the unused direction's vtable slots NULL. Those static functions then
 * have no referrer, so LTO + wasm-ld GC drop them and their upstream codec
 * closure (e.g. the whole zstd encoder for a decompress-only module). The vtbl
 * struct layout is unchanged — compress_utils.c sees NULL and never dispatches
 * there because that direction's cu_* entry points aren't exported either.
 */
const cu_algorithm_vtbl_t cu_zstd_vtbl = {
    .name                     = "zstd",
#ifndef CU_OMIT_COMPRESS
    .compress_bound           = zstd_compress_bound,
    .compress                 = zstd_compress,
    .compress_stream_create   = zstd_cstream_create,
    .compress_stream_write    = zstd_cstream_write,
    .compress_stream_finish   = zstd_cstream_finish,
    .compress_stream_destroy  = zstd_cstream_destroy,
#endif
#ifndef CU_OMIT_DECOMPRESS
    .decompress               = zstd_decompress,
    .decompress_size_hint     = zstd_decompress_size_hint,
    .decompress_stream_create = zstd_dstream_create,
    .decompress_stream_write  = zstd_dstream_write,
    .decompress_stream_finish = zstd_dstream_finish,
    .decompress_stream_destroy = zstd_dstream_destroy,
#endif
};
