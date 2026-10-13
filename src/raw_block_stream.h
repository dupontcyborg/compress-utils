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
/* Resumes exactly where input or output last ran out. */
static cu_status_t raw_run(raw_decoder_t* stream, uint8_t* output, size_t* output_length,
                           int final) {
    size_t capacity = *output_length, written = 0;
    unsigned byte;
    *output_length = 0;
    if (stream->error) return stream->error;
    for (;;) {
        switch (stream->phase) {
            case RAW_FIRST:
                if (!raw_read(stream, &byte)) goto need_input;
                if (byte > 17) {
                    stream->remaining = byte - 17;
                    stream->next_context = stream->remaining < 4 ? (unsigned)stream->remaining : 4;
                    stream->trailing_literals = 1;
                    stream->phase = RAW_LITERAL;
                } else
                    raw_lzo_opcode(stream, byte);
                break;
            case RAW_TOKEN:
                if (!raw_read(stream, &stream->token)) goto need_input;
                stream->remaining = stream->token >> 4;
                stream->final_literals = stream->remaining;
                stream->phase = stream->remaining == 15 ? RAW_LITERAL_EXT : RAW_LITERAL;
                break;
            case RAW_LITERAL_EXT:
            case RAW_MATCH_EXT:
            case RAW_LZO_EXT: {
                if (!raw_read(stream, &byte)) goto need_input;
                size_t* length =
                    stream->phase == RAW_LZO_EXT ? &stream->extension : &stream->remaining;
                size_t increment = stream->phase == RAW_LZO_EXT && !byte ? 255 : byte;
                if (*length > SIZE_MAX - increment) goto size_error;
                *length += increment;
                if (raw_check_length(stream, *length) != CU_OK) goto failed;
                if (stream->phase == RAW_LZO_EXT ? byte != 0 : byte != 255) {
                    if (stream->phase == RAW_LITERAL_EXT) {
                        stream->final_literals = stream->remaining;
                        stream->phase = RAW_LITERAL;
                    } else if (stream->phase == RAW_MATCH_EXT) {
                        if (raw_start_match(stream) != CU_OK) goto failed;
                    } else {
                        stream->remaining = stream->extension;
                        stream->phase = stream->token < 16 ? RAW_LITERAL : RAW_LZO_OFFSET;
                    }
                }
                break;
            }
            case RAW_LITERAL:
                if (raw_check_length(stream, stream->remaining) != CU_OK) goto failed;
                if (!stream->remaining) {
                    if (!stream->lzo)
                        stream->phase = RAW_OFFSET_LOW;
                    else {
                        stream->context = stream->trailing_literals ? stream->next_context : 4;
                        stream->phase = RAW_LZO_OP;
                    }
                    break;
                }
                if (stream->input_offset == stream->input_length) goto need_input;
                if (written == capacity) goto need_output;
                raw_read(stream, &byte);
                output[written++] = (uint8_t)byte;
                stream->history[stream->total++ & (RAW_HISTORY_SIZE - 1)] = (uint8_t)byte;
                stream->remaining--;
                break;
            case RAW_OFFSET_LOW:
                if (!raw_read(stream, &stream->word)) goto need_input;
                stream->phase = RAW_OFFSET_HIGH;
                break;
            case RAW_OFFSET_HIGH:
                if (!raw_read(stream, &byte)) goto need_input;
                stream->distance = stream->word | (byte << 8);
                stream->remaining = (stream->token & 15) + 4;
                if ((stream->token & 15) == 15)
                    stream->phase = RAW_MATCH_EXT;
                else if (raw_start_match(stream) != CU_OK)
                    goto failed;
                break;
            case RAW_LZO_OP:
                if (!raw_read(stream, &byte)) goto need_input;
                raw_lzo_opcode(stream, byte);
                break;
            case RAW_LZO_OFFSET:
                if (!raw_read(stream, &byte)) goto need_input;
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
                            if (stream->remaining != 3) goto corrupt;
                            stream->phase = RAW_DONE;
                            break;
                        }
                        stream->distance += 16384;
                    }
                }
                if (raw_start_match(stream) != CU_OK) goto failed;
                break;
            case RAW_MATCH:
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
                if (written == capacity) goto need_output;
                byte = stream->history[(stream->total - stream->distance) & (RAW_HISTORY_SIZE - 1)];
                output[written++] = (uint8_t)byte;
                stream->history[stream->total++ & (RAW_HISTORY_SIZE - 1)] = (uint8_t)byte;
                stream->remaining--;
                break;
            case RAW_DONE:
                if (stream->input_offset != stream->input_length) goto corrupt;
                raw_release_input(stream);
                *output_length = written;
                return CU_OK;
        }
    }
need_input:
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
need_output:
    *output_length = written;
    return CU_ERR_BUF_TOO_SMALL;
size_error:
    raw_fail(stream, CU_ERR_SIZE_LIMIT);
    goto failed;
corrupt:
    raw_fail(stream, CU_ERR_DECOMPRESSION);
failed:
    *output_length = written;
    return stream->error;
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
