/*
 * Nonterminal flush (cu_compress_stream_flush), issue #54: after a flush, a
 * decoder given exactly the bytes produced so far recovers all input written
 * so far, without further input or finish, and the same open stream keeps
 * going. Checked for every codec that can flush, with output buffers from
 * 7 bytes to 64 KiB on both ends, the issue's four transport profiles, and its message
 * patterns: payloads around 64 KiB and 128 KiB, a large message just past
 * 64 KiB after a small record, and many small messages. After each
 * checkpoint the decoder also gets three empty writes, which must succeed
 * and emit nothing.
 */
#include "compress_utils.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value)                                                                \
    do {                                                                            \
        if (!(value)) {                                                             \
            fprintf(stderr, "flush check failed: %s:%d: %s (%s)\n", __FILE__,       \
                    __LINE__, #value, cu_last_error());                             \
            exit(1);                                                                \
        }                                                                           \
    } while (0)

static int failures = 0;

typedef enum { WRITE, FLUSH, FINISH } op_t;

/* Runs one compress-stream operation to completion, appending its output. */
static void run(cu_compress_stream_t* s, op_t op, const uint8_t* in, size_t n, size_t cap,
                uint8_t* dst, size_t* total, size_t max) {
    static uint8_t buf[65536];
    cu_status_t st;
    do {
        size_t got = cap;
        st = op == WRITE    ? cu_compress_stream_write(s, in, n, buf, &got)
             : op == FLUSH  ? cu_compress_stream_flush(s, buf, &got)
                            : cu_compress_stream_finish(s, buf, &got);
        in = NULL;
        n = 0;
        CHECK(st == CU_OK || st == CU_ERR_BUF_TOO_SMALL);
        CHECK(got <= cap && *total + got <= max);
        memcpy(dst + *total, buf, got);
        *total += got;
    } while (st == CU_ERR_BUF_TOO_SMALL);
}

/* How compressed bytes reach the decoder: piece sizes, cycled. The cycle
 * position carries across batches, as on a real connection. 0 = each batch
 * in one piece. */
typedef struct {
    const char* name;
    const size_t* steps;
    size_t count;
} transport_t;

static const size_t WHOLE[] = {0}, ONE[] = {1}, PAGE[] = {4096}, IRREGULAR[] = {1, 3, 7, 97, 4093};
static const transport_t TRANSPORTS[] = {
    {"whole", WHOLE, 1},
    {"4096", PAGE, 1},
    {"1,3,7,97,4093", IRREGULAR, 5},
    {"1", ONE, 1},
};

/* Writes `len` bytes to the decoder, draining through `cap`-byte output
 * buffers until CU_OK, appending output. */
static void decode_piece(cu_decompress_stream_t* d, const uint8_t* p, size_t len, size_t cap,
                         uint8_t* dst, size_t* total, size_t max) {
    static uint8_t buf[65536];
    cu_status_t st;
    do {
        size_t got = cap;
        st = cu_decompress_stream_write(d, p, len, buf, &got);
        p = NULL;
        len = 0;
        CHECK(st == CU_OK || st == CU_ERR_BUF_TOO_SMALL);
        CHECK(*total + got <= max);
        memcpy(dst + *total, buf, got);
        *total += got;
    } while (st == CU_ERR_BUF_TOO_SMALL);
}

/* Delivers one batch of compressed bytes the way `t` slices them. */
static void feed(cu_decompress_stream_t* d, const uint8_t* in, size_t n, const transport_t* t,
                 size_t* cursor, size_t cap, uint8_t* dst, size_t* total, size_t max) {
    for (size_t off = 0; off < n;) {
        size_t step = t->steps[*cursor % t->count];
        size_t len = step == 0 || step > n - off ? n - off : step;
        if (step != 0) ++*cursor;
        decode_piece(d, in + off, len, cap, dst, total, max);
        off += len;
    }
}

/* Message contents: 0 repeated 'A', 1 pseudorandom, 2 text-like, 3 'B'. */
static void make(int kind, uint8_t* p, size_t n, unsigned seed) {
    unsigned x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        p[i] = kind == 0   ? 'A'
               : kind == 1 ? (uint8_t)(x >> 16)
               : kind == 2 ? (uint8_t)("flush me "[i % 9] + (i / 777) % 3)
                           : 'B';
    }
}

typedef struct {
    size_t size;
    int kind;
} message_t;

enum { MAX_MESSAGES = 128 };

typedef struct {
    const char* name;
    message_t messages[MAX_MESSAGES];
    size_t count;
} sequence_t;

static sequence_t SEQUENCES[3];

static void build_sequences(void) {
    /* Repeated and pseudorandom payloads around 64 KiB and 128 KiB. */
    sequence_t* mix = &SEQUENCES[0];
    mix->name = "64K/128K mix";
    const message_t sizes[] = {{131072, 0}, {65537, 1}, {37, 3}, {131073, 1},
                               {1, 2},      {65536, 0}, {4096, 2}};
    memcpy(mix->messages, sizes, sizeof(sizes));
    mix->count = sizeof(sizes) / sizeof(sizes[0]);

    /* A small record, then a message one byte past 64 KiB, then a short one:
     * the issue's boundary case, where receivers came up one byte short. */
    sequence_t* boundary = &SEQUENCES[1];
    boundary->name = "record, 65537, 37";
    boundary->messages[0] = (message_t){422, 1};
    boundary->messages[1] = (message_t){65537, 0};
    boundary->messages[2] = (message_t){37, 3};
    boundary->count = 3;

    /* Request/response traffic: a record, then many small messages. */
    sequence_t* traffic = &SEQUENCES[2];
    traffic->name = "record + 100 x 98";
    traffic->messages[0] = (message_t){422, 1};
    for (size_t i = 1; i <= 100; i++) traffic->messages[i] = (message_t){98, (int)(i % 3)};
    traffic->count = 101;
}

/* Writes each message, flushes, and checks the decoder has all of it. Both
 * ends drain through `cap`-byte output buffers. */
static void check_checkpoints(cu_algorithm_t algo, const sequence_t* seq, size_t cap,
                              const transport_t* t) {
    enum { MAX = 1 << 20 };
    static uint8_t plain[MAX], wire[MAX + MAX / 2], back[MAX];
    size_t plain_len = 0, wire_len = 0, sent = 0, back_len = 0, cursor = 0;
    cu_compress_stream_t* cs = NULL;
    cu_decompress_stream_t* ds = NULL;
    CHECK(cu_compress_stream_create(algo, 5, &cs) == CU_OK);
    CHECK(cu_decompress_stream_create(algo, &ds) == CU_OK);
    for (size_t m = 0; m < seq->count; m++) {
        const message_t* msg = &seq->messages[m];
        make(msg->kind, plain + plain_len, msg->size, (unsigned)m + 1);
        run(cs, WRITE, plain + plain_len, msg->size, cap, wire, &wire_len, sizeof(wire));
        plain_len += msg->size;
        run(cs, FLUSH, NULL, 0, cap, wire, &wire_len, sizeof(wire));
        feed(ds, wire + sent, wire_len - sent, t, &cursor, cap, back, &back_len, sizeof(back));
        sent = wire_len;
        if (back_len != plain_len || memcmp(back, plain, plain_len)) {
            fprintf(stderr, "%s [%s] (out %zu, transport %s): checkpoint %zu decoded %zu of %zu "
                    "bytes\n", cu_algorithm_name(algo), seq->name, cap, t->name, m, back_len,
                    plain_len);
            failures++;
            goto done;
        }
        /* Empty writes, as from a receiver polling for more, succeed and emit
         * nothing: the checkpoint was complete without them. */
        for (int i = 0; i < 3; i++) decode_piece(ds, NULL, 0, cap, back, &back_len, sizeof(back));
        CHECK(back_len == plain_len);
        /* A second flush with nothing new emits nothing. */
        size_t before = wire_len;
        run(cs, FLUSH, NULL, 0, cap, wire, &wire_len, sizeof(wire));
        CHECK(wire_len == before);
    }
    run(cs, FINISH, NULL, 0, cap, wire, &wire_len, sizeof(wire));
    feed(ds, wire + sent, wire_len - sent, t, &cursor, cap, back, &back_len, sizeof(back));
    {
        static uint8_t tail[65536];
        size_t got = sizeof(tail);
        CHECK(cu_decompress_stream_finish(ds, tail, &got) == CU_OK && got == 0);
    }
    CHECK(back_len == plain_len && !memcmp(back, plain, plain_len));

    /* The flushed stream is one ordinary frame to one-shot decoding too. */
    {
        size_t len = sizeof(back);
        CHECK(cu_decompress(algo, wire, wire_len, back, &len) == CU_OK);
        CHECK(len == plain_len && !memcmp(back, plain, plain_len));
    }
done:
    cu_compress_stream_destroy(cs);
    cu_decompress_stream_destroy(ds);
}

/* Protocol: writes are rejected mid-flush, finish completes a pending flush,
 * and flushing a finished stream fails. */
static void check_protocol(cu_algorithm_t algo) {
    static uint8_t plain[200000], wire[400000], back[200000];
    make(1, plain, sizeof(plain), 9);
    cu_compress_stream_t* cs = NULL;
    CHECK(cu_compress_stream_create(algo, 5, &cs) == CU_OK);
    size_t wire_len = 0;
    run(cs, WRITE, plain, sizeof(plain), 65536, wire, &wire_len, sizeof(wire));
    uint8_t one[1];
    size_t got = 1;
    /* 200 KB of random input can't flush into one byte. */
    CHECK(cu_compress_stream_flush(cs, one, &got) == CU_ERR_BUF_TOO_SMALL);
    memcpy(wire + wire_len, one, got);
    wire_len += got;
    got = 64;
    CHECK(cu_compress_stream_write(cs, plain, 1, wire, &got) == CU_ERR_STREAM_STATE);
    run(cs, FINISH, NULL, 0, 65536, wire, &wire_len, sizeof(wire));
    got = 64;
    CHECK(cu_compress_stream_flush(cs, wire, &got) == CU_ERR_STREAM_FINISHED);
    cu_compress_stream_destroy(cs);
    size_t len = sizeof(back);
    CHECK(cu_decompress(algo, wire, wire_len, back, &len) == CU_OK);
    CHECK(len == sizeof(plain) && !memcmp(back, plain, len));
}

int main(void) {
    const cu_algorithm_t algos[] = {CU_ALGO_ZSTD,    CU_ALGO_LZ4,    CU_ALGO_ZLIB,
                                    CU_ALGO_GZIP,    CU_ALGO_DEFLATE, CU_ALGO_BROTLI,
                                    CU_ALGO_XZ};
    const size_t caps[] = {65536, 4096, 7};
    const size_t transports = sizeof(TRANSPORTS) / sizeof(TRANSPORTS[0]);
    build_sequences();
    for (size_t a = 0; a < sizeof(algos) / sizeof(algos[0]); a++) {
        if (!cu_algorithm_available(algos[a])) continue;
        int before = failures;
        for (size_t q = 0; q < sizeof(SEQUENCES) / sizeof(SEQUENCES[0]); q++)
            for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++)
                for (size_t t = 0; t < transports; t++) {
                    /* 1-byte transport with tiny output buffers only repeats
                     * the 64 KiB case at many times the cost. */
                    if (TRANSPORTS[t].steps == ONE && caps[c] != 65536) continue;
                    check_checkpoints(algos[a], &SEQUENCES[q], caps[c], &TRANSPORTS[t]);
                }
        check_protocol(algos[a]);
        printf("%-8s %s\n", cu_algorithm_name(algos[a]), failures == before ? "ok" : "FAILED");
    }

    /* Snappy writes one raw block at finish, and bzip2 byte-aligns only at
     * end of stream, so neither can flush mid-stream. */
    const cu_algorithm_t no_flush[] = {CU_ALGO_SNAPPY, CU_ALGO_BZ2};
    for (size_t a = 0; a < sizeof(no_flush) / sizeof(no_flush[0]); a++) {
        if (!cu_algorithm_available(no_flush[a])) continue;
        cu_compress_stream_t* cs = NULL;
        CHECK(cu_compress_stream_create(no_flush[a], 5, &cs) == CU_OK);
        uint8_t out[64];
        size_t got = sizeof(out);
        CHECK(cu_compress_stream_write(cs, (const uint8_t*)"abc", 3, out, &got) == CU_OK);
        got = sizeof(out);
        CHECK(cu_compress_stream_flush(cs, out, &got) == CU_ERR_UNSUPPORTED_ALGO);
        cu_compress_stream_destroy(cs);
    }

    if (failures) {
        fprintf(stderr, "%d flush failures\n", failures);
        return 1;
    }
    printf("OK\n");
    return 0;
}
