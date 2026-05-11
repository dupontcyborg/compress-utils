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

typedef struct {
    LZ4F_cctx* cctx;
    LZ4F_preferences_t prefs;
    uint8_t* pending;
    size_t   pending_len;
    size_t   pending_cap;
    int      finishing;
    int      header_written;
    int      finished;
} lz4_cstream_state_t;

static cu_status_t pending_append(uint8_t** pending, size_t* pending_len, size_t* pending_cap,
                                  const uint8_t* src, size_t n) {
    if (n == 0) return CU_OK;
    size_t need = *pending_len + n;
    if (need > *pending_cap) {
        size_t new_cap = *pending_cap ? *pending_cap * 2 : 16 * 1024;
        while (new_cap < need) new_cap *= 2;
        uint8_t* p = realloc(*pending, new_cap);
        if (!p) { cu_set_last_error("lz4: oom"); return CU_ERR_OOM; }
        *pending = p;
        *pending_cap = new_cap;
    }
    memcpy(*pending + *pending_len, src, n);
    *pending_len += n;
    return CU_OK;
}

static cu_status_t lz4_cstream_create(int level, void** out_state) {
    lz4_cstream_state_t* st = calloc(1, sizeof(*st));
    if (!st) { cu_set_last_error("lz4: oom"); return CU_ERR_OOM; }
    size_t r = LZ4F_createCompressionContext(&st->cctx, LZ4F_VERSION);
    if (LZ4F_isError(r)) {
        cu_status_t s = map_lz4f_err(r, CU_ERR_OOM);
        free(st);
        return s;
    }
    /* Streaming: we don't know total content size, so omit contentSize
     * flag (size_hint will report SIZE_UNKNOWN for streamed frames). */
    memset(&st->prefs, 0, sizeof(st->prefs));
    st->prefs.compressionLevel = lz4_native_level(level);
    st->prefs.frameInfo.contentChecksumFlag = LZ4F_contentChecksumEnabled;
    *out_state = st;
    return CU_OK;
}

/* Helper: write begin header into out if not yet written. Returns CU_OK
 * or BUF_TOO_SMALL; on success, advances *written. */
static cu_status_t cstream_emit_begin(lz4_cstream_state_t* st,
                                      uint8_t* out, size_t cap, size_t* written) {
    if (st->header_written) return CU_OK;
    size_t avail = cap - *written;
    size_t need = LZ4F_HEADER_SIZE_MAX;
    if (avail < need) return CU_ERR_BUF_TOO_SMALL;
    size_t r = LZ4F_compressBegin(st->cctx, out + *written, avail, &st->prefs);
    if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_COMPRESSION);
    *written += r;
    st->header_written = 1;
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
    cu_status_t s = pending_append(&st->pending, &st->pending_len, &st->pending_cap, in, in_len);
    if (s != CU_OK) return s;

    size_t cap = *out_len;
    size_t written = 0;

    s = cstream_emit_begin(st, out, cap, &written);
    if (s != CU_OK) { *out_len = written; return s; }

    /* Compress pending in chunks that fit the remaining output. */
    while (st->pending_len > 0) {
        size_t avail = cap - written;
        /* Worst case for `chunk` bytes is LZ4F_compressBound(chunk). Pick
         * the largest chunk that fits. */
        if (avail < LZ4F_compressBound(1, &st->prefs)) {
            *out_len = written;
            return CU_ERR_BUF_TOO_SMALL;
        }
        /* Binary-search the largest chunk that fits is overkill; just
         * pick min(pending_len, max_chunk_for_avail). The bound is
         * roughly avail - 16 for header per block. */
        size_t chunk = st->pending_len;
        while (chunk > 0 && LZ4F_compressBound(chunk, &st->prefs) > avail) {
            chunk /= 2;
        }
        if (chunk == 0) {
            *out_len = written;
            return CU_ERR_BUF_TOO_SMALL;
        }
        size_t r = LZ4F_compressUpdate(st->cctx,
                                       out + written, avail,
                                       st->pending, chunk, NULL);
        if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_COMPRESSION);
        written += r;
        memmove(st->pending, st->pending + chunk, st->pending_len - chunk);
        st->pending_len -= chunk;
    }

    *out_len = written;
    return CU_OK;
}

static cu_status_t lz4_cstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    lz4_cstream_state_t* st = (lz4_cstream_state_t*)state;
    st->finishing = 1;
    if (st->finished) { *out_len = 0; return CU_OK; }

    size_t cap = *out_len;
    size_t written = 0;

    cu_status_t s = cstream_emit_begin(st, out, cap, &written);
    if (s != CU_OK) { *out_len = written; return s; }

    /* Drain pending. */
    while (st->pending_len > 0) {
        size_t avail = cap - written;
        size_t chunk = st->pending_len;
        while (chunk > 0 && LZ4F_compressBound(chunk, &st->prefs) > avail) {
            chunk /= 2;
        }
        if (chunk == 0) {
            *out_len = written;
            return CU_ERR_BUF_TOO_SMALL;
        }
        size_t r = LZ4F_compressUpdate(st->cctx,
                                       out + written, avail,
                                       st->pending, chunk, NULL);
        if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_COMPRESSION);
        written += r;
        memmove(st->pending, st->pending + chunk, st->pending_len - chunk);
        st->pending_len -= chunk;
    }

    /* End frame. */
    size_t avail = cap - written;
    size_t end_bound = LZ4F_compressBound(0, &st->prefs);
    if (avail < end_bound) {
        *out_len = written;
        return CU_ERR_BUF_TOO_SMALL;
    }
    size_t r = LZ4F_compressEnd(st->cctx, out + written, avail, NULL);
    if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_COMPRESSION);
    written += r;
    st->finished = 1;
    *out_len = written;
    return CU_OK;
}

static void lz4_cstream_destroy(void* state) {
    lz4_cstream_state_t* st = (lz4_cstream_state_t*)state;
    if (!st) return;
    if (st->cctx) LZ4F_freeCompressionContext(st->cctx);
    free(st->pending);
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

static cu_status_t lz4_dstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    lz4_dstream_state_t* st = (lz4_dstream_state_t*)state;
    if (st->frame_done && in_len > 0) {
        cu_set_last_error("lz4: data after end of frame");
        return CU_ERR_DECOMPRESSION;
    }
    cu_status_t s = pending_append(&st->pending, &st->pending_len, &st->pending_cap, in, in_len);
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
    .compress_bound            = lz4_compress_bound,
    .compress                  = lz4_compress,
    .decompress                = lz4_decompress,
    .decompress_size_hint      = lz4_decompress_size_hint,
    .compress_stream_create    = lz4_cstream_create,
    .compress_stream_write     = lz4_cstream_write,
    .compress_stream_finish    = lz4_cstream_finish,
    .compress_stream_destroy   = lz4_cstream_destroy,
    .decompress_stream_create  = lz4_dstream_create,
    .decompress_stream_write   = lz4_dstream_write,
    .decompress_stream_finish  = lz4_dstream_finish,
    .decompress_stream_destroy = lz4_dstream_destroy,
};
