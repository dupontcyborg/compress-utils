/*
 * snappy_core.c - Snappy raw block format: compressor and decompressor.
 *
 * Original implementation of the frozen Snappy format
 * (https://github.com/google/snappy/blob/main/format_description.txt). The
 * design (tag-driven decode loop, 16-byte literal and copy fast paths, copy
 * pattern expansion for short offsets, 64 KiB fragments with a 16-bit hash
 * table in the encoder) follows the approach of Google's snappy; see
 * PROVENANCE.md. BSD notices from Google and Intel are retained in LICENSE.
 *
 * Decoder structure, three tiers, each running for as long as its slack lasts:
 *   1. a branchless loop that treats literals and copies uniformly and stores a
 *      fixed 64 bytes per element;
 *   2. a branchy loop with 16-byte chunk copies and short-offset expansion;
 *   3. a byte-exact checked loop for the tail.
 * Tiers 1 and 2 never read or write outside the buffers: their slack constants
 * cover the widest access any one element can make, and the only
 * data-dependent bounds (long literal lengths, copy offsets) are checked
 * explicitly. Anything a tier does not handle falls through to the next one.
 */

#include "snappy_core.h"

#include <stdlib.h>
#include <string.h>

#if defined(_MSC_VER) && !defined(__clang__)
#  include <intrin.h>
#endif

#if defined(__ARM_NEON) || defined(__aarch64__)
#  include <arm_neon.h>
#  if defined(__ARM_FEATURE_CRC32)
#    include <arm_acle.h>
#    define SC_HAVE_CRC32 1
#  endif
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  include <emmintrin.h>
#  if defined(__SSE4_2__)
#    include <nmmintrin.h>
#    define SC_HAVE_CRC32 1
#  endif
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define SC_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define SC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define SC_INLINE      static inline __attribute__((always_inline))
#  define SC_NOINLINE    static __attribute__((noinline))
#else
#  define SC_LIKELY(x)   (x)
#  define SC_UNLIKELY(x) (x)
#  define SC_INLINE      static __inline
#  define SC_NOINLINE    static
#endif

#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && \
    __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define SC_BIG_ENDIAN 1
#else
#  define SC_BIG_ENDIAN 0
#endif

#if SC_BIG_ENDIAN
SC_INLINE uint32_t sc_bswap32(uint32_t v) {
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8) | ((v >> 8) & 0xFF00u) | (v >> 24);
}
#endif

SC_INLINE uint64_t sc_ld64(const uint8_t* p) {
    uint64_t v;
    memcpy(&v, p, 8);
#if SC_BIG_ENDIAN
    v = ((uint64_t)sc_bswap32((uint32_t)v) << 32) | sc_bswap32((uint32_t)(v >> 32));
#endif
    return v;
}

SC_INLINE uint32_t sc_ld16(const uint8_t* p) {
    uint16_t v;
    memcpy(&v, p, 2);
#if SC_BIG_ENDIAN
    v = (uint16_t)((v << 8) | (v >> 8));
#endif
    return v;
}

SC_INLINE uint32_t sc_ld32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
#if SC_BIG_ENDIAN
    v = sc_bswap32(v);
#endif
    return v;
}

/* 16-byte copy of non-overlapping ranges; one vector load and one store. */
SC_INLINE void sc_copy16(uint8_t* dst, const uint8_t* src) {
    memcpy(dst, src, 16);
}

/* 16-byte copy that is well defined (load all, then store all) even when the
 * ranges overlap, as the short-offset expansion needs. */
SC_INLINE void sc_copy16_ov(uint8_t* dst, const uint8_t* src) {
#if defined(__ARM_NEON) || defined(__aarch64__)
    vst1q_u8(dst, vld1q_u8(src));
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    _mm_storeu_si128((__m128i*)dst, _mm_loadu_si128((const __m128i*)src));
#else
    uint8_t t[16];
    memcpy(t, src, 16);
    memcpy(dst, t, 16);
#endif
}

/* Trailing bytes after the tag byte, indexed by tag & 3 (literals handle their
 * own length bytes), and the mask that keeps that many low bytes. */
static const uint8_t  k_trailer[4] = {0, 1, 2, 4};
static const uint32_t k_mask[5]    = {0, 0xFFu, 0xFFFFu, 0xFFFFFFu, 0xFFFFFFFFu};

/* Tier 2 slack. Input: the widest read from ip is a 16-byte literal load at
 * ip+1 (17 bytes); copy tags and literal length bytes read at most 5. Output:
 * element lengths are <= 64 for everything this tier writes blindly, and the
 * widest overshoot is a copy of 64 bytes by 16-byte chunks starting from a
 * short-offset expansion (< 80). */
#define SC_IN_SLACK  17
#define SC_OUT_SLACK 80

snappy_core_status_t snappy_core_uncompressed_length(
    const uint8_t* in, size_t in_len, size_t* out_len
) {
    uint32_t result = 0;
    for (unsigned i = 0; i < 5; i++) {
        if (i >= in_len) return SNAPPY_CORE_TRUNCATED;
        uint32_t c = in[i];
        uint32_t val = c & 0x7F;
        if (i == 4 && val > 0x0F) return SNAPPY_CORE_CORRUPT;  /* > 32 bits */
        result |= val << (7 * i);
        if (c < 0x80) {
            *out_len = result;
            return SNAPPY_CORE_OK;
        }
    }
    return SNAPPY_CORE_CORRUPT;  /* continuation bit on the 5th byte */
}

/* Byte-exact decode of whatever remains; every access is checked. */
SC_NOINLINE snappy_core_status_t decode_checked(
    const uint8_t* ip, const uint8_t* ip_end,
    uint8_t* op, uint8_t* op_end, const uint8_t* out_start
) {
    while (ip < ip_end) {
        uint32_t tag = *ip++;
        if ((tag & 3) == 0) {
            uint64_t len = tag >> 2;
            if (len >= 60) {
                unsigned extra = (unsigned)len - 59;
                if ((size_t)(ip_end - ip) < extra) return SNAPPY_CORE_TRUNCATED;
                len = 0;
                for (unsigned k = 0; k < extra; k++) len |= (uint64_t)ip[k] << (8 * k);
                ip += extra;
            }
            len += 1;
            if ((uint64_t)(ip_end - ip) < len) return SNAPPY_CORE_TRUNCATED;
            if ((uint64_t)(op_end - op) < len) return SNAPPY_CORE_CORRUPT;
            memcpy(op, ip, (size_t)len);
            ip += len;
            op += len;
        } else {
            unsigned t = tag & 3;
            unsigned nb = k_trailer[t];
            if ((size_t)(ip_end - ip) < nb) return SNAPPY_CORE_TRUNCATED;
            uint32_t trailer = 0;
            for (unsigned k = 0; k < nb; k++) trailer |= (uint32_t)ip[k] << (8 * k);
            ip += nb;
            size_t len, offset;
            if (t == 1) {
                len = ((tag >> 2) & 7) + 4;
                offset = ((size_t)(tag & 0xE0) << 3) | trailer;
            } else {
                len = (tag >> 2) + 1;
                offset = trailer;
            }
            if (offset - 1 >= (size_t)(op - out_start)) return SNAPPY_CORE_CORRUPT;
            if (len > (size_t)(op_end - op)) return SNAPPY_CORE_CORRUPT;
            const uint8_t* src = op - offset;
            if (offset >= len) {
                memcpy(op, src, len);
            } else {
                for (size_t k = 0; k < len; k++) op[k] = src[k];
            }
            op += len;
        }
    }
    return op == op_end ? SNAPPY_CORE_OK : SNAPPY_CORE_CORRUPT;
}

/* One element with the general (branchy) logic. Requires at least SC_IN_SLACK
 * bytes of input and SC_OUT_SLACK bytes of output ahead of ip and op, which the
 * callers guarantee; the data-dependent bounds are checked here. */
SC_INLINE snappy_core_status_t decode_element(
    const uint8_t** pip, uint8_t** pop,
    const uint8_t* ip_end, uint8_t* op_end, const uint8_t* out_start
) {
    const uint8_t* ip = *pip;
    uint8_t* op = *pop;
    uint32_t tag = *ip;
    if ((tag & 3) == 0) {
        size_t len = (tag >> 2) + 1;
        if (SC_LIKELY(len <= 16)) {
            sc_copy16(op, ip + 1);
            *pip = ip + len + 1;
            *pop = op + len;
            return SNAPPY_CORE_OK;
        }
        if (len <= 60) {
            ip++;
            size_t avail = (size_t)(ip_end - ip);
            if (SC_UNLIKELY(avail < len)) return SNAPPY_CORE_TRUNCATED;
            if (avail >= ((len + 15) & ~(size_t)15)) {
                const uint8_t* s = ip;
                uint8_t* d = op;
                uint8_t* e = op + len;
                do { sc_copy16(d, s); d += 16; s += 16; } while (d < e);
            } else {
                memcpy(op, ip, len);
            }
            *pip = ip + len;
            *pop = op + len;
            return SNAPPY_CORE_OK;
        }
        /* Long literal: 1-4 length bytes follow the tag. */
        unsigned extra = (tag >> 2) - 59;
        uint64_t llen = (uint64_t)(sc_ld32(ip + 1) & k_mask[extra]) + 1;
        ip += 1 + extra;
        if (SC_UNLIKELY((uint64_t)(ip_end - ip) < llen)) return SNAPPY_CORE_TRUNCATED;
        if (SC_UNLIKELY((uint64_t)(op_end - op) < llen)) return SNAPPY_CORE_CORRUPT;
        memcpy(op, ip, (size_t)llen);
        *pip = ip + llen;
        *pop = op + llen;
        return SNAPPY_CORE_OK;
    }

    unsigned t = tag & 3;
    uint32_t trailer = sc_ld32(ip + 1) & k_mask[k_trailer[t]];
    size_t len, offset;
    if (t == 1) {
        len = ((tag >> 2) & 7) + 4;
        offset = ((size_t)(tag & 0xE0) << 3) | trailer;
    } else {
        len = (tag >> 2) + 1;
        offset = trailer;
    }
    *pip = ip + 1 + k_trailer[t];
    if (SC_UNLIKELY(offset - 1 >= (size_t)(op - out_start))) return SNAPPY_CORE_CORRUPT;

    /* len <= 64 and at least SC_OUT_SLACK bytes of output remain. */
    const uint8_t* s = op - offset;
    uint8_t* d = op;
    uint8_t* e = op + len;
    if (offset >= 16) {
        do { sc_copy16(d, s); d += 16; s += 16; } while (d < e);
    } else {
        /* Short offset: copy the period at doubling distances until it spans
         * a chunk, then continue in 16-byte chunks at that distance (a
         * multiple of the period, so the data stays correct). */
        while ((size_t)(d - s) < 16) {
            sc_copy16_ov(d, s);
            d += d - s;
        }
        while (d < e) {
            sc_copy16(d, s);
            d += 16;
            s += 16;
        }
    }
    *pop = op + len;
    return SNAPPY_CORE_OK;
}

/* Per-tag table for the branchless loop: element length (1..64) in bits 0-7,
 * copy-1 offset bits 8-10 in bits 8-10, SC_T_RARE for elements it does not
 * handle (literals longer than 60 bytes, 4-byte-offset copies). */
#define SC_T_RARE 0x8000u
#define SC_ENT(c)                                                           \
    ((uint16_t)(((c) & 3) == 0                                              \
        ? (((c) >> 2) < 60 ? ((c) >> 2) + 1 : SC_T_RARE)                    \
        : ((c) & 3) == 1                                                    \
            ? ((((c) >> 2) & 7) + 4) | ((((c) >> 5) & 7) << 8)              \
            : ((c) & 3) == 2 ? ((c) >> 2) + 1 : SC_T_RARE))
#define SC_R4(c)  SC_ENT(c), SC_ENT((c) + 1), SC_ENT((c) + 2), SC_ENT((c) + 3)
#define SC_R16(c) SC_R4(c), SC_R4((c) + 4), SC_R4((c) + 8), SC_R4((c) + 12)
#define SC_R64(c) SC_R16(c), SC_R16((c) + 16), SC_R16((c) + 32), SC_R16((c) + 48)
static const uint16_t k_tag[256] = {
    SC_R64(0), SC_R64(64), SC_R64(128), SC_R64(192)
};

/* Tier 1 slack. Input: a literal reads 64 bytes at ip+1, and the next tag is
 * read at most 61 bytes ahead. Output: every element stores 64 bytes (and a
 * rare element handed to decode_element needs SC_OUT_SLACK). The loop runs two
 * elements per pass while there is slack for both (an element advances ip by
 * at most 61 and op by at most 64), then one at a time. */
#define SC_IN_SLACK1  66
#define SC_OUT_SLACK1 80
#define SC_IN_SLACK2  (61 + SC_IN_SLACK1)
#define SC_OUT_SLACK2 (64 + SC_OUT_SLACK1)

snappy_core_status_t snappy_core_decode(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t out_cap, size_t* out_len
) {
    size_t ulen;
    snappy_core_status_t st = snappy_core_uncompressed_length(in, in_len, &ulen);
    if (st != SNAPPY_CORE_OK) return st;
    *out_len = ulen;
    if (ulen > out_cap) return SNAPPY_CORE_OUTPUT_FULL;

    /* Header length: re-walk the varint (1-5 bytes, already validated). */
    const uint8_t* ip = in;
    while (*ip++ & 0x80) {}
    const uint8_t* const ip_end = in + in_len;

    uint8_t* op = out;
    uint8_t* const op_end = out + ulen;

    /* Tier 1: branchless, literals and copies handled uniformly; see the slack
     * constants above. */
    {
        const uint8_t* lim1 = (size_t)(ip_end - ip) > SC_IN_SLACK1 ? ip_end - SC_IN_SLACK1 : ip;
        uint8_t* olim1 = (size_t)(op_end - op) > SC_OUT_SLACK1 ? op_end - SC_OUT_SLACK1 : op;
        size_t c;
        if (ip < lim1 && op < olim1) {
            c = *ip;
        top:
            {
                /* Two elements per pass need twice the slack. */
                const uint8_t* lim2 = (size_t)(ip_end - ip) > SC_IN_SLACK2 ? ip_end - SC_IN_SLACK2 : ip;
                uint8_t* olim2 = (size_t)(op_end - op) > SC_OUT_SLACK2 ? op_end - SC_OUT_SLACK2 : op;
#define SC_STEP()                                                                      \
    do {                                                                               \
        size_t e = k_tag[c];                                                           \
        size_t len = e & 0xFF;                                                         \
        size_t t = c & 3;                                                              \
        size_t is_lit = (t == 0);                                                      \
        size_t n = (size_t)sc_ld16(ip + 1);                                            \
        size_t offset = (n & (t == 1 ? 0xFFu : 0xFFFFu)) + (e & 0x700);                \
        size_t c_lit = ((const volatile uint8_t*)ip)[2 + (c >> 2)];                    \
        size_t c_copy = ((const volatile uint8_t*)ip)[1 + t];                          \
        const uint8_t* ip_lit = ip + 2 + (c >> 2);                                     \
        const uint8_t* ip_copy = ip + 1 + t;                                           \
        const uint8_t* src = is_lit ? ip + 1 : op - offset;                            \
        int ok = ((e & SC_T_RARE) == 0) &                                              \
                 (is_lit | ((offset - 1 < (size_t)(op - out)) & (offset >= len)));     \
        if (SC_UNLIKELY(!ok)) {                                                        \
            snappy_core_status_t r = decode_element(&ip, &op, ip_end, op_end, out);    \
            if (r != SNAPPY_CORE_OK) return r;                                         \
            if (ip >= ip_end) goto tier1_done;                                         \
            c = *ip;                                                                   \
            goto top;                                                                  \
        }                                                                              \
        /* A copy with offset < 16 reads bytes it is about to write, so these must be  \
         * load-then-store (only the first len bytes of the 64 are meaningful; the     \
         * rest is overwritten later). */                                              \
        sc_copy16_ov(op, src);                                                         \
        sc_copy16_ov(op + 16, src + 16);                                               \
        sc_copy16_ov(op + 32, src + 32);                                               \
        sc_copy16_ov(op + 48, src + 48);                                               \
        op += len;                                                                     \
        ip = is_lit ? ip_lit : ip_copy;                                                \
        c = is_lit ? c_lit : c_copy;                                                   \
    } while (0)
                while (ip < lim2 && op < olim2) {
                    SC_STEP();
                    SC_STEP();
                }
                while (ip < lim1 && op < olim1) {
                    SC_STEP();
                }
#undef SC_STEP
            }
        }
    tier1_done:;
    }

    /* Tier 2: branchy, 17 bytes of input and 80 of output slack. */
    {
        const uint8_t* lim = (size_t)(ip_end - ip) > SC_IN_SLACK ? ip_end - SC_IN_SLACK : ip;
        uint8_t* olim = (size_t)(op_end - op) > SC_OUT_SLACK ? op_end - SC_OUT_SLACK : op;
        while (ip < lim && op < olim) {
            snappy_core_status_t r = decode_element(&ip, &op, ip_end, op_end, out);
            if (r != SNAPPY_CORE_OK) return r;
        }
    }

    /* Tier 3: byte-exact tail. */
    return decode_checked(ip, ip_end, op, op_end, out);
}

/* ============================================================================
 * Compressor
 * ============================================================================ */

size_t snappy_core_max_compressed_length(size_t n) {
    return 32 + n + n / 6;
}

/* Number of trailing zero bits of a non-zero value. */
SC_INLINE unsigned sc_ctz64(uint64_t v) {
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctzll(v);
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    unsigned long i;
    _BitScanForward64(&i, v);
    return (unsigned)i;
#else
    unsigned n = 0;
    while (!(v & 1)) { v >>= 1; n++; }
    return n;
#endif
}

/* Hash table index of 4 input bytes. With a hardware CRC32C instruction (ARMv8
 * CRC, SSE4.2, detected at compile time) the low bits of the CRC index the
 * table (all its output bits are good); otherwise the high bits of a
 * multiplicative hash do. */
SC_INLINE uint32_t sc_hash(uint32_t v, unsigned shift, uint32_t mask) {
#if defined(SC_HAVE_CRC32) && (defined(__aarch64__) || defined(__ARM_FEATURE_CRC32))
    (void)shift;
    return __crc32cw(0, v) & mask;
#elif defined(SC_HAVE_CRC32)
    (void)shift;
    return _mm_crc32_u32(0, v) & mask;
#else
    (void)mask;
    return (v * 0x1E35A7BDu) >> shift;
#endif
}

/* Length of the common prefix of a and b, comparing at most up to b_end. The
 * caller guarantees a < b <= b_end. */
SC_INLINE size_t sc_match_len(const uint8_t* a, const uint8_t* b, const uint8_t* b_end) {
    const uint8_t* b0 = b;
    while (b_end - b >= 8) {
        uint64_t x = sc_ld64(a) ^ sc_ld64(b);
        if (x) return (size_t)(b - b0) + (sc_ctz64(x) >> 3);
        a += 8;
        b += 8;
    }
    while (b < b_end && *a == *b) { a++; b++; }
    return (size_t)(b - b0);
}

/* Writes a literal element. With `fast`, the caller guarantees that the input
 * is readable up to the next multiple of 16 bytes past `lit + len` and the
 * output has 16 bytes of slack, so long literals copy in whole 16-byte chunks
 * instead of calling memcpy. */
SC_INLINE uint8_t* emit_literal(uint8_t* op, const uint8_t* lit, size_t len, int fast) {
    size_t n = len - 1;
    if (fast && len <= 16) {
        *op++ = (uint8_t)(n << 2);
        sc_copy16(op, lit);
        return op + len;
    }
    if (n < 60) {
        *op++ = (uint8_t)(n << 2);
    } else {
        uint8_t* tag = op++;
        unsigned count = 0;
        while (n) { *op++ = (uint8_t)n; n >>= 8; count++; }
        *tag = (uint8_t)((59 + count) << 2);
    }
    if (fast) {
        uint8_t* d = op;
        uint8_t* const e = op + len;
        do { sc_copy16(d, lit); d += 16; lit += 16; } while (d < e);
    } else {
        memcpy(op, lit, len);
    }
    return op + len;
}

/* Copy elements. The output has slack, so a 4-byte store may write one byte
 * more than the element needs. */
SC_INLINE void sc_st32(uint8_t* p, uint32_t v) {
#if SC_BIG_ENDIAN
    v = sc_bswap32(v);
#endif
    memcpy(p, &v, 4);
}

/* A copy of 4..11 bytes: copy-1 (2 bytes) when offset < 2048, else copy-2 (3
 * bytes). Branch-free: which one applies is hard to predict. */
SC_INLINE uint8_t* emit_copy_short(uint8_t* op, size_t offset, size_t len) {
    uint32_t u = (uint32_t)((len << 2) + (offset << 8));
    uint32_t copy1 = 1u - (4u << 2) + (uint32_t)((offset >> 3) & 0xE0);
    uint32_t copy2 = 2u - (1u << 2);
    int is1 = offset < 2048;
    u += is1 ? copy1 : copy2;
    sc_st32(op, u);
    return op + (is1 ? 2 : 3);
}

/* One copy-2 element of 1..64 bytes. */
SC_INLINE uint8_t* emit_copy2(uint8_t* op, size_t offset, size_t len) {
    sc_st32(op, 2u + (uint32_t)((len - 1) << 2) + (uint32_t)(offset << 8));
    return op + 3;
}

/* A copy of any length >= 4, split so that every element is >= 4 bytes.
 * `is_short` says len < 12. */
SC_INLINE uint8_t* emit_copy(uint8_t* op, size_t offset, size_t len, int is_short) {
    if (is_short) return emit_copy_short(op, offset, len);
    while (SC_UNLIKELY(len >= 68)) {
        op = emit_copy2(op, offset, 64);
        len -= 64;
    }
    if (len > 64) {
        op = emit_copy2(op, offset, 60);
        len -= 60;
    }
    if (len < 12) return emit_copy_short(op, offset, len);
    return emit_copy2(op, offset, len);
}

/* Fragments are at most 64 KiB so every offset fits a copy-2 element. */
#define SC_FRAGMENT 65536u
#define SC_INPUT_MARGIN 15u
#define SC_MIN_TABLE_BITS 8u
#define SC_MAX_TABLE_BITS 15u
#define SC_STACK_TABLE_BITS 11u

/* Compresses one fragment of up to 64 KiB; returns the new output pointer.
 *
 * Match search: the first 16 positions after a match are probed one by one
 * (four 64-bit loads feed sixteen hashes); after that the probe step grows
 * geometrically with the distance since the last match, so incompressible
 * data is skipped through quickly while compressible data keeps a dense
 * search. */
static uint8_t* compress_fragment(
    const uint8_t* input, size_t input_size, uint8_t* op,
    uint16_t* table, unsigned table_bits
) {
    const unsigned shift = 32 - table_bits;
    const uint32_t mask = ((uint32_t)1 << table_bits) - 1;
    const uint8_t* const ip_end = input + input_size;
    const uint8_t* const base_ip = input;
    const uint8_t* ip = input;
    const uint8_t* next_emit = input;

    memset(table, 0, ((size_t)1 << table_bits) * sizeof(uint16_t));

    if (input_size >= SC_INPUT_MARGIN) {
        const uint8_t* const ip_limit = ip_end - SC_INPUT_MARGIN;
        uint32_t preload = sc_ld32(ip + 1);
        for (;;) {
            const uint8_t* candidate;
            uint64_t data;
            uint32_t skip = 32;

            next_emit = ip++;
            data = sc_ld64(ip);

            if (ip_limit - ip >= 16) {
                size_t delta = (size_t)(ip - base_ip);
                unsigned j, k;
                for (j = 0; j < 4; j++) {
                    for (k = 0; k < 4; k++) {
                        unsigned i = 4 * j + k;
                        uint32_t dword = i == 0 ? preload : (uint32_t)data;
                        uint16_t* entry = &table[sc_hash(dword, shift, mask)];
                        candidate = base_ip + *entry;
                        *entry = (uint16_t)(delta + i);
                        if (SC_UNLIKELY(sc_ld32(candidate) == dword)) {
                            /* i + 1 literal bytes, at most 16. */
                            *op = (uint8_t)(i << 2);
                            sc_copy16(op + 1, next_emit);
                            ip += i;
                            op += i + 2;
                            goto emit_match;
                        }
                        data >>= 8;
                    }
                    data = sc_ld64(ip + 4 * j + 4);
                }
                ip += 16;
                skip += 16;
            }
            for (;;) {
                uint16_t* entry = &table[sc_hash((uint32_t)data, shift, mask)];
                uint32_t step = skip >> 5;
                const uint8_t* next_ip = ip + step;
                skip += step;
                if (SC_UNLIKELY(next_ip > ip_limit)) goto emit_remainder;
                candidate = base_ip + *entry;
                *entry = (uint16_t)(ip - base_ip);
                if (SC_UNLIKELY((uint32_t)data == sc_ld32(candidate))) break;
                data = sc_ld32(next_ip);
                ip = next_ip;
            }

            op = emit_literal(op, next_emit, (size_t)(ip - next_emit), 1);

        emit_match:
            /* A 4-byte match at ip: extend it, emit it, and keep emitting
             * copies for as long as the bytes right after one also match. */
            do {
                const uint8_t* base = ip;
                const uint8_t* s2 = ip + 4;
                size_t matched;
                uint32_t prev_hash, cur_hash;
                if (SC_LIKELY(s2 <= ip_end - 16)) {
                    uint64_t a1 = sc_ld64(candidate + 4);
                    uint64_t a2 = sc_ld64(s2);
                    if (SC_LIKELY(a1 != a2)) {
                        /* Match under 12 bytes. The 8 bytes at the new ip - 1
                         * are assembled from two loads that don't depend on
                         * the match length (only a shift does), which keeps
                         * the loads off the critical chain from candidate to
                         * the next candidate. */
                        unsigned bits = sc_ctz64(a1 ^ a2) & ~7u;
                        uint64_t w0 = sc_ld64(s2 - 1);
                        uint64_t w1 = sc_ld64(s2 + 7);
                        matched = 4 + (bits >> 3);
                        ip = s2 + (bits >> 3);
                        data = (w0 >> bits) | ((w1 << 1) << (63 - bits));
                        op = emit_copy(op, (size_t)(base - candidate), matched, 1);
                        goto copy_done;
                    }
                }
                matched = 4 + sc_match_len(candidate + 4, s2, ip_end);
                ip += matched;
                op = emit_copy(op, (size_t)(base - candidate), matched, matched < 12);
                if (SC_LIKELY(ip < ip_limit)) data = sc_ld64(ip - 1);
            copy_done:
                next_emit = ip;
                if (SC_UNLIKELY(ip >= ip_limit)) goto emit_remainder;
                /* Index the position before ip, then test the one at ip. */
                prev_hash = sc_hash((uint32_t)data, shift, mask);
                table[prev_hash] = (uint16_t)(ip - base_ip - 1);
                cur_hash = sc_hash((uint32_t)(data >> 8), shift, mask);
                candidate = base_ip + table[cur_hash];
                table[cur_hash] = (uint16_t)(ip - base_ip);
            } while ((uint32_t)(data >> 8) == sc_ld32(candidate));

            preload = (uint32_t)(data >> 16);
        }
    }

emit_remainder:
    if (next_emit < ip_end) {
        op = emit_literal(op, next_emit, (size_t)(ip_end - next_emit), 0);
    }
    return op;
}

snappy_core_status_t snappy_core_encode(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t out_cap, size_t* out_len
) {
    if ((uint64_t)in_len > 0xFFFFFFFFu) return SNAPPY_CORE_CORRUPT;
    if (out_cap < snappy_core_max_compressed_length(in_len)) return SNAPPY_CORE_OUTPUT_FULL;

    uint8_t* op = out;
    uint32_t v = (uint32_t)in_len;
    while (v >= 0x80) { *op++ = (uint8_t)(v | 0x80); v >>= 7; }
    *op++ = (uint8_t)v;

    uint16_t stack_table[(size_t)1 << SC_STACK_TABLE_BITS];
    uint16_t* heap_table = NULL;
    size_t left = in_len;
    while (left > 0) {
        size_t frag = left < SC_FRAGMENT ? left : SC_FRAGMENT;
        /* Table sized to the fragment: smallest power of two >= frag within
         * [2^8, 2^15]. */
        unsigned bits = SC_MIN_TABLE_BITS;
        while (bits < SC_MAX_TABLE_BITS && ((size_t)1 << bits) < frag) bits++;
        uint16_t* table = stack_table;
        if (bits > SC_STACK_TABLE_BITS) {
            if (!heap_table) {
                heap_table = (uint16_t*)malloc(((size_t)1 << SC_MAX_TABLE_BITS) * sizeof(uint16_t));
                if (!heap_table) return SNAPPY_CORE_NOMEM;
            }
            table = heap_table;
        }
        op = compress_fragment(in, frag, op, table, bits);
        in += frag;
        left -= frag;
    }
    free(heap_table);
    *out_len = (size_t)(op - out);
    return SNAPPY_CORE_OK;
}
