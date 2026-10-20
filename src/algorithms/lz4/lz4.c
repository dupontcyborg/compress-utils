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
 * content-size flag is set at encode time. One-shot encoding always sets
 * it, so cu_decompress_size_hint succeeds for those frames.
 *
 * Concatenated frames (and skippable frames) decode as one output, as the
 * lz4 CLI does; input must end exactly at the end of a frame.
 */

#include "algorithm_registry.h"
#include "compress_utils.h"
#include "utils/single_stream.h"

#include <lz4.h>
#include <lz4frame.h>

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

static uint32_t lz4_read32(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Walks the frame and block headers of every frame in the input, without
 * decoding. Returns CU_OK with the summed content size when every frame
 * records one, CU_ERR_SIZE_UNKNOWN when one doesn't, or an error for input
 * that isn't a complete sequence of frames. Skippable frames count as empty. */
static cu_status_t lz4_frames_content_size(const uint8_t* in, size_t in_len,
                                           unsigned long long* total) {
    size_t pos = 0;
    unsigned long long sum = 0;
    int known = 1;
    while (pos < in_len) {
        if (in_len - pos < 8) return CU_ERR_TRUNCATED;
        uint32_t magic = lz4_read32(in + pos);
        if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {  /* skippable frame */
            size_t skip = lz4_read32(in + pos + 4);
            if (in_len - pos - 8 < skip) return CU_ERR_TRUNCATED;
            pos += 8 + skip;
            continue;
        }
        if (magic != 0x184D2204u) return CU_ERR_DECOMPRESSION;
        uint8_t flg = in[pos + 4];
        int block_checksum = flg & 0x10, has_size = flg & 0x08, content_checksum = flg & 0x04;
        size_t header = 7 + (has_size ? 8 : 0) + (flg & 0x01 ? 4 : 0);
        if (in_len - pos < header) return CU_ERR_TRUNCATED;
        if (has_size) {
            unsigned long long n = lz4_read32(in + pos + 6) |
                                   (unsigned long long)lz4_read32(in + pos + 10) << 32;
            if (n > ~0ULL - sum) return CU_ERR_SIZE_LIMIT;
            sum += n;
        } else {
            known = 0;
        }
        pos += header;
        for (;;) {  /* data blocks, up to the end mark */
            if (in_len - pos < 4) return CU_ERR_TRUNCATED;
            uint32_t block = lz4_read32(in + pos);
            pos += 4;
            if (block == 0) break;
            size_t body = (block & 0x7FFFFFFFu) + (block_checksum ? 4 : 0);
            if (in_len - pos < body) return CU_ERR_TRUNCATED;
            pos += body;
        }
        if (content_checksum) {
            if (in_len - pos < 4) return CU_ERR_TRUNCATED;
            pos += 4;
        }
    }
    if (!known) return CU_ERR_SIZE_UNKNOWN;
    *total = sum;
    return CU_OK;
}

/* The first frame's declared content size, or 0 when it has none. */
static cu_status_t lz4_first_frame_size(const uint8_t* in, size_t in_len,
                                        unsigned long long* size) {
    LZ4F_dctx* dctx = NULL;
    size_t r = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_OOM);
    LZ4F_frameInfo_t info;
    size_t probe = in_len;
    r = LZ4F_getFrameInfo(dctx, &info, in, &probe);
    LZ4F_freeDecompressionContext(dctx);
    if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_DECOMPRESSION);
    *size = info.contentSize;
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
    /* With every frame's size known, check it against the caller buffer and
     * cap up front. Otherwise decode into the buffer and let the decoder
     * report malformed or truncated input. */
    unsigned long long total = 0;
    cu_status_t sized = lz4_frames_content_size(in, in_len, &total);
    if (sized == CU_ERR_SIZE_LIMIT) {
        cu_set_last_error("lz4: declared sizes overflow");
        return sized;
    }
    if (sized != CU_OK && sized != CU_ERR_SIZE_UNKNOWN) {
        cu_status_t s = lz4_first_frame_size(in, in_len, &total);
        if (s != CU_OK) return s;
    }
    size_t cap_limit = cu_get_max_decompressed_size();
    if (sized == CU_OK || total > 0) {
        if (cap_limit > 0 && total > cap_limit) {
            cu_set_last_errorf("lz4: declared size %llu exceeds cap %zu", total, cap_limit);
            return CU_ERR_SIZE_LIMIT;
        }
        if (*out_len < total) {
            *out_len = (size_t)total;
            return CU_ERR_BUF_TOO_SMALL;
        }
    }

    LZ4F_dctx* dctx = NULL;
    size_t r = LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION);
    if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_OOM);
    const uint8_t* src = in;
    size_t src_remaining = in_len;
    uint8_t* dst = out;
    size_t dst_remaining = *out_len;
    int at_frame_end = 0;
    while (src_remaining > 0) {
        size_t src_size = src_remaining;
        size_t dst_size = dst_remaining;
        r = LZ4F_decompress(dctx, dst, &dst_size, src, &src_size, NULL);
        if (LZ4F_isError(r)) {
            LZ4F_freeDecompressionContext(dctx);
            return map_lz4f_err(r, CU_ERR_DECOMPRESSION);
        }
        src += src_size;
        src_remaining -= src_size;
        dst += dst_size;
        dst_remaining -= dst_size;
        at_frame_end = r == 0;  /* any input left starts the next frame */
        if (src_size == 0 && dst_size == 0) {
            /* No progress with input left: the output is full. */
            LZ4F_freeDecompressionContext(dctx);
            if (sized != CU_OK) {
                cu_set_last_error("lz4: output buffer too small (size not in frame)");
                return CU_ERR_SIZE_UNKNOWN;
            }
            *out_len = (size_t)total;
            return CU_ERR_BUF_TOO_SMALL;
        }
    }
    LZ4F_freeDecompressionContext(dctx);
    if (!at_frame_end) {
        cu_set_last_error("lz4: truncated frame");
        return CU_ERR_TRUNCATED;
    }
    *out_len = (size_t)(dst - out);
    return CU_OK;
}

static cu_status_t lz4_decompress_size_hint(
    const uint8_t* in, size_t in_len, size_t* out_size
) {
    if (in_len == 0) return CU_ERR_TRUNCATED;
    unsigned long long n = 0;
    cu_status_t s = lz4_frames_content_size(in, in_len, &n);
    if (s == CU_ERR_SIZE_UNKNOWN) return s;
    if (s != CU_OK) {
        /* Malformed or truncated: report what the first frame declares, and
         * let decompression say what is wrong. */
        s = lz4_first_frame_size(in, in_len, &n);
        if (s != CU_OK) return s;
        if (n == 0) return CU_ERR_SIZE_UNKNOWN;
    }
    if (n > (size_t)-1) {
        cu_set_last_error("lz4: declared size exceeds size_t");
        return CU_ERR_SIZE_LIMIT;
    }
    *out_size = (size_t)n;
    return CU_OK;
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

/* Drains out_buf, writes the frame header if needed, and feeds all buffered
 * input to the codec. Returns 1 when the caller's buffer filled first. */
static int cstream_emit_pending(lz4_cstream_state_t* st, uint8_t* out, size_t cap,
                                size_t* written, cu_status_t* err) {
    *err = CU_OK;
    if (out_drain(st, out, cap, written)) return 1;
    if (!st->header_written) {
        size_t r = LZ4F_compressBegin(st->cctx, st->out_buf, st->out_cap, &st->prefs);
        if (LZ4F_isError(r)) { *err = map_lz4f_err(r, CU_ERR_COMPRESSION); return 0; }
        st->out_tail = r;
        st->header_written = 1;
        if (out_drain(st, out, cap, written)) return 1;
    }
    while (st->in_tail > st->in_head) {
        *err = cstream_feed_one_block(st);
        if (*err != CU_OK) return 0;
        if (out_drain(st, out, cap, written)) return 1;
    }
    return 0;
}

static cu_status_t lz4_cstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    lz4_cstream_state_t* st = (lz4_cstream_state_t*)state;
    st->finishing = 1;
    size_t cap = *out_len;
    size_t written = 0;
    cu_status_t err;

    if (st->finished) {
        int more = out_drain(st, out, cap, &written);
        *out_len = written;
        return more ? CU_ERR_BUF_TOO_SMALL : CU_OK;
    }
    if (cstream_emit_pending(st, out, cap, &written, &err)) {
        *out_len = written;
        return CU_ERR_BUF_TOO_SMALL;
    }
    if (err != CU_OK) return err;

    /* End frame. */
    size_t r = LZ4F_compressEnd(st->cctx, st->out_buf, st->out_cap, NULL);
    if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_COMPRESSION);
    st->out_tail = r;
    st->finished = 1;

    int more = out_drain(st, out, cap, &written);
    *out_len = written;
    return more ? CU_ERR_BUF_TOO_SMALL : CU_OK;
}

/* Emits the partial block LZ4F buffers internally, so everything written so
 * far can be decoded; the frame stays open. */
static cu_status_t lz4_cstream_flush(
    void* state, uint8_t* out, size_t* out_len
) {
    lz4_cstream_state_t* st = (lz4_cstream_state_t*)state;
    if (st->finishing) {
        cu_set_last_error("lz4: flush after finish started");
        return CU_ERR_STREAM_STATE;
    }
    size_t cap = *out_len;
    size_t written = 0;
    cu_status_t err;

    if (cstream_emit_pending(st, out, cap, &written, &err)) {
        *out_len = written;
        return CU_ERR_BUF_TOO_SMALL;
    }
    if (err != CU_OK) return err;

    size_t r = LZ4F_flush(st->cctx, st->out_buf, st->out_cap, NULL);
    if (LZ4F_isError(r)) return map_lz4f_err(r, CU_ERR_COMPRESSION);
    st->out_tail = r;

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
    uint8_t* pending;       /* unconsumed input; live bytes are [pending_off, pending_len) */
    size_t   pending_off;
    size_t   pending_len;
    size_t   pending_cap;
    int      at_frame_end;  /* the last input consumed completed a frame */
    int      single;        /* CU_DSTREAM_SINGLE: stop after the first data frame */
    int      ended;         /* single: that frame has been decoded and flushed */
    cu_frame_magic_t magic; /* single: first bytes of the frame being decoded */
    cu_unused_t unused;     /* single: input received after the end */
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

static cu_status_t lz4_dstream_create_single(void** out_state) {
    cu_status_t s = lz4_dstream_create(out_state);
    if (s == CU_OK) ((lz4_dstream_state_t*)*out_state)->single = 1;
    return s;
}

/* Decodes `src` into `out` until the input is consumed or the output fills,
 * continuing across frames (in single mode, stopping at the end of the first
 * data frame). A full `out` returns BUF_TOO_SMALL even when all input was
 * consumed, since LZ4F may still hold decoded output; the drain call then
 * runs the decoder again. Reports the input consumed. */
static cu_status_t dstream_decode(lz4_dstream_state_t* st, const uint8_t* src, size_t src_len,
                                  size_t* consumed, uint8_t* out, size_t* out_len) {
    static const uint8_t empty = 0;
    size_t cap = *out_len, written = 0;
    *consumed = 0;
    if (!src) src = &empty;
    for (;;) {
        size_t src_size = src_len - *consumed, dst_size = cap - written;
        size_t r = LZ4F_decompress(st->dctx, out + written, &dst_size, src + *consumed,
                                   &src_size, NULL);
        if (LZ4F_isError(r)) {
            *out_len = written;
            return map_lz4f_err(r, CU_ERR_DECOMPRESSION);
        }
        if (st->single && src_size > 0) {
            cu_frame_magic_feed(&st->magic, src + *consumed, src_size);
        }
        *consumed += src_size;
        written += dst_size;
        /* Single mode: 0 also ends a skippable frame; only a data frame ends
         * the stream. Either way its output is fully flushed. */
        if (st->single && r == 0) {
            if (!cu_frame_magic_skippable(&st->magic)) {
                st->ended = 1;
                st->at_frame_end = 1;
                *out_len = written;
                return CU_OK;
            }
            st->magic.n = 0;
        }
        /* 0 ends a frame, with its output fully flushed, and the decoder then
         * expects the next one; any other value after consuming input means a
         * frame is in progress. */
        if (r == 0) st->at_frame_end = 1;
        else if (src_size > 0) st->at_frame_end = 0;
        *out_len = written;
        if (*consumed == src_len && (r == 0 || written < cap)) return CU_OK;
        if (written == cap) return CU_ERR_BUF_TOO_SMALL;
        if (src_size == 0 && dst_size == 0) {
            cu_set_last_error("lz4: decoder made no progress");
            return CU_ERR_DECOMPRESSION;
        }
    }
}

/* Appends to the live pending bytes, first moving them to the front. Draining
 * only advances pending_off, so each byte is moved at most once per append. */
static cu_status_t dstream_pending_append(lz4_dstream_state_t* st,
                                          const uint8_t* src, size_t n) {
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
        if (!p) { cu_set_last_error("lz4: oom"); return CU_ERR_OOM; }
        st->pending = p;
        st->pending_cap = new_cap;
    }
    memcpy(st->pending + st->pending_len, src, n);
    st->pending_len += n;
    return CU_OK;
}

/* Decodes retained input first, then `in`; keeps whatever is left unconsumed
 * when the output fills. */
static cu_status_t dstream_pump(lz4_dstream_state_t* st, const uint8_t* in, size_t in_len,
                                uint8_t* out, size_t* out_len) {
    size_t consumed = 0;
    cu_status_t s;
    if (st->pending_off < st->pending_len) {
        s = dstream_pending_append(st, in, in_len);
        if (s != CU_OK) return s;
        size_t live = st->pending_len - st->pending_off;
        s = dstream_decode(st, st->pending + st->pending_off, live, &consumed, out, out_len);
        if (s == CU_ERR_BUF_TOO_SMALL) {
            st->pending_off += consumed;
            return s;
        }
        if (s == CU_OK && st->ended) {
            s = cu_unused_append(&st->unused, st->pending + st->pending_off + consumed,
                                 live - consumed);
        }
        st->pending_off = st->pending_len = 0;
        return s;
    }
    s = dstream_decode(st, in, in_len, &consumed, out, out_len);
    if (s == CU_ERR_BUF_TOO_SMALL && consumed < in_len) {
        cu_status_t a = dstream_pending_append(st, in + consumed, in_len - consumed);
        if (a != CU_OK) return a;
    } else if (s == CU_OK && st->ended && consumed < in_len) {
        s = cu_unused_append(&st->unused, in + consumed, in_len - consumed);
    }
    return s;
}

static cu_status_t lz4_dstream_write(
    void* state, const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    lz4_dstream_state_t* st = (lz4_dstream_state_t*)state;
    /* Single mode past the end: its output was flushed when the frame ended. */
    if (st->ended) {
        *out_len = 0;
        return CU_OK;
    }
    return dstream_pump(st, in, in_len, out, out_len);
}

static cu_status_t lz4_dstream_finish(
    void* state, uint8_t* out, size_t* out_len
) {
    lz4_dstream_state_t* st = (lz4_dstream_state_t*)state;
    if (st->ended) {
        *out_len = 0;
        return CU_OK;
    }
    cu_status_t s = dstream_pump(st, NULL, 0, out, out_len);
    if (s != CU_OK) return s;
    if (st->single ? !st->ended : !st->at_frame_end) {
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
    cu_unused_free(&st->unused);
    free(st);
}

static int lz4_dstream_end(const void* state, const uint8_t** unused, size_t* unused_len) {
    const lz4_dstream_state_t* st = (const lz4_dstream_state_t*)state;
    if (!st->ended) return 0;
    cu_unused_get(&st->unused, unused, unused_len);
    return 1;
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
    .compress_stream_flush     = lz4_cstream_flush,
    .compress_stream_destroy   = lz4_cstream_destroy,
#endif
#ifndef CU_OMIT_DECOMPRESS
    .decompress                = lz4_decompress,
    .decompress_size_hint      = lz4_decompress_size_hint,
    .decompress_stream_create  = lz4_dstream_create,
    .decompress_stream_write   = lz4_dstream_write,
    .decompress_stream_finish  = lz4_dstream_finish,
    .decompress_stream_destroy = lz4_dstream_destroy,
    .decompress_stream_create_single = lz4_dstream_create_single,
    .decompress_stream_end     = lz4_dstream_end,
#endif
};
