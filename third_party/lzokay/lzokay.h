/* SPDX-License-Identifier: MIT */
#ifndef LZOKAY_H
#define LZOKAY_H

#include <stddef.h>
#include <stdint.h>

/* Results from the bounded raw LZO1X decoder. */
typedef enum {
    LZOKAY_OK,
    LZOKAY_TRUNCATED,
    LZOKAY_OUTPUT_FULL,
    LZOKAY_SIZE_LIMIT,
    LZOKAY_INVALID,
} lzokay_status_t;

/* Decode one block; NULL output validates and counts without allocating.
 * On success, produced contains the actual decoded length. */
lzokay_status_t lzokay_decode(const uint8_t* input, size_t length,
                             uint8_t* output, size_t capacity, size_t* produced);

#endif
