/*
 * single_stream.h — shared helpers for single-stream decoding
 * (CU_DSTREAM_SINGLE; see the streaming-decompression comment in
 * compress_utils.h).
 *
 *   - cu_unused_t: the input a decoder received after the end of its stream,
 *     kept for cu_decompress_stream_unused.
 *   - cu_frame_magic_t: the first four bytes of the zstd/LZ4 frame being
 *     decoded. Both decoders report the end of a skippable frame exactly as
 *     the end of a data frame, so single mode tells them apart by magic.
 *
 * Internal header — not part of the public ABI.
 */

#ifndef CU_SINGLE_STREAM_H
#define CU_SINGLE_STREAM_H

#include "algorithm_registry.h"
#include "compress_utils.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t* data;
    size_t   len;
    size_t   cap;
} cu_unused_t;

static inline cu_status_t cu_unused_append(cu_unused_t* u, const uint8_t* src, size_t n) {
    if (n == 0) return CU_OK;
    if (n > SIZE_MAX - u->len) {
        cu_set_last_error("unused input too large");
        return CU_ERR_OOM;
    }
    size_t need = u->len + n;
    if (need > u->cap) {
        uint8_t* p = realloc(u->data, need);
        if (!p) {
            cu_set_last_error("out of memory keeping input after end of stream");
            return CU_ERR_OOM;
        }
        u->data = p;
        u->cap = need;
    }
    memcpy(u->data + u->len, src, n);
    u->len += n;
    return CU_OK;
}

static inline void cu_unused_get(const cu_unused_t* u, const uint8_t** data, size_t* len) {
    if (data) *data = u->data;
    if (len) *len = u->len;
}

static inline void cu_unused_free(cu_unused_t* u) {
    free(u->data);
    u->data = NULL;
    u->len = u->cap = 0;
}

/* zstd and LZ4 share the skippable-frame magic range 0x184D2A50..5F (LE). */
typedef struct {
    uint8_t bytes[4];
    uint8_t n;
} cu_frame_magic_t;

/* Records the first bytes consumed from the current frame. */
static inline void cu_frame_magic_feed(cu_frame_magic_t* m, const uint8_t* src, size_t n) {
    while (m->n < 4 && n > 0) {
        m->bytes[m->n++] = *src++;
        n--;
    }
}

static inline int cu_frame_magic_skippable(const cu_frame_magic_t* m) {
    if (m->n < 4) return 0;
    uint32_t v = (uint32_t)m->bytes[0] | ((uint32_t)m->bytes[1] << 8)
               | ((uint32_t)m->bytes[2] << 16) | ((uint32_t)m->bytes[3] << 24);
    return (v & 0xFFFFFFF0u) == 0x184D2A50u;
}

#endif  /* CU_SINGLE_STREAM_H */
