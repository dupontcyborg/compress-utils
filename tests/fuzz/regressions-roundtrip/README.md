# Round-trip fuzz regression inputs

Each file here is an input for `fuzz_roundtrip` (`[FUZZ_ALGOS index][level byte][mode byte][data]`, see `tests/fuzz/fuzz_roundtrip.c`) that once crashed, hung or failed the round trip, kept after the fix so the bug can't come back. The `fuzz_roundtrip_regressions` ctest replays every file on every build, with no libFuzzer needed.

Name files `<codec>-<short-description>`, and say in the commit that adds one what bug it reproduces.

| File | Bug |
|------|-----|
| `zlib-flush-fills-buffer-exactly` | A nonterminal flush that exactly filled the caller's output buffer returned `CU_ERR_BUF_TOO_SMALL`, so the drain call made zlib append another flush marker, and with an output buffer under 7 bytes the flush never ended, emitting markers forever (`dfl_cstream_pump` in `src/algorithms/zlib/deflate_backend.h`; same hang on madler zlib and zlib-ng). `dfl_cstream_pump` now asks `deflatePending` whether the flush is complete. |
| `deflate-flush-fills-buffer-exactly` | The same bug through the raw DEFLATE wrapper. |
