# Fuzz regression inputs

Each file here is a fuzz input (`[FUZZ_ALGOS index][mode byte][payload]`, see `tests/fuzz/fuzz_algos.h`) that once crashed, hung or tripped a sanitizer, kept after the fix so the bug can't come back. The `fuzz_regressions` ctest replays every file through the harness on every build, with no libFuzzer needed.

Name files `<codec>-<short-description>`, and say in the commit that adds one what bug it reproduces.

| File | Bug |
|------|-----|
| `xz-zero-byte-bomb` | A single `0x00` byte made the xz stream decoder return `CU_ERR_BUF_TOO_SMALL` from `finish` forever (fixed 2026-07, `src/algorithms/xz/xz.c`). |
