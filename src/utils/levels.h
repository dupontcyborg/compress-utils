/*
 * levels.h — shared compression-level helpers.
 *
 * Each algorithm maps the user-facing level (1..10) to its native range.
 * Most algorithms use one of two patterns:
 *
 *   - clamp: user 1..10 truncated/clamped to native min..max
 *     (used by zlib, bz2)
 *   - scale: user 1..10 linearly scaled to 1..native_max
 *     (used by zstd)
 *
 * Algorithms with non-uniform mappings (brotli has a special case for
 * user=10, lz4 splits fast vs HC modes) handle their own conversion.
 *
 * Internal header — not part of the public ABI.
 */

#ifndef CU_LEVELS_H
#define CU_LEVELS_H

static inline int cu_clamp_level(int user, int native_min, int native_max) {
    if (user < native_min) return native_min;
    if (user > native_max) return native_max;
    return user;
}

/* Linearly scale user 1..10 -> 1..native_max. Always returns ≥1. */
static inline int cu_scale_level(int user, int native_max) {
    int n = (user * native_max) / 10;
    if (n < 1) return 1;
    if (n > native_max) return native_max;
    return n;
}

#endif  /* CU_LEVELS_H */
