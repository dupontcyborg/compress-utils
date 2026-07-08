/*
 * bz2.c — bzip2 vtable for compress-utils.
 *
 * bzip2's wire format does not carry the decompressed size:
 *   - cu_decompress_size_hint returns CU_ERR_SIZE_UNKNOWN.
 *   - one-shot decompress uses BZ2_bzBuffToBuffDecompress into the
 *     caller's buffer and returns SIZE_UNKNOWN if BZ_OUTBUFF_FULL.
 *
 * Streaming uses BZ2_bzCompress / BZ2_bzDecompress with the stashed-tail
 * protocol.
 */

#include "algorithm_registry.h"
#include "compress_utils.h"
#include "utils/levels.h"

#include "bz2/bzlib.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* bzip2: user 1..10 → bz2 native 1..9 (clamp). */
static int bz2_native_level(int user_level) {
    return cu_clamp_level(user_level, 1, 9);
}

static const char* bz2_errstr(int code) {
    switch (code) {
        case BZ_OK:                return "ok";
        case BZ_RUN_OK:            return "run ok";
        case BZ_FLUSH_OK:          return "flush ok";
        case BZ_FINISH_OK:         return "finish ok";
        case BZ_STREAM_END:        return "stream end";
        case BZ_SEQUENCE_ERROR:    return "sequence error";
        case BZ_PARAM_ERROR:       return "param error";
        case BZ_MEM_ERROR:         return "out of memory";
        case BZ_DATA_ERROR:        return "corrupted data";
        case BZ_DATA_ERROR_MAGIC:  return "bad magic";
        case BZ_IO_ERROR:          return "io error";
        case BZ_UNEXPECTED_EOF:    return "unexpected eof";
        case BZ_OUTBUFF_FULL:      return "output buffer full";
        case BZ_CONFIG_ERROR:      return "config error";
        default:                   return "unknown";
    }
}

/* ============================================================================
 * One-shot
 * ============================================================================ */

static size_t bz2_compress_bound(size_t in_len) {
    /* bzip2 docs: worst case is input + 1% + 600 bytes. */
    return in_len + (in_len / 100) + 600;
}

static cu_status_t bz2_compress(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len,
    int level
) {
    size_t cap = *out_len;
    size_t needed = bz2_compress_bound(in_len);
    if (cap < needed) {
        *out_len = needed;
        return CU_ERR_BUF_TOO_SMALL;
    }
    unsigned int dest_len = (unsigned int)cap;
    int r = BZ2_bzBuffToBuffCompress(
        (char*)out, &dest_len,
        (char*)(uintptr_t)in, (unsigned int)in_len,
        bz2_native_level(level),
        0,  /* verbosity */
        0   /* workFactor (default 30) */
    );
    if (r != BZ_OK) {
        cu_set_last_errorf("bz2: %s", bz2_errstr(r));
        return r == BZ_MEM_ERROR ? CU_ERR_OOM : CU_ERR_COMPRESSION;
    }
    *out_len = dest_len;
    return CU_OK;
}

static cu_status_t bz2_decompress(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    if (in_len == 0) {
        cu_set_last_error("bz2: empty input");
        return CU_ERR_TRUNCATED;
    }
    unsigned int dest_len = (unsigned int)*out_len;
    size_t cap_limit = cu_get_max_decompressed_size();
    int r = BZ2_bzBuffToBuffDecompress(
        (char*)out, &dest_len,
        (char*)(uintptr_t)in, (unsigned int)in_len,
        0, 0
    );
    if (r == BZ_OK) {
        if (cap_limit > 0 && dest_len > cap_limit) {
            cu_set_last_errorf("bz2: decompressed size %u exceeds cap %zu", dest_len, cap_limit);
            return CU_ERR_SIZE_LIMIT;
        }
        *out_len = dest_len;
        return CU_OK;
    }
    if (r == BZ_OUTBUFF_FULL) {
        cu_set_last_error("bz2: output buffer too small (size not encoded in stream)");
        return CU_ERR_SIZE_UNKNOWN;
    }
    if (r == BZ_UNEXPECTED_EOF) {
        cu_set_last_error("bz2: truncated input");
        return CU_ERR_TRUNCATED;
    }
    cu_set_last_errorf("bz2: %s", bz2_errstr(r));
    return CU_ERR_DECOMPRESSION;
}

static cu_status_t bz2_decompress_size_hint(
    const uint8_t* in, size_t in_len, size_t* out_size
) {
    (void)in; (void)in_len; (void)out_size;
    return CU_ERR_SIZE_UNKNOWN;
}

/* ============================================================================
 * Streaming compression
 * ============================================================================ */

typedef struct {
    bz_stream strm;
    int       strm_inited;
    uint8_t*  pending;
    size_t    pending_len;
    size_t    pending_cap;
    int      finishing;
    int      stream_end;
} bz2_cstream_state_t;

static cu_status_t pending_append(uint8_t** pending, size_t* pending_len, size_t* pending_cap,
                                  const uint8_t* src, size_t n) {
    if (n == 0) return CU_OK;
    size_t need = *pending_len + n;
    if (need > *pending_cap) {
        size_t new_cap = *pending_cap ? *pending_cap * 2 : 16 * 1024;
        while (new_cap < need) new_cap *= 2;
        uint8_t* p = realloc(*pending, new_cap);
        if (!p) { cu_set_last_error("bz2: oom"); return CU_ERR_OOM; }
        *pending = p;
        *pending_cap = new_cap;
    }
    memcpy(*pending + *pending_len, src, n);
    *pending_len += n;
    return CU_OK;
}

static cu_status_t bz2_cstream_create(int level, void** out_state) {
    bz2_cstream_state_t* st = calloc(1, sizeof(*st));
    if (!st) { cu_set_last_error("bz2: oom"); return CU_ERR_OOM; }
    int r = BZ2_bzCompressInit(&st->strm, bz2_native_level(level), 0, 0);
    if (r != BZ_OK) {
        cu_set_last_errorf("bz2: %s", bz2_errstr(r));
        free(st);
        return r == BZ_MEM_ERROR ? CU_ERR_OOM : CU_ERR_COMPRESSION;
    }
    st->strm_inited = 1;
    *out_state = st;
    return CU_OK;
}

static cu_status_t cstream_pump(bz2_cstream_state_t* st, int action,
                                uint8_t* out, size_t* out_len) {
    size_t cap = *out_len;
    size_t written = 0;

    st->strm.next_in  = (char*)st->pending;
    st->strm.avail_in = (unsigned int)st->pending_len;
    for (;;) {
        st->strm.next_out  = (char*)(out + written);
        st->strm.avail_out = (unsigned int)(cap - written);
        unsigned int avail_out_before = st->strm.avail_out;
        int r = BZ2_bzCompress(&st->strm, action);
        size_t produced = (size_t)(avail_out_before - st->strm.avail_out);
        written += produced;
        if (r == BZ_STREAM_END) {
            st->stream_end = 1;
            size_t consumed = st->pending_len - st->strm.avail_in;
            if (consumed > 0 && st->strm.avail_in > 0) {
                memmove(st->pending, st->pending + consumed, st->strm.avail_in);
            }
            st->pending_len = st->strm.avail_in;
            *out_len = written;
            return CU_OK;
        }
        if (r == BZ_RUN_OK || r == BZ_FINISH_OK || r == BZ_FLUSH_OK) {
            /* Output buffer might be full and more to do, or input exhausted. */
            if (st->strm.avail_out == 0 && (st->strm.avail_in > 0 || action != BZ_RUN)) {
                size_t consumed = st->pending_len - st->strm.avail_in;
                if (consumed > 0 && st->strm.avail_in > 0) {
                    memmove(st->pending, st->pending + consumed, st->strm.avail_in);
                }
                st->pending_len = st->strm.avail_in;
                *out_len = written;
                return CU_ERR_BUF_TOO_SMALL;
            }
            if (action == BZ_RUN && st->strm.avail_in == 0) {
                st->pending_len = 0;
                *out_len = written;
                return CU_OK;
            }
            /* otherwise loop and produce more */
            continue;
        }
        cu_set_last_errorf("bz2: %s", bz2_errstr(r));
        return CU_ERR_COMPRESSION;
    }
}

static cu_status_t bz2_cstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    bz2_cstream_state_t* st = (bz2_cstream_state_t*)state;
    if (st->finishing) {
        cu_set_last_error("bz2: write after finish");
        return CU_ERR_STREAM_STATE;
    }
    cu_status_t s = pending_append(&st->pending, &st->pending_len, &st->pending_cap, in, in_len);
    if (s != CU_OK) return s;
    return cstream_pump(st, BZ_RUN, out, out_len);
}

static cu_status_t bz2_cstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    bz2_cstream_state_t* st = (bz2_cstream_state_t*)state;
    st->finishing = 1;
    if (st->stream_end) { *out_len = 0; return CU_OK; }
    return cstream_pump(st, BZ_FINISH, out, out_len);
}

static void bz2_cstream_destroy(void* state) {
    bz2_cstream_state_t* st = (bz2_cstream_state_t*)state;
    if (!st) return;
    if (st->strm_inited) BZ2_bzCompressEnd(&st->strm);
    free(st->pending);
    free(st);
}

/* ============================================================================
 * Streaming decompression
 * ============================================================================ */

typedef struct {
    bz_stream strm;
    int       strm_inited;
    uint8_t*  pending;
    size_t    pending_len;
    size_t    pending_cap;
    int       stream_end;
} bz2_dstream_state_t;

static cu_status_t bz2_dstream_create(void** out_state) {
    bz2_dstream_state_t* st = calloc(1, sizeof(*st));
    if (!st) { cu_set_last_error("bz2: oom"); return CU_ERR_OOM; }
    int r = BZ2_bzDecompressInit(&st->strm, 0, 0);
    if (r != BZ_OK) {
        cu_set_last_errorf("bz2: %s", bz2_errstr(r));
        free(st);
        return r == BZ_MEM_ERROR ? CU_ERR_OOM : CU_ERR_DECOMPRESSION;
    }
    st->strm_inited = 1;
    *out_state = st;
    return CU_OK;
}

static cu_status_t dstream_pump(bz2_dstream_state_t* st,
                                uint8_t* out, size_t* out_len) {
    size_t cap = *out_len;
    size_t written = 0;

    st->strm.next_in  = (char*)st->pending;
    st->strm.avail_in = (unsigned int)st->pending_len;

    for (;;) {
        st->strm.next_out  = (char*)(out + written);
        st->strm.avail_out = (unsigned int)(cap - written);
        unsigned int avail_out_before = st->strm.avail_out;
        int r = BZ2_bzDecompress(&st->strm);
        size_t produced = (size_t)(avail_out_before - st->strm.avail_out);
        written += produced;

        if (r == BZ_STREAM_END) {
            st->stream_end = 1;
            size_t consumed = st->pending_len - st->strm.avail_in;
            if (consumed > 0 && st->strm.avail_in > 0) {
                memmove(st->pending, st->pending + consumed, st->strm.avail_in);
            }
            st->pending_len = st->strm.avail_in;
            *out_len = written;
            return CU_OK;
        }
        if (r == BZ_OK) {
            if (st->strm.avail_out == 0 && st->strm.avail_in > 0) {
                /* output full, more input remaining */
                size_t consumed = st->pending_len - st->strm.avail_in;
                if (consumed > 0) {
                    memmove(st->pending, st->pending + consumed, st->strm.avail_in);
                }
                st->pending_len = st->strm.avail_in;
                *out_len = written;
                return CU_ERR_BUF_TOO_SMALL;
            }
            if (st->strm.avail_in == 0) {
                /* All current input consumed, not yet at stream end. */
                st->pending_len = 0;
                *out_len = written;
                return CU_OK;
            }
            continue;
        }
        cu_set_last_errorf("bz2: %s", bz2_errstr(r));
        return CU_ERR_DECOMPRESSION;
    }
}

static cu_status_t bz2_dstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    bz2_dstream_state_t* st = (bz2_dstream_state_t*)state;
    if (st->stream_end && in_len > 0) {
        cu_set_last_error("bz2: data after end of stream");
        return CU_ERR_DECOMPRESSION;
    }
    cu_status_t s = pending_append(&st->pending, &st->pending_len, &st->pending_cap, in, in_len);
    if (s != CU_OK) return s;
    return dstream_pump(st, out, out_len);
}

static cu_status_t bz2_dstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    bz2_dstream_state_t* st = (bz2_dstream_state_t*)state;
    /* If the stream is already complete and there's no pending input,
     * skip the pump (BZ2_bzDecompress would return BZ_SEQUENCE_ERROR). */
    if (st->stream_end && st->pending_len == 0) {
        *out_len = 0;
        return CU_OK;
    }
    cu_status_t s = dstream_pump(st, out, out_len);
    if (s != CU_OK) return s;
    if (!st->stream_end) {
        cu_set_last_error("bz2: truncated stream at finish");
        return CU_ERR_TRUNCATED;
    }
    return CU_OK;
}

static void bz2_dstream_destroy(void* state) {
    bz2_dstream_state_t* st = (bz2_dstream_state_t*)state;
    if (!st) return;
    if (st->strm_inited) BZ2_bzDecompressEnd(&st->strm);
    free(st->pending);
    free(st);
}

/* ============================================================================
 * Vtable
 * ============================================================================ */

const cu_algorithm_vtbl_t cu_bz2_vtbl = {
    .name                      = "bz2",
    /* Direction split (#7): see zstd.c for the CU_OMIT_* rationale. */
#ifndef CU_OMIT_COMPRESS
    .compress_bound            = bz2_compress_bound,
    .compress                  = bz2_compress,
    .compress_stream_create    = bz2_cstream_create,
    .compress_stream_write     = bz2_cstream_write,
    .compress_stream_finish    = bz2_cstream_finish,
    .compress_stream_destroy   = bz2_cstream_destroy,
#endif
#ifndef CU_OMIT_DECOMPRESS
    .decompress                = bz2_decompress,
    .decompress_size_hint      = bz2_decompress_size_hint,
    .decompress_stream_create  = bz2_dstream_create,
    .decompress_stream_write   = bz2_dstream_write,
    .decompress_stream_finish  = bz2_dstream_finish,
    .decompress_stream_destroy = bz2_dstream_destroy,
#endif
};
