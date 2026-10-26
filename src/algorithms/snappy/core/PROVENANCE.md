# snappy_core provenance

A first-party C implementation of the Snappy raw block format: a compressor and a decompressor in one self-contained file pair (`snappy_core.c`, `snappy_core.h`) with no dependency on the compress-utils ABI. `src/algorithms/snappy/snappy.c` is the adapter that maps its result codes to `cu_status_t`. The unit is deliberately free of project headers so it can be extracted into its own repository.

## Base

It replaces the vendored pure-C port andikleen/snappy-c at commit `fef67ac6effb40c83fb5e37eaf243381fa056f9b` (a port of snappy 1.1.0, BSD 3-clause, Copyright 2005 Google Inc. and Copyright 2011 Intel Corporation), which used to live in `third_party/snappy/`. snappy-c's `snappy.c` and our portability shim `compat.h` are gone from the tree. The reason for replacing it is the C ecosystem benchmark (see the "Findings from the C ecosystem benchmarks" section of `TODO.md`): snappy-c's decoder was about 2x and its encoder about 1.4x slower than Google's C++ snappy.

The wire format is the frozen Snappy raw block format (`format_description.txt` in google/snappy): a varint of the uncompressed length, then literal and copy elements. There is no upstream to track and nothing to bump.

## What is original

All of the code is written for compress-utils. It is not a transcription or a transpilation of snappy-c or of Google's C++ snappy, and no C++ is involved.

- The decoder is written from the format description: a branchless fast loop (table-driven tag decode, preloaded next tag, fixed 64-byte copies), a branchy tier with 16-byte chunk copies and short-offset pattern expansion, and a byte-exact checked tail. Every length, offset and capacity is validated, and the fast tiers rely on explicit input and output slack constants rather than per-element bounds checks.
- The encoder follows the standard Snappy design: 64 KiB fragments, a 16-bit offset hash table sized to the fragment (up to 2^15 entries), a four-byte match test, a skip heuristic that grows geometrically with the distance since the last match, and the usual copy-element splitting.
- The decoder and encoder use techniques that are public knowledge from Google's snappy and from the literature on LZ77 decoders and encoders: preloading the next tag to shorten the dependency chain, forcing the candidate loads for the next tag with `volatile` reads so the compiler keeps them parallel, assembling the next hash input from loads that do not depend on the match length, branch-free choice between one- and two-byte-offset copies, and a CRC32C hash where the CPU has the instruction. The code is our own expression of those ideas.
- Portability is by compile-time detection only: NEON or SSE2 for the 16-byte overlapping-safe copy, ARMv8 CRC or SSE4.2 CRC for the match hash when `__ARM_FEATURE_CRC32` or `__SSE4_2__` is defined, `__builtin_ctzll` or `_BitScanForward64` for match lengths, and `memcpy` for every unaligned access. Everything has a portable scalar path.

## Behavior notes

The compressed bytes differ from snappy-c's and from Google's, and they can differ between builds: the hash function depends on whether the target has a CRC32C instruction at compile time. Every output is a valid block that any Snappy decoder accepts. The size bound is the standard `32 + n + n/6`.

## Verification

`tests/interop/snappy_core_diff_test.cc` links this unit directly against the Google C++ implementation in `third_party/snappy-oracle` (test-only, never shipped). It checks both directions on many inputs, and on corrupted, truncated and generated inputs it requires the same accept or reject and the same bytes as the oracle, with the buffers placed against guard pages so any out-of-bounds access faults. The fuzz harness in `tests/fuzz` covers the decoder through the library API.

## License

BSD 3-clause notices from Google and Intel are retained in `LICENSE` because the design and the format knowledge derive from their code, together with compress-utils' own MIT license for the original parts.
