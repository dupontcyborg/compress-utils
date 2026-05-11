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

/* ============================================================================
 * Version
 * ============================================================================ */

#define CU_STR_(x) #x
#define CU_STR(x) CU_STR_(x)

const char* cu_version(void) {
    return CU_STR(CU_VERSION_MAJOR) "." CU_STR(CU_VERSION_MINOR) "." CU_STR(CU_VERSION_PATCH);
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

/* Default: 1 GiB. Atomic-ish via word-sized writes; we don't bother with
 * <stdatomic.h> here — the worst case from a torn read is a stale cap
 * being applied for one call. */
#define CU_DEFAULT_MAX_DECOMPRESSED_SIZE ((size_t)1024 * 1024 * 1024)

static size_t g_max_decompressed_size = CU_DEFAULT_MAX_DECOMPRESSED_SIZE;

void cu_set_max_decompressed_size(size_t bytes) {
    g_max_decompressed_size = bytes;
}

size_t cu_get_max_decompressed_size(void) {
    return g_max_decompressed_size;
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
    if (!v) return 0;
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
    return v->decompress_size_hint(in, in_len, out_size);
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
};

struct cu_decompress_stream {
    const cu_algorithm_vtbl_t* vtbl;
    void* state;
    int finished;
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

    cu_clear_last_error();
    return stream->vtbl->compress_stream_write(stream->state, in, in_len, out, out_len);
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
    if (!out_stream) return CU_ERR_INVALID_ARG;
    *out_stream = NULL;

    const cu_algorithm_vtbl_t* v;
    cu_status_t s = resolve(algo, &v);
    if (s != CU_OK) return s;

    cu_decompress_stream_t* stream = calloc(1, sizeof(*stream));
    if (!stream) {
        cu_set_last_error("out of memory allocating cu_decompress_stream_t");
        return CU_ERR_OOM;
    }
    stream->vtbl = v;

    cu_clear_last_error();
    s = v->decompress_stream_create(&stream->state);
    if (s != CU_OK) {
        free(stream);
        return s;
    }

    *out_stream = stream;
    return CU_OK;
}

cu_status_t cu_decompress_stream_write(
    cu_decompress_stream_t* stream,
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t* out_len
) {
    if (!stream || !out_len)            return CU_ERR_INVALID_ARG;
    if (in_len > 0 && !in)              return CU_ERR_INVALID_ARG;
    if (*out_len > 0 && !out)           return CU_ERR_INVALID_ARG;
    if (stream->finished) {
        cu_set_last_error("write to finished decompress stream");
        return CU_ERR_STREAM_FINISHED;
    }

    cu_clear_last_error();
    return stream->vtbl->decompress_stream_write(stream->state, in, in_len, out, out_len);
}

cu_status_t cu_decompress_stream_finish(
    cu_decompress_stream_t* stream,
    uint8_t* out, size_t* out_len
) {
    if (!stream || !out_len)            return CU_ERR_INVALID_ARG;
    if (*out_len > 0 && !out)           return CU_ERR_INVALID_ARG;

    cu_clear_last_error();
    cu_status_t s = stream->vtbl->decompress_stream_finish(stream->state, out, out_len);
    if (s == CU_OK) {
        stream->finished = 1;
    }
    return s;
}

void cu_decompress_stream_destroy(cu_decompress_stream_t* stream) {
    if (!stream) return;
    if (stream->vtbl && stream->state) {
        stream->vtbl->decompress_stream_destroy(stream->state);
    }
    free(stream);
}
