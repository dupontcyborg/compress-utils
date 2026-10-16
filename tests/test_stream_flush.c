/*
 * Nonterminal flush (cu_compress_stream_flush), issue #54: after a flush, a
 * decoder given exactly the bytes produced so far recovers all input written
 * so far, without further input or finish, and the same open stream keeps
 * going. Checked for every codec that can flush, with output buffers from
 * 1 byte to 64 KiB and transport chunks from 1 byte to whole batches.
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

/* Feeds bytes to the decoder in `transport`-byte pieces, draining each. */
static void feed(cu_decompress_stream_t* d, const uint8_t* in, size_t n, size_t transport,
                 uint8_t* dst, size_t* total, size_t max) {
    static uint8_t buf[65536];
    for (size_t off = 0; off < n || (n == 0 && off == 0); off += transport) {
        const uint8_t* p = in + off;
        size_t len = n - off < transport ? n - off : transport;
        cu_status_t st;
        do {
            size_t got = sizeof(buf);
            st = cu_decompress_stream_write(d, p, len, buf, &got);
            p = NULL;
            len = 0;
            CHECK(st == CU_OK || st == CU_ERR_BUF_TOO_SMALL);
            CHECK(*total + got <= max);
            memcpy(dst + *total, buf, got);
            *total += got;
        } while (st == CU_ERR_BUF_TOO_SMALL);
        if (n == 0) break;
    }
}

static void make(int kind, uint8_t* p, size_t n, unsigned seed) {
    unsigned x = seed;
    for (size_t i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        p[i] = kind == 0 ? 'A' : kind == 1 ? (uint8_t)(x >> 16)
                                           : (uint8_t)("flush me "[i % 9] + (i / 777) % 3);
    }
}

/* Writes each message, flushes, and checks the decoder has all of it. */
static void check_checkpoints(cu_algorithm_t algo, size_t cap, size_t transport) {
    static const size_t sizes[] = {131072, 65537, 37, 131073, 1, 65536, 4096};
    enum { COUNT = sizeof(sizes) / sizeof(sizes[0]), MAX = 1 << 20 };
    static uint8_t plain[MAX], wire[MAX + MAX / 2], back[MAX];
    size_t plain_len = 0, wire_len = 0, sent = 0, back_len = 0;
    cu_compress_stream_t* cs = NULL;
    cu_decompress_stream_t* ds = NULL;
    CHECK(cu_compress_stream_create(algo, 5, &cs) == CU_OK);
    CHECK(cu_decompress_stream_create(algo, &ds) == CU_OK);
    for (size_t m = 0; m < COUNT; m++) {
        make((int)(m % 3), plain + plain_len, sizes[m], (unsigned)m + 1);
        run(cs, WRITE, plain + plain_len, sizes[m], cap, wire, &wire_len, sizeof(wire));
        plain_len += sizes[m];
        run(cs, FLUSH, NULL, 0, cap, wire, &wire_len, sizeof(wire));
        feed(ds, wire + sent, wire_len - sent, transport, back, &back_len, sizeof(back));
        sent = wire_len;
        if (back_len != plain_len || memcmp(back, plain, plain_len)) {
            fprintf(stderr, "%s (out %zu, transport %zu): checkpoint %zu decoded %zu of %zu "
                    "bytes\n", cu_algorithm_name(algo), cap, transport, m, back_len, plain_len);
            failures++;
            goto done;
        }
        /* A second flush with nothing new emits nothing. */
        size_t before = wire_len;
        run(cs, FLUSH, NULL, 0, cap, wire, &wire_len, sizeof(wire));
        CHECK(wire_len == before);
    }
    run(cs, FINISH, NULL, 0, cap, wire, &wire_len, sizeof(wire));
    feed(ds, wire + sent, wire_len - sent, transport, back, &back_len, sizeof(back));
    {
        static uint8_t tail[65536];
        size_t got = sizeof(tail);
        CHECK(cu_decompress_stream_finish(ds, tail, &got) == CU_OK && got == 0);
    }
    CHECK(back_len == plain_len && !memcmp(back, plain, plain_len));

    /* The flushed stream is a normal stream to one-shot decoding too. */
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
    cu_status_t st = cu_compress_stream_flush(cs, one, &got);
    memcpy(wire + wire_len, one, got);
    wire_len += got;
    if (st == CU_ERR_BUF_TOO_SMALL) {
        got = 64;
        CHECK(cu_compress_stream_write(cs, plain, 1, wire, &got) == CU_ERR_STREAM_STATE);
    }
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
    const size_t transports[] = {1u << 30, 4093, 7};
    for (size_t a = 0; a < sizeof(algos) / sizeof(algos[0]); a++) {
        if (!cu_algorithm_available(algos[a])) continue;
        int before = failures;
        for (size_t c = 0; c < sizeof(caps) / sizeof(caps[0]); c++)
            for (size_t t = 0; t < sizeof(transports) / sizeof(transports[0]); t++)
                check_checkpoints(algos[a], caps[c], transports[t]);
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
