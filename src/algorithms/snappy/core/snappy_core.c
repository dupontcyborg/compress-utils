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
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#  include <emmintrin.h>
#endif

/* SC_UNPRED marks a condition as unpredictable, which steers the compiler to a
 * conditional move rather than a branch (used where the choice is literal
 * versus copy). */
#if defined(__GNUC__) || defined(__clang__)
#  define SC_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define SC_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define SC_INLINE      static inline __attribute__((always_inline))
#  if defined(__clang__)
#    define SC_UNPRED(x) __builtin_unpredictable(!!(x))
#  elif __GNUC__ >= 9
#    define SC_UNPRED(x) __builtin_expect_with_probability(!!(x), 1, 0.5)
#  else
#    define SC_UNPRED(x) (x)
#  endif
#  define SC_NOINLINE    static __attribute__((noinline))
#else
#  define SC_LIKELY(x)   (x)
#  define SC_UNLIKELY(x) (x)
#  define SC_UNPRED(x)   (x)
#  if defined(_MSC_VER)
#    define SC_INLINE    static __forceinline
#  else
#    define SC_INLINE    static __inline
#  endif
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

/* Per-tag table for the branchless loop, one 64-bit entry per tag byte:
 *   bits  0-7   element length: 1..64, or 65 for a tag the loop does not handle
 *               (literals over 60 bytes, 4-byte-offset copies)
 *   bits  8-18  addend for the offset: copy-1 offset bits 8-10 (as a value),
 *               the literal length for literals, 0 otherwise
 *   bits 32-47  mask for the offset bytes after the tag (0xFF for copy-1, 0xFFFF
 *               for copy-2, 0 otherwise)
 * So offset = (next two bytes & mask) + addend, and "len <= offset <= bytes
 * already written" is the one test that accepts every element sc_step handles:
 * a literal has offset == len, and an unhandled tag has offset 0 < 65. */
#define SC_ENT_(len, add, mask) ((uint64_t)(len) | ((uint64_t)(add) << 8) | ((uint64_t)(mask) << 32))
#define SC_ENT(c)                                                           \
    (((c) & 3) == 0                                                         \
        ? (((c) >> 2) < 60 ? SC_ENT_(((c) >> 2) + 1, ((c) >> 2) + 1, 0)     \
                           : SC_ENT_(65, 0, 0))                             \
        : ((c) & 3) == 1                                                    \
            ? SC_ENT_((((c) >> 2) & 7) + 4, (((c) >> 5) & 7) << 8, 0xFF)    \
            : ((c) & 3) == 2 ? SC_ENT_(((c) >> 2) + 1, 0, 0xFFFF)           \
                             : SC_ENT_(65, 0, 0))
#define SC_R4(c)  SC_ENT(c), SC_ENT((c) + 1), SC_ENT((c) + 2), SC_ENT((c) + 3)
#define SC_R16(c) SC_R4(c), SC_R4((c) + 4), SC_R4((c) + 8), SC_R4((c) + 12)
#define SC_R64(c) SC_R16(c), SC_R16((c) + 16), SC_R16((c) + 32), SC_R16((c) + 48)
static const uint64_t k_tag[256] = {
    SC_R64(0), SC_R64(64), SC_R64(128), SC_R64(192)
};

/* Tier 1 slack. Input: a literal reads 64 bytes at ip+1, and the next tag is
 * read at most 61 bytes ahead. Output: every element stores 64 bytes (and an
 * element handed to decode_element needs SC_OUT_SLACK). Both limits are checked
 * before every element. */
#define SC_IN_SLACK1  66
#define SC_OUT_SLACK1 80

/* One branchless element: literals and copies are handled uniformly (a literal
 * "copies" from ip + 1, a copy from op - offset), with a fixed 64-byte store.
 * Returns 0, with *pip, *pop and *pc untouched, for an element this path does
 * not handle (unhandled tags, or a copy whose offset is shorter than its length
 * or reaches before the output start); the caller then runs it through
 * decode_element. The caller guarantees SC_IN_SLACK1 input bytes and
 * SC_OUT_SLACK1 output bytes, and at least 65 bytes already written. */
SC_INLINE int sc_step(const uint8_t** pip, uint8_t** pop, size_t* pc, const uint8_t* out) {
    const uint8_t* ip = *pip;
    uint8_t* op = *pop;
    size_t c = *pc;
    uint64_t e = k_tag[c];
    size_t len = e & 0xFF;
    size_t t = c & 3;
    size_t n = (size_t)sc_ld16(ip + 1);
    size_t offset = (n & (size_t)(e >> 32)) + ((e >> 8) & 0x7FF);
    size_t adv_lit = 2 + (c >> 2);
    size_t adv_copy = 1 + t;
    const uint8_t* src = t == 0 ? ip + 1 : op - offset;
    /* len <= offset <= op - out. At least 65 bytes are written, so the
     * subtraction on the right cannot wrap, and one that wraps on the left
     * (offset < len) is larger than anything on the right. */
    if (SC_UNLIKELY(offset - len > (size_t)(op - out) - len)) return 0;
    /* A copy with offset < 16 reads bytes it is about to write, so these must
     * be load-then-store (only the first len bytes of the 64 are meaningful;
     * the rest is overwritten later). */
    sc_copy16_ov(op, src);
    sc_copy16_ov(op + 16, src + 16);
    sc_copy16_ov(op + 32, src + 32);
    sc_copy16_ov(op + 48, src + 48);
    /* The tag after this element is adv_lit bytes on for a literal (its data
     * follows the tag byte) and adv_copy for a copy. A branch on
     * literal-versus-copy mispredicts constantly on text, so this has to be a
     * conditional move. Selecting the distance and loading the next tag once
     * saves instructions, which wins on x86 and on gcc (older gcc turns this
     * select into a branch if it is moved above the check). clang on aarch64
     * (Apple Silicon, Neoverse) is latency-bound on this chain and does better
     * loading both candidates ahead of the select. */
#if defined(__clang__) && defined(__aarch64__)
    size_t c_lit = ((const volatile uint8_t*)ip)[adv_lit];
    size_t c_copy = ((const volatile uint8_t*)ip)[adv_copy];
    *pop = op + len;
    *pip = t == 0 ? ip + adv_lit : ip + adv_copy;
    *pc = t == 0 ? c_lit : c_copy;
#else
    size_t adv = SC_UNPRED(t == 0) ? adv_lit : adv_copy;
    *pop = op + len;
    *pip = ip + adv;
    *pc = ip[adv];
#endif
    return 1;
}

/* The general path for an element sc_step declined, then the next tag byte for
 * the loop to resume with. Past the end of the input there is no next tag; the
 * loops' bounds end before that and the value is unused. */
SC_INLINE snappy_core_status_t sc_slow(
    const uint8_t** pip, uint8_t** pop, size_t* pc,
    const uint8_t* ip_end, uint8_t* op_end, const uint8_t* out
) {
    snappy_core_status_t r = decode_element(pip, pop, ip_end, op_end, out);
    *pc = *pip < ip_end ? **pip : 0;
    return r;
}

/* Tier 1: runs sc_step for as long as the slack lasts. The bounds are fixed
 * pointers (an element only moves ip and op forward), so an element handed to
 * decode_element does not disturb the loop. */
static snappy_core_status_t decode_tier1(
    const uint8_t** pip, uint8_t** pop,
    const uint8_t* ip_end, uint8_t* op_end, const uint8_t* out
) {
    const uint8_t* ip = *pip;
    uint8_t* op = *pop;
    const uint8_t* lim = (size_t)(ip_end - ip) > SC_IN_SLACK1 ? ip_end - SC_IN_SLACK1 : ip;
    uint8_t* olim = (size_t)(op_end - op) > SC_OUT_SLACK1 ? op_end - SC_OUT_SLACK1 : op;
    /* sc_step needs 65 bytes already written; get them the general way. */
    while (ip < lim && op < olim && (size_t)(op - out) < 65) {
        snappy_core_status_t r = decode_element(&ip, &op, ip_end, op_end, out);
        if (r != SNAPPY_CORE_OK) return r;
    }
    size_t c = ip < lim ? *ip : 0;
    while (ip < lim && op < olim) {
        if (SC_LIKELY(sc_step(&ip, &op, &c, out))) continue;
        snappy_core_status_t r = sc_slow(&ip, &op, &c, ip_end, op_end, out);
        if (r != SNAPPY_CORE_OK) return r;
    }
    *pip = ip;
    *pop = op;
    return SNAPPY_CORE_OK;
}

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

    st = decode_tier1(&ip, &op, ip_end, op_end, out);
    if (st != SNAPPY_CORE_OK) return st;

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

/* Hash table index of 4 input bytes: the high bits of a multiplicative hash.
 * The same function on every target, so the compressed bytes do not depend on
 * the build. (A hardware CRC32C hash was measured and gained at most 4%.) */
SC_INLINE uint32_t sc_hash(uint32_t v, unsigned shift) {
    return (v * 0x1E35A7BDu) >> shift;
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

/* Probes the 16 positions ip .. ip + 15 against the hash table (four 64-bit
 * loads feed sixteen hashes), indexing each as it goes. Returns the index of
 * the first one whose 4 bytes equal those at its table candidate, with the
 * candidate in *cand, or -1. `preload` is the 4 bytes at ip. Written out as
 * macros so that every compiler unrolls it the same way. */
#define SC_PROBE(i, dword)                                                    \
    do {                                                                      \
        uint32_t d_ = (dword);                                                \
        uint16_t* entry_ = &table[sc_hash(d_, shift)];                        \
        const uint8_t* cand_ = base_ip + *entry_;                             \
        *entry_ = (uint16_t)(delta + (i));                                    \
        if (SC_UNLIKELY(sc_ld32(cand_) == d_)) {                              \
            *cand = cand_;                                                    \
            return (i);                                                       \
        }                                                                     \
    } while (0)
#define SC_PROBE4(j, data)                                                    \
    SC_PROBE(4 * (j), (uint32_t)(data));                                      \
    SC_PROBE(4 * (j) + 1, (uint32_t)((data) >> 8));                           \
    SC_PROBE(4 * (j) + 2, (uint32_t)((data) >> 16));                          \
    SC_PROBE(4 * (j) + 3, (uint32_t)((data) >> 24))
SC_INLINE int probe16(
    uint16_t* table, unsigned shift, const uint8_t* base_ip,
    const uint8_t* ip, uint32_t preload, const uint8_t** cand
) {
    size_t delta = (size_t)(ip - base_ip);
    uint64_t data = sc_ld64(ip);
    SC_PROBE(0, preload);
    SC_PROBE(1, (uint32_t)(data >> 8));
    SC_PROBE(2, (uint32_t)(data >> 16));
    SC_PROBE(3, (uint32_t)(data >> 24));
    data = sc_ld64(ip + 4);
    SC_PROBE4(1, data);
    data = sc_ld64(ip + 8);
    SC_PROBE4(2, data);
    data = sc_ld64(ip + 12);
    SC_PROBE4(3, data);
    return -1;
}

/* Writes whatever input is left after the last match as the final literal. */
SC_INLINE uint8_t* emit_tail(uint8_t* op, const uint8_t* next_emit, const uint8_t* ip_end) {
    if (next_emit < ip_end) {
        op = emit_literal(op, next_emit, (size_t)(ip_end - next_emit), 0);
    }
    return op;
}

/* Compresses one fragment of up to 64 KiB; returns the new output pointer.
 *
 * Match search: the first 16 positions after a match are probed one by one
 * (four 64-bit loads feed sixteen hashes); after that the probe step grows
 * geometrically with the distance since the last match, so incompressible
 * data is skipped through quickly while compressible data keeps a dense
 * search. Once a probe finds a 4-byte match it is extended and emitted, and
 * copies keep being emitted for as long as the bytes right after one also
 * match. */
static uint8_t* compress_fragment(
    const uint8_t* input, size_t input_size, uint8_t* op,
    uint16_t* table, unsigned table_bits
) {
    const unsigned shift = 32 - table_bits;
    const uint8_t* const ip_end = input + input_size;
    const uint8_t* const base_ip = input;
    const uint8_t* ip = input;
    const uint8_t* next_emit = input;

    memset(table, 0, ((size_t)1 << table_bits) * sizeof(uint16_t));

    if (input_size < SC_INPUT_MARGIN) return emit_tail(op, next_emit, ip_end);

    const uint8_t* const ip_limit = ip_end - SC_INPUT_MARGIN;
    uint32_t preload = sc_ld32(ip + 1);
    for (;;) {
        const uint8_t* candidate;
        uint64_t data;
        uint32_t skip = 32;
        int found = 0;

        next_emit = ip++;
        data = sc_ld64(ip);

        if (ip_limit - ip >= 16) {
            int i = probe16(table, shift, base_ip, ip, preload, &candidate);
            if (i >= 0) {
                /* i + 1 literal bytes, at most 16. */
                *op = (uint8_t)(i << 2);
                sc_copy16(op + 1, next_emit);
                ip += i;
                op += i + 2;
                found = 1;
            } else {
                ip += 16;
                skip += 16;
                data = sc_ld64(ip);
            }
        }
        if (!found) {
            for (;;) {
                uint16_t* entry = &table[sc_hash((uint32_t)data, shift)];
                uint32_t step = skip >> 5;
                const uint8_t* next_ip = ip + step;
                skip += step;
                if (SC_UNLIKELY(next_ip > ip_limit)) return emit_tail(op, next_emit, ip_end);
                candidate = base_ip + *entry;
                *entry = (uint16_t)(ip - base_ip);
                if (SC_UNLIKELY((uint32_t)data == sc_ld32(candidate))) break;
                data = sc_ld32(next_ip);
                ip = next_ip;
            }
            op = emit_literal(op, next_emit, (size_t)(ip - next_emit), 1);
        }

        /* A 4-byte match at ip: extend it, emit it, and index the positions
         * around its end. */
        do {
            const uint8_t* base = ip;
            const uint8_t* s2 = ip + 4;
            size_t matched;
            uint32_t prev_hash, cur_hash;
            int short_match = 0;
            uint64_t w0 = 0, w1 = 0;
            unsigned bits = 0;
            if (SC_LIKELY(s2 <= ip_end - 16)) {
                uint64_t a1 = sc_ld64(candidate + 4);
                uint64_t a2 = sc_ld64(s2);
                if (SC_LIKELY(a1 != a2)) {
                    /* Match under 12 bytes. The 8 bytes at the new ip - 1 are
                     * assembled from two loads that don't depend on the match
                     * length (only a shift does), which keeps the loads off
                     * the critical chain from candidate to the next
                     * candidate. */
                    short_match = 1;
                    bits = sc_ctz64(a1 ^ a2) & ~7u;
                    w0 = sc_ld64(s2 - 1);
                    w1 = sc_ld64(s2 + 7);
                }
            }
            if (short_match) {
                matched = 4 + (bits >> 3);
                ip = s2 + (bits >> 3);
                data = (w0 >> bits) | ((w1 << 1) << (63 - bits));
                op = emit_copy(op, (size_t)(base - candidate), matched, 1);
            } else {
                matched = 4 + sc_match_len(candidate + 4, s2, ip_end);
                ip += matched;
                op = emit_copy(op, (size_t)(base - candidate), matched, matched < 12);
                if (SC_LIKELY(ip < ip_limit)) data = sc_ld64(ip - 1);
            }
            next_emit = ip;
            if (SC_UNLIKELY(ip >= ip_limit)) return emit_tail(op, next_emit, ip_end);
            /* Index the position before ip, then test the one at ip. */
            prev_hash = sc_hash((uint32_t)data, shift);
            table[prev_hash] = (uint16_t)(ip - base_ip - 1);
            cur_hash = sc_hash((uint32_t)(data >> 8), shift);
            candidate = base_ip + table[cur_hash];
            table[cur_hash] = (uint16_t)(ip - base_ip);
        } while ((uint32_t)(data >> 8) == sc_ld32(candidate));

        preload = (uint32_t)(data >> 16);
    }
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
