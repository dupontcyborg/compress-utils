# Fuzz regression inputs

Each file here is a fuzz input (`[FUZZ_ALGOS index][mode byte][payload]`, see `tests/fuzz/fuzz_algos.h`) that once crashed, hung or tripped a sanitizer, kept after the fix so the bug can't come back. The `fuzz_regressions` ctest replays every file through the harness on every build, with no libFuzzer needed.

Name files `<codec>-<short-description>`, and say in the commit that adds one what bug it reproduces.

| File | Bug |
|------|-----|
| `xz-zero-byte-bomb` | A single `0x00` byte made the xz stream decoder return `CU_ERR_BUF_TOO_SMALL` from `finish` forever (fixed 2026-07, `src/algorithms/xz/xz.c`). |
| `bz2-finish-after-error-inuse16` | In default stream mode, `finish` after a failed write re-entered libbzip2 on its error state: a write past `inUse16[16]`. Errors are now final in every mode (fixed 2026-10, `dstream_settle` in `src/compress_utils.c`). Caught by UBSan builds. |
| `bz2-finish-after-error-origptr` | Same bug, surfacing as a signed-shift overflow of `origPtr` in libbzip2. Caught by UBSan builds. |
| `bz2-failed-stream-out-len` | Once a stream had failed (here at a 4 KiB `cu_decompress_stream_set_max_output` limit), `write` and `finish` returned the error without setting `*out_len`, leaving the caller's buffer capacity there, so a caller counting output saw bytes that were never written. Every early return now reports 0 bytes (fixed 2026-10, `src/compress_utils.c`). Caught by the harness's output-limit check. |
| `brotli-empty-input-null-pending`, `bz2-empty-input-null-pending`, `xz-empty-input-null-pending` | Empty input (two bytes: the codec and mode, no payload) through `cu_decompress_alloc`, whose first stream write is empty: the stream decoders computed `pending + pending_off` while `pending` was still NULL, undefined behaviour (fixed 2026-10, `dstream_pump` in `brotli.c` and `bz2.c`, `stream_pump` in `xz.c`). Caught by UBSan builds. |
