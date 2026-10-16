/*
 * deflate_backend.h — shared zlib/gzip implementation for compress-utils.
 *
 * zlib and gzip are the same DEFLATE codec (the zlib library) differing only
 * in the wrapper written around the compressed data:
 *   - zlib wrapper (RFC 1950): windowBits 15
 *   - gzip wrapper (RFC 1952): windowBits 15 + 16 = 31
 *
 * Neither wire format carries the decompressed size (gzip's ISIZE trailer is
 * mod 2^32 and needs the whole stream), so decompress_size_hint always returns
 * CU_ERR_SIZE_UNKNOWN and the bindings fall back to streaming.
 *
 * Data after the end of the compressed stream depends on the wrapper:
 *   - gzip: another member (`cat a.gz b.gz`, pigz), decoded and appended as
 *     the gzip CLI does. Zero padding between or after members is skipped.
 *   - zlib: an error.
 *   - raw DEFLATE: ignored, since a raw stream cut from a container can be
 *     followed by the container's own bytes.
 *
 * This header is internal and included by exactly two .c files.
 */

#ifndef CU_DEFLATE_BACKEND_H
#define CU_DEFLATE_BACKEND_H

#include "algorithm_registry.h"
#include "compress_utils.h"
#include "utils/levels.h"

#include <zlib.h>

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* windowBits selecting the wrapper the zlib library writes/reads. */
#define CU_DFL_ZLIB_WBITS 15
#define CU_DFL_GZIP_WBITS 31

/* At the end of a gzip member, skip zero padding; returns whether another
 * member follows in strm's input. */
static int dfl_next_gzip_member(z_stream* strm) {
    while (strm->avail_in > 0 && *strm->next_in == 0) {
        strm->next_in++;
        strm->avail_in--;
    }
    return strm->avail_in > 0;
}

/* zlib/gzip: user 1..10 → zlib native 1..9 (clamp). */
static int dfl_native_level(int user_level) {
    return cu_clamp_level(user_level, 1, 9);
}

static cu_status_t dfl_map_error(int code, cu_status_t fallback) {
    if (code == Z_OK || code == Z_STREAM_END) return CU_OK;
    cu_set_last_errorf("zlib: code %d (%s)", code, zError(code));
    return fallback;
}

/* ============================================================================
 * One-shot
 * ============================================================================ */

static size_t dfl_compress_bound(size_t in_len, int window_bits) {
    uLong bound = compressBound((uLong)in_len);
    /* compressBound accounts for the zlib wrapper; the gzip wrapper is larger
     * (10-byte header + 8-byte trailer). Add margin so the bound stays a true
     * upper bound for gzip output too. */
    if (window_bits > 15) bound += 18;
    return bound;
}

static cu_status_t dfl_compress(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len,
    int level, int window_bits
) {
    size_t needed = dfl_compress_bound(in_len, window_bits);
    if (*out_len < needed) {
        *out_len = needed;
        return CU_ERR_BUF_TOO_SMALL;
    }
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    /* deflateInit2 with windowBits 15 / memLevel 8 / default strategy produces
     * output identical to compress2(); windowBits 31 adds the gzip wrapper. */
    int r = deflateInit2(&strm, dfl_native_level(level), Z_DEFLATED,
                         window_bits, 8, Z_DEFAULT_STRATEGY);
    if (r != Z_OK) return dfl_map_error(r, CU_ERR_COMPRESSION);

    strm.next_in   = (Bytef*)in;
    strm.avail_in  = (uInt)in_len;
    strm.next_out  = out;
    strm.avail_out = (uInt)*out_len;

    r = deflate(&strm, Z_FINISH);
    if (r != Z_STREAM_END) {
        deflateEnd(&strm);
        return dfl_map_error(r, CU_ERR_COMPRESSION);
    }
    *out_len = (size_t)strm.total_out;
    deflateEnd(&strm);
    return CU_OK;
}

static cu_status_t dfl_decompress(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len,
    int window_bits
) {
    if (in_len == 0) {
        cu_set_last_error("zlib: empty input");
        return CU_ERR_TRUNCATED;
    }
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    int r = inflateInit2(&strm, window_bits);
    if (r != Z_OK) return dfl_map_error(r, CU_ERR_DECOMPRESSION);

    strm.next_in   = (Bytef*)in;
    strm.avail_in  = (uInt)in_len;
    strm.next_out  = out;
    strm.avail_out = (uInt)*out_len;

    size_t cap_limit = cu_get_max_decompressed_size();
    cu_status_t ret;

    r = inflate(&strm, Z_FINISH);
    int trailing = 0;
    while (r == Z_STREAM_END && strm.avail_in > 0) {
        if (window_bits == CU_DFL_ZLIB_WBITS) {
            trailing = 1;
            break;
        }
        if (window_bits != CU_DFL_GZIP_WBITS || !dfl_next_gzip_member(&strm)) break;
        r = inflateReset(&strm);
        if (r == Z_OK) r = inflate(&strm, Z_FINISH);
    }
    /* inflateReset zeroes total_out, so measure from the output pointer. */
    size_t produced = (size_t)(strm.next_out - out);

    if (trailing) {
        cu_set_last_error("zlib: data after end of stream");
        ret = CU_ERR_DECOMPRESSION;
    } else if (r == Z_STREAM_END) {
        if (cap_limit > 0 && produced > cap_limit) {
            cu_set_last_errorf("zlib: decompressed size %zu exceeds cap %zu",
                               produced, cap_limit);
            ret = CU_ERR_SIZE_LIMIT;
        } else {
            *out_len = produced;
            ret = CU_OK;
        }
    } else if (r == Z_BUF_ERROR || (r == Z_OK && strm.avail_out == 0)) {
        /* Output exhausted; the wire format doesn't encode the size. */
        cu_set_last_error("zlib: output buffer too small (size not encoded in stream)");
        ret = CU_ERR_SIZE_UNKNOWN;
    } else if (r == Z_DATA_ERROR) {
        cu_set_last_error("zlib: corrupted data");
        ret = CU_ERR_DECOMPRESSION;
    } else {
        ret = dfl_map_error(r, CU_ERR_DECOMPRESSION);
    }
    inflateEnd(&strm);
    return ret;
}

static cu_status_t dfl_decompress_size_hint(
    const uint8_t* in, size_t in_len, size_t* out_size
) {
    (void)in; (void)in_len; (void)out_size;
    /* zlib carries no size; gzip's ISIZE is mod 2^32 and needs the whole
     * stream — not a reliable cheap probe, so report unknown for both. */
    return CU_ERR_SIZE_UNKNOWN;
}

/* ============================================================================
 * Streaming (shared: state carries the z_stream, so pumps are wrapper-agnostic)
 * ============================================================================ */

typedef struct {
    z_stream strm;
    uint8_t* pending;      /* unconsumed input; live bytes are [pending_off, pending_len) */
    size_t   pending_off;  /* decompression only; compression keeps it 0 */
    size_t   pending_len;
    size_t   pending_cap;
    int      finishing;
    int      stream_end;
    int      kind;  /* 0 = compress, 1 = decompress */
    int      window_bits;
} dfl_stream_state_t;

/* Appends to the live pending bytes, first moving them to the front. Draining
 * only advances pending_off, so each byte is moved at most once per append
 * rather than once per output page. */
static cu_status_t dfl_pending_append(dfl_stream_state_t* st, const uint8_t* src, size_t n) {
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

/* ---- Streaming compression ---- */

static cu_status_t dfl_cstream_create(int level, int window_bits, void** out_state) {
    dfl_stream_state_t* st = calloc(1, sizeof(*st));
    if (!st) { cu_set_last_error("zlib: oom"); return CU_ERR_OOM; }
    st->kind = 0;
    int r = deflateInit2(&st->strm, dfl_native_level(level), Z_DEFLATED,
                         window_bits, 8, Z_DEFAULT_STRATEGY);
    if (r != Z_OK) {
        cu_status_t s = dfl_map_error(r, CU_ERR_COMPRESSION);
        free(st);
        return s;
    }
    *out_state = st;
    return CU_OK;
}

static cu_status_t dfl_cstream_pump(dfl_stream_state_t* st, int flush_flag,
                                    uint8_t* out, size_t* out_len) {
    size_t cap = *out_len;
    size_t written = 0;
    int z_ret = Z_OK;

    /* Deflate pending input first. Without a flush this also runs with no
     * input, so a drain call collects output deflate still holds. */
    if (st->pending_len > 0 || flush_flag == Z_NO_FLUSH) {
        st->strm.next_in  = st->pending;
        st->strm.avail_in = (uInt)st->pending_len;
        st->strm.next_out  = out + written;
        st->strm.avail_out = (uInt)(cap - written);
        z_ret = deflate(&st->strm, Z_NO_FLUSH);
        size_t produced = (cap - written) - st->strm.avail_out;
        written += produced;
        if (z_ret != Z_OK && z_ret != Z_BUF_ERROR) {
            *out_len = written;
            return dfl_map_error(z_ret, CU_ERR_COMPRESSION);
        }
        size_t consumed = st->pending_len - st->strm.avail_in;
        if (st->strm.avail_in > 0 || st->strm.avail_out == 0) {
            /* Output filled: input may remain, and deflate may hold output. */
            if (st->strm.avail_in > 0) {
                memmove(st->pending, st->pending + consumed, st->strm.avail_in);
            }
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
                return dfl_map_error(z_ret, CU_ERR_COMPRESSION);
            }
            if (st->strm.avail_out == 0) {
                /* Out of room and more to write. */
                *out_len = written;
                return CU_ERR_BUF_TOO_SMALL;
            }
            /* Z_OK / Z_BUF_ERROR with room remaining means deflate is done
             * producing for this flush_flag. */
            break;
        }
    }

    *out_len = written;
    return CU_OK;
}

static cu_status_t dfl_cstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    dfl_stream_state_t* st = (dfl_stream_state_t*)state;
    if (st->finishing) {
        cu_set_last_error("zlib: write after finish");
        return CU_ERR_STREAM_STATE;
    }
    cu_status_t s = dfl_pending_append(st, in, in_len);
    if (s != CU_OK) return s;
    return dfl_cstream_pump(st, Z_NO_FLUSH, out, out_len);
}

static cu_status_t dfl_cstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    dfl_stream_state_t* st = (dfl_stream_state_t*)state;
    st->finishing = 1;
    if (st->stream_end) {
        *out_len = 0;
        return CU_OK;
    }
    return dfl_cstream_pump(st, Z_FINISH, out, out_len);
}

static void dfl_stream_destroy(void* state);  /* fwd — shared by both dirs */

/* ---- Streaming decompression ---- */

static cu_status_t dfl_dstream_create(int window_bits, void** out_state) {
    dfl_stream_state_t* st = calloc(1, sizeof(*st));
    if (!st) { cu_set_last_error("zlib: oom"); return CU_ERR_OOM; }
    st->kind = 1;
    st->window_bits = window_bits;
    int r = inflateInit2(&st->strm, window_bits);
    if (r != Z_OK) {
        cu_status_t s = dfl_map_error(r, CU_ERR_DECOMPRESSION);
        free(st);
        return s;
    }
    *out_state = st;
    return CU_OK;
}

/* Inflates `in` into `out`; returns the unconsumed input length in *left.
 * A full `out` returns BUF_TOO_SMALL even when all input was consumed, since
 * inflate may still hold output; the drain call then runs inflate again.
 * zlib counts in uInt, so input and output are fed in pieces of at most
 * UINT_MAX bytes. */
static cu_status_t dfl_inflate(dfl_stream_state_t* st, const uint8_t* in, size_t in_len,
                               uint8_t* out, size_t* out_len, size_t* left) {
    size_t cap = *out_len > UINT_MAX ? UINT_MAX : *out_len;
    size_t rest = in_len;
    st->strm.next_in   = (Bytef*)in;
    st->strm.avail_in  = 0;
    st->strm.next_out  = out;
    st->strm.avail_out = (uInt)cap;
    cu_status_t s = CU_OK;

    for (;;) {
        if (st->strm.avail_in == 0 && rest > 0) {
            st->strm.avail_in = rest > UINT_MAX ? UINT_MAX : (uInt)rest;
            rest -= st->strm.avail_in;
        }
        if (st->stream_end) {
            if (st->strm.avail_in == 0) break;
            if (st->window_bits == CU_DFL_ZLIB_WBITS) {
                cu_set_last_error("zlib: data after end of stream");
                s = CU_ERR_DECOMPRESSION;
                break;
            }
            if (st->window_bits != CU_DFL_GZIP_WBITS) {
                st->strm.avail_in = 0;  /* raw DEFLATE: not ours */
                rest = 0;
                break;
            }
            if (!dfl_next_gzip_member(&st->strm)) continue;  /* refill, or end */
            int r = inflateReset(&st->strm);
            if (r != Z_OK) { s = dfl_map_error(r, CU_ERR_DECOMPRESSION); break; }
            st->stream_end = 0;
        }
        int z_ret = inflate(&st->strm, Z_NO_FLUSH);
        if (z_ret == Z_STREAM_END) {
            st->stream_end = 1;
            continue;
        }
        if (z_ret != Z_OK && z_ret != Z_BUF_ERROR) {
            s = dfl_map_error(z_ret, CU_ERR_DECOMPRESSION);
            break;
        }
        if (st->strm.avail_out == 0) { s = CU_ERR_BUF_TOO_SMALL; break; }
        if (st->strm.avail_in == 0 && rest == 0) break;  /* needs more input */
        /* No progress despite input and output room: never expected from
         * zlib, but fail rather than drop the input or spin. */
        if (z_ret == Z_BUF_ERROR && st->strm.avail_in > 0) {
            s = dfl_map_error(z_ret, CU_ERR_DECOMPRESSION);
            break;
        }
    }

    *out_len = cap - st->strm.avail_out;
    *left = st->strm.avail_in + rest;
    return s;
}

/* Decodes retained input first, then `in`; keeps whatever is left unconsumed
 * when the output fills. */
static cu_status_t dfl_dstream_pump(dfl_stream_state_t* st, const uint8_t* in, size_t in_len,
                                    uint8_t* out, size_t* out_len) {
    size_t left = 0;
    cu_status_t s;
    if (st->pending_off < st->pending_len) {
        s = dfl_pending_append(st, in, in_len);
        if (s != CU_OK) return s;
        size_t live = st->pending_len - st->pending_off;
        s = dfl_inflate(st, st->pending + st->pending_off, live, out, out_len, &left);
        if (s == CU_ERR_BUF_TOO_SMALL) st->pending_off += live - left;
        else st->pending_off = st->pending_len = 0;
        return s;
    }
    s = dfl_inflate(st, in, in_len, out, out_len, &left);
    if (s == CU_ERR_BUF_TOO_SMALL && left > 0) {
        cu_status_t a = dfl_pending_append(st, in + (in_len - left), left);
        if (a != CU_OK) return a;
    }
    return s;
}

static cu_status_t dfl_dstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    dfl_stream_state_t* st = (dfl_stream_state_t*)state;
    if (st->stream_end && in_len > 0 && st->window_bits == CU_DFL_ZLIB_WBITS) {
        cu_set_last_error("zlib: data after end of stream");
        return CU_ERR_DECOMPRESSION;
    }
    return dfl_dstream_pump(st, in, in_len, out, out_len);
}

static cu_status_t dfl_dstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    dfl_stream_state_t* st = (dfl_stream_state_t*)state;
    cu_status_t s = dfl_dstream_pump(st, NULL, 0, out, out_len);
    if (s != CU_OK) return s;
    if (!st->stream_end) {
        cu_set_last_error("zlib: truncated stream at finish");
        return CU_ERR_TRUNCATED;
    }
    return CU_OK;
}

/* ---- Shared teardown (both directions) ---- */

static void dfl_stream_destroy(void* state) {
    dfl_stream_state_t* st = (dfl_stream_state_t*)state;
    if (!st) return;
    if (st->kind == 0) {
        deflateEnd(&st->strm);
    } else {
        inflateEnd(&st->strm);
    }
    free(st->pending);
    free(st);
}

#endif  /* CU_DEFLATE_BACKEND_H */
