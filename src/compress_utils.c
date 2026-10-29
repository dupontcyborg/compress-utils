/*
 * compress_utils.c — public ABI implementation.
 *
 * Dispatches to per-algorithm vtables via algorithm_registry. Owns the
 * common cu_compress_stream_t / cu_decompress_stream_t wrapper structs
 * (which carry an algorithm pointer + algorithm-specific state).
 */

#include "compress_utils.h"
#include "algorithm_registry.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_MSC_VER) || defined(__clang__)
#  include <stdatomic.h>
#endif

/* ============================================================================
 * Version
 * ============================================================================ */

#define CU_STR_(x) #x
#define CU_STR(x) CU_STR_(x)

const char* cu_version(void) {
    /* CU_BUILD_VERSION is injected by CMake from `git describe --tags`
     * (see cmake/GitVersion.cmake). It carries the full tag including any
     * prerelease suffix — e.g. "0.7.0", "0.7.0-rc.1", "0.7.0-rc.1-3-gabcd".
     * Falls back to the header's static MAJOR.MINOR.PATCH macros if the
     * build wasn't given a value (out-of-tree builds with no git history). */
#ifdef CU_BUILD_VERSION
    return CU_BUILD_VERSION;
#else
    return CU_STR(CU_VERSION_MAJOR) "." CU_STR(CU_VERSION_MINOR) "." CU_STR(CU_VERSION_PATCH);
#endif
}

/* ============================================================================
 * Algorithm names
 * ============================================================================ */

const char* cu_algorithm_name(cu_algorithm_t algo) {
    const cu_algorithm_vtbl_t* v = cu_registry_lookup(algo);
    if (v) return v->name;
    /* Allow the LZMA alias to report a name even on builds that route it
     * through XZ — handled inside the registry. */
    return NULL;
}

int cu_algorithm_available(cu_algorithm_t algo) {
    return cu_registry_lookup(algo) != NULL ? 1 : 0;
}

/* ============================================================================
 * Errors
 * ============================================================================ */

/* Thread-local last-error buffer. Bounded so we never allocate in the
 * hot path. Truncation is fine — callers wanting structured detail can
 * use the status code. */
#define CU_ERROR_BUF_LEN 256

#if defined(_MSC_VER)
#  define CU_THREAD_LOCAL __declspec(thread)
#else
#  define CU_THREAD_LOCAL _Thread_local
#endif

static CU_THREAD_LOCAL char g_last_error[CU_ERROR_BUF_LEN];

void cu_set_last_error(const char* msg) {
    if (!msg) {
        g_last_error[0] = '\0';
        return;
    }
    size_t n = strlen(msg);
    if (n >= CU_ERROR_BUF_LEN) n = CU_ERROR_BUF_LEN - 1;
    memcpy(g_last_error, msg, n);
    g_last_error[n] = '\0';
}

void cu_set_last_errorf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_last_error, CU_ERROR_BUF_LEN, fmt, ap);
    va_end(ap);
    if (n < 0) g_last_error[0] = '\0';
}

const char* cu_last_error(void) {
    return g_last_error;
}

void cu_clear_last_error(void) {
    g_last_error[0] = '\0';
}

const char* cu_strerror(cu_status_t code) {
    switch (code) {
        case CU_OK:                   return "ok";
        case CU_ERR_INVALID_ARG:      return "invalid argument";
        case CU_ERR_INVALID_LEVEL:    return "compression level out of range (1..10)";
        case CU_ERR_BUF_TOO_SMALL:    return "output buffer too small";
        case CU_ERR_SIZE_UNKNOWN:     return "decompressed size not encoded in wire format; use streaming";
        case CU_ERR_UNSUPPORTED_ALGO: return "algorithm not compiled into this build";
        case CU_ERR_COMPRESSION:      return "compression failed";
        case CU_ERR_DECOMPRESSION:    return "decompression failed";
        case CU_ERR_TRUNCATED:        return "compressed input truncated";
        case CU_ERR_SIZE_LIMIT:       return "decompressed size exceeds configured limit";
        case CU_ERR_STREAM_FINISHED:  return "stream is already finished";
        case CU_ERR_STREAM_STATE:     return "operation invalid for current stream state";
        case CU_ERR_OOM:              return "out of memory";
        case CU_ERR_INTERNAL:         return "internal error";
    }
    return "unknown error";
}

/* ============================================================================
 * Decompression size cap
 * ============================================================================ */

/* Default: 1 GiB. Bindings set it from one thread while others decode, so
 * it's atomic. Relaxed ordering is enough: each call reads it once. MSVC's C
 * compiler has no <stdatomic.h> without /experimental:c11atomics; there an
 * aligned volatile size_t is read and written atomically. */
#define CU_DEFAULT_MAX_DECOMPRESSED_SIZE ((size_t)1024 * 1024 * 1024)

#if defined(_MSC_VER) && !defined(__clang__)
static volatile size_t g_max_decompressed_size = CU_DEFAULT_MAX_DECOMPRESSED_SIZE;
#  define CU_CAP_STORE(v) (g_max_decompressed_size = (v))
#  define CU_CAP_LOAD()   (g_max_decompressed_size)
#else
static _Atomic size_t g_max_decompressed_size = CU_DEFAULT_MAX_DECOMPRESSED_SIZE;
#  define CU_CAP_STORE(v) atomic_store_explicit(&g_max_decompressed_size, (v), memory_order_relaxed)
#  define CU_CAP_LOAD()   atomic_load_explicit(&g_max_decompressed_size, memory_order_relaxed)
#endif

void cu_set_max_decompressed_size(size_t bytes) {
    CU_CAP_STORE(bytes);
}

size_t cu_get_max_decompressed_size(void) {
    return CU_CAP_LOAD();
}

/* ============================================================================
 * One-shot dispatch
 * ============================================================================ */

static cu_status_t resolve(cu_algorithm_t algo, const cu_algorithm_vtbl_t** out_v) {
    const cu_algorithm_vtbl_t* v = cu_registry_lookup(algo);
    if (!v) {
        cu_set_last_errorf("algorithm %d is not available in this build", (int)algo);
        return CU_ERR_UNSUPPORTED_ALGO;
    }
    *out_v = v;
    return CU_OK;
}

size_t cu_compress_bound(size_t in_len, cu_algorithm_t algo) {
    const cu_algorithm_vtbl_t* v = cu_registry_lookup(algo);
    if (!v || !v->compress_bound) return 0;
    return v->compress_bound(in_len);
}

cu_status_t cu_compress(
    cu_algorithm_t algo,
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len,
    int level
) {
    if (!out_len)                       return CU_ERR_INVALID_ARG;
    if (in_len > 0 && !in)              return CU_ERR_INVALID_ARG;
    if (*out_len > 0 && !out)           return CU_ERR_INVALID_ARG;
    if (level < 1 || level > 10) {
        cu_set_last_error("compression level must be between 1 and 10");
        return CU_ERR_INVALID_LEVEL;
    }

    const cu_algorithm_vtbl_t* v;
    cu_status_t s = resolve(algo, &v);
    if (s != CU_OK) return s;

    cu_clear_last_error();
    if (!v->compress) { cu_set_last_error("operation unsupported for this algorithm"); return CU_ERR_UNSUPPORTED_ALGO; }
    return v->compress(in, in_len, out, out_len, level);
}

cu_status_t cu_decompress(
    cu_algorithm_t algo,
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    if (!out_len)                       return CU_ERR_INVALID_ARG;
    if (in_len > 0 && !in)              return CU_ERR_INVALID_ARG;
    if (*out_len > 0 && !out)           return CU_ERR_INVALID_ARG;

    const cu_algorithm_vtbl_t* v;
    cu_status_t s = resolve(algo, &v);
    if (s != CU_OK) return s;

    cu_clear_last_error();
    if (!v->decompress) { cu_set_last_error("operation unsupported for this algorithm"); return CU_ERR_UNSUPPORTED_ALGO; }
    return v->decompress(in, in_len, out, out_len);
}

cu_status_t cu_decompress_size_hint(
    cu_algorithm_t algo,
    const uint8_t* in, size_t in_len,
    size_t* out_size
) {
    if (!out_size)         return CU_ERR_INVALID_ARG;
    if (in_len > 0 && !in) return CU_ERR_INVALID_ARG;

    const cu_algorithm_vtbl_t* v;
    cu_status_t s = resolve(algo, &v);
    if (s != CU_OK) return s;

    cu_clear_last_error();
    if (!v->decompress_size_hint) { cu_set_last_error("operation unsupported for this algorithm"); return CU_ERR_UNSUPPORTED_ALGO; }
    s = v->decompress_size_hint(in, in_len, out_size);
    /* The declared size comes from the input, so a caller that allocates it
     * would let a few header bytes choose the allocation. */
    size_t cap = CU_CAP_LOAD();
    if (s == CU_OK && cap > 0 && *out_size > cap) {
        cu_set_last_errorf("%s: declared size %zu exceeds cap %zu", v->name, *out_size, cap);
        return CU_ERR_SIZE_LIMIT;
    }
    return s;
}

/* ============================================================================
 * Streaming
 * ============================================================================
 *
 * The public ABI gives consumers an opaque pointer to one of these
 * wrapper structs. They carry the algorithm vtable + algorithm-specific
 * state owned by the codec.
 */

struct cu_compress_stream {
    const cu_algorithm_vtbl_t* vtbl;
    void* state;
    int finished;
    int flushing;  /* a flush returned BUF_TOO_SMALL and must be completed */
    int dirty;     /* input written since the last completed flush */
};

struct cu_decompress_stream {
    const cu_algorithm_vtbl_t* vtbl;
    void* state;
    int finished;
    int single;  /* CU_DSTREAM_SINGLE */
    int eof;     /* single: the end was decoded and all its output delivered */
    cu_status_t failed;  /* the codec's first error; the stream is dead */
    size_t max_output;   /* cu_decompress_stream_set_max_output; 0 = none */
    size_t produced;     /* output delivered so far */
};

cu_status_t cu_compress_stream_create(
    cu_algorithm_t algo,
    int level,
    cu_compress_stream_t** out_stream
) {
    if (!out_stream)                return CU_ERR_INVALID_ARG;
    *out_stream = NULL;
    if (level < 1 || level > 10) {
        cu_set_last_error("compression level must be between 1 and 10");
        return CU_ERR_INVALID_LEVEL;
    }

    const cu_algorithm_vtbl_t* v;
    cu_status_t s = resolve(algo, &v);
    if (s != CU_OK) return s;

    if (!v->compress_stream_create) {
        cu_set_last_error("streaming compress is not supported for this algorithm");
        return CU_ERR_UNSUPPORTED_ALGO;
    }

    cu_compress_stream_t* stream = calloc(1, sizeof(*stream));
    if (!stream) {
        cu_set_last_error("out of memory allocating cu_compress_stream_t");
        return CU_ERR_OOM;
    }
    stream->vtbl = v;

    cu_clear_last_error();
    s = v->compress_stream_create(level, &stream->state);
    if (s != CU_OK) {
        free(stream);
        return s;
    }

    *out_stream = stream;
    return CU_OK;
}

cu_status_t cu_compress_stream_write(
    cu_compress_stream_t* stream,
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    if (!stream || !out_len)            return CU_ERR_INVALID_ARG;
    if (in_len > 0 && !in)              return CU_ERR_INVALID_ARG;
    if (*out_len > 0 && !out)           return CU_ERR_INVALID_ARG;
    if (stream->finished) {
        cu_set_last_error("write to finished compress stream");
        return CU_ERR_STREAM_FINISHED;
    }
    if (stream->flushing) {
        cu_set_last_error("write before the pending flush completed; call flush until CU_OK");
        return CU_ERR_STREAM_STATE;
    }

    cu_clear_last_error();
    if (in_len > 0) stream->dirty = 1;
    return stream->vtbl->compress_stream_write(stream->state, in, in_len, out, out_len);
}

cu_status_t cu_compress_stream_flush(
    cu_compress_stream_t* stream,
    uint8_t* out, size_t* out_len
) {
    if (!stream || !out_len)            return CU_ERR_INVALID_ARG;
    if (*out_len > 0 && !out)           return CU_ERR_INVALID_ARG;
    if (stream->finished) {
        cu_set_last_error("flush of finished compress stream");
        return CU_ERR_STREAM_FINISHED;
    }
    if (!stream->vtbl->compress_stream_flush) {
        cu_set_last_errorf("%s: streams can't flush mid-stream", stream->vtbl->name);
        return CU_ERR_UNSUPPORTED_ALGO;
    }
    /* Nothing written since the last flush: nothing to emit. Skipping the
     * codec keeps repeated flushes free (zlib would add a marker each time). */
    if (!stream->dirty && !stream->flushing) {
        *out_len = 0;
        return CU_OK;
    }

    cu_clear_last_error();
    cu_status_t s = stream->vtbl->compress_stream_flush(stream->state, out, out_len);
    if (s == CU_ERR_BUF_TOO_SMALL) {
        stream->flushing = 1;
    } else if (s == CU_OK) {
        stream->flushing = 0;
        stream->dirty = 0;
    }
    return s;
}

cu_status_t cu_compress_stream_finish(
    cu_compress_stream_t* stream,
    uint8_t* out, size_t* out_len
) {
    if (!stream || !out_len)            return CU_ERR_INVALID_ARG;
    if (*out_len > 0 && !out)           return CU_ERR_INVALID_ARG;

    cu_clear_last_error();
    cu_status_t s = stream->vtbl->compress_stream_finish(stream->state, out, out_len);
    if (s == CU_OK) {
        stream->finished = 1;
    }
    return s;
}

void cu_compress_stream_destroy(cu_compress_stream_t* stream) {
    if (!stream) return;
    if (stream->vtbl && stream->state) {
        stream->vtbl->compress_stream_destroy(stream->state);
    }
    free(stream);
}

cu_status_t cu_decompress_stream_create(
    cu_algorithm_t algo,
    cu_decompress_stream_t** out_stream
) {
    return cu_decompress_stream_create_ex(algo, 0, out_stream);
}

cu_status_t cu_decompress_stream_create_ex(
    cu_algorithm_t algo,
    uint32_t flags,
    cu_decompress_stream_t** out_stream
) {
    if (!out_stream) return CU_ERR_INVALID_ARG;
    *out_stream = NULL;
    if (flags & ~(uint32_t)CU_DSTREAM_SINGLE) {
        cu_set_last_errorf("unknown decompress stream flags 0x%x", (unsigned)flags);
        return CU_ERR_INVALID_ARG;
    }
    int single = (flags & CU_DSTREAM_SINGLE) != 0;

    const cu_algorithm_vtbl_t* v;
    cu_status_t s = resolve(algo, &v);
    if (s != CU_OK) return s;

    if (!v->decompress_stream_create) {
        cu_set_last_error("streaming decompress is not supported for this algorithm");
        return CU_ERR_UNSUPPORTED_ALGO;
    }
    if (single && (!v->decompress_stream_create_single || !v->decompress_stream_end)) {
        cu_set_last_errorf("%s: single-stream decoding is not supported", v->name);
        return CU_ERR_UNSUPPORTED_ALGO;
    }

    cu_decompress_stream_t* stream = calloc(1, sizeof(*stream));
    if (!stream) {
        cu_set_last_error("out of memory allocating cu_decompress_stream_t");
        return CU_ERR_OOM;
    }
    stream->vtbl = v;
    stream->single = single;

    cu_clear_last_error();
    s = single ? v->decompress_stream_create_single(&stream->state)
               : v->decompress_stream_create(&stream->state);
    if (s != CU_OK) {
        free(stream);
        return s;
    }

    *out_stream = stream;
    return CU_OK;
}

/* Single mode: the codec has decoded the end of the stream (its output may
 * still be draining). */
static int dstream_ended(const cu_decompress_stream_t* stream) {
    return stream->single && stream->vtbl->decompress_stream_end(stream->state, NULL, NULL);
}

/* A codec error kills the stream in every mode: the codec is never re-entered
 * after failing, since several can't resume from an error state (libbzip2
 * reads and writes out of bounds). In single mode it also means the stream can
 * never report eof with output or unused input missing (e.g. after running out
 * of memory keeping the unused input). Single mode: a call that returned CU_OK
 * after the end has delivered all of the stream's output. */
static cu_status_t dstream_settle(cu_decompress_stream_t* stream, cu_status_t s) {
    if (s != CU_OK && s != CU_ERR_BUF_TOO_SMALL) stream->failed = s;
    else if (s == CU_OK && dstream_ended(stream)) stream->eof = 1;
    return s;
}

/* Applies the stream's output limit to a codec call that wrote *out_len
 * bytes: output past the limit is cut off and the call fails with
 * CU_ERR_SIZE_LIMIT (unless the codec already failed). */
static cu_status_t dstream_limit(cu_decompress_stream_t* stream, cu_status_t s, size_t* out_len) {
    if (stream->max_output > 0) {
        size_t room = stream->produced < stream->max_output
                          ? stream->max_output - stream->produced : 0;
        if (*out_len > room) {
            *out_len = room;
            if (s == CU_OK || s == CU_ERR_BUF_TOO_SMALL) {
                cu_set_last_errorf("%s: decompressed output exceeds limit %zu",
                                   stream->vtbl->name, stream->max_output);
                s = CU_ERR_SIZE_LIMIT;
            }
        }
    }
    stream->produced += *out_len;
    return s;
}

/* Every call on a failed stream repeats its error. */
static cu_status_t dstream_failed(const cu_decompress_stream_t* stream) {
    if (stream->failed != CU_OK) {
        cu_set_last_errorf("%s: stream failed earlier (%s)", stream->vtbl->name,
                           cu_strerror(stream->failed));
    }
    return stream->failed;
}

cu_status_t cu_decompress_stream_write(
    cu_decompress_stream_t* stream,
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    if (!stream || !out_len)            return CU_ERR_INVALID_ARG;
    if (in_len > 0 && !in)              return CU_ERR_INVALID_ARG;
    if (*out_len > 0 && !out)           return CU_ERR_INVALID_ARG;
    /* Every return without calling the codec wrote nothing. */
    size_t out_cap = *out_len;
    *out_len = 0;
    if (stream->eof) {
        if (in_len > 0) {
            cu_set_last_error("write after end of stream; the input is not consumed");
            return CU_ERR_STREAM_FINISHED;
        }
        return CU_OK;
    }
    if (stream->finished) {
        cu_set_last_error("write to finished decompress stream");
        return CU_ERR_STREAM_FINISHED;
    }
    if (stream->failed != CU_OK) return dstream_failed(stream);
    if (in_len > 0 && dstream_ended(stream)) {
        cu_set_last_error("write after end of stream before its output was drained; "
                          "drain with empty writes or finish");
        return CU_ERR_STREAM_STATE;
    }

    cu_clear_last_error();
    *out_len = out_cap;
    cu_status_t s = stream->vtbl->decompress_stream_write(stream->state, in, in_len, out, out_len);
    return dstream_settle(stream, dstream_limit(stream, s, out_len));
}

cu_status_t cu_decompress_stream_finish(
    cu_decompress_stream_t* stream,
    uint8_t* out, size_t* out_len
) {
    if (!stream || !out_len)            return CU_ERR_INVALID_ARG;
    if (*out_len > 0 && !out)           return CU_ERR_INVALID_ARG;
    /* Every return without calling the codec wrote nothing. */
    size_t out_cap = *out_len;
    *out_len = 0;

    if (stream->eof) {
        stream->finished = 1;
        return CU_OK;
    }
    if (stream->failed != CU_OK) return dstream_failed(stream);
    *out_len = out_cap;

    cu_clear_last_error();
    /* Single mode past the end: only output is left, which a write drains. */
    cu_status_t s = dstream_ended(stream)
        ? stream->vtbl->decompress_stream_write(stream->state, NULL, 0, out, out_len)
        : stream->vtbl->decompress_stream_finish(stream->state, out, out_len);
    s = dstream_settle(stream, dstream_limit(stream, s, out_len));
    if (s == CU_OK) {
        stream->finished = 1;
    }
    return s;
}

cu_status_t cu_decompress_stream_set_max_output(cu_decompress_stream_t* stream, size_t bytes) {
    if (!stream) return CU_ERR_INVALID_ARG;
    stream->max_output = bytes;
    return CU_OK;
}

int cu_decompress_stream_eof(const cu_decompress_stream_t* stream) {
    return stream ? stream->eof : 0;
}

cu_status_t cu_decompress_stream_unused(
    const cu_decompress_stream_t* stream,
    const uint8_t** data, size_t* len
) {
    if (!stream || !data || !len) return CU_ERR_INVALID_ARG;
    *data = NULL;
    *len = 0;
    if (stream->eof) {
        stream->vtbl->decompress_stream_end(stream->state, data, len);
        if (*len == 0) *data = NULL;
    }
    return CU_OK;
}

void cu_decompress_stream_destroy(cu_decompress_stream_t* stream) {
    if (!stream) return;
    if (stream->vtbl && stream->state) {
        stream->vtbl->decompress_stream_destroy(stream->state);
    }
    free(stream);
}

/* ============================================================================
 * One-shot decompress into a library-allocated buffer
 * ============================================================================
 *
 * Growth policy for formats with no recorded size: one malloc'd buffer that starts at four times the input (at least CU_ALLOC_MIN_CAP) and doubles once less than 1/CU_ALLOC_GROW_FRACTION of it is free, so a codec is never handed a window under CU_ALLOC_MIN_CAP / CU_ALLOC_GROW_FRACTION and a small output never makes it grow at all (a fixed free-space threshold would double the minimum buffer for a 4 KB result, which costs a `memory.grow` per call in WASM). Growing with realloc lets the allocator extend or remap a large block instead of copying it. With a cap the buffer never exceeds cap + 1 bytes: the stream rejects output past the cap, and the one spare byte lets output of exactly the cap finish without another grow. Input is fed in CU_ALLOC_FEED chunks so the decoder never holds a second copy of all of it, and the chunks are big enough that the per-call cost doesn't show (64 KiB ones cost zlib about 4%).
 *
 * The result is shrunk only when more than half the buffer is unused, which is when the slack matters (a small output in the minimum buffer, a start far above the real size). Shrinking costs a copy on some allocators (macOS) and about 5% for the fastest codecs, and the unused tail of a big buffer is untouched memory, so a typical result is returned as is.
 */
#define CU_ALLOC_MIN_CAP        ((size_t)64 * 1024)
#define CU_ALLOC_GROW_FRACTION  8
#define CU_ALLOC_FEED       ((size_t)256 * 1024)

/* Shrinks `buf` to `n` bytes (at least 1, so it stays a valid allocation). A failed shrink keeps the larger block. */
static uint8_t* shrink_to(uint8_t* buf, size_t n) {
    uint8_t* p = realloc(buf, n ? n : 1);
    return p ? p : buf;
}

static cu_status_t decompress_alloc_oom(size_t bytes) {
    cu_set_last_errorf("out of memory allocating %zu bytes of decompressed output", bytes);
    return CU_ERR_OOM;
}

/* Streams `in` into a growing buffer, bounded by `limit` (0 = none). */
static cu_status_t decompress_alloc_stream(
    cu_algorithm_t algo,
    const uint8_t* in, size_t in_len,
    size_t limit,
    uint8_t** out, size_t* out_len
) {
    cu_decompress_stream_t* ds = NULL;
    cu_status_t s = cu_decompress_stream_create(algo, &ds);
    if (s != CU_OK) return s;
    cu_decompress_stream_set_max_output(ds, limit);

    size_t ceiling = limit == 0 || limit == SIZE_MAX ? SIZE_MAX : limit + 1;
    size_t cap = in_len > SIZE_MAX / 4 ? SIZE_MAX : in_len * 4;
    if (cap < CU_ALLOC_MIN_CAP) cap = CU_ALLOC_MIN_CAP;
    if (cap > ceiling) cap = ceiling;
    uint8_t* buf = malloc(cap);
    /* A start that can't be had (a huge input with no cap) is retried smaller; the buffer grows from there as output appears. */
    while (!buf && cap > CU_ALLOC_MIN_CAP) {
        cap = cap / 2 < CU_ALLOC_MIN_CAP ? CU_ALLOC_MIN_CAP : cap / 2;
        buf = malloc(cap);
    }
    if (!buf) {
        cu_decompress_stream_destroy(ds);
        return decompress_alloc_oom(cap);
    }

    size_t pos = 0;
    const uint8_t* src = in;
    size_t src_left = in_len;
    size_t chunk = src_left < CU_ALLOC_FEED ? src_left : CU_ALLOC_FEED;
    int finishing = 0;
    for (;;) {
        if (cap - pos < cap / CU_ALLOC_GROW_FRACTION && cap < ceiling) {
            size_t grown = cap > SIZE_MAX / 2 ? SIZE_MAX : cap * 2;
            if (grown > ceiling) grown = ceiling;
            uint8_t* p = realloc(buf, grown);
            /* Keep going in what is left if it can't grow. */
            if (p) { buf = p; cap = grown; }
            else if (cap == pos) { s = decompress_alloc_oom(grown); break; }
        }
        size_t n = cap - pos;
        s = finishing ? cu_decompress_stream_finish(ds, buf + pos, &n)
                      : cu_decompress_stream_write(ds, chunk ? src : NULL, chunk, buf + pos, &n);
        pos += n;
        /* A write retains its input even when it reports the output full, so the rest is drained with empty writes before the next chunk. */
        if (chunk) {
            src += chunk;
            src_left -= chunk;
            chunk = 0;
        }
        if (s == CU_ERR_BUF_TOO_SMALL) {
            if (cap == pos && cap == ceiling) {
                /* No room, and none can be made: only reachable with no cap. */
                s = decompress_alloc_oom(cap);
                break;
            }
            continue;
        }
        if (s != CU_OK || finishing) break;
        if (src_left == 0) finishing = 1;
        else chunk = src_left < CU_ALLOC_FEED ? src_left : CU_ALLOC_FEED;
    }
    cu_decompress_stream_destroy(ds);
    if (s != CU_OK) {
        free(buf);
        return s;
    }
    if (cap / 2 > pos) buf = shrink_to(buf, pos);
    *out = buf;
    *out_len = pos;
    return CU_OK;
}

cu_status_t cu_decompress_alloc(
    cu_algorithm_t algo,
    const uint8_t* in, size_t in_len,
    uint8_t** out, size_t* out_len
) {
    if (!out || !out_len)  return CU_ERR_INVALID_ARG;
    *out = NULL;
    *out_len = 0;
    if (in_len > 0 && !in) return CU_ERR_INVALID_ARG;

    const cu_algorithm_vtbl_t* v;
    cu_status_t s = resolve(algo, &v);
    if (s != CU_OK) return s;
    if (!v->decompress) {
        cu_set_last_error("operation unsupported for this algorithm");
        return CU_ERR_UNSUPPORTED_ALGO;
    }

    size_t hint = 0;
    s = cu_decompress_size_hint(algo, in, in_len, &hint);
    if (s == CU_OK) {
        /* The hint comes from the input: if it can't be allocated, or the decode shows it was short, fall through to the stream, which grows only as real output appears. */
        uint8_t* buf = malloc(hint ? hint : 1);
        if (buf) {
            size_t n = hint;
            cu_clear_last_error();
            s = v->decompress(in, in_len, buf, &n);
            if (s == CU_OK) {
                *out = n < hint ? shrink_to(buf, n) : buf;
                *out_len = n;
                return CU_OK;
            }
            free(buf);
            if (s != CU_ERR_SIZE_UNKNOWN && s != CU_ERR_BUF_TOO_SMALL) return s;
        }
    } else if (s != CU_ERR_SIZE_UNKNOWN) {
        return s;
    }

    cu_clear_last_error();
    return decompress_alloc_stream(algo, in, in_len, CU_CAP_LOAD(), out, out_len);
}

void cu_buffer_free(void* buf) {
    free(buf);
}
