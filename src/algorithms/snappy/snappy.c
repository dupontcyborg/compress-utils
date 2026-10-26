/*
 * snappy.c — Snappy vtable for compress-utils.
 *
 * Wire format: the raw Snappy block format (a varint of the uncompressed
 * length followed by the compressed stream), as produced by
 * snappy_core_encode() / consumed by snappy_core_decode(). This is the format
 * used by `python -m snappy` (raw mode), Java's Snappy.compress(), and the
 * reference C++ snappy::Compress(). It is NOT the Snappy *framing* format
 * (the `.sz` stream format used by CLIs / streaming libraries).
 *
 * Implementation: our own pure-C codec in core/ (snappy_core.c, a self-contained
 * unit that knows nothing of this ABI; see core/PROVENANCE.md). This keeps the
 * whole release library pure C — no libc++/libstdc++ runtime dependency in any
 * binding. google/snappy (C++) is retained at third_party/snappy-oracle purely
 * as the differential-test oracle; it never ships. The wire format is the
 * frozen Snappy raw block format, so output decodes with any Snappy decoder
 * (verified in both directions against the oracle by the CTest differential
 * tests). Compressed bytes are valid but are not identical to other encoders'.
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
 * snappy_core_uncompressed_length).
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
 *
 * Single-stream decoding (CU_DSTREAM_SINGLE) finds the end of the block as
 * input arrives by walking its element tags (lengths only — offsets are left
 * to snappy_core_decode), then decodes it and keeps the input after it as
 * unused. It still emits nothing until the block is complete.
 *
 * Both stream decoders allocate the block's declared length, which the caller
 * can't bound (the allocation comes before any output), so they enforce the
 * decompressed-size cap themselves: captured at creation, as the raw LZ4/LZO
 * stream decoders do. Single mode checks it as soon as the length varint has
 * been read; the default mode at finish, before allocating.
 */

#include "algorithm_registry.h"
#include "compress_utils.h"
#include "utils/single_stream.h"

#include "snappy_core.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Maps a core result to a cu status. The core takes explicit capacities, so
 * the adapter sizes buffers itself before every call (BUF_TOO_SMALL and the
 * size cap are decided here); a failure is one of: malformed block, truncated
 * block, or allocation failure. All decode failures are CU_ERR_DECOMPRESSION,
 * as they always have been for this codec. */
static cu_status_t snappy_err(snappy_core_status_t st, cu_status_t fallback, const char* what) {
    switch (st) {
        case SNAPPY_CORE_OK:
            return CU_OK;
        case SNAPPY_CORE_NOMEM:
            cu_set_last_errorf("snappy: %s failed (out of memory)", what);
            return CU_ERR_OOM;
        case SNAPPY_CORE_TRUNCATED:
            cu_set_last_errorf("snappy: %s failed (truncated block)", what);
            return fallback;
        default:
            cu_set_last_errorf("snappy: %s failed (invalid block)", what);
            return fallback;
    }
}

/* Reads the declared length; false if the varint is malformed or truncated. */
static int snappy_declared_length(const uint8_t* in, size_t in_len, size_t* out) {
    return snappy_core_uncompressed_length(in, in_len, out) == SNAPPY_CORE_OK;
}

/* ============================================================================
 * One-shot
 * ============================================================================ */

static size_t snappy_compress_bound(size_t in_len) {
    return snappy_core_max_compressed_length(in_len);
}

static cu_status_t snappy_compress_oneshot(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len,
    int level
) {
    (void)level;  /* Snappy has no compression levels. */
    size_t needed = snappy_core_max_compressed_length(in_len);
    if (*out_len < needed) {
        *out_len = needed;
        return CU_ERR_BUF_TOO_SMALL;
    }
    size_t clen = 0;
    snappy_core_status_t st = snappy_core_encode(
        in ? in : (const uint8_t*)"", in_len, out, *out_len, &clen);
    if (st != SNAPPY_CORE_OK) return snappy_err(st, CU_ERR_COMPRESSION, "compress");
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
    if (!snappy_declared_length(in, in_len, &needed)) {
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
    size_t n = 0;
    snappy_core_status_t st = snappy_core_decode(in, in_len, out, needed, &n);
    if (st != SNAPPY_CORE_OK) return snappy_err(st, CU_ERR_DECOMPRESSION, "uncompress");
    *out_len = n;
    return CU_OK;
}

static cu_status_t snappy_decompress_size_hint(
    const uint8_t* in, size_t in_len, size_t* out_size
) {
    if (in_len == 0) return CU_ERR_TRUNCATED;
    size_t sz = 0;
    if (!snappy_declared_length(in, in_len, &sz)) {
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
    /* Single-stream decoding (CU_DSTREAM_SINGLE). */
    size_t   cap;        /* decompression: size cap captured at creation; 0 = none */
    /* Single-stream decoding only. */
    int      single;
    int      ended;      /* the block is complete and decoded into out_buf */
    size_t   scan_pos;   /* next in_buf byte the tag scan hasn't consumed */
    int      have_len;   /* the length varint has been read */
    uint64_t declared;   /* uncompressed length from the varint */
    uint64_t counted;    /* output length of the elements scanned so far */
    cu_unused_t unused;  /* input received after the end */
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
    cu_unused_free(&st->unused);
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
        size_t bound = snappy_core_max_compressed_length(st->in_len);
        st->out_buf = malloc(bound ? bound : 1);
        if (!st->out_buf) { cu_set_last_error("snappy: oom"); return CU_ERR_OOM; }
        size_t clen = 0;
        snappy_core_status_t cs = snappy_core_encode(
            st->in_buf ? st->in_buf : (const uint8_t*)"", st->in_len,
            st->out_buf, bound, &clen);
        if (cs != SNAPPY_CORE_OK) return snappy_err(cs, CU_ERR_COMPRESSION, "compress");
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
    st->cap = cu_get_max_decompressed_size();
    *out_state = st;
    return CU_OK;
}

static cu_status_t snappy_dstream_create_single(void** out_state) {
    snappy_stream_state_t* st = calloc(1, sizeof(*st));
    if (!st) { cu_set_last_error("snappy: oom"); return CU_ERR_OOM; }
    st->single = 1;
    st->cap = cu_get_max_decompressed_size();
    *out_state = st;
    return CU_OK;
}

/* Single mode: walks the elements buffered in in_buf from scan_pos, without
 * decoding them, until their output lengths reach the declared length. Sets
 * *end to the block's length once complete, else 0 (more input needed). An
 * element is only consumed once all its bytes are buffered, so a later call
 * re-reads a partial one. Rejects any length that can't fit the declared
 * length before waiting for its bytes, so a malformed block can't make the
 * stream buffer without bound. */
static cu_status_t sn_scan(snappy_stream_state_t* st, size_t* end) {
    const uint8_t* p = st->in_buf;
    size_t n = st->in_len;
    size_t pos = st->scan_pos;
    *end = 0;

    if (!st->have_len) {
        uint64_t v = 0;
        for (unsigned i = 0;; i++) {
            if (i == 5) {
                cu_set_last_error("snappy: invalid header (length varint too long)");
                return CU_ERR_DECOMPRESSION;
            }
            if (pos + i >= n) return CU_OK;
            uint8_t b = p[pos + i];
            v |= (uint64_t)(b & 0x7F) << (7 * i);
            if (!(b & 0x80)) {
                pos += i + 1;
                break;
            }
        }
        if (v > UINT32_MAX || v > SIZE_MAX) {
            cu_set_last_error("snappy: invalid header (length out of range)");
            return CU_ERR_DECOMPRESSION;
        }
        if (st->cap > 0 && v > st->cap) {
            cu_set_last_errorf("snappy: decompressed size %llu exceeds cap %zu",
                               (unsigned long long)v, st->cap);
            return CU_ERR_SIZE_LIMIT;
        }
        st->have_len = 1;
        st->declared = v;
    }

    while (st->counted < st->declared && pos < n) {
        uint8_t tag = p[pos];
        uint64_t len, size;
        switch (tag & 3) {
            case 0: {  /* literal: length in the tag, or in 1-4 following bytes */
                unsigned code = tag >> 2;
                unsigned extra = code < 60 ? 0 : code - 59;
                if (n - pos - 1 < extra) goto more;
                uint64_t l = code;
                if (extra) {
                    l = 0;
                    for (unsigned k = 0; k < extra; k++) l |= (uint64_t)p[pos + 1 + k] << (8 * k);
                }
                len = l + 1;
                size = 1 + extra + len;
                break;
            }
            case 1:  len = 4 + ((tag >> 2) & 7); size = 2; break;  /* copy, 1-byte offset */
            case 2:  len = (tag >> 2) + 1;       size = 3; break;  /* copy, 2-byte offset */
            default: len = (tag >> 2) + 1;       size = 5; break;  /* copy, 4-byte offset */
        }
        if (len > st->declared - st->counted) {
            cu_set_last_error("snappy: element runs past the declared length");
            return CU_ERR_DECOMPRESSION;
        }
        if ((uint64_t)(n - pos) < size) break;
        pos += (size_t)size;
        st->counted += len;
    }
more:
    st->scan_pos = pos;
    if (st->counted == st->declared) *end = pos;
    return CU_OK;
}

static cu_status_t sn_dstream_write_single(
    snappy_stream_state_t* st, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    size_t cap = *out_len;
    size_t written = 0;
    *out_len = 0;

    if (!st->ended) {
        cu_status_t s = sn_in_append(st, in, in_len);
        if (s != CU_OK) return s;
        size_t end;
        s = sn_scan(st, &end);
        if (s != CU_OK || end == 0) return s;

        /* The block is in_buf[0, end); the rest came after it. */
        s = cu_unused_append(&st->unused, st->in_buf + end, st->in_len - end);
        if (s != CU_OK) return s;
        size_t usize = (size_t)st->declared;
        st->out_buf = malloc(usize ? usize : 1);
        if (!st->out_buf) { cu_set_last_error("snappy: oom"); return CU_ERR_OOM; }
        size_t n = 0;
        snappy_core_status_t ds = snappy_core_decode(st->in_buf, end, st->out_buf, usize, &n);
        if (ds != SNAPPY_CORE_OK) return snappy_err(ds, CU_ERR_DECOMPRESSION, "uncompress");
        free(st->in_buf);
        st->in_buf = NULL;
        st->in_len = st->in_cap = 0;
        st->out_tail = usize;
        st->produced = 1;
        st->ended = 1;
    }

    int more = sn_out_drain(st, out, cap, &written);
    *out_len = written;
    return more ? CU_ERR_BUF_TOO_SMALL : CU_OK;
}

static cu_status_t snappy_dstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    snappy_stream_state_t* st = (snappy_stream_state_t*)state;
    if (st->single) return sn_dstream_write_single(st, in, in_len, out, out_len);
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

    if (st->single) {
        if (st->ended) return sn_dstream_write_single(st, NULL, 0, out, out_len);
        *out_len = 0;
        cu_set_last_error("snappy: truncated block at finish");
        return CU_ERR_TRUNCATED;
    }

    if (!st->produced) {
        if (st->in_len == 0) {
            cu_set_last_error("snappy: truncated stream at finish");
            return CU_ERR_TRUNCATED;
        }
        size_t usize = 0;
        if (!snappy_declared_length(st->in_buf, st->in_len, &usize)) {
            cu_set_last_error("snappy: invalid header");
            return CU_ERR_DECOMPRESSION;
        }
        if (st->cap > 0 && usize > st->cap) {
            *out_len = 0;
            cu_set_last_errorf("snappy: decompressed size %zu exceeds cap %zu", usize, st->cap);
            return CU_ERR_SIZE_LIMIT;
        }
        st->out_buf = malloc(usize ? usize : 1);
        if (!st->out_buf) { cu_set_last_error("snappy: oom"); return CU_ERR_OOM; }
        size_t n = 0;
        snappy_core_status_t ds = snappy_core_decode(st->in_buf, st->in_len,
                                                     st->out_buf, usize, &n);
        if (ds != SNAPPY_CORE_OK) return snappy_err(ds, CU_ERR_DECOMPRESSION, "uncompress");
        st->out_tail = usize;
        st->produced = 1;
    }

    int more = sn_out_drain(st, out, cap, &written);
    *out_len = written;
    return more ? CU_ERR_BUF_TOO_SMALL : CU_OK;
}

static int snappy_dstream_end(const void* state, const uint8_t** unused, size_t* unused_len) {
    const snappy_stream_state_t* st = (const snappy_stream_state_t*)state;
    if (!st->single || !st->ended) return 0;
    cu_unused_get(&st->unused, unused, unused_len);
    return 1;
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
    .decompress_stream_create_single = snappy_dstream_create_single,
    .decompress_stream_end     = snappy_dstream_end,
#endif
};
