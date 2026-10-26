# snappy_core provenance

A first-party C implementation of the Snappy raw block format: a compressor and a decompressor in one self-contained file pair (`snappy_core.c`, `snappy_core.h`) with no dependency on the compress-utils ABI. `src/algorithms/snappy/snappy.c` is the adapter that maps its result codes to `cu_status_t`. The unit is deliberately free of project headers so it can be extracted into its own repository.

## Base

It replaces the vendored pure-C port andikleen/snappy-c at commit `fef67ac6effb40c83fb5e37eaf243381fa056f9b` (a port of snappy 1.1.0, BSD 3-clause, Copyright 2005 Google Inc. and Copyright 2011 Intel Corporation), which used to live in `third_party/snappy/`. snappy-c's `snappy.c` and our portability shim `compat.h` are gone from the tree. The reason for replacing it is the C ecosystem benchmark (see the "Findings from the C ecosystem benchmarks" section of `TODO.md`): snappy-c's decoder was about 2x and its encoder about 1.4x slower than Google's C++ snappy.

The wire format is the frozen Snappy raw block format (`format_description.txt` in google/snappy): a varint of the uncompressed length, then literal and copy elements. There is no upstream to track and nothing to bump.

## What is original

All of the code is written for compress-utils. It is not a transcription or a transpilation of snappy-c or of Google's C++ snappy, and no C++ is involved.

- The decoder is written from the format description: a branchless fast loop (table-driven tag decode, preloaded next tag, fixed 64-byte copies, one unsigned comparison that validates the element), a branchy tier with 16-byte chunk copies and short-offset pattern expansion, and a byte-exact checked tail. Every length, offset and capacity is validated, and the fast tiers rely on explicit input and output slack constants rather than per-element bounds checks. The fast loop is plain structured C (no `goto`); its one literal-versus-copy select is written so that gcc 13 to 15, clang and Apple clang all compile it to a conditional move, with a second form on clang for aarch64.
- The encoder follows the standard Snappy design: 64 KiB fragments, a 16-bit offset hash table sized to the fragment (up to 2^15 entries), a four-byte match test, a skip heuristic that grows geometrically with the distance since the last match, and the usual copy-element splitting. The hash is the same multiplicative hash on every target.
- The decoder and encoder use techniques that are public knowledge from Google's snappy and from the literature on LZ77 decoders and encoders: preloading the next tag to shorten the dependency chain, assembling the next hash input from loads that do not depend on the match length, and branch-free choice between one- and two-byte-offset copies. The code is our own expression of those ideas.
- Portability is by compile-time detection only: NEON or SSE2 for the 16-byte overlapping-safe copy, `__builtin_ctzll` or `_BitScanForward64` for match lengths, and `memcpy` for every unaligned access. Everything has a portable scalar path. There is no assembly and no per-CPU code path in the encoder.

## Behavior notes

The compressed bytes differ from snappy-c's and from Google's, but they are the same on every target, compiler and optimisation level: the encoder has no CPU-dependent paths, so the output is a function of the input alone. (An earlier version used a hardware CRC32C hash where the target had one; it gained at most 4% on the machines tried and made the output depend on the build, so it was removed.) Every output is a valid block that any Snappy decoder accepts. The size bound is the standard `32 + n + n/6`.

## Verification

`tests/interop/snappy_core_diff_test.cc` links this unit directly against the Google C++ implementation in `third_party/snappy-oracle` (test-only, never shipped). It checks both directions on many inputs, and on corrupted, truncated and generated inputs it requires the same accept or reject and the same bytes as the oracle, with the buffers placed against guard pages so any out-of-bounds access faults. The fuzz harness in `tests/fuzz` covers the decoder through the library API.

## License

BSD 3-clause notices from Google and Intel are retained in `LICENSE` because the design and the format knowledge derive from their code, together with compress-utils' own MIT license for the original parts.
