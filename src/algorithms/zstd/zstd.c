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
 *   - decompress sums the content sizes of every frame in the input
 *     (concatenated frames, as zstd/pzstd write them, decode as one
 *     output). If all frames record their size, fails fast if out is too
 *     small. If any doesn't (e.g. a producer without pledgedSrcSize),
 *     falls back to ZSTD_decompressStream into the caller's buffer;
 *     returns CU_ERR_SIZE_UNKNOWN if that exhausts the buffer.
 *
 * Streaming:
 *   - Write/finish honor the public ABI's "fill buffer, return
 *     BUF_TOO_SMALL with unconsumed state preserved" protocol. State
 *     buffers the unconsumed tail of `in` between calls so the caller
 *     can drain with (in=NULL, in_len=0).
 *   - Decompression continues across frame boundaries, including skippable
 *     frames; finish() reports truncation unless the input ended exactly
 *     at the end of a frame.
 */

#define ZSTD_STATIC_LINKING_ONLY  /* ZSTD_findDecompressedSize */

#include "algorithm_registry.h"
#include "compress_utils.h"
#include "utils/levels.h"

#include <zstd.h>

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

/* Total decompressed size of every frame in the input, or
 * ZSTD_CONTENTSIZE_UNKNOWN if any frame doesn't record its size. Malformed or
 * truncated input falls back to the first frame's header, so the decoder
 * itself reports what is wrong. */
static unsigned long long zstd_content_size(const uint8_t* in, size_t in_len) {
    unsigned long long n = ZSTD_findDecompressedSize(in, in_len);
    return n == ZSTD_CONTENTSIZE_ERROR ? ZSTD_getFrameContentSize(in, in_len) : n;
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

    unsigned long long content_size = zstd_content_size(in, in_len);

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
        /* 0 ends a frame; any input left is the next frame. */
        frame_done = ret == 0;
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
    /* Handles short/invalid input gracefully; no need for the static-only
     * ZSTD_FRAMEHEADERSIZE_MIN macro. */
    unsigned long long n = zstd_content_size(in, in_len);
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
    /* Unconsumed bytes from a previous write call; live bytes are
     * [pending_off, pending_len). Heap-allocated; may be NULL when empty. */
    uint8_t* pending;
    size_t   pending_off;
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
 * Append `n` bytes of `src` to the live pending bytes, first moving them to
 * the front, and growing as needed.
 */
static cu_status_t pending_append(zstd_cstream_state_t* st, const uint8_t* src, size_t n) {
    /* Avoid the trivial case (no-op append shouldn't trigger realloc). */
    if (n == 0) return CU_OK;
    if (st->pending_off > 0) {
        st->pending_len -= st->pending_off;
        memmove(st->pending, st->pending + st->pending_off, st->pending_len);
        st->pending_off = 0;
    }
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

/* Feed `ib` to the encoder until it's consumed or `ob` fills. A full `ob`
 * returns BUF_TOO_SMALL even when all input was consumed, since zstd may still
 * hold compressed output; the drain call then runs the encoder again. */
static cu_status_t cstream_pump(zstd_cstream_state_t* st, ZSTD_inBuffer* ib, ZSTD_outBuffer* ob) {
    for (;;) {
        size_t r = ZSTD_compressStream2(st->cs, ob, ib, ZSTD_e_continue);
        if (ZSTD_isError(r)) return map_zstd_error(r, CU_ERR_COMPRESSION);
        if (ob->pos == ob->size) return CU_ERR_BUF_TOO_SMALL;
        if (ib->pos == ib->size) return CU_OK;
    }
}

/* Encodes retained input first, then `in`; keeps whatever is left unconsumed
 * when the output fills. */
static cu_status_t cstream_feed(zstd_cstream_state_t* st, const uint8_t* in, size_t in_len,
                                ZSTD_outBuffer* ob) {
    cu_status_t s;
    if (st->pending_off < st->pending_len) {
        ZSTD_inBuffer ib = { st->pending + st->pending_off, st->pending_len - st->pending_off, 0 };
        s = cstream_pump(st, &ib, ob);
        st->pending_off += ib.pos;
        if (s == CU_ERR_BUF_TOO_SMALL) {
            cu_status_t a = pending_append(st, in, in_len);
            return a != CU_OK ? a : s;
        }
        if (s != CU_OK) return s;
        st->pending_off = st->pending_len = 0;
    }
    ZSTD_inBuffer ib = { in, in_len, 0 };
    s = cstream_pump(st, &ib, ob);
    if (s == CU_ERR_BUF_TOO_SMALL) {
        cu_status_t a = pending_append(st, in + ib.pos, in_len - ib.pos);
        if (a != CU_OK) return a;
    }
    return s;
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
    ZSTD_outBuffer ob = { out, *out_len, 0 };
    cu_status_t s = cstream_feed(st, in, in_len, &ob);
    *out_len = ob.pos;
    return s;
}

/* Feeds retained input, then runs `directive` (ZSTD_e_flush or ZSTD_e_end)
 * until zstd reports nothing left to write. */
static cu_status_t cstream_drain(zstd_cstream_state_t* st, ZSTD_EndDirective directive,
                                 uint8_t* out, size_t* out_len) {
    ZSTD_outBuffer ob = { out, *out_len, 0 };

    /* Feed any retained input first; the directive below flushes the rest. */
    if (st->pending_off < st->pending_len) {
        cu_status_t s = cstream_feed(st, NULL, 0, &ob);
        if (s != CU_OK) {
            *out_len = ob.pos;
            return s;
        }
    }

    ZSTD_inBuffer ib = { NULL, 0, 0 };
    for (;;) {
        size_t r = ZSTD_compressStream2(st->cs, &ob, &ib, directive);
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

static cu_status_t zstd_cstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    zstd_cstream_state_t* st = (zstd_cstream_state_t*)state;
    st->finishing = 1;
    return cstream_drain(st, ZSTD_e_end, out, out_len);
}

/* Ends the current block, so everything written so far can be decoded; the
 * frame stays open. */
static cu_status_t zstd_cstream_flush(
    void* state, uint8_t* out, size_t* out_len
) {
    zstd_cstream_state_t* st = (zstd_cstream_state_t*)state;
    if (st->finishing) {
        cu_set_last_error("zstd: flush after finish started");
        return CU_ERR_STREAM_STATE;
    }
    return cstream_drain(st, ZSTD_e_flush, out, out_len);
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
    uint8_t* pending;       /* unconsumed input; live bytes are [pending_off, pending_len) */
    size_t   pending_off;
    size_t   pending_len;
    size_t   pending_cap;
    int      at_frame_end;  /* the last input consumed completed a frame */
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

/* Appends to the live pending bytes, first moving them to the front. Draining
 * only advances pending_off, so each byte is moved at most once per append
 * rather than once per output page. */
static cu_status_t dstream_pending_append(zstd_dstream_state_t* st, const uint8_t* src, size_t n) {
    if (n == 0) return CU_OK;
    if (st->pending_off > 0) {
        st->pending_len -= st->pending_off;
        memmove(st->pending, st->pending + st->pending_off, st->pending_len);
        st->pending_off = 0;
    }
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

/* Feed `ib` to the decoder until it's consumed or `ob` fills. A full `ob`
 * returns BUF_TOO_SMALL even when all input was consumed, since zstd may still
 * hold output to flush (the drain call then runs the decoder again), unless
 * the decoder just finished a frame, which leaves nothing held. */
static cu_status_t dstream_pump(zstd_dstream_state_t* st, ZSTD_inBuffer* ib, ZSTD_outBuffer* ob) {
    for (;;) {
        size_t before = ib->pos;
        size_t r = ZSTD_decompressStream(st->ds, ob, ib);
        if (ZSTD_isError(r)) return map_zstd_error(r, CU_ERR_DECOMPRESSION);
        /* 0 ends a frame (the decoder then expects the next one); any other
         * value after consuming input means a frame is in progress. */
        if (r == 0) st->at_frame_end = 1;
        else if (ib->pos > before) st->at_frame_end = 0;
        if (ib->pos == ib->size && (r == 0 || ob->pos < ob->size)) return CU_OK;
        if (ob->pos == ob->size) return CU_ERR_BUF_TOO_SMALL;
    }
}

static cu_status_t zstd_dstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    zstd_dstream_state_t* st = (zstd_dstream_state_t*)state;
    ZSTD_outBuffer ob = { out, *out_len, 0 };
    cu_status_t s;

    /* Drain pending input first. */
    if (st->pending_off < st->pending_len) {
        ZSTD_inBuffer ib = { st->pending + st->pending_off, st->pending_len - st->pending_off, 0 };
        s = dstream_pump(st, &ib, &ob);
        st->pending_off += ib.pos;
        *out_len = ob.pos;
        if (s == CU_ERR_BUF_TOO_SMALL) {
            cu_status_t a = dstream_pending_append(st, in, in_len);
            return a != CU_OK ? a : s;
        }
        if (s != CU_OK) return s;
        st->pending_off = st->pending_len = 0;
    }

    ZSTD_inBuffer ib = { in, in_len, 0 };
    s = dstream_pump(st, &ib, &ob);
    *out_len = ob.pos;
    if (s == CU_ERR_BUF_TOO_SMALL) {
        cu_status_t a = dstream_pending_append(st, in + ib.pos, in_len - ib.pos);
        if (a != CU_OK) return a;
    }
    return s;
}

static cu_status_t zstd_dstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    zstd_dstream_state_t* st = (zstd_dstream_state_t*)state;
    /* Decode any pending input and flush what the decoder still holds. */
    cu_status_t s = zstd_dstream_write(state, NULL, 0, out, out_len);
    if (s != CU_OK) return s;
    if (!st->at_frame_end) {
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
    .compress_stream_flush    = zstd_cstream_flush,
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
