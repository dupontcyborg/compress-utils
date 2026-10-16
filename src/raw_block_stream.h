/* SPDX-License-Identifier: MIT
 * LZO transition logic adapted from AxioDL/lzokay, copyright (c) 2018 Jack Andersen,
 * revision db2df1fcbebc2ed06c10f727f72567d40f06a2be (third_party/lzokay/PROVENANCE.md).
 */
/* Incremental raw block decoding. No framing is added. History is fixed at
 * 64 KiB; only the remainder of the current submitted input chunk is retained
 * while the caller drains a small output buffer. No whole-block output buffer.
 * LZO transitions follow the MIT lzokay decoder (see its provenance notice).
 */
#ifndef CU_RAW_BLOCK_STREAM_H
#define CU_RAW_BLOCK_STREAM_H
#include "algorithm_registry.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define RAW_HISTORY_SIZE 65536u

typedef enum {
    RAW_FIRST,
    RAW_TOKEN,
    RAW_LITERAL_EXT,
    RAW_LITERAL,
    RAW_OFFSET_LOW,
    RAW_OFFSET_HIGH,
    RAW_MATCH_EXT,
    RAW_MATCH,
    RAW_LZO_OP,
    RAW_LZO_EXT,
    RAW_LZO_OFFSET,
    RAW_DONE
} raw_phase_t;

typedef struct {
    uint8_t history[RAW_HISTORY_SIZE];
    uint8_t* input;
    size_t input_length, input_offset;
    size_t total, limit, remaining, distance, extension;
    size_t final_literals, last_match_start;
    unsigned token, word, word_bytes, context, next_context;
    int lzo, has_match, trailing_literals;
    raw_phase_t phase;
    cu_status_t error;
} raw_decoder_t;

static void raw_release_input(raw_decoder_t* stream) {
    free(stream->input);
    stream->input = NULL;
    stream->input_length = stream->input_offset = 0;
}
static void raw_stream_destroy(void* state) {
    raw_decoder_t* stream = (raw_decoder_t*)state;
    if (stream) {
        raw_release_input(stream);
        free(stream);
    }
}
static cu_status_t raw_stream_create(int lzo, void** state) {
    raw_decoder_t* stream = (raw_decoder_t*)calloc(1, sizeof(*stream));
    if (!stream) return CU_ERR_OOM;
    stream->lzo = lzo;
    stream->phase = lzo ? RAW_FIRST : RAW_TOKEN;
    stream->limit = cu_get_max_decompressed_size();
    if (!stream->limit) stream->limit = SIZE_MAX;
    *state = stream;
    return CU_OK;
}
static cu_status_t raw_fail(raw_decoder_t* stream, cu_status_t error) {
    stream->error = error;
    raw_release_input(stream);
    cu_set_last_error(error == CU_ERR_TRUNCATED    ? "truncated raw block"
                      : error == CU_ERR_SIZE_LIMIT ? "raw block exceeds output limit"
                                                   : "invalid raw block stream");
    return error;
}
static int raw_read(raw_decoder_t* stream, unsigned* value) {
    if (stream->input_offset == stream->input_length) return 0;
    *value = stream->input[stream->input_offset++];
    return 1;
}
static cu_status_t raw_check_length(raw_decoder_t* stream, size_t length) {
    return length > stream->limit - stream->total ? raw_fail(stream, CU_ERR_SIZE_LIMIT) : CU_OK;
}
static cu_status_t raw_start_match(raw_decoder_t* stream) {
    if (!stream->distance || stream->distance > stream->total ||
        stream->distance >= RAW_HISTORY_SIZE)
        return raw_fail(stream, CU_ERR_DECOMPRESSION);
    if (raw_check_length(stream, stream->remaining) != CU_OK) return stream->error;
    stream->has_match = 1;
    stream->last_match_start = stream->total;
    stream->phase = RAW_MATCH;
    return CU_OK;
}
/* Interpret an LZO opcode once all preceding output has been emitted. */
static void raw_lzo_opcode(raw_decoder_t* stream, unsigned token) {
    stream->token = token;
    stream->word = stream->word_bytes = 0;
    if (token >= 64) {
        stream->remaining = (token >> 5) + 1;
        stream->next_context = token & 3;
        stream->phase = RAW_LZO_OFFSET;
    } else if (token >= 16) {
        stream->remaining = (token & (token >= 32 ? 31 : 7)) + 2;
        if (stream->remaining == 2) {
            stream->extension = token >= 32 ? 33 : 9;
            stream->phase = RAW_LZO_EXT;
        } else
            stream->phase = RAW_LZO_OFFSET;
    } else if (stream->context == 0) {
        stream->remaining = token + 3;
        stream->trailing_literals = 0;
        if (token == 0) {
            stream->extension = 18;
            stream->phase = RAW_LZO_EXT;
        } else
            stream->phase = RAW_LITERAL;
    } else {
        stream->remaining = stream->context == 4 ? 3 : 2;
        stream->next_context = token & 3;
        stream->phase = RAW_LZO_OFFSET;
    }
}
/* History: within one raw_run call, matches copy from the caller's output
 * buffer, which holds everything decoded so far in this call; the ring holds
 * the last RAW_HISTORY_SIZE bytes decoded by earlier calls. raw_run copies the
 * new output into the ring once, on its way out. */

/* Copies a match of `n` bytes to output + written. */
static void raw_copy_match(raw_decoder_t* stream, uint8_t* output, size_t written, size_t n) {
    uint8_t* dst = output + written;
    size_t distance = stream->distance;
    if (distance > written) { /* starts in bytes from earlier calls */
        size_t k = distance - written < n ? distance - written : n;
        size_t at = (stream->total - distance) & (RAW_HISTORY_SIZE - 1);
        size_t first = k < RAW_HISTORY_SIZE - at ? k : RAW_HISTORY_SIZE - at;
        memcpy(dst, stream->history + at, first);
        memcpy(dst + first, stream->history, k - first);
        dst += k;
        n -= k;
        stream->total += k;
    }
    stream->total += n;
    /* The rest is in this call's output. A match longer than its distance
     * repeats its own output, so copy at most `distance` bytes at a time. */
    while (n > 0) {
        size_t piece = n < distance ? n : distance;
        memcpy(dst, dst - distance, piece);
        dst += piece;
        n -= piece;
    }
}
/* Saves the end of this call's output to the ring. */
static void raw_keep_history(raw_decoder_t* stream, const uint8_t* output, size_t written) {
    size_t n = written < RAW_HISTORY_SIZE ? written : RAW_HISTORY_SIZE;
    const uint8_t* bytes = output + written - n;
    size_t at = (stream->total - n) & (RAW_HISTORY_SIZE - 1);
    size_t first = n < RAW_HISTORY_SIZE - at ? n : RAW_HISTORY_SIZE - at;
    memcpy(stream->history + at, bytes, first);
    memcpy(stream->history, bytes + first, n - first);
}
#ifndef CU_RAW_STREAM_LZO_ONLY
/* Decodes whole LZ4 sequences straight from the input while each one fits in
 * the input and output left. It changes nothing until a sequence fits, and
 * leaves anything unusual (a buffer edge, the block's last literals, an
 * invalid offset or the size limit) to the byte-wise path in raw_decode, which
 * resumes at the same sequence and reports any error. */
static void raw_lz4_sequences(raw_decoder_t* stream, uint8_t* output, size_t capacity,
                              size_t* written) {
    const uint8_t* in = stream->input;
    size_t end = stream->input_length;
    for (;;) {
        size_t at = stream->input_offset, room = capacity - *written;
        if (at == end) return;
        unsigned token = in[at++], byte;
        size_t literals = token >> 4;
        if (literals == 15) {
            do {
                if (at == end) return;
                byte = in[at++];
                literals += byte;
                if (literals > room) return;
            } while (byte == 255);
        }
        if (literals > room || literals > end - at) return;
        size_t literal_at = at;
        at += literals;
        if (end - at < 2) return; /* possibly the block's last literals */
        size_t distance = in[at] | (size_t)in[at + 1] << 8, match = (token & 15) + 4;
        at += 2;
        if ((token & 15) == 15) {
            do {
                if (at == end) return;
                byte = in[at++];
                match += byte;
                if (match > room) return;
            } while (byte == 255);
        }
        if (match > room - literals || literals + match > stream->limit - stream->total) return;
        if (!distance || distance > stream->total + literals || distance >= RAW_HISTORY_SIZE)
            return;

        /* Short copies move a fixed 16 bytes when the input and output have
         * room for it: the extra bytes are overwritten next, or lie past what
         * this call reports as written. */
        uint8_t* dst = output + *written;
        if (literals <= 16 && room >= 16 && end - literal_at >= 16)
            memcpy(dst, in + literal_at, 16);
        else
            memcpy(dst, in + literal_at, literals);
        *written += literals;
        stream->total += literals;
        stream->final_literals = literals;
        stream->has_match = 1;
        stream->last_match_start = stream->total;
        stream->distance = distance;
        dst += literals;
        if (match <= 16 && distance >= 16 && distance <= *written && room - literals >= 16) {
            memcpy(dst, dst - distance, 16);
            stream->total += match;
        } else {
            raw_copy_match(stream, output, *written, match);
        }
        *written += match;
        stream->input_offset = at;
    }
}
#endif
/* Exits from raw_decode, each reporting the `written` bytes. */
static cu_status_t raw_stop(raw_decoder_t* stream, size_t written, size_t* output_length) {
    *output_length = written;
    return stream->error;
}
static cu_status_t raw_need_output(size_t written, size_t* output_length) {
    *output_length = written;
    return CU_ERR_BUF_TOO_SMALL;
}
/* Input ran out: wait for more, or at finish accept only a complete block.
 * Raw LZ4 has no end marker; it may end before a match offset once the last
 * sequence ends with at least 5 literals, 12+ bytes after the last match
 * started (or the block has no match at all). */
static cu_status_t raw_need_input(raw_decoder_t* stream, size_t written, size_t* output_length,
                                  int final) {
    raw_release_input(stream);
    *output_length = written;
    if (!final) return CU_OK;
    if (!stream->lzo && stream->phase == RAW_OFFSET_LOW &&
        (!stream->has_match ||
         (stream->final_literals >= 5 && stream->total - stream->last_match_start >= 12))) {
        stream->phase = RAW_DONE;
        return CU_OK;
    }
    return raw_fail(stream, CU_ERR_TRUNCATED);
}
/* Resumes exactly where input or output last ran out. Literal runs and
 * matches are copied in bulk, as far as input and output room allow. */
static cu_status_t raw_decode(raw_decoder_t* stream, uint8_t* output, size_t* output_length,
                              int final) {
    size_t capacity = *output_length, written = 0;
    unsigned byte;
    *output_length = 0;
    if (stream->error) return stream->error;
    for (;;) {
        switch (stream->phase) {
            case RAW_FIRST:
                if (!raw_read(stream, &byte))
                    return raw_need_input(stream, written, output_length, final);
                if (byte > 17) {
                    stream->remaining = byte - 17;
                    stream->next_context = stream->remaining < 4 ? (unsigned)stream->remaining : 4;
                    stream->trailing_literals = 1;
                    stream->phase = RAW_LITERAL;
                } else
                    raw_lzo_opcode(stream, byte);
                break;
            case RAW_TOKEN:
#ifndef CU_RAW_STREAM_LZO_ONLY
                raw_lz4_sequences(stream, output, capacity, &written);
#endif
                if (!raw_read(stream, &stream->token))
                    return raw_need_input(stream, written, output_length, final);
                stream->remaining = stream->token >> 4;
                stream->final_literals = stream->remaining;
                stream->phase = stream->remaining == 15 ? RAW_LITERAL_EXT : RAW_LITERAL;
                break;
            case RAW_LITERAL_EXT:
            case RAW_MATCH_EXT:
            case RAW_LZO_EXT: {
                if (!raw_read(stream, &byte))
                    return raw_need_input(stream, written, output_length, final);
                size_t* length =
                    stream->phase == RAW_LZO_EXT ? &stream->extension : &stream->remaining;
                size_t increment = stream->phase == RAW_LZO_EXT && !byte ? 255 : byte;
                if (*length > SIZE_MAX - increment) {
                    raw_fail(stream, CU_ERR_SIZE_LIMIT);
                    return raw_stop(stream, written, output_length);
                }
                *length += increment;
                if (raw_check_length(stream, *length) != CU_OK)
                    return raw_stop(stream, written, output_length);
                if (stream->phase == RAW_LZO_EXT ? byte != 0 : byte != 255) {
                    if (stream->phase == RAW_LITERAL_EXT) {
                        stream->final_literals = stream->remaining;
                        stream->phase = RAW_LITERAL;
                    } else if (stream->phase == RAW_MATCH_EXT) {
                        if (raw_start_match(stream) != CU_OK)
                            return raw_stop(stream, written, output_length);
                    } else {
                        stream->remaining = stream->extension;
                        stream->phase = stream->token < 16 ? RAW_LITERAL : RAW_LZO_OFFSET;
                    }
                }
                break;
            }
            case RAW_LITERAL: {
                if (raw_check_length(stream, stream->remaining) != CU_OK)
                    return raw_stop(stream, written, output_length);
                if (!stream->remaining) {
                    if (!stream->lzo)
                        stream->phase = RAW_OFFSET_LOW;
                    else {
                        stream->context = stream->trailing_literals ? stream->next_context : 4;
                        stream->phase = RAW_LZO_OP;
                    }
                    break;
                }
                size_t available = stream->input_length - stream->input_offset;
                if (!available) return raw_need_input(stream, written, output_length, final);
                if (written == capacity) return raw_need_output(written, output_length);
                size_t n = stream->remaining;
                if (n > available) n = available;
                if (n > capacity - written) n = capacity - written;
                memcpy(output + written, stream->input + stream->input_offset, n);
                stream->input_offset += n;
                stream->total += n;
                written += n;
                stream->remaining -= n;
                break;
            }
            case RAW_OFFSET_LOW:
                if (!raw_read(stream, &stream->word))
                    return raw_need_input(stream, written, output_length, final);
                stream->phase = RAW_OFFSET_HIGH;
                break;
            case RAW_OFFSET_HIGH:
                if (!raw_read(stream, &byte))
                    return raw_need_input(stream, written, output_length, final);
                stream->distance = stream->word | (byte << 8);
                stream->remaining = (stream->token & 15) + 4;
                if ((stream->token & 15) == 15)
                    stream->phase = RAW_MATCH_EXT;
                else if (raw_start_match(stream) != CU_OK)
                    return raw_stop(stream, written, output_length);
                break;
            case RAW_LZO_OP:
                if (!raw_read(stream, &byte))
                    return raw_need_input(stream, written, output_length, final);
                raw_lzo_opcode(stream, byte);
                break;
            case RAW_LZO_OFFSET:
                if (!raw_read(stream, &byte))
                    return raw_need_input(stream, written, output_length, final);
                if (stream->token >= 64) {
                    stream->distance = (byte << 3) + ((stream->token >> 2) & 7) + 1;
                } else if (stream->token < 16) {
                    stream->distance =
                        (stream->token >> 2) + (byte << 2) + (stream->context == 4 ? 2049 : 1);
                } else {
                    stream->word |= byte << (8 * stream->word_bytes++);
                    if (stream->word_bytes < 2) break;
                    stream->next_context = stream->word & 3;
                    stream->distance = stream->word >> 2;
                    if (stream->token >= 32)
                        stream->distance++;
                    else {
                        stream->distance += (size_t)(stream->token & 8) << 11;
                        if (!stream->distance) {
                            if (stream->remaining != 3) {
                                raw_fail(stream, CU_ERR_DECOMPRESSION);
                                return raw_stop(stream, written, output_length);
                            }
                            stream->phase = RAW_DONE;
                            break;
                        }
                        stream->distance += 16384;
                    }
                }
                if (raw_start_match(stream) != CU_OK)
                    return raw_stop(stream, written, output_length);
                break;
            case RAW_MATCH: {
                if (!stream->remaining) {
                    if (!stream->lzo)
                        stream->phase = RAW_TOKEN;
                    else {
                        stream->remaining = stream->next_context;
                        stream->trailing_literals = 1;
                        stream->phase = RAW_LITERAL;
                    }
                    break;
                }
                if (written == capacity) return raw_need_output(written, output_length);
                size_t n = stream->remaining;
                if (n > capacity - written) n = capacity - written;
                raw_copy_match(stream, output, written, n);
                written += n;
                stream->remaining -= n;
                break;
            }
            case RAW_DONE:
                if (stream->input_offset != stream->input_length) {
                    raw_fail(stream, CU_ERR_DECOMPRESSION);
                    return raw_stop(stream, written, output_length);
                }
                raw_release_input(stream);
                *output_length = written;
                return CU_OK;
        }
    }
}
static cu_status_t raw_run(raw_decoder_t* stream, uint8_t* output, size_t* output_length,
                           int final) {
    cu_status_t status = raw_decode(stream, output, output_length, final);
    raw_keep_history(stream, output, *output_length);
    return status;
}
static cu_status_t raw_stream_write(void* state, const uint8_t* input, size_t length,
                                    uint8_t* output, size_t* output_length) {
    raw_decoder_t* stream = (raw_decoder_t*)state;
    if (stream->error) {
        *output_length = 0;
        return stream->error;
    }
    if (length) {
        if (stream->phase == RAW_DONE) {
            *output_length = 0;
            return raw_fail(stream, CU_ERR_DECOMPRESSION);
        }
        if (stream->input) {
            *output_length = 0;
            return raw_fail(stream, CU_ERR_STREAM_STATE);
        }
        stream->input = (uint8_t*)malloc(length);
        if (!stream->input) {
            *output_length = 0;
            return raw_fail(stream, CU_ERR_OOM);
        }
        memcpy(stream->input, input, length);
        stream->input_length = length;
    }
    return raw_run(stream, output, output_length, 0);
}
static cu_status_t raw_stream_finish(void* state, uint8_t* output, size_t* output_length) {
    return raw_run((raw_decoder_t*)state, output, output_length, 1);
}
#endif
