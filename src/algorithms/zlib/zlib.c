/*
 * zlib.c — zlib vtable for compress-utils.
 *
 * zlib's wire format never carries the decompressed size, so:
 *   - cu_decompress_size_hint always returns CU_ERR_SIZE_UNKNOWN.
 *   - cu_decompress decompresses with inflate() into the caller's buffer
 *     and returns CU_ERR_SIZE_UNKNOWN if the buffer is exhausted.
 *
 * Streaming uses deflate()/inflate() with a stashed-tail protocol
 * matching the public ABI's "fill buffer, return BUF_TOO_SMALL, drain"
 * pattern.
 */

#include "algorithm_registry.h"
#include "compress_utils.h"

#include "zlib/zlib.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Level mapping
 * ============================================================================ */

static int zlib_native_level(int user_level) {
    /* zlib supports 1..9. User 1..10 -> clamp 10 to 9. */
    if (user_level > 9) return 9;
    if (user_level < 1) return 1;
    return user_level;
}

static cu_status_t map_zlib_error(int code, cu_status_t fallback) {
    if (code == Z_OK || code == Z_STREAM_END) return CU_OK;
    cu_set_last_errorf("zlib: code %d (%s)", code, zError(code));
    return fallback;
}

/* ============================================================================
 * One-shot
 * ============================================================================ */

static size_t zlib_compress_bound(size_t in_len) {
    return compressBound((uLong)in_len);
}

static cu_status_t zlib_compress(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len,
    int level
) {
    uLongf cap = (uLongf)*out_len;
    uLongf needed = compressBound((uLong)in_len);
    if (cap < needed) {
        *out_len = needed;
        return CU_ERR_BUF_TOO_SMALL;
    }
    int r = compress2(out, &cap, in, (uLong)in_len, zlib_native_level(level));
    if (r != Z_OK) return map_zlib_error(r, CU_ERR_COMPRESSION);
    *out_len = (size_t)cap;
    return CU_OK;
}

static cu_status_t zlib_decompress(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    if (in_len == 0) {
        cu_set_last_error("zlib: empty input");
        return CU_ERR_TRUNCATED;
    }
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    int r = inflateInit(&strm);
    if (r != Z_OK) return map_zlib_error(r, CU_ERR_DECOMPRESSION);

    strm.next_in  = (Bytef*)in;
    strm.avail_in = (uInt)in_len;
    strm.next_out  = out;
    strm.avail_out = (uInt)*out_len;

    size_t cap_limit = cu_get_max_decompressed_size();
    cu_status_t ret;

    r = inflate(&strm, Z_FINISH);
    if (r == Z_STREAM_END) {
        if (cap_limit > 0 && strm.total_out > cap_limit) {
            cu_set_last_errorf("zlib: decompressed size %llu exceeds cap %zu",
                               (unsigned long long)strm.total_out, cap_limit);
            ret = CU_ERR_SIZE_LIMIT;
        } else {
            *out_len = (size_t)strm.total_out;
            ret = CU_OK;
        }
    } else if (r == Z_BUF_ERROR || (r == Z_OK && strm.avail_out == 0)) {
        /* Output exhausted; zlib can't tell us how much more. */
        cu_set_last_error("zlib: output buffer too small (size not encoded in stream)");
        ret = CU_ERR_SIZE_UNKNOWN;
    } else if (r == Z_DATA_ERROR) {
        cu_set_last_error("zlib: corrupted data");
        ret = CU_ERR_DECOMPRESSION;
    } else {
        ret = map_zlib_error(r, CU_ERR_DECOMPRESSION);
    }
    inflateEnd(&strm);
    return ret;
}

static cu_status_t zlib_decompress_size_hint(
    const uint8_t* in, size_t in_len, size_t* out_size
) {
    (void)in; (void)in_len; (void)out_size;
    /* zlib wire format never carries the decompressed size. */
    return CU_ERR_SIZE_UNKNOWN;
}

/* ============================================================================
 * Streaming (helpers shared between compress & decompress paths)
 * ============================================================================ */

typedef struct {
    z_stream strm;
    uint8_t* pending;
    size_t   pending_len;
    size_t   pending_cap;
    int      finishing;
    int      stream_end;
    int      kind;  /* 0 = compress, 1 = decompress */
} zlib_stream_state_t;

static cu_status_t pending_append(zlib_stream_state_t* st, const uint8_t* src, size_t n) {
    if (n == 0) return CU_OK;
    size_t need = st->pending_len + n;
    if (need > st->pending_cap) {
        size_t new_cap = st->pending_cap ? st->pending_cap * 2 : 16 * 1024;
        while (new_cap < need) new_cap *= 2;
        uint8_t* p = realloc(st->pending, new_cap);
        if (!p) {
            cu_set_last_error("zlib: out of memory");
            return CU_ERR_OOM;
        }
        st->pending = p;
        st->pending_cap = new_cap;
    }
    memcpy(st->pending + st->pending_len, src, n);
    st->pending_len += n;
    return CU_OK;
}

/* ============================================================================
 * Streaming compression
 * ============================================================================ */

static cu_status_t zlib_cstream_create(int level, void** out_state) {
    zlib_stream_state_t* st = calloc(1, sizeof(*st));
    if (!st) { cu_set_last_error("zlib: oom"); return CU_ERR_OOM; }
    st->kind = 0;
    int r = deflateInit(&st->strm, zlib_native_level(level));
    if (r != Z_OK) {
        cu_status_t s = map_zlib_error(r, CU_ERR_COMPRESSION);
        free(st);
        return s;
    }
    *out_state = st;
    return CU_OK;
}

static cu_status_t cstream_pump(zlib_stream_state_t* st, int flush_flag,
                                uint8_t* out, size_t* out_len) {
    size_t cap = *out_len;
    size_t written = 0;
    int z_ret = Z_OK;

    /* Drain pending tail first. */
    if (st->pending_len > 0) {
        st->strm.next_in  = st->pending;
        st->strm.avail_in = (uInt)st->pending_len;
        st->strm.next_out  = out + written;
        st->strm.avail_out = (uInt)(cap - written);
        z_ret = deflate(&st->strm, Z_NO_FLUSH);
        size_t produced = (cap - written) - st->strm.avail_out;
        written += produced;
        if (z_ret != Z_OK && z_ret != Z_BUF_ERROR) {
            *out_len = written;
            return map_zlib_error(z_ret, CU_ERR_COMPRESSION);
        }
        size_t consumed = st->pending_len - st->strm.avail_in;
        if (st->strm.avail_in > 0) {
            /* Output filled with pending data unconsumed. */
            memmove(st->pending, st->pending + consumed, st->strm.avail_in);
            st->pending_len = st->strm.avail_in;
            *out_len = written;
            return CU_ERR_BUF_TOO_SMALL;
        }
        st->pending_len = 0;
    }

    /* No new input here — caller threaded any new input through pending. */
    if (flush_flag != Z_NO_FLUSH) {
        for (;;) {
            st->strm.next_in  = NULL;
            st->strm.avail_in = 0;
            st->strm.next_out  = out + written;
            st->strm.avail_out = (uInt)(cap - written);
            z_ret = deflate(&st->strm, flush_flag);
            size_t produced = (cap - written) - st->strm.avail_out;
            written += produced;
            if (z_ret == Z_STREAM_END) {
                st->stream_end = 1;
                *out_len = written;
                return CU_OK;
            }
            if (z_ret != Z_OK && z_ret != Z_BUF_ERROR) {
                *out_len = written;
                return map_zlib_error(z_ret, CU_ERR_COMPRESSION);
            }
            if (st->strm.avail_out == 0) {
                /* Out of room and more to write. */
                *out_len = written;
                return CU_ERR_BUF_TOO_SMALL;
            }
            /* Z_OK / Z_BUF_ERROR with room remaining means deflate is done
             * producing for this flush_flag. Should have hit STREAM_END if
             * Z_FINISH; if Z_NO_FLUSH this should be unreachable. */
            break;
        }
    }

    *out_len = written;
    return CU_OK;
}

static cu_status_t zlib_cstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    zlib_stream_state_t* st = (zlib_stream_state_t*)state;
    if (st->finishing) {
        cu_set_last_error("zlib: write after finish");
        return CU_ERR_STREAM_STATE;
    }
    /* Append new input to pending and pump. Simple and correct; the alt
     * (direct feed + stash leftover) requires tracking strm.next_in
     * across calls. */
    cu_status_t s = pending_append(st, in, in_len);
    if (s != CU_OK) return s;
    return cstream_pump(st, Z_NO_FLUSH, out, out_len);
}

static cu_status_t zlib_cstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    zlib_stream_state_t* st = (zlib_stream_state_t*)state;
    st->finishing = 1;
    if (st->stream_end) {
        *out_len = 0;
        return CU_OK;
    }
    return cstream_pump(st, Z_FINISH, out, out_len);
}

static void zlib_cstream_destroy(void* state) {
    zlib_stream_state_t* st = (zlib_stream_state_t*)state;
    if (!st) return;
    deflateEnd(&st->strm);
    free(st->pending);
    free(st);
}

/* ============================================================================
 * Streaming decompression
 * ============================================================================ */

static cu_status_t zlib_dstream_create(void** out_state) {
    zlib_stream_state_t* st = calloc(1, sizeof(*st));
    if (!st) { cu_set_last_error("zlib: oom"); return CU_ERR_OOM; }
    st->kind = 1;
    int r = inflateInit(&st->strm);
    if (r != Z_OK) {
        cu_status_t s = map_zlib_error(r, CU_ERR_DECOMPRESSION);
        free(st);
        return s;
    }
    *out_state = st;
    return CU_OK;
}

static cu_status_t dstream_pump(zlib_stream_state_t* st,
                                uint8_t* out, size_t* out_len) {
    size_t cap = *out_len;
    size_t written = 0;

    if (st->pending_len > 0) {
        st->strm.next_in  = st->pending;
        st->strm.avail_in = (uInt)st->pending_len;
        st->strm.next_out  = out + written;
        st->strm.avail_out = (uInt)(cap - written);
        int z_ret = inflate(&st->strm, Z_NO_FLUSH);
        size_t produced = (cap - written) - st->strm.avail_out;
        written += produced;
        if (z_ret == Z_STREAM_END) {
            st->stream_end = 1;
        } else if (z_ret != Z_OK && z_ret != Z_BUF_ERROR) {
            *out_len = written;
            return map_zlib_error(z_ret, CU_ERR_DECOMPRESSION);
        }
        size_t consumed = st->pending_len - st->strm.avail_in;
        if (st->strm.avail_in > 0 && !st->stream_end) {
            memmove(st->pending, st->pending + consumed, st->strm.avail_in);
            st->pending_len = st->strm.avail_in;
            *out_len = written;
            return CU_ERR_BUF_TOO_SMALL;
        }
        st->pending_len = 0;
    }
    *out_len = written;
    return CU_OK;
}

static cu_status_t zlib_dstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    zlib_stream_state_t* st = (zlib_stream_state_t*)state;
    if (st->stream_end && in_len > 0) {
        cu_set_last_error("zlib: data after end of stream");
        return CU_ERR_DECOMPRESSION;
    }
    cu_status_t s = pending_append(st, in, in_len);
    if (s != CU_OK) return s;
    return dstream_pump(st, out, out_len);
}

static cu_status_t zlib_dstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    zlib_stream_state_t* st = (zlib_stream_state_t*)state;
    cu_status_t s = dstream_pump(st, out, out_len);
    if (s != CU_OK) return s;
    if (!st->stream_end) {
        cu_set_last_error("zlib: truncated stream at finish");
        return CU_ERR_TRUNCATED;
    }
    return CU_OK;
}

static void zlib_dstream_destroy(void* state) {
    zlib_stream_state_t* st = (zlib_stream_state_t*)state;
    if (!st) return;
    inflateEnd(&st->strm);
    free(st->pending);
    free(st);
}

/* ============================================================================
 * Vtable
 * ============================================================================ */

const cu_algorithm_vtbl_t cu_zlib_vtbl = {
    .name                      = "zlib",
    .compress_bound            = zlib_compress_bound,
    .compress                  = zlib_compress,
    .decompress                = zlib_decompress,
    .decompress_size_hint      = zlib_decompress_size_hint,
    .compress_stream_create    = zlib_cstream_create,
    .compress_stream_write     = zlib_cstream_write,
    .compress_stream_finish    = zlib_cstream_finish,
    .compress_stream_destroy   = zlib_cstream_destroy,
    .decompress_stream_create  = zlib_dstream_create,
    .decompress_stream_write   = zlib_dstream_write,
    .decompress_stream_finish  = zlib_dstream_finish,
    .decompress_stream_destroy = zlib_dstream_destroy,
};
