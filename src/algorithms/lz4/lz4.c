/*
 * lz4.c — LZ4 vtable for compress-utils.
 *
 * Wire-format unification: this implementation uses LZ4 frame format
 * (LZ4F_*) for both one-shot and streaming. The legacy C++ one-shot
 * used raw LZ4 blocks with a 4-byte size prefix, which was incompatible
 * with the streaming path (which already used LZ4F). The new ABI fixes
 * this — one-shot output and streaming output are bit-compatible and
 * interoperable with the standard `lz4` CLI / .lz4 files.
 *
 * Size hint: LZ4 frames optionally carry the content size if the
 * content-size flag is set at encode time. We always set it on encode,
 * so cu_decompress_size_hint succeeds for frames produced by this
 * library.
 */

#include "algorithm_registry.h"
#include "compress_utils.h"

#include "lz4/lz4.h"
#include "lz4/lz4frame.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int lz4_native_level(int user_level) {
    /* LZ4F_compressionLevel: 0..12 (0/1 = LZ4 fast; 3+ = LZ4HC).
     * User 1..10 -> mapping designed to give a meaningful spread:
     *   1..3 -> 0..2 (fast mode)
     *   4..10 -> 4..12 (HC mode 4..12).
     */
    if (user_level <= 3) return user_level - 1;
    /* level 4..10 -> 4..12: (level - 4) * 8 / 6 + 4 -> approx */
    return 4 + ((user_level - 4) * 8) / 6;
}

static cu_status_t map_lz4f_err(size_t code, cu_status_t fallback) {
    if (!LZ4F_isError(code)) return CU_OK;
    cu_set_last_errorf("lz4: %s", LZ4F_getErrorName(code));
    return fallback;
}

static void make_prefs(LZ4F_preferences_t* prefs, int user_level, size_t content_size) {
    memset(prefs, 0, sizeof(*prefs));
    prefs->compressionLevel = lz4_native_level(user_level);
    /* Always embed the content size in the frame header — enables
     * cu_decompress_size_hint for our own output, and is the standard
     * thing to do. */
    prefs->frameInfo.contentSize = content_size;
    prefs->frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    prefs->frameInfo.blockMode = LZ4F_blockLinked;
}

/* ============================================================================
 * One-shot
 * ============================================================================ */

static size_t lz4_compress_bound(size_t in_len) {
    LZ4F_preferences_t prefs;
    memset(&prefs, 0, sizeof(prefs));
    prefs.frameInfo.contentSize = in_len;
    prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    return LZ4F_compressFrameBound(in_len, &prefs);
}

static cu_status_t lz4_compress(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len,
    int level
) {
    LZ4F_preferences_t prefs;
    make_prefs(&prefs, level, in_len);
    size_t cap = *out_len;
    size_t needed = LZ4F_compressFrameBound(in_len, &prefs);
    if (cap < needed) {
        *out_len = needed;
        return CU_ERR_BUF_TOO_SMALL;
    }
    size_t r = LZ4F_compressFrame(out, cap, in, in_len, &prefs);
    if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_COMPRESSION);
    *out_len = r;
    return CU_OK;
}

static cu_status_t lz4_decompress(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    if (in_len == 0) {
        cu_set_last_error("lz4: empty input");
        return CU_ERR_TRUNCATED;
    }
    LZ4F_dctx* dctx = NULL;
    size_t r = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_OOM);

    /* Probe content size if available; check against caller buffer/cap. */
    LZ4F_frameInfo_t info;
    size_t in_size_probe = in_len;
    r = LZ4F_getFrameInfo(dctx, &info, in, &in_size_probe);
    if (LZ4F_isError(r)) {
        LZ4F_freeDecompressionContext(dctx);
        return map_lz4f_err(r, CU_ERR_DECOMPRESSION);
    }
    size_t cap_limit = cu_get_max_decompressed_size();
    if (info.contentSize > 0) {
        if (cap_limit > 0 && info.contentSize > cap_limit) {
            cu_set_last_errorf("lz4: declared size %llu exceeds cap %zu",
                               (unsigned long long)info.contentSize, cap_limit);
            LZ4F_freeDecompressionContext(dctx);
            return CU_ERR_SIZE_LIMIT;
        }
        if (*out_len < info.contentSize) {
            *out_len = (size_t)info.contentSize;
            LZ4F_freeDecompressionContext(dctx);
            return CU_ERR_BUF_TOO_SMALL;
        }
    }

    /* Decompress the rest. */
    size_t src_remaining = in_len - in_size_probe;
    const uint8_t* src = in + in_size_probe;
    size_t dst_remaining = *out_len;
    uint8_t* dst = out;
    size_t total_out = 0;

    while (src_remaining > 0) {
        size_t src_size = src_remaining;
        size_t dst_size = dst_remaining;
        r = LZ4F_decompress(dctx, dst, &dst_size, src, &src_size, NULL);
        if (LZ4F_isError(r)) {
            LZ4F_freeDecompressionContext(dctx);
            return map_lz4f_err(r, CU_ERR_DECOMPRESSION);
        }
        total_out += dst_size;
        dst += dst_size;
        dst_remaining -= dst_size;
        src += src_size;
        src_remaining -= src_size;
        if (r == 0) break;  /* frame complete */
        if (dst_remaining == 0 && src_remaining > 0) {
            LZ4F_freeDecompressionContext(dctx);
            if (info.contentSize == 0) {
                cu_set_last_error("lz4: output buffer too small (size not in frame)");
                return CU_ERR_SIZE_UNKNOWN;
            }
            *out_len = (size_t)info.contentSize;
            return CU_ERR_BUF_TOO_SMALL;
        }
    }
    LZ4F_freeDecompressionContext(dctx);
    *out_len = total_out;
    return CU_OK;
}

static cu_status_t lz4_decompress_size_hint(
    const uint8_t* in, size_t in_len, size_t* out_size
) {
    if (in_len == 0) return CU_ERR_TRUNCATED;
    LZ4F_dctx* dctx = NULL;
    size_t r = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_OOM);

    LZ4F_frameInfo_t info;
    size_t in_probe = in_len;
    r = LZ4F_getFrameInfo(dctx, &info, in, &in_probe);
    cu_status_t ret;
    if (LZ4F_isError(r)) {
        ret = map_lz4f_err(r, CU_ERR_DECOMPRESSION);
    } else if (info.contentSize > 0) {
        *out_size = (size_t)info.contentSize;
        ret = CU_OK;
    } else {
        ret = CU_ERR_SIZE_UNKNOWN;
    }
    LZ4F_freeDecompressionContext(dctx);
    return ret;
}

/* ============================================================================
 * Streaming compression
 * ============================================================================ */

/*
 * LZ4 streaming compression uses two internal buffers because LZ4F's
 * incremental API doesn't compose with small caller-provided output
 * buffers (LZ4F_compressBound is pessimistic — assumes a flush of a
 * full 64 KB block could happen on any call). Design:
 *
 *   - in_buf:  uncompressed bytes the caller has handed us, waiting to
 *              be fed to LZ4F_compressUpdate in BLOCK_SIZE chunks.
 *   - out_buf: compressed bytes produced by the codec, waiting to be
 *              given to the caller's `out` in BUF_TOO_SMALL slices.
 *
 * On each write/finish call:
 *   1. Drain out_buf into caller's `out`.
 *   2. If out_buf is empty, append `in` to in_buf, then feed one block
 *      from in_buf into LZ4F_compressUpdate (which fills out_buf).
 *   3. Drain again, repeat.
 */

#define LZ4_BLOCK_SIZE (64 * 1024)

typedef struct {
    LZ4F_cctx* cctx;
    LZ4F_preferences_t prefs;

    uint8_t* in_buf;
    size_t   in_head;
    size_t   in_tail;
    size_t   in_cap;

    uint8_t* out_buf;
    size_t   out_head;
    size_t   out_tail;
    size_t   out_cap;

    int      header_written;
    int      finishing;
    int      finished;
} lz4_cstream_state_t;

static cu_status_t lz4_cstream_create(int level, void** out_state) {
    lz4_cstream_state_t* st = calloc(1, sizeof(*st));
    if (!st) { cu_set_last_error("lz4: oom"); return CU_ERR_OOM; }
    size_t r = LZ4F_createCompressionContext(&st->cctx, LZ4F_VERSION);
    if (LZ4F_isError(r)) {
        cu_status_t s = map_lz4f_err(r, CU_ERR_OOM);
        free(st);
        return s;
    }
    memset(&st->prefs, 0, sizeof(st->prefs));
    st->prefs.compressionLevel = lz4_native_level(level);
    st->prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;

    st->out_cap = LZ4F_compressBound(LZ4_BLOCK_SIZE, &st->prefs) + 128;
    st->out_buf = malloc(st->out_cap);
    if (!st->out_buf) {
        LZ4F_freeCompressionContext(st->cctx);
        free(st);
        cu_set_last_error("lz4: oom");
        return CU_ERR_OOM;
    }
    *out_state = st;
    return CU_OK;
}

static cu_status_t in_buf_append(lz4_cstream_state_t* st, const uint8_t* src, size_t n) {
    if (n == 0) return CU_OK;
    /* Compact: shift head bytes to start if room would be created. */
    if (st->in_head > 0 && st->in_tail - st->in_head + n > st->in_cap - st->in_head) {
        memmove(st->in_buf, st->in_buf + st->in_head, st->in_tail - st->in_head);
        st->in_tail -= st->in_head;
        st->in_head = 0;
    }
    if (st->in_tail + n > st->in_cap) {
        size_t new_cap = st->in_cap ? st->in_cap * 2 : LZ4_BLOCK_SIZE;
        while (new_cap < st->in_tail + n) new_cap *= 2;
        uint8_t* p = realloc(st->in_buf, new_cap);
        if (!p) { cu_set_last_error("lz4: oom"); return CU_ERR_OOM; }
        st->in_buf = p;
        st->in_cap = new_cap;
    }
    memcpy(st->in_buf + st->in_tail, src, n);
    st->in_tail += n;
    return CU_OK;
}

/* Drain out_buf into caller. Returns 1 if out_buf still has data. */
static int out_drain(lz4_cstream_state_t* st,
                     uint8_t* out, size_t cap, size_t* written) {
    size_t available = st->out_tail - st->out_head;
    size_t avail_out = cap - *written;
    size_t n = available < avail_out ? available : avail_out;
    if (n > 0) {
        memcpy(out + *written, st->out_buf + st->out_head, n);
        *written += n;
        st->out_head += n;
    }
    if (st->out_head == st->out_tail) {
        st->out_head = 0;
        st->out_tail = 0;
        return 0;
    }
    return 1;
}

/* Feed one block from in_buf to the codec, filling out_buf. Requires
 * out_buf to be empty. Returns CU_OK or a codec error. Sets a flag if
 * in_buf is exhausted. */
static cu_status_t cstream_feed_one_block(lz4_cstream_state_t* st) {
    size_t in_avail = st->in_tail - st->in_head;
    if (in_avail == 0) return CU_OK;
    size_t chunk = in_avail < LZ4_BLOCK_SIZE ? in_avail : LZ4_BLOCK_SIZE;
    size_t r = LZ4F_compressUpdate(st->cctx,
                                   st->out_buf, st->out_cap,
                                   st->in_buf + st->in_head, chunk, NULL);
    if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_COMPRESSION);
    st->out_tail = r;
    st->in_head += chunk;
    if (st->in_head == st->in_tail) {
        st->in_head = 0;
        st->in_tail = 0;
    }
    return CU_OK;
}

static cu_status_t lz4_cstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    lz4_cstream_state_t* st = (lz4_cstream_state_t*)state;
    if (st->finishing) {
        cu_set_last_error("lz4: write after finish");
        return CU_ERR_STREAM_STATE;
    }
    size_t cap = *out_len;
    size_t written = 0;

    /* Append new input first. */
    cu_status_t s = in_buf_append(st, in, in_len);
    if (s != CU_OK) return s;

    /* Drain anything leftover in out_buf. */
    if (out_drain(st, out, cap, &written)) { *out_len = written; return CU_ERR_BUF_TOO_SMALL; }

    /* Emit header if needed. */
    if (!st->header_written) {
        size_t r = LZ4F_compressBegin(st->cctx, st->out_buf, st->out_cap, &st->prefs);
        if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_COMPRESSION);
        st->out_tail = r;
        st->header_written = 1;
        if (out_drain(st, out, cap, &written)) { *out_len = written; return CU_ERR_BUF_TOO_SMALL; }
    }

    /* Consume in_buf one block at a time. */
    while (st->in_tail > st->in_head) {
        s = cstream_feed_one_block(st);
        if (s != CU_OK) return s;
        if (out_drain(st, out, cap, &written)) { *out_len = written; return CU_ERR_BUF_TOO_SMALL; }
    }

    *out_len = written;
    return CU_OK;
}

static cu_status_t lz4_cstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    lz4_cstream_state_t* st = (lz4_cstream_state_t*)state;
    st->finishing = 1;
    size_t cap = *out_len;
    size_t written = 0;

    if (out_drain(st, out, cap, &written)) { *out_len = written; return CU_ERR_BUF_TOO_SMALL; }

    if (st->finished) { *out_len = written; return CU_OK; }

    /* Header (for zero-input finish). */
    if (!st->header_written) {
        size_t r = LZ4F_compressBegin(st->cctx, st->out_buf, st->out_cap, &st->prefs);
        if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_COMPRESSION);
        st->out_tail = r;
        st->header_written = 1;
        if (out_drain(st, out, cap, &written)) { *out_len = written; return CU_ERR_BUF_TOO_SMALL; }
    }

    /* Drain pending input. */
    while (st->in_tail > st->in_head) {
        cu_status_t s = cstream_feed_one_block(st);
        if (s != CU_OK) return s;
        if (out_drain(st, out, cap, &written)) { *out_len = written; return CU_ERR_BUF_TOO_SMALL; }
    }

    /* End frame. */
    size_t r = LZ4F_compressEnd(st->cctx, st->out_buf, st->out_cap, NULL);
    if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_COMPRESSION);
    st->out_tail = r;
    st->finished = 1;

    int more = out_drain(st, out, cap, &written);
    *out_len = written;
    return more ? CU_ERR_BUF_TOO_SMALL : CU_OK;
}

static void lz4_cstream_destroy(void* state) {
    lz4_cstream_state_t* st = (lz4_cstream_state_t*)state;
    if (!st) return;
    if (st->cctx) LZ4F_freeCompressionContext(st->cctx);
    free(st->in_buf);
    free(st->out_buf);
    free(st);
}

/* ============================================================================
 * Streaming decompression
 * ============================================================================ */

typedef struct {
    LZ4F_dctx* dctx;
    uint8_t* pending;
    size_t   pending_len;
    size_t   pending_cap;
    int      frame_done;
} lz4_dstream_state_t;

static cu_status_t lz4_dstream_create(void** out_state) {
    lz4_dstream_state_t* st = calloc(1, sizeof(*st));
    if (!st) { cu_set_last_error("lz4: oom"); return CU_ERR_OOM; }
    size_t r = LZ4F_createDecompressionContext(&st->dctx, LZ4F_VERSION);
    if (LZ4F_isError(r)) {
        cu_status_t s = map_lz4f_err(r, CU_ERR_OOM);
        free(st);
        return s;
    }
    *out_state = st;
    return CU_OK;
}

static cu_status_t dstream_pump(lz4_dstream_state_t* st,
                                uint8_t* out, size_t* out_len) {
    size_t cap = *out_len;
    size_t written = 0;

    while (st->pending_len > 0 && !st->frame_done) {
        size_t avail = cap - written;
        if (avail == 0) {
            *out_len = written;
            return CU_ERR_BUF_TOO_SMALL;
        }
        size_t src_size = st->pending_len;
        size_t dst_size = avail;
        size_t r = LZ4F_decompress(st->dctx,
                                   out + written, &dst_size,
                                   st->pending, &src_size, NULL);
        if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_DECOMPRESSION);
        written += dst_size;
        /* Consume src_size bytes from pending. */
        if (src_size > 0) {
            memmove(st->pending, st->pending + src_size, st->pending_len - src_size);
            st->pending_len -= src_size;
        }
        if (r == 0) {
            st->frame_done = 1;
            break;
        }
        if (src_size == 0 && dst_size == 0) {
            /* Decoder needs more input — pending is empty after consume,
             * caller should provide more. */
            break;
        }
    }

    *out_len = written;
    return CU_OK;
}

static cu_status_t dstream_pending_append(lz4_dstream_state_t* st,
                                          const uint8_t* src, size_t n) {
    if (n == 0) return CU_OK;
    size_t need = st->pending_len + n;
    if (need > st->pending_cap) {
        size_t new_cap = st->pending_cap ? st->pending_cap * 2 : 16 * 1024;
        while (new_cap < need) new_cap *= 2;
        uint8_t* p = realloc(st->pending, new_cap);
        if (!p) { cu_set_last_error("lz4: oom"); return CU_ERR_OOM; }
        st->pending = p;
        st->pending_cap = new_cap;
    }
    memcpy(st->pending + st->pending_len, src, n);
    st->pending_len += n;
    return CU_OK;
}

static cu_status_t lz4_dstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    lz4_dstream_state_t* st = (lz4_dstream_state_t*)state;
    if (st->frame_done && in_len > 0) {
        cu_set_last_error("lz4: data after end of frame");
        return CU_ERR_DECOMPRESSION;
    }
    cu_status_t s = dstream_pending_append(st, in, in_len);
    if (s != CU_OK) return s;
    return dstream_pump(st, out, out_len);
}

static cu_status_t lz4_dstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    lz4_dstream_state_t* st = (lz4_dstream_state_t*)state;
    cu_status_t s = dstream_pump(st, out, out_len);
    if (s != CU_OK) return s;
    if (!st->frame_done) {
        cu_set_last_error("lz4: truncated frame at finish");
        return CU_ERR_TRUNCATED;
    }
    return CU_OK;
}

static void lz4_dstream_destroy(void* state) {
    lz4_dstream_state_t* st = (lz4_dstream_state_t*)state;
    if (!st) return;
    if (st->dctx) LZ4F_freeDecompressionContext(st->dctx);
    free(st->pending);
    free(st);
}

/* ============================================================================
 * Vtable
 * ============================================================================ */

const cu_algorithm_vtbl_t cu_lz4_vtbl = {
    .name                      = "lz4",
    /* Direction split (#7): see zstd.c for the CU_OMIT_* rationale. */
#ifndef CU_OMIT_COMPRESS
    .compress_bound            = lz4_compress_bound,
    .compress                  = lz4_compress,
    .compress_stream_create    = lz4_cstream_create,
    .compress_stream_write     = lz4_cstream_write,
    .compress_stream_finish    = lz4_cstream_finish,
    .compress_stream_destroy   = lz4_cstream_destroy,
#endif
#ifndef CU_OMIT_DECOMPRESS
    .decompress                = lz4_decompress,
    .decompress_size_hint      = lz4_decompress_size_hint,
    .decompress_stream_create  = lz4_dstream_create,
    .decompress_stream_write   = lz4_dstream_write,
    .decompress_stream_finish  = lz4_dstream_finish,
    .decompress_stream_destroy = lz4_dstream_destroy,
#endif
};
