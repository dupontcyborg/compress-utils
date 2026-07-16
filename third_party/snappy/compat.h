/*
 * compat.h — portable userspace shim for andikleen/snappy-c's snappy.c.
 *
 * REPLACES the upstream compat.h, which is Linux-kernel-derived and GCC/Clang-
 * only (it uses `typeof`, statement-expressions `({...})`, `__builtin_expect`,
 * and <endian.h>). This version is written to compile on every toolchain
 * compress-utils targets — GCC, Clang, zig cc (wasm32), and MSVC — using only
 * C11 (`_Generic`) + memcpy for unaligned access. snappy.c is vendored
 * unmodified; only this shim changed.
 *
 * Scope: the non-scatter-gather (SG undefined) build path, which is all the
 * compress-utils vtable uses. All targets are little-endian (x86_64, aarch64,
 * wasm32); a byte-swap fallback is provided for big-endian for correctness.
 */
#ifndef CU_SNAPPY_COMPAT_H
#define CU_SNAPPY_COMPAT_H

#include <assert.h>
#include <errno.h>   /* snappy.c returns -EIO / -ENOMEM */
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* snappy.c uses ssize_t (POSIX). It comes from <sys/types.h> everywhere we
 * build except MSVC, where it must be typedef'd. (macOS happened to pull it in
 * transitively; wasi-libc does not, so include it explicitly.) */
#if defined(_MSC_VER)
#  include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#else
#  include <sys/types.h>
#endif

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* ---- unaligned load/store via memcpy (no typeof / statement-expressions) ----
 * snappy.c only ever calls these through UNALIGNED_LOAD/STORE{16,32,64}, which
 * cast the pointer to a u16, u32 or u64 pointer first. We dispatch on
 * `sizeof(*(p))` (a compile-time constant) rather than C11 `_Generic`, because
 * `_Generic` requires C11 mode — MSVC's default C mode (used by the Rust `cc`
 * crate, which doesn't pass /std:c11) rejects it. The `sizeof` ternary works in
 * any C mode (C89+); the untaken branch is valid but never evaluated. */
static inline u16 cu_ld16(const void *p) { u16 v; memcpy(&v, p, 2); return v; }
static inline u32 cu_ld32(const void *p) { u32 v; memcpy(&v, p, 4); return v; }
static inline u64 cu_ld64(const void *p) { u64 v; memcpy(&v, p, 8); return v; }
static inline void cu_st16(void *p, u16 v) { memcpy(p, &v, 2); }
static inline void cu_st32(void *p, u32 v) { memcpy(p, &v, 4); }
static inline void cu_st64(void *p, u64 v) { memcpy(p, &v, 8); }

#define get_unaligned(p) \
    (sizeof(*(p)) == 2 ? (u32)cu_ld16((const void *)(p)) : cu_ld32((const void *)(p)))
#define get_unaligned64(p) cu_ld64((const void *)(p))
#define put_unaligned(v, p) \
    (sizeof(*(p)) == 2 ? cu_st16((void *)(p), (u16)(v)) : cu_st32((void *)(p), (u32)(v)))
#define put_unaligned64(v, p) cu_st64((void *)(p), (u64)(v))

/* ---- little-endian helpers ----
 *
 * CRITICAL: snappy.c gates its byte-extraction (`is_little_endian()`,
 * `GetUint32AtOffset`) and its fast match-finder on the macro `__LITTLE_ENDIAN__`.
 * clang predefines it on LE targets, but GCC and MSVC do NOT — and if it's
 * absent, `is_little_endian()` returns false and the encoder uses the big-endian
 * shift on a little-endian machine, producing a corrupt match finder (false
 * matches → output the reference decoder rejects). Upstream's compat.h defined
 * it via <endian.h>; we define it portably here. Do NOT remove. */
#if !defined(__LITTLE_ENDIAN__)
#  if !defined(__BYTE_ORDER__) || (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#    define __LITTLE_ENDIAN__ 1  /* GCC/MSVC LE targets; clang already sets it */
#  endif
#endif

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
static inline u32 cu_le32toh(u32 x) {
    return ((x & 0xFFu) << 24) | ((x & 0xFF00u) << 8) |
           ((x >> 8) & 0xFF00u) | ((x >> 24) & 0xFFu);
}
static inline u16 cu_htole16(u16 x) { return (u16)((x << 8) | (x >> 8)); }
#else
static inline u32 cu_le32toh(u32 x) { return x; }
static inline u16 cu_htole16(u16 x) { return x; }
#endif
#define le32toh(x) cu_le32toh(x)
#define htole16(x) cu_htole16(x)
#define get_unaligned_le32(p)     (le32toh(get_unaligned((const u32 *)(p))))
#define put_unaligned_le16(v, p)  (put_unaligned(htole16((u16)(v)), (u16 *)(p)))

/* ---- misc kernel-isms snappy.c expects ---- */
#define BUG_ON(x)        assert(!(x))
#define vmalloc(x)       malloc(x)
#define vfree(x)         free(x)
#define EXPORT_SYMBOL(x)
#define ARRAY_SIZE(x)    (sizeof(x) / sizeof(*(x)))
#define min_t(t, x, y)   ((t)(x) < (t)(y) ? (t)(x) : (t)(y))
#define max_t(t, x, y)   ((t)(x) > (t)(y) ? (t)(x) : (t)(y))
/* Must be usable in #if — so a literal, not sizeof(). */
#if defined(__SIZEOF_LONG__)
#  define BITS_PER_LONG (__SIZEOF_LONG__ * 8)
#elif defined(_WIN32)
#  define BITS_PER_LONG 32   /* MSVC: long is 32-bit even on 64-bit Windows */
#else
#  define BITS_PER_LONG 64
#endif

#if defined(__GNUC__) || defined(__clang__)
#  define likely(x)   __builtin_expect(!!(x), 1)
#  define unlikely(x) __builtin_expect(!!(x), 0)
#else
#  define likely(x)   (x)
#  define unlikely(x) (x)
#endif

/* MSVC has no __builtin_clz/ctz/ctzll — snappy.c calls them directly. Map them
 * to the equivalent bit-scan intrinsics (only when not a GCC/Clang builtin). */
#if defined(_MSC_VER) && !defined(__clang__)
#  include <intrin.h>
static inline int cu_builtin_clz(unsigned x) { unsigned long i; _BitScanReverse(&i, x); return 31 - (int)i; }
static inline int cu_builtin_ctz(unsigned x) { unsigned long i; _BitScanForward(&i, x); return (int)i; }
static inline int cu_builtin_ctzll(unsigned long long x) { unsigned long i; _BitScanForward64(&i, x); return (int)i; }
#  define __builtin_clz(x)   cu_builtin_clz(x)
#  define __builtin_ctz(x)   cu_builtin_ctz(x)
#  define __builtin_ctzll(x) cu_builtin_ctzll(x)
#endif

#endif /* CU_SNAPPY_COMPAT_H */
