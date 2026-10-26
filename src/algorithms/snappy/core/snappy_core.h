/*
 * snappy_core.h - Snappy raw block format, compressor and decompressor.
 *
 * Self-contained: no dependency on the compress-utils ABI. The adapter in
 * src/algorithms/snappy/snappy.c maps these result codes to cu_status_t. See
 * PROVENANCE.md for the origin of the format knowledge and what is original.
 *
 * Copyright 2011 Google Inc. (format and algorithm design, BSD 3-clause; see
 * LICENSE). Implementation (C) compress-utils contributors.
 */
#ifndef SNAPPY_CORE_H
#define SNAPPY_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SNAPPY_CORE_OK = 0,
    SNAPPY_CORE_TRUNCATED,    /* input ends inside the header or an element */
    SNAPPY_CORE_CORRUPT,      /* malformed block (bad varint, offset, length) */
    SNAPPY_CORE_OUTPUT_FULL,  /* output capacity smaller than the declared length */
    SNAPPY_CORE_NOMEM         /* working-memory allocation failed (compress) */
} snappy_core_status_t;

/* Reads the uncompressed length from the block's leading varint (1-5 bytes,
 * value <= UINT32_MAX). Does not look at the rest of the block. */
snappy_core_status_t snappy_core_uncompressed_length(
    const uint8_t* in, size_t in_len, size_t* out_len);

/* Decodes one block. `out_cap` is the capacity of `out`; the block's declared
 * length must fit in it, else SNAPPY_CORE_OUTPUT_FULL (with *out_len set to the
 * declared length). Never reads outside in[0, in_len) or writes outside
 * out[0, out_cap), whatever the input. On success *out_len is the declared
 * length and the block was consumed exactly; on any failure the contents of
 * `out` are unspecified. */
snappy_core_status_t snappy_core_decode(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t out_cap, size_t* out_len);

/* Largest possible compressed size of `in_len` input bytes (Snappy's standard
 * bound: 32 + n + n/6). */
size_t snappy_core_max_compressed_length(size_t in_len);

/* Compresses `in` into one raw Snappy block. `out_cap` must be at least
 * snappy_core_max_compressed_length(in_len), else SNAPPY_CORE_OUTPUT_FULL.
 * Input longer than UINT32_MAX bytes is refused with SNAPPY_CORE_CORRUPT (the
 * format cannot describe it). Allocates its working memory (at most 64 KiB) and
 * can return SNAPPY_CORE_NOMEM. The output is a valid block but is not
 * byte-identical to any other encoder's. */
snappy_core_status_t snappy_core_encode(
    const uint8_t* in, size_t in_len,
    uint8_t* out, size_t out_cap, size_t* out_len);

#ifdef __cplusplus
}
#endif

#endif /* SNAPPY_CORE_H */
