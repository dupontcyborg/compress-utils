/*
 * compress_utils.h — canonical C ABI for the compress-utils library.
 *
 * This header is the source of truth for the public surface. C, C++, Python,
 * WASM, and all future language bindings consume this ABI. The C++ binding in
 * `bindings/cpp/` is a header-only RAII wrapper over these symbols.
 *
 * ABI stability: unstable until v1.0.0. The CU_VERSION_* macros below track
 * the current version; check cu_version() at runtime if you need to gate on
 * specific behavior.
 *
 * Memory model: the library never allocates memory it hands back to the
 * caller. All output buffers are caller-allocated. Use the cu_*_bound()
 * helpers to size them. See "Allocation model" below for the unknown-size
 * decompression pattern.
 *
 * Thread safety:
 *   - One-shot functions (cu_compress, cu_decompress) are thread-safe.
 *   - Stream handles are NOT thread-safe; use one handle per thread.
 *   - cu_last_error() is thread-local.
 *
 * Error handling: functions return a cu_status_t code. CU_OK (0) indicates
 * success; any non-zero value indicates failure. A human-readable message
 * for the most recent error on the calling thread is available via
 * cu_last_error(); a static string for any code is available via
 * cu_strerror(code).
 */

#ifndef COMPRESS_UTILS_H
#define COMPRESS_UTILS_H

#include <stddef.h>
#include <stdint.h>

/*
 * Symbol visibility. Define CU_BUILD_SHARED when building this library as
 * a shared lib, and CU_USE_SHARED when consuming it as a shared lib. The
 * static-library and "build static, consume directly" cases need no
 * defines.
 */
#if defined(_WIN32) || defined(_WIN64)
#  if defined(CU_BUILD_SHARED)
#    define CU_API __declspec(dllexport)
#  elif defined(CU_USE_SHARED)
#    define CU_API __declspec(dllimport)
#  else
#    define CU_API
#  endif
#else
#  if defined(CU_BUILD_SHARED)
#    define CU_API __attribute__((visibility("default")))
#  else
#    define CU_API
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Versioning
 * ============================================================================ */

#define CU_VERSION_MAJOR 0
#define CU_VERSION_MINOR 1
#define CU_VERSION_PATCH 0

/* Returns a static string of the form "MAJOR.MINOR.PATCH". */
CU_API const char* cu_version(void);

/* ============================================================================
 * Algorithms
 * ============================================================================ */

typedef enum {
    CU_ALGO_ZSTD   = 0,
    CU_ALGO_BROTLI = 1,
    CU_ALGO_ZLIB   = 2,
    CU_ALGO_BZ2    = 3,
    CU_ALGO_LZ4    = 4,
    CU_ALGO_XZ     = 5,
    CU_ALGO_LZMA   = 6, /* alias for XZ; produces .xz frames */
    CU_ALGO_SNAPPY = 7,
    CU_ALGO_GZIP   = 8  /* DEFLATE with the gzip wrapper (RFC 1952) */
} cu_algorithm_t;

/*
 * Returns the lowercase canonical name ("zstd", "brotli", ...) for an
 * algorithm, or NULL for an unknown value. Static lifetime.
 */
CU_API const char* cu_algorithm_name(cu_algorithm_t algo);

/*
 * Returns 1 if the algorithm was included in this build (per the
 * INCLUDE_<ALGO> CMake options), 0 otherwise. Useful for bindings that
 * want to expose only the algorithms actually compiled in.
 */
CU_API int cu_algorithm_available(cu_algorithm_t algo);

/* ============================================================================
 * Status codes
 * ============================================================================ */

typedef enum {
    CU_OK                    = 0,

    /* Caller-side errors */
    CU_ERR_INVALID_ARG       = 1,   /* NULL pointer, bad enum value, etc. */
    CU_ERR_INVALID_LEVEL     = 2,   /* level outside 1..10 */
    CU_ERR_BUF_TOO_SMALL     = 3,   /* output buffer insufficient; *out_len holds the required size */
    CU_ERR_SIZE_UNKNOWN      = 4,   /* wire format does not carry decompressed size; use cu_decompress_stream_t */
    CU_ERR_UNSUPPORTED_ALGO  = 5,   /* algorithm not compiled into this build */

    /* Codec errors */
    CU_ERR_COMPRESSION       = 6,   /* underlying codec rejected the input or failed mid-operation */
    CU_ERR_DECOMPRESSION     = 7,   /* codec reported corrupted/invalid compressed data */
    CU_ERR_TRUNCATED         = 8,   /* compressed input ended before the codec was done */
    CU_ERR_SIZE_LIMIT        = 9,   /* decompressed size would exceed cu_set_max_decompressed_size */

    /* Stream errors */
    CU_ERR_STREAM_FINISHED   = 10,  /* write/finish called on an already-finished stream */
    CU_ERR_STREAM_STATE      = 11,  /* operation invalid for current stream state */

    /* System errors */
    CU_ERR_OOM               = 12,  /* internal allocation (codec context, etc.) failed */
    CU_ERR_INTERNAL          = 13   /* unexpected internal failure; check cu_last_error() */
} cu_status_t;

/*
 * Returns a static human-readable string for a status code. Returns
 * "unknown error" for unrecognized values. Never NULL.
 */
CU_API const char* cu_strerror(cu_status_t code);

/*
 * Returns the human-readable message for the most recent error on the
 * calling thread, or "" if no error has occurred. The returned pointer is
 * valid until the next cu_* call on this thread that produces an error.
 */
CU_API const char* cu_last_error(void);

/* Clears the calling thread's last-error state. */
CU_API void cu_clear_last_error(void);

/* ============================================================================
 * One-shot compression
 * ============================================================================
 *
 * Allocation model: caller allocates `out` with capacity at least
 * cu_compress_bound(in_len, algo). On entry, *out_len holds the capacity of
 * `out`; on successful return, *out_len holds the number of bytes written.
 *
 * On CU_ERR_BUF_TOO_SMALL, *out_len holds the size that would have been
 * required (if the codec can compute it cheaply; otherwise it is left at
 * the input capacity and the caller should grow with cu_compress_bound).
 *
 * level: 1..10. 1 = fastest, 10 = smallest. Mapped per-algorithm — see
 * doc/levels.md for the per-algorithm native ranges.
 */

/*
 * Returns the maximum possible compressed size for an input of `in_len`
 * bytes under the given algorithm. Always a safe upper bound; never
 * returns 0 unless the input is too large for the codec (in which case
 * the algorithm's hard limit applies).
 */
CU_API size_t cu_compress_bound(size_t in_len, cu_algorithm_t algo);

CU_API cu_status_t cu_compress(
    cu_algorithm_t algo,
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len,
    int level
);

/* ============================================================================
 * One-shot decompression
 * ============================================================================
 *
 * Different compressed formats carry decompressed-size information
 * differently:
 *
 *   - ZSTD frames written with a known content size: size available.
 *   - ZSTD frames from streaming encoders without pledgedSrcSize: not
 *     available.
 *   - XZ streams: size available (via stream footer / index).
 *   - LZ4 frames with the content-size flag set: size available.
 *   - LZ4 frames without the flag, raw LZ4 blocks, brotli, zlib, deflate,
 *     gzip, bz2: size is never available without decompressing.
 *
 * The recommended pattern is therefore:
 *
 *   size_t out_size;
 *   cu_status_t s = cu_decompress_size_hint(algo, in, in_len, &out_size);
 *   if (s == CU_OK) {
 *       uint8_t* out = my_alloc(out_size);
 *       s = cu_decompress(algo, in, in_len, out, &out_size);
 *       // ...
 *   } else if (s == CU_ERR_SIZE_UNKNOWN) {
 *       // wire format does not carry the size; use cu_decompress_stream_t
 *   } else {
 *       // input is malformed
 *   }
 *
 * cu_decompress itself is caller-allocates and never grows. Return codes:
 *
 *   CU_OK                 — success, *out_len holds bytes written
 *   CU_ERR_BUF_TOO_SMALL  — output buffer was insufficient and the codec
 *                           was able to report the required size; *out_len
 *                           holds the size the caller should allocate
 *   CU_ERR_SIZE_UNKNOWN   — output buffer was insufficient and the codec
 *                           cannot report the required size from the
 *                           wire format; caller should switch to the
 *                           streaming API
 *   CU_ERR_SIZE_LIMIT     — decompressed size would exceed the global
 *                           cap (see cu_set_max_decompressed_size)
 *   CU_ERR_DECOMPRESSION  — input is corrupted or invalid
 *   CU_ERR_TRUNCATED      — input ended before the codec was done
 */
CU_API cu_status_t cu_decompress(
    cu_algorithm_t algo,
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
);

/*
 * Inspect the start of a compressed buffer and report the decompressed
 * size if the wire format encodes it. Reads at most a few hundred bytes
 * from `in`; cheap to call.
 *
 * Returns:
 *   CU_OK                 — *out_size holds the exact decompressed size
 *   CU_ERR_SIZE_UNKNOWN   — wire format does not carry the size; the
 *                           caller should use cu_decompress_stream_t
 *   CU_ERR_DECOMPRESSION  — header is malformed
 *   CU_ERR_TRUNCATED      — input is shorter than the minimum header
 *
 * Notes:
 *   - For ZSTD inputs containing multiple concatenated frames, this
 *     returns the decompressed size of the *first* frame only. Callers
 *     handling concatenated frames should use the streaming API.
 *   - For XZ, parses the stream footer; requires `in` to contain the
 *     complete stream (i.e. `in_len` reaches the end-of-stream marker).
 */
CU_API cu_status_t cu_decompress_size_hint(
    cu_algorithm_t algo,
    const uint8_t* in, size_t in_len,
    size_t* out_size
);

/*
 * Sets a global cap on the decompressed size that cu_decompress will
 * accept. Defaults to 1 GiB. Set to 0 to disable. Inputs that would
 * decompress to more than this return CU_ERR_SIZE_LIMIT. Stream
 * decompression is not affected — callers of the streaming API are
 * expected to enforce their own caps.
 *
 * Thread-safe; takes effect for subsequent calls.
 */
CU_API void cu_set_max_decompressed_size(size_t bytes);

/* ============================================================================
 * Streaming compression
 * ============================================================================
 *
 * Lifetime:
 *   1. cu_compress_stream_create()
 *   2. zero or more cu_compress_stream_write() calls
 *   3. cu_compress_stream_finish() (possibly multiple times if the caller's
 *      output buffer fills before all internal state is flushed — see below)
 *   4. cu_compress_stream_destroy()
 *
 * Write/finish protocol: both calls take a caller-allocated output buffer
 * (out, *out_len). On return *out_len is the number of bytes written.
 *
 * If the codec produced more output than the buffer could hold, returns
 * CU_ERR_BUF_TOO_SMALL with the buffer fully written and the unconsumed
 * input (or remaining flush data) retained inside the stream. The caller
 * should drain the rest by calling the same function again with the
 * buffer reset. This is the difference vs. the prior C++ streaming API,
 * which silently dropped overflow.
 *
 * cu_compress_stream_finish() returns CU_OK when no further output remains.
 * Until then it returns CU_ERR_BUF_TOO_SMALL or fills the buffer fully.
 */

typedef struct cu_compress_stream cu_compress_stream_t;

CU_API cu_status_t cu_compress_stream_create(
    cu_algorithm_t algo,
    int level,
    cu_compress_stream_t** out_stream
);

/*
 * Feed input to the stream. Returns CU_OK when all of `in` has been
 * consumed and all currently-available output has been written. Returns
 * CU_ERR_BUF_TOO_SMALL when output filled with input remaining; caller
 * must call again with a fresh buffer (the stream remembers the unconsumed
 * portion of `in` internally, so on the retry the caller passes
 * (in=NULL, in_len=0) to drain).
 */
CU_API cu_status_t cu_compress_stream_write(
    cu_compress_stream_t* stream,
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
);

/*
 * Flush remaining state and finalize the frame. Returns CU_OK once all
 * trailing data has been written. Returns CU_ERR_BUF_TOO_SMALL if more
 * output remains; caller drains with a fresh buffer.
 */
CU_API cu_status_t cu_compress_stream_finish(
    cu_compress_stream_t* stream,
    uint8_t* out, size_t* out_len
);

CU_API void cu_compress_stream_destroy(cu_compress_stream_t* stream);

/* ============================================================================
 * Streaming decompression
 * ============================================================================
 *
 * Same protocol as streaming compression. cu_decompress_stream_finish()
 * verifies that the input ended on a frame boundary; truncated input
 * returns CU_ERR_TRUNCATED.
 */

typedef struct cu_decompress_stream cu_decompress_stream_t;

CU_API cu_status_t cu_decompress_stream_create(
    cu_algorithm_t algo,
    cu_decompress_stream_t** out_stream
);

CU_API cu_status_t cu_decompress_stream_write(
    cu_decompress_stream_t* stream,
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
);

CU_API cu_status_t cu_decompress_stream_finish(
    cu_decompress_stream_t* stream,
    uint8_t* out, size_t* out_len
);

CU_API void cu_decompress_stream_destroy(cu_decompress_stream_t* stream);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* COMPRESS_UTILS_H */
