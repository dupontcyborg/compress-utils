#ifndef THIRD_PARTY_SNAPPY_OPENSOURCE_CMAKE_CONFIG_H_
#define THIRD_PARTY_SNAPPY_OPENSOURCE_CMAKE_CONFIG_H_

/*
 * Portable, hand-authored replacement for snappy's CMake-generated config.h.
 *
 * The upstream build detects these at configure time, which produces a
 * target-specific header. compress-utils vendors ONE header that resolves the
 * same knobs at *compile* time via standard compiler/target macros, so a single
 * committed file is correct for every target we build (x86-64, arm64, wasm32,
 * Windows). Regenerated? No — this file is maintained by hand; vendor-codecs.py
 * never overwrites it. If snappy's config surface changes on a version bump,
 * update this to match snappy-stubs-public.h.in / the upstream config.h.in.
 */

/* Compiler capability macros. Present on GCC/Clang (incl. the wasm and
 * clang-cl frontends); absent on classic MSVC. */
#if defined(__GNUC__) || defined(__clang__)
#define HAVE_ATTRIBUTE_ALWAYS_INLINE 1
#define HAVE_BUILTIN_CTZ 1
#define HAVE_BUILTIN_EXPECT 1
#define HAVE_BUILTIN_PREFETCH 1
#else
#define HAVE_ATTRIBUTE_ALWAYS_INLINE 0
#define HAVE_BUILTIN_CTZ 0
#define HAVE_BUILTIN_EXPECT 0
#define HAVE_BUILTIN_PREFETCH 0
#endif

/* POSIX headers / functions. Three cases: Windows (none), WASI/wasm (no mmap,
 * no sys/resource — wasi-libc lacks them), and generic Unix (all present).
 * These values match what the upstream configure detects on each target. They
 * guard optional fast paths and test-only code, none of which is on the core
 * compress/decompress path we compile. */
#if defined(_WIN32)
#define HAVE_FUNC_MMAP 0
#define HAVE_FUNC_SYSCONF 0
#define HAVE_SYS_MMAN_H 0
#define HAVE_SYS_RESOURCE_H 0
#define HAVE_SYS_TIME_H 0
#define HAVE_SYS_UIO_H 0
#define HAVE_UNISTD_H 0
#define HAVE_WINDOWS_H 1
#elif defined(__wasi__) || defined(__wasm__)
#define HAVE_FUNC_MMAP 0
#define HAVE_FUNC_SYSCONF 1
#define HAVE_SYS_MMAN_H 0
#define HAVE_SYS_RESOURCE_H 0
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_UNISTD_H 1
#define HAVE_WINDOWS_H 0
#else
#define HAVE_FUNC_MMAP 1
#define HAVE_FUNC_SYSCONF 1
#define HAVE_SYS_MMAN_H 1
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_UIO_H 1
#define HAVE_UNISTD_H 1
#define HAVE_WINDOWS_H 0
#endif

/* Optional comparison libraries — used only by snappy's benchmarks/tests,
 * which we do not vendor. Always 0 so the core never links them. */
#define HAVE_LIBZ 0
#define HAVE_LIBLZO2 0
#define HAVE_LIBLZ4 0

/* x86 SIMD: keyed off the compiler's target-feature macros, so this matches
 * whatever ISA baseline the consumer compiles for. */
#if defined(__SSSE3__)
#define SNAPPY_HAVE_SSSE3 1
#else
#define SNAPPY_HAVE_SSSE3 0
#endif

#if defined(__SSE4_2__)
#define SNAPPY_HAVE_X86_CRC32 1
#else
#define SNAPPY_HAVE_X86_CRC32 0
#endif

#if defined(__BMI2__)
#define SNAPPY_HAVE_BMI2 1
#else
#define SNAPPY_HAVE_BMI2 0
#endif

/* ARM NEON + optional CRC32 acceleration. */
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#define SNAPPY_HAVE_NEON 1
#else
#define SNAPPY_HAVE_NEON 0
#endif

#if defined(__ARM_FEATURE_CRC32)
#define SNAPPY_HAVE_NEON_CRC32 1
#else
#define SNAPPY_HAVE_NEON_CRC32 0
#endif

/* Endianness from the compiler's byte-order macro; default little-endian
 * (every target we support is little-endian). */
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define SNAPPY_IS_BIG_ENDIAN 1
#else
#define SNAPPY_IS_BIG_ENDIAN 0
#endif

#endif  // THIRD_PARTY_SNAPPY_OPENSOURCE_CMAKE_CONFIG_H_
