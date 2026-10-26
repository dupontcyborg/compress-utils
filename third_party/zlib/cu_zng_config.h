/*
 * cu_zng_config.h — portable, compile-time-detected build config for the
 * vendored zlib-ng. HAND-AUTHORED; tools/vendor-codecs.py never overwrites it.
 *
 * Upstream's CMake/configure probes the toolchain and passes the result as a
 * long list of -D flags. We replace that probe with feature tests the
 * preprocessor can do, so one header is correct on every target and every
 * binding build (CMake, cgo, cc-rs, zig, MSVC) with no configure step and no
 * per-file flags. Every vendored translation unit is a generated wrapper in
 * third_party/zlib/cu/ whose first line includes this file, which is how the
 * macros reach upstream's zbuild.h before it reads them.
 *
 * Policy:
 *   - zlib-compatible API (ZLIB_COMPAT): public symbols keep zlib's names.
 *   - Runtime CPU detection stays ON: SIMD code is selected through zlib-ng's
 *     functable after a cpuid / getauxval / sysctl probe, so an older CPU
 *     never executes an instruction it lacks.
 *   - WITH_ALL_FALLBACKS: the generic C implementations are always compiled,
 *     so every dispatch slot has a portable, always-correct fallback.
 *   - SIMD is enabled only for x86-64 and AArch64. Everything else (wasm32,
 *     32-bit x86/ARM, riscv, ppc, s390x, loongarch) takes the generic paths.
 *   - The gz* file API and infback are not vendored (compress-utils never
 *     uses them).
 */

#ifndef CU_ZNG_CONFIG_H
#define CU_ZNG_CONFIG_H

#define ZLIB_COMPAT 1
#define WITH_OPTIM 1
#define WITH_ALL_FALLBACKS 1

/* ---- Compiler capabilities (what CMake's check_c_source_compiles probes). */
#if defined(__GNUC__) || defined(__clang__)
#  define HAVE_ATTRIBUTE_ALIGNED 1
#  define HAVE_BUILTIN_ASSUME_ALIGNED 1
#  define HAVE_BUILTIN_CTZ 1
#  define HAVE_BUILTIN_CTZLL 1
#  if !defined(_WIN32)
#    define HAVE_VISIBILITY_HIDDEN 1
#  endif
#endif

/* ---- x86-64 --------------------------------------------------------------
 * Each X86_* macro compiles one SIMD translation unit. The units themselves are
 * wrapped (see the cu/ directory) in a target pragma on GCC/Clang, because upstream
 * enables the instruction set with per-file -m flags that no binding can pass.
 * Toolchains too old for the intrinsics fall back to generic C. */
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(_M_ARM64EC) && \
    (defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 9) || \
     (defined(_MSC_VER) && _MSC_VER >= 1920 && !defined(__GNUC__)))
#  define X86_FEATURES 1
#  if defined(_MSC_VER) && !defined(__clang__) && _MSC_VER < 1930
#    define WITHOUT_CHORBA_SSE 1 /* the SSE Chorba CRC needs VS 2022 (upstream CMake) */
#  endif
#  define X86_SSE2 1
#  define X86_SSSE3 1
#  define X86_SSE41 1
#  define X86_SSE42 1
#  define X86_PCLMULQDQ_CRC 1
#  define X86_AVX2 1
#  define X86_AVX512 1
#  define X86_AVX512VNNI 1
#  define X86_VPCLMULQDQ_CRC 1
#  define X86_HAVE_XSAVE_INTRIN 1
#  if defined(__has_include)
#    if __has_include(<cpuid.h>)
#      define HAVE_CPUID_GNU 1
#    endif
#  endif
#  if !defined(HAVE_CPUID_GNU) && defined(_MSC_VER)
#    define HAVE_CPUID_MS 1
#  endif
#  if !defined(HAVE_CPUID_GNU) && !defined(HAVE_CPUID_MS)
#    undef X86_FEATURES
#    undef X86_SSE2
#    undef X86_SSSE3
#    undef X86_SSE41
#    undef X86_SSE42
#    undef X86_PCLMULQDQ_CRC
#    undef X86_AVX2
#    undef X86_AVX512
#    undef X86_AVX512VNNI
#    undef X86_VPCLMULQDQ_CRC
#    undef X86_HAVE_XSAVE_INTRIN
#  endif
#endif

/* ---- AArch64 -------------------------------------------------------------
 * NEON is architectural on AArch64; the CRC32 unit carries its own target
 * attribute upstream, so no wrapper pragma is needed. */
#if defined(__aarch64__) || (defined(_M_ARM64) && !defined(_M_ARM64EC)) || \
    defined(_M_ARM64EC)
#  define ARM_FEATURES 1
#  define ARM_NEON 1
#  define ARM_CRC32 1
#  if defined(__has_include)
#    if __has_include(<arm_acle.h>)
#      define HAVE_ARM_ACLE_H 1
#    endif
#  endif
#  if defined(_MSC_VER) && !defined(__clang__)
#    define ARM_NEON_HASLD4 1
#    define ARM_CRC32_INTRIN 1
#  else
#    if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 10)
#      define ARM_NEON_HASLD4 1
#    endif
#    if defined(HAVE_ARM_ACLE_H)
#      define ARM_CRC32_INTRIN 1
#    endif
#  endif
#  if defined(__linux__) && defined(__aarch64__)
/* Runtime CRC32 detection reads HWCAP_CRC32 through getauxval (glibc, musl and
 * bionic all ship <sys/auxv.h> and define the bit on AArch64). */
#    if defined(__has_include)
#      if __has_include(<sys/auxv.h>)
#        define HAVE_SYS_AUXV_H 1
#        define ARM_AUXV_HAS_CRC32 1
#      endif
#    endif
#  endif
#endif

/* The x86 SIMD units include the intrinsics headers themselves, but a target
 * pragma must not wrap the first inclusion of a system header, so pull them in
 * here, before any wrapper pragma opens. (No-ops on every other target.) */
#if defined(X86_FEATURES)
#  if defined(_MSC_VER) && !defined(__clang__)
#    include <intrin.h>
#  endif
#  include <immintrin.h>
#  if defined(HAVE_CPUID_GNU)
#    include <cpuid.h>
#  endif
#endif
#if defined(ARM_FEATURES)
#  if defined(_MSC_VER) && !defined(__clang__)
#    include <arm64_neon.h>
#    include <intrin.h>
#  else
#    include <arm_neon.h>
#    if defined(HAVE_ARM_ACLE_H)
#      include <arm_acle.h>
#    endif
#  endif
#endif

#endif /* CU_ZNG_CONFIG_H */
