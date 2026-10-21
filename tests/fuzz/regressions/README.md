# Fuzz regression inputs

Each file here is a fuzz input (`[FUZZ_ALGOS index][mode byte][payload]`, see `tests/fuzz/fuzz_algos.h`) that once crashed, hung or tripped a sanitizer, kept after the fix so the bug can't come back. The `fuzz_regressions` ctest replays every file through the harness on every build, with no libFuzzer needed.

Name files `<codec>-<short-description>`, and say in the commit that adds one what bug it reproduces.

| File | Bug |
|------|-----|
| `xz-zero-byte-bomb` | A single `0x00` byte made the xz stream decoder return `CU_ERR_BUF_TOO_SMALL` from `finish` forever (fixed 2026-07, `src/algorithms/xz/xz.c`). |
| `bz2-finish-after-error-inuse16` | In default stream mode, `finish` after a failed write re-entered libbzip2 on its error state: a write past `inUse16[16]`. Errors are now final in every mode (fixed 2026-10, `dstream_settle` in `src/compress_utils.c`). Caught by UBSan builds. |
| `bz2-finish-after-error-origptr` | Same bug, surfacing as a signed-shift overflow of `origPtr` in libbzip2. Caught by UBSan builds. |
