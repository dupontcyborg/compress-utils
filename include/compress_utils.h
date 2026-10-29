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
 * Memory model: output buffers are caller-allocated. Use the cu_*_bound()
 * helpers to size them. The one exception is cu_decompress_alloc(), which
 * returns a buffer the library allocated for you; free it with
 * cu_buffer_free(). See "One-shot decompression" below for the unknown-size
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
#define CU_VERSION_MINOR 11
#define CU_VERSION_PATCH 2

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
    CU_ALGO_GZIP   = 8, /* DEFLATE with the gzip wrapper (RFC 1952) */
    CU_ALGO_DEFLATE = 9, /* Raw DEFLATE (RFC 1951), no wrapper */
    CU_ALGO_LZ4_RAW = 10, /* Raw LZ4 block; caller supplies output capacity */
    CU_ALGO_LZO    = 11 /* raw LZO1X, decompression only */
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
    CU_ERR_SIZE_UNKNOWN      = 4,   /* wire format does not carry decompressed size; use cu_decompress_alloc or cu_decompress_stream_t */
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
 *       // wire format does not carry the size; use cu_decompress_alloc, or
 *       // cu_decompress_stream_t
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
 *                           wire format; caller should switch to
 *                           cu_decompress_alloc or the streaming API
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
 *   CU_ERR_SIZE_LIMIT     — the declared size exceeds the global cap (see
 *                           cu_set_max_decompressed_size), so the caller
 *                           never allocates a size an attacker chose;
 *                           *out_size still holds the declared size
 *   CU_ERR_DECOMPRESSION  — header is malformed
 *   CU_ERR_TRUNCATED      — input is shorter than the minimum header
 *
 * Notes:
 *   - For ZSTD inputs containing multiple concatenated frames, this
 *     returns the total over all frames if every one records its size, and
 *     CU_ERR_SIZE_UNKNOWN if any does not.
 *   - For XZ, parses the stream footer and index; requires `in` to be
 *     exactly one complete stream. Concatenated streams, stream padding,
 *     trailing bytes and damaged footers return CU_ERR_SIZE_UNKNOWN, so the
 *     caller streams and the decoder reports any error.
 */
CU_API cu_status_t cu_decompress_size_hint(
    cu_algorithm_t algo,
    const uint8_t* in, size_t in_len,
    size_t* out_size
);

/*
 * One-shot decompress into a buffer the library allocates, for input whose size
 * isn't known up front. On CU_OK, *out holds a malloc'd buffer of *out_len
 * bytes (never NULL, even for empty output) that the caller owns and must
 * release with cu_buffer_free(); on any error *out is NULL and *out_len is 0.
 * Thread-safe, like cu_decompress.
 *
 * It does what the pattern above does, so a caller needs neither a size hint
 * nor its own growth policy:
 *
 *   - If the wire format records the size (cu_decompress_size_hint returns
 *     CU_OK), it allocates exactly that and decodes straight into it.
 *   - Otherwise (formats with no recorded size, an xz input that isn't exactly
 *     one Stream) it streams into a buffer that starts at four times the input
 *     (at least 64 KiB) and doubles once less than an eighth of it is free.
 *     Input is fed to the decoder in 256 KiB chunks. The result is shrunk with
 *     realloc only when more than half the buffer would otherwise go unused, so
 *     it can carry up to as much spare capacity as data.
 *   - A size recorded in a header is never trusted blindly: if it can't be
 *     allocated, or the decode shows it was short, the stream path decodes into
 *     a buffer that only grows as real output appears.
 *
 * Output is capped by cu_get_max_decompressed_size() exactly as in
 * cu_decompress and cu_decompress_size_hint: more than the cap fails with
 * CU_ERR_SIZE_LIMIT and exactly the cap succeeds. The growing buffer never
 * exceeds cap + 1 bytes, so doubling can't overshoot the cap. With the cap
 * disabled (0) the output is bounded only by memory.
 *
 * Returns:
 *   CU_OK                 — *out and *out_len are set
 *   CU_ERR_INVALID_ARG    — out or out_len is NULL, or in is NULL with
 *                           in_len > 0
 *   CU_ERR_UNSUPPORTED_ALGO, CU_ERR_DECOMPRESSION, CU_ERR_TRUNCATED,
 *   CU_ERR_SIZE_LIMIT     — as cu_decompress
 *   CU_ERR_OOM            — the output buffer could not be allocated (a
 *                           status, never an abort)
 *
 * Concatenated frames, members and Streams decode as one, as in cu_decompress.
 * The cost over a caller-owned buffer is one copy if the caller then moves the
 * result into its own container, so prefer a sized buffer (hint, allocate,
 * cu_decompress) when you already know the size or a bound on it.
 */
CU_API cu_status_t cu_decompress_alloc(
    cu_algorithm_t algo,
    const uint8_t* in, size_t in_len,
    uint8_t** out, size_t* out_len
);

/*
 * Releases a buffer returned by cu_decompress_alloc. NULL is a no-op. Always
 * free with this, not free(): the library and the caller may use different C
 * runtimes (a DLL on Windows).
 */
CU_API void cu_buffer_free(void* buf);

/*
 * Sets a global cap on the decompressed size that cu_decompress,
 * cu_decompress_size_hint and cu_decompress_alloc will accept. Defaults to 1
 * GiB. Set to 0 to disable. Inputs that would decompress to more than this
 * return CU_ERR_SIZE_LIMIT.
 *
 * Streams don't apply it by default: a stream may legitimately decode more than
 * any in-memory cap. Set a stream's limit with
 * cu_decompress_stream_set_max_output. cu_decompress_alloc, which every
 * binding's one-shot decompress uses for formats that don't record their size,
 * sets that limit to this cap, so the cap holds for every format. Raw LZ4/LZO
 * and Snappy stream decoders also capture this cap at creation (Snappy because
 * it allocates the declared size before emitting anything).
 *
 * Thread-safe; takes effect for subsequent calls.
 */
CU_API void cu_set_max_decompressed_size(size_t bytes);

/* Returns the current cap set by cu_set_max_decompressed_size (0 = none). */
CU_API size_t cu_get_max_decompressed_size(void);

/* ============================================================================
 * Streaming compression
 * ============================================================================
 *
 * Lifetime:
 *   1. cu_compress_stream_create()
 *   2. zero or more cu_compress_stream_write() calls, optionally with
 *      cu_compress_stream_flush() between them
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

/*
 * Nonterminal flush: emit everything a decoder needs to recover all input
 * written so far, without further input or finish, while keeping the frame
 * open for more writes. Use it to deliver a message on a long-lived stream.
 *
 * Same drain protocol: CU_ERR_BUF_TOO_SMALL means call flush again with a
 * fresh buffer, until CU_OK. Until then, writes return CU_ERR_STREAM_STATE;
 * finish is allowed and includes the flush. A flush with nothing written
 * since the last one emits nothing.
 *
 * Each flush costs a few bytes and some ratio (a block boundary), so flush
 * per message, not per small write. Snappy (one raw block) and bzip2 (which
 * byte-aligns only at end of stream) can't flush mid-stream and return
 * CU_ERR_UNSUPPORTED_ALGO.
 */
CU_API cu_status_t cu_compress_stream_flush(
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
 * returns CU_ERR_TRUNCATED. CU_ERR_BUF_TOO_SMALL can also mean the output
 * filled exactly with output still held inside the decoder, so keep
 * draining until a call returns CU_OK.
 *
 * A caller that knows the output size may pass a buffer of exactly that size.
 * Once all of a complete stream's input has been written and the buffer is
 * full, cu_decompress_stream_finish() returns CU_OK on its first call, even
 * with no room left to offer (NULL, 0). One caveat: if the buffer fills while
 * some input is still to be written (a checksum or end marker split from the
 * data by the way the input is chunked, e.g. fed a few bytes at a time), the
 * decoder can't tell held output from a trailer still to come, so the write
 * returns CU_ERR_BUF_TOO_SMALL and draining without room can't succeed (zstd
 * then fails with CU_ERR_DECOMPRESSION after repeated calls that make no
 * progress). Offer at least one spare byte of room in that case.
 *
 * Any error from decoding (corrupt or truncated input, CU_ERR_SIZE_LIMIT,
 * CU_ERR_OOM, ...) is final: every later write or finish returns the same
 * status without calling into the codec again. Destroy the stream.
 *
 * Concatenated zstd and LZ4 frames (including skippable frames), gzip
 * members (with any zero padding between them) and xz streams (with stream
 * padding) decode as one stream, here and in cu_decompress, as the zstd, lz4,
 * gzip and xz CLIs do. zlib rejects data after the end of its stream with
 * CU_ERR_DECOMPRESSION; raw DEFLATE ignores it, since it usually belongs to an
 * enclosing container.
 *
 * Single-stream mode (CU_DSTREAM_SINGLE, via cu_decompress_stream_create_ex)
 * instead stops at the end of the first zstd frame, LZ4 frame, gzip member,
 * zlib stream, raw DEFLATE stream, xz stream, brotli stream, bzip2 stream or
 * Snappy block, even if a valid frame follows, and hands back the input that
 * came after it, so the caller can read what follows (another message, a
 * container's own bytes). Leading zstd/LZ4 skippable frames are skipped; the
 * end is the end of the first data frame. In this mode:
 *
 *   - The write that reaches the end decodes up to it and keeps the rest of
 *     its input as "unused" rather than consuming it: CU_OK with
 *     cu_decompress_stream_eof() set means the remainder of `in` is in
 *     cu_decompress_stream_unused(), not decoded.
 *   - cu_decompress_stream_eof() becomes nonzero once the end has been decoded
 *     and every byte of output delivered; while output is still being drained
 *     (CU_ERR_BUF_TOO_SMALL) it stays 0, and writes with data return
 *     CU_ERR_STREAM_STATE without consuming anything. Drain with empty writes
 *     or cu_decompress_stream_finish().
 *   - After eof, a write with data returns CU_ERR_STREAM_FINISHED and consumes
 *     nothing (the caller still owns those bytes); an empty write and finish
 *     return CU_OK with no output. finish before the end returns
 *     CU_ERR_TRUNCATED, as in the default mode.
 *   - Any error from decoding (corrupt or truncated input, CU_ERR_OOM, ...) is
 *     final: every later write or finish returns the same status, and eof is
 *     never set.
 *   - A nonterminal flush (cu_compress_stream_flush) ends a block, not a
 *     frame, so flushed data is delivered exactly as in the default mode and
 *     eof is not set.
 *   - Snappy has no incremental decoder, so single-mode Snappy emits nothing
 *     until its block is complete. Like every Snappy stream decoder, it
 *     captures cu_set_max_decompressed_size at creation; it rejects a larger
 *     declared size with CU_ERR_SIZE_LIMIT as soon as the size has been read,
 *     before buffering the block.
 *   - Raw LZ4 and LZO return CU_ERR_UNSUPPORTED_ALGO: raw LZ4 has no end marker.
 *
 * Raw LZ4 and LZO1X decode exactly one block and emit output during write.
 * finish marks the end of raw LZ4 input; LZO requires its explicit end marker.
 * These decoders retain a 64 KiB history and undrained input from the current
 * write, never prior chunks or the full output. Their cumulative output limit
 * is captured from cu_set_max_decompressed_size at creation (0 disables it).
 * Emitted bytes are provisional until finish succeeds. No container framing
 * or dictionary support is implied. Compression support is unchanged.
 */

typedef struct cu_decompress_stream cu_decompress_stream_t;

CU_API cu_status_t cu_decompress_stream_create(
    cu_algorithm_t algo,
    cu_decompress_stream_t** out_stream
);

/*
 * Limits the total decompressed output of a stream to `bytes` (0 = no limit,
 * the default). Output up to the limit is delivered; the call that would go
 * past it delivers only the bytes up to the limit and returns
 * CU_ERR_SIZE_LIMIT, which is final like any other decoding error. Exactly
 * `bytes` of output is allowed. Counts all output since the stream was
 * created, so it can be set or changed at any time.
 *
 * To decompress untrusted input of unknown size into memory, set this to the
 * most you're willing to hold, e.g. cu_get_max_decompressed_size().
 */
CU_API cu_status_t cu_decompress_stream_set_max_output(
    cu_decompress_stream_t* stream,
    size_t bytes
);

/* Flags for cu_decompress_stream_create_ex. */
#define CU_DSTREAM_SINGLE 0x1u  /* stop after the first frame/member/stream */

/*
 * cu_decompress_stream_create with flags. Flags 0 is exactly
 * cu_decompress_stream_create. Unknown flag bits return CU_ERR_INVALID_ARG;
 * CU_DSTREAM_SINGLE on raw LZ4 or LZO returns CU_ERR_UNSUPPORTED_ALGO.
 */
CU_API cu_status_t cu_decompress_stream_create_ex(
    cu_algorithm_t algo,
    uint32_t flags,
    cu_decompress_stream_t** out_stream
);

/*
 * Single-stream mode: nonzero once the end of the stream has been decoded and
 * all of its output delivered. Always 0 for streams created without
 * CU_DSTREAM_SINGLE.
 */
CU_API int cu_decompress_stream_eof(const cu_decompress_stream_t* stream);

/*
 * Single-stream mode: the input bytes received after the end of the stream,
 * in order, including any the stream held internally. Empty (len 0) until
 * eof, and for streams created without CU_DSTREAM_SINGLE. *data stays valid
 * until the next call on the stream or its destruction.
 */
CU_API cu_status_t cu_decompress_stream_unused(
    const cu_decompress_stream_t* stream,
    const uint8_t** data, size_t* len
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
