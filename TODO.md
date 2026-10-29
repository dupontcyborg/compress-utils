# Compress Utils TODO

## Next up (2026-10)

Direction: stop adding breadth and find out where the library wins. Likely positioning is "every format, including the browser, hardened against untrusted input", to be confirmed by the ecosystem benchmarks.

- [x] **Fix the decompressed-size cap bypass** (see Bugs; fixed 2026-10-22).
- [x] **README: untrusted-input section** (2026-10-22): root README "Untrusted input" section (output cap for every format, stream limits, bounded decoder memory, final errors, `.xz`-only decoding, fuzzing in CI with a regression corpus, pinned offline builds), linked from each binding's "Errors and limits".
- [x] **Deflate backend: zlib, gzip and raw deflate now run on zlib-ng 2.3.3** (2026-10), in its zlib-compatible mode, with per-ISA wrappers instead of a configure step (see `third_party/VENDOR.md`). Level 6 compresses about 3x and decompresses about 2x faster on every machine measured: Apple M-series (49 to 118 MB/s, 453 to 750 MB/s on silesia-mini), Graviton4 (20 to 63, 339 to 615 on dickens), AMD Zen3 (17 to 52, 319 to 655) and Intel Sapphire Rapids (18 to 56, 275 to 610); up to 3.3x compress on 1-4 KB messages at level 1. Runtime dispatch (NEON, AVX2, AVX-512, PCLMUL, ARMv8 CRC) is worth +30% to +54% on decompression and nothing on compression at levels 1 and 6; hardware CRC adds about 4% to gzip decompression. Verified through the C, Go (cgo) and Rust (cc-rs) builds on all three machines, including a plain `-march=x86-64` build where all the AVX code comes from the per-file target wrappers; output is byte-identical between arm64 and x86-64 for zlib. **User-visible:** output bytes differ from madler zlib at every level; level 1 is zlib-ng's quick strategy (about 24% larger output on text for about 2x the speed); the level map is now 1-10 to native 1-7, 9, 9, 9 (native 8 is within 0.1% of 7, and 9 is about 0.5% larger than 7 on text but about 2% smaller on binary), so levels 8-10 are one setting. Zlib-only native library +34 KiB (arm64); WASM zlib 86.9 to 104.4 KiB (size budgets raised). Not run: MSVC/Windows (CI only).
- [ ] **Ecosystem benchmark baselines** — plan in [docs/ecosystem-benchmarks.md](docs/ecosystem-benchmarks.md). The harness supports them (`impl` field, baseline overlays) but no driver exists. Python: cramjam, zstandard, stdlib zlib/bz2/lzma/`compression.zstd` (3.14). JS: fflate, pako, a zstd WASM package. Rust: the native crates. Until this lands, "high-performance" in the README is a claim, not a result.
- [x] **lz4 on WASM uses 64-bit registers** (2026-10). wasm32 has native 64-bit integers but 32-bit pointers, and lz4 keys `reg_t` on `__x86_64__`, so the WASM build used a 32-bit `reg_t` and hashed the fast compressor's input with `LZ4_hash4`, where every native 64-bit build uses `LZ4_hash5`. The WASM build now compiles a patched copy of `third_party/lz4` (`third_party/patches/lz4/0001-wasm-wide-registers.patch`, a one-line `reg_t` change; see `third_party/VENDOR.md`); the vendored tree itself stays verbatim and no other binding sees the patch. Measured at level 1 (7 samples): compress is +20% to +44% on dickens and sao and 0% to 14% on osdb (median over the three large files: Graviton4 +40% on Node and +26% on Bun, Xeon 6975P +30% on Node and +20% on Bun); decompress, HC levels (5 and 10) and inputs under 64 KB are unchanged. **User-visible:** output of our levels 1 and 2 changes (it becomes what native 64-bit builds produce, within a byte or so on the corpus files tried): `sao` 2.9% bigger, `osdb` 0.6% bigger, `dickens` 0.06% smaller at level 1; all output still decodes with the reference implementation. Module sizes: full 123,853 to 126,963 B, compress-only 99,073 to 102,183 B, decompress-only unchanged. Not measured: Apple Silicon, Firefox, Safari, streaming throughput, level 2. Left out on purpose: lz4's fast decode loop (`-DLZ4_FAST_DEC_LOOP=1`: below the bar on large files, -22% to -53% decompress under Bun on x86-64, +11.5% decompress-only module) and a variant that keeps the 4-byte hash (the speedup comes from the hash change, not the wider compare). Validated with 120,000 corrupted and truncated streams against a native build, the repo's WASM suite and the `lz4` CLI; every lz4 bump must re-check (see VENDOR.md).
- [x] **zstd on WASM uses 64-bit registers** (2026-10). wasm32 has native 64-bit integers but 32-bit pointers, and zstd keys its bit-container and reload decisions on `sizeof(size_t)`, so it took the slow 32-bit paths. The WASM build now compiles a patched copy of `third_party/zstd` (`third_party/patches/zstd/0001-wasm-wide-registers.patch`: decoder, encoder flush decisions and the row-match-finder mask; see `third_party/VENDOR.md`); the vendored tree itself stays verbatim and no other binding sees the patch. Measured on dickens, osdb and sao at levels 1 and 5 with 5 samples, median change over the six cells (and range): decompress Apple M-series +15.5% on Node (+9.9 to +16.4) and +17.7% on Bun (+13.4 to +24.3); Graviton4 +12.8% on Node (+8.3 to +14.9) and +18.7% on Bun (+16.3 to +23.3); Xeon 6975P +13.8% on Node (+6.5 to +18.2) and +12.6% on Bun (+10.7 to +18.0). Compress is smaller and concentrated at level 5 and on x86: medians +2.8% to +7.6%, range +1.5% to +23.4%. Compressed output is byte-identical, and small-message gains are smaller. Not measured: levels 8 and above, Firefox, Safari, streaming throughput. Module sizes: full 414,254 to 412,086 B, decompress-only 97,011 to 98,277 B, compress-only 347,704 to 343,954 B. **User-visible:** valid streams decode exactly as before; on malformed streams the decoder now matches a 64-bit native build without the fast Huffman loop (about 0.5% of randomly corrupted streams got a different verdict or output). Left out on purpose: the fast 4-stream Huffman loop (a gain on JavaScriptCore/Graviton, a loss on V8/Graviton, unsupported by upstream with 32-bit pointers), `ZSTD_count` widening and a simd128 variant (no gain). Validated with zstd's own fuzzer, zstreamtest, decodecorpus and regression suite on wasm32-wasi and on i386 with forced 64-bit registers. Every zstd bump must re-run those (see VENDOR.md).
- [x] **brotli on WASM uses 64-bit registers** (2026-10). wasm32 has native 64-bit integers but 32-bit pointers, and brotli picks its register width from the target architecture, so it took the 32-bit bit-reader and match-length paths. The WASM build now defines upstream's `BROTLI_BUILD_64_BIT` on the brotli objects and compiles a patched copy of `third_party/brotli` (`third_party/patches/brotli`: `0001` lets the decoder use its unaligned-load refill on wasm, `0002` keeps the scalar H5/H6 hashers that the flag would otherwise swap for the "SIMD" H58/H68, which on wasm are a scalar SWAR matcher; see `third_party/VENDOR.md`). Measured on dickens, osdb and sao at levels 1, 5, 6 and 7 with 5 samples, median change over the large-file cells: decompress Apple M-series +10.4% on Node and +13.4% on Bun, Graviton4 +10.7% and +9.7%, Xeon 6975P +9.4% and +14.3%. Compress median +1.2% to +6.0% except Graviton4 on Node at -4.8%; the worst single compress cell is -11% (Graviton4 Node, osdb and sao at levels 5 and 6), and decompress has no cell below -6% (Xeon Node, sao level 1, within noise). Compressed output is byte-identical at every cell. Module sizes: full 503,756 to 507,004 B, decompress-only 192,278 to 191,097 B, compress-only 457,453 to 461,882 B. Not measured: Firefox, Safari, streaming throughput, level 10 beyond a 1 MB prefix (compress -8% to +4%, decompress +8% to +16%). The "SIMD" hashers alone are up to 2x faster on binary-like inputs at levels 5 and 6 but 10% to 31% slower on text and small messages, so they stay off. Brotli's own upstream test suite was not run against the patched sources; a bump must re-check output against a native build (see VENDOR.md).
- [x] **xz: one-shot compression caps the dictionary at the input size** (2026-10), as 7-Zip does. liblzma set up the preset's full dictionary on every call (8 MiB at level 6, 64 MiB at level 10) with a match finder whose tables scale with it, which dominated on small inputs. `cu_compress` now builds the preset with `lzma_lzma_preset` and lowers `dict_size` to the input size rounded up to a power of two, at least 4 KiB, never above the preset's own, then encodes with `lzma_stream_buffer_encode` (same `.xz` container, CRC64 check and options as `lzma_easy_buffer_encode`). Measured on an Apple M-series Mac (dickens, one call per message, 4 KB and 1 KB messages): in C, 4 KB messages compress 1.2x faster at level 6 (11.7 to 14.2 MB/s) and 1.9x at level 10 (7.5 to 14.1), 1 KB messages 1.9x (8.2 to 15.6) and 4.8x (3.2 to 15.4); level 1 moves by about 2% (its dictionary is already 256 KiB) and decompression is unchanged. In WASM, 4 KB messages compress 9x faster at level 6 (0.89 to 8.0 MB/s) and 34x at level 10 (0.24 to 8.1), 1 KB messages 20x (0.24 to 4.7) and 78x (0.06 to 4.7), and level 1 1.3-1.6x. On the 10 MB `dickens` file C is neutral at levels 1, 6 and 10 (output byte-identical in size, compress 3.26 vs 3.24 MB/s at level 6). This implementation on 4 KB messages, geometric mean over levels 1-10: 2.41x on a c8i.xlarge (x86-64, Xeon 6975P-C; 1.3x at level 1, 1.7x at level 6, 4.6x at level 10) and 2.37x on a c8g.xlarge (Graviton4; 1.3x, 2.0x and 5.0x), against 2.40x and 2.09x measured earlier with a liblzma driver fitting the dictionary the same way; on 1 KB messages level 10 is 4.4x (c8i) and 19x (c8g). Ratio: the payload is byte-identical at levels 5-10 (bt4); at levels 1-4 (hc3/hc4, whose hash table size follows the dictionary) a few messages differ by a few bytes, at most +0.06% in total. **User-visible:** the header records the smaller dictionary, so the bytes differ from `xz -N` for small inputs (still standard `.xz`; the reference `xz`, Python `lzma` and every binding decode it), and a decoder needs only that much memory (a 4 KB message decodes within 1 MiB instead of 8 MiB). Incompressible input that liblzma stores uncompressed records 4 KiB, as before. The streaming compressor does not know the input size and keeps the preset's dictionary. Levels 7-10 (presets 6-9) differ only in dictionary size, so they are now one setting for inputs up to 8 MiB, which settles the "level 9 maps to preset 8" question on small inputs; the map is unchanged. liblzma still clears a fixed 64K-entry hash table per call, which is the remaining small-input floor (about 520 KiB, not tunable without patching liblzma).
- [ ] **Ecosystem benchmark baselines** — plan in [docs/ecosystem-benchmarks.md](docs/ecosystem-benchmarks.md). The harness supports them (`impl` field, baseline overlays) but no driver exists. Python: cramjam, zstandard, stdlib zlib/bz2/lzma/`compression.zstd` (3.14). JS: `node:zlib`, fflate, pako, a zstd WASM package. Rust: the native crates. Until this lands, "high-performance" in the README is a claim, not a result.
- [ ] **Path to 1.0:** settle the ABI questions that are expensive to change later, then freeze the C ABI.
  - [ ] An escape hatch past the 1–10 level (parameters struct: window size, zstd long mode, ...).
  - [ ] zstd dictionaries: decide the API shape, even if it ships after 1.0.
  - [ ] Fix the open stream bugs below.
  - [ ] Add CHANGELOG.md.

## Findings from the C ecosystem benchmarks (2026-10-22)

Measured on branch `ecosystem-benchmarks` (M-series MacBook, single-threaded, release builds; details in `docs/ecosystem-benchmarks.md`). Proposals, not yet decided:

- [ ] **xz: cap the dictionary at the input size** (rounded up to a power of two, at least 4 KiB), as 7-Zip does. liblzma otherwise sets up the preset's full dictionary (8 MiB at preset 6, 64 MiB at 9) on every call. Measured as `c-liblzma-fitdict`: compression up to 7.5× faster on 1 KB-4 KB messages at high levels (0.13× the time at level 9), ratio unchanged, neutral on large files; matches the LZMA SDK's small-input lead. Output headers then record the smaller dictionary, so bytes differ from `xz -N` (still standard `.xz`).
- [x] **Deflate backend (zlib, gzip, deflate): we shipped the slowest mainstream implementation** (madler zlib). At equal or better ratio, on silesia-mini: libdeflate 2.7× compress / 2.45× decompress (one-shot only, no streaming); zlib-ng 1.4× / 1.6× (one-shot and streaming, ~+40 KiB binary); zlib-rs similar to zlib-ng. Recommendation: zlib-ng. Lifts every binding. **Done (2026-10-26):** zlib-ng 2.3.3; re-measured at 115 / 809 MB/s on large files in C (was 50 / 470), level with zlib-ng and zlib-rs, behind libdeflate; see "Effect of the zlib-ng and snappy changes" in `docs/ecosystem-benchmarks.md`.
- [x] **Snappy: Google's C++ snappy decompresses 1.5-2.0× faster** than the andikleen C port we switched to in 2026-07, and compresses ~1.3-1.5× faster at 0.6% lower ratio. Trade-off against taking back the C++ runtime dependency. **Done (2026-10-26):** our own first-party C core, no C++ runtime; re-measured at 1,099 / 4,286 MB/s on large files in C (was 811 / 2,106) against Google's 1,163 / 4,258, and ahead of every Rust, Go and Python competitor except klauspost's and cramjam's decoders on 1 KB messages.
- [ ] **Level map on small inputs:** our xz level 9 maps to preset 8, which is slower than preset 9 at the same ratio on small inputs; revisit together with the lz4 levels 1/2 item below.
- [ ] **Small fixed per-call costs:** one-shot lz4 decompression is 5-12% slower than raw liblz4 on 1 KB messages, brotli decompression 2-4% slower everywhere. Everything else is within 1-2% of the raw library, with byte-identical output.
- [ ] **Reusable compressor/decompressor objects: post-1.0.** Measured gain is modest: zstd 0-8% even on 1 KB messages (it already sizes its tables to the pledged input size); libdeflate 1.2-1.5× on messages of 4 KB and under, nothing from 16 KB up.
- [ ] **Document single-codec builds.** A stripped static binary with the default all-codec archive carries 1.6 MiB; with one codec (`INCLUDE_<CODEC>`) it's 11-811 KiB, and decode-only (`-DCU_OMIT_COMPRESS`) less. Neither is documented as a supported native configuration.
- [x] ~~Benchmarks: rerun github-users with ~9 samples before publishing~~ — dropped 2026-10-23 (C is done; keep the benchmarks lean). Caveat for anyone quoting it: at 3-5 samples, identical code varies 0.6-1.9× level to level on that tier (per-call allocation noise); the block tiers and large files are stable.

## Bugs & Critical Issues

- [x] **zstd and LZ4 streaming decompress never finished into an exactly-sized output buffer (fixed 2026-10-27).** Once a frame had ended, a call with no input made libzstd and `LZ4F_decompress` report a nonzero hint (they now want the next frame header), and `dstream_pump` / `dstream_decode` read that hint with a full output buffer as "output still held", so `cu_decompress_stream_finish` returned `CU_ERR_BUF_TOO_SMALL` forever on a complete stream. The C benchmark driver sizes its output buffer exactly, then retried finish `CU_DRAIN_MAX` (1,048,576) times and fell out of the loop without an error, so the ecosystem comparison timed that spin: C-driver stream decompress for zstd and LZ4 read 2-4x below libzstd and liblz4 (about 15 ms of calls on a 9 ms decode of dickens) while one-shot and the other codecs were fine; the Go and Rust bindings decode through a fixed scratch window refilled each call, never an exactly-full buffer, so they never hit it (not checked for the others). Fix: both codecs return `CU_OK` at once for a call with no input after a frame end, since the decoder holds nothing then; the driver now treats an exhausted drain guard as an error instead of silently continuing. The other codecs (brotli, zlib, gzip, deflate, bzip2, xz, Snappy, raw LZ4) already finished in one call; checked with an exactly-sized buffer at several chunk sizes. Dickens stream decompress in the C driver, MB/s, before then after, one-shot in brackets: c8i zstd 1 349 to 1,113 (1,329), zstd 6 339 to 1,025 (1,267), lz4 1 435 to 1,864 (2,054), lz4 6 438 to 1,831 (1,946); c8g zstd 1 319 to 811 (865), zstd 6 290 to 681 (800), lz4 1 451 to 1,361 (1,463), lz4 6 444 to 1,298 (1,333); Apple M-series zstd 1 298 to 1,475, zstd 6 308 to 1,592, lz4 1 719 to 2,680, lz4 6 718 to 2,573. That is parity with the native libraries measured on the EC2 machines on 2026-10-27 (libzstd 1,138 and 1,012 on c8i, 804 and 655 on c8g; liblz4 1,814 and 1,771, 1,357 and 1,294), which were not re-run. **User-visible:** a stream into a buffer of exactly the output size now finishes with `CU_OK` (it used to return `CU_ERR_BUF_TOO_SMALL` indefinitely), so a `set_max_output` equal to the output size also works there; no change otherwise. The 2026-10-27 ecosystem results for zstd and LZ4 stream decompress through the C driver are invalid and must be re-run. Not run: the Python and WASM suites, and stream throughput through the bindings. Regression test: `tests/test_stream_exact_output.c` (every codec, chunk sizes 4 KiB to larger than the stream, output windows, truncated input, the output limit at its boundary).
- [ ] **A write into a full output buffer can't tell held output from a trailer still to come.** When the buffer fills exactly while part of the stream is still unwritten (a checksum or end marker split from the data by the input chunking, e.g. fed 1 byte at a time into a buffer of exactly the output size), zlib, gzip, raw DEFLATE, bzip2, LZ4 and zstd (frames with a checksum, the `zstd` CLI default) return `CU_ERR_BUF_TOO_SMALL`; draining without room can't succeed, and zstd then fails the stream with `CU_ERR_DECOMPRESSION` after repeated calls that make no progress. One spare byte of room avoids it (tested for every codec, documented in `include/compress_utils.h`), and the compressors here don't write zstd checksums, so only foreign frames hit it. Fix would be per codec: consume the trailer without output room, or return `CU_OK` when no output can be held.
- [x] **Nonterminal flush never finished with an output buffer under 7 bytes (zlib, gzip, deflate; fixed 2026-10-25).** A flush that exactly filled the caller's buffer returned `CU_ERR_BUF_TOO_SMALL`, and the drain call made zlib append another flush marker, so the caller received markers forever. Found by the new `fuzz_roundtrip` harness (the same hang on madler zlib and zlib-ng). `dfl_cstream_pump` now asks `deflatePending` whether the flush is complete. Regression inputs in `tests/fuzz/regressions-roundtrip/`.

- [x] **The decompressed-size cap was bypassed for formats without a recorded size (fixed 2026-10-22).** `set_max_decompressed_size` (default 1 GiB) was only enforced when the size probe succeeded (zstd, lz4, Snappy). For zlib, gzip, deflate, brotli, bz2 and xz, one-shot `decompress()` fell back to an uncapped streaming decode in C++, Python, Go and WASM: with a 1 MiB cap, a 102-byte brotli and a 110-byte bz2 input each decompressed to 64 MiB. Separately, `cu_decompress_size_hint` returned the declared size without checking the cap, so bindings allocated an attacker-chosen size before `cu_decompress` rejected it. Fix: a per-stream output limit in the C core (`cu_decompress_stream_set_max_output`, exposed in every binding as `set_max_output` / `max_output` / `SetMaxOutput` / `maxOutput`), which every binding's one-shot fallback sets to the global cap (`cu_get_max_decompressed_size`, now public); and the size hint returns `CU_ERR_SIZE_LIMIT` above the cap. Rust's binding-side cap mirror was removed, so every binding enforces the cap the same way. Tests per codec in C, C++, Python, Go, Rust and WASM.
- [ ] **lz4 levels 1 and 2 produce identical output.** They map to native 0 and 1 (`lz4_native_level` in `src/algorithms/lz4/lz4.c`), which LZ4F both treats as its default fast mode, so the 1..10 scale loses a step at the fast end. Found while building `benchmarks/levels.json` (2026-10-22). A fix changes what level 2 produces, so it's a behavior change; consider spreading 1..3 over LZ4's acceleration settings instead.
- [ ] **xz `size_hint` contradicts the header docs.** `include/compress_utils.h` says XZ size is available via the stream footer/index, but `xz_decompress_size_hint` returns `CU_ERR_SIZE_UNKNOWN` (TODO in `src/algorithms/xz/xz.c`). Either parse the index or fix the docs.
- [x] **xz one-shot decompress was 10-25% slower than stdlib `lzma` in Python; fixed 2026-10-28** (this also settles the "xz `size_hint` contradicts the header docs" item). Three causes, measured on dickens (10 MB) with the same compressed bytes against stdlib `lzma` (Ubuntu liblzma 5.8.3). (1) xz had no size hint, so every binding's one-shot decode took the streaming fallback: the whole input in one write, drained through 64 KiB windows. The xz stream state moved the unconsumed input to the front of its `pending` buffer after every window (`memmove` of up to the whole input, about 155 times for a 3 MB input), quadratic in input size over window size; `pending_off` now advances instead and the move happens once per append, as zstd and lz4 already do. Alone that took Python one-shot at level 6 from 84.6 to 92.0 MB/s on c8g and from 89.9 to 101.0 on c8i. (2) `xz_decompress_size_hint` now reads the Stream Footer and Index and returns the exact size when the input is exactly one Stream; concatenated Streams, Stream Padding, trailing bytes, damaged footers or an Index over 16 MiB of memory return `CU_ERR_SIZE_UNKNOWN` and stream as before. One-shot then decodes into one exactly-sized buffer with no scratch copy or vector growth. (3) x86-64 only: liblzma uses CLMUL for CRC64 (the .xz check) only when `HAVE_USABLE_CLMUL` is defined, and the manifest's portable define set cannot carry it (it pulls in x86 intrinsics), so we ran the table-driven CRC: plain C one-shot at level 6 on c8i 126 MB/s, 140 with the define, 143 for the system liblzma. `algorithms/xz/CMakeLists.txt` now defines it for GCC and Clang targeting x86-64 behind a compile check; MSVC, arm64, universal macOS and WASM are unchanged (arm64 was already at parity with the system liblzma). Python one-shot / stream, MB/s, before then after, stdlib in brackets, 21 samples: c8i level 6 89.9 to 138.2 (141.1) / 114.5 to 127.9 (132.8), level 1 67.7 to 80.4 (83.7) / 75.7 to 79.8 (82.9); c8g level 6 84.6 to 95.9 (94.5) / 90.9 to 89.8 (91.2), level 1 60.1 to 64.9 (64.6) / 64.4 to 64.1 (64.4); level 10 matches level 6 on both. The Apple M-series Mac never showed the gap (parity before, and after). The remaining c8i gap is 2-4% in both modes. **User-visible:** for a single-Stream input `cu_decompress` into a too-small buffer now returns `CU_ERR_BUF_TOO_SMALL` with the size in `*out_len` (it returned `CU_ERR_SIZE_UNKNOWN`), as the header promises for formats with a size hint; and a binding now allocates the Index's declared size up front (capped by `set_max_decompressed_size`, 1 GiB by default), as it does for zstd. Tests: `tests/test_xz_size_hint.c` (exact sizes one-shot and streamed, the cap, concatenated Streams, padding, trailing bytes, truncation, a damaged footer, a many-Block Stream from the reference CLI) and `TestXzOneShotInputs` in `bindings/python/tests/test_interop.py`; C suite, Python, Go and Rust tests pass on the Mac, and C and Python on c8i and c8g. Not run: the WASM suite. Follow-ups from the same investigation are the next four entries; what is still open is listed under them.
- [x] **Python one-shot decompress decodes straight into the result** (2026-10-28). `decompress` used the C++ wrapper: a `std::vector` zero-filled to the size, then a copy into `bytes`; for formats with no recorded size a 64 KiB scratch buffer appended into a vector that doubled by copying, then the same copy. It now allocates an uninitialized `bytes` of the recorded size (or `expected_size`) and decodes into it, and for no-size formats streams into one `realloc`-grown buffer without the GIL and copies once. Errors, the size cap and GIL release rules are unchanged (`cu::detail::check` still raises). Python one-shot MB/s, fresh process, dickens level 6, before to after: c8i zstd 692 to 1,244, brotli 318 to 500, zlib 335 to 597, gzip 341 to 599, bz2 32.8 to 35.3, xz unchanged at 141 (stdlib 141); c8g zstd 715 to 766, brotli 372 to 410, zlib 491 to 600, gzip 490 to 595, bz2 36.8 to 40.1; Apple M-series zstd 2,109 to 2,147, brotli 699 to 703, zlib 772 to 894, gzip 728 to 842. It also removed the c8i effect where, after a stream and join had run in the process, one-shot level 10 ran 9% under level 6 (126 to 141 MB/s), which is probably the fresh-page-fault cost of the vector's growth (the dip went away, but page faults were not measured). Test: `test_output_far_larger_than_input` (10 MB from a few KB for every framed format, so the buffer doubles many times).
- [x] **brotli and bz2 stream decoders keep a read offset into pending input instead of moving it every window** (2026-10-28), as xz, zstd and lz4 do. Measured through Python's old fallback path: brotli no measurable change (c8i 302 to 310 MB/s), bz2 3% to 8% (c8g 35.2 to 38.0, Mac 55.4 to 60.1, c8i 33.4 to 34.5); the brotli fallback's remaining 40% gap to its exact-buffer path was the Python wrapper (previous entry), not the core: the C stream decoder runs 464 MB/s through 16 KiB to 16 MiB windows against 498 one-shot. The compression-side pumps of brotli and bz2 still move leftover input per window; not measured, expected small next to compression time.
- [x] **Rust and Go builds compiled liblzma without `NDEBUG` and without CLMUL** (2026-10-28). `cc` and cgo do not define `NDEBUG` (CMake's Release does, as does every upstream release build), and liblzma keeps `assert`s in its hot decoder paths: the archives Rust built ran xz decompress at 104 MB/s in a plain C harness against 142 with `-DNDEBUG` (c8i, level 6; level 10 100 to 136). `bindings/rust/build.rs` now defines `NDEBUG` when the profile's opt-level is not 0 and `HAVE_USABLE_CLMUL` for GCC/Clang on x86-64; the Go generator (`tools/gen-go-cgo.py`) emits `-DNDEBUG` and `-DHAVE_USABLE_CLMUL` for amd64. xz level 6 decompress, one-shot / stream MB/s, dickens, before to after: c8i Rust 96.3 / 89.1 to 139.1 / 130.9, Go 94.8 / 85.0 to 137.8 / 125.4 (CLMUL alone took Rust to 103.6 / 97.7); c8g unchanged, Rust 98.4 / 93.8 to 96.4 / 92.5 and Go 94.4 / 91.9 to 95.7 / 91.3, which were already at C parity there. Only xz was measured; other codecs' asserts in these builds were not checked. This was most of the "Rust xz is slow on c8i" finding, and the ecosystem results for Rust and Go xz on x86 (2026-10-27) predate it. Not done: MSVC and arm64 CRC (`HAVE_ARM64_CRC32` accelerates CRC32 only, not the CRC64 check, effect not measured).
- [ ] **One-shot decompress growth lives in every binding, not in the core.** `cu_decompress` never grows, so C++, Rust, Go, WASM and (before this work) Python each carry their own "ask for the size, else stream" policy, and they differ (C++ and WASM write the whole input at once, Rust and Go feed 64 KiB chunks), which is why the xz `memmove` cost showed up in some and not others. Python now has a good version of the fallback (`decompress_streaming` in `bindings/python/compress_utils_py.cpp`: one `realloc`-grown buffer, no GIL, one copy at the end). Next: port that loop to the C++ wrapper (`bindings/cpp/include/compress_utils.hpp`, whose fallback is still the slow shape, a 64 KiB scratch appended into a vector, so C++ users of brotli, bz2, zlib and gzip one-shot do not get the Python gain) and to the WASM `decompressStreaming` (one `write` of the whole input, not measured at all). A core-owned helper (a `cu_decompress_alloc`-style call returning an allocation to free, plus a free function) is a design decision and not a cleanup: it adds public ABI, only serves the unknown-size fallback because no binding can adopt a core-allocated buffer without a copy (Python, Rust, Go and C++ all copy; WASM would not), and the sized path must keep allocating natively. Revisit it only if the per-binding copies of the policy keep drifting.
- [ ] **`test_input_cannot_be_resized_mid_call` is timing dependent.** It decodes gzip while a thread resizes the input and asserts at least one resize was refused; on a loaded 4-core machine it failed 2 of about 10 parallel `ctest -j4` runs on c8g (`AssertionError: 0 not greater than 0`). Unrelated to xz (found while testing it). Make it deterministic: hold the call open with an event the resizer sets, instead of racing the decode.
- [X] **XZ decompress hang / bomb on malformed input (fixed 2026-07).** Feeding a
  non-xz-magic byte (e.g. a lone `0x00`) to the xz decompressor never terminated.
  Two independent causes, both fixed in `src/algorithms/xz/xz.c`:
  1. `xz_dstream_create`/`xz_decompress` used `lzma_auto_decoder`, which falls
     back to the legacy `.lzma` (LZMA_ALONE) format on a non-`0xFD` first byte;
     that format can declare "unknown size", so liblzma expanded garbage without
     bound → **switched to `lzma_stream_decoder`** (`.xz`-only, no `.lzma`).
  2. `stream_pump` mapped `LZMA_BUF_ERROR` (no forward progress on a truncated
     stream) to `CU_ERR_BUF_TOO_SMALL` — a *drain-and-retry* signal — so the
     caller looped forever with zero output. Now `LZMA_BUF_ERROR`/no-progress on
     FINISH is **terminal → `CU_ERR_TRUNCATED`**; the one-shot path likewise stops
     treating `LZMA_OK`-without-`STREAM_END` as success.
  Affected every source binding (Rust already had a binding-layer guard; Go was
  vulnerable — now fixed at the core). Regression test:
  `tests/test_compress_utils.c::test_reject_garbage` drains `finish()` in a
  bounded loop for every codec and fails if it doesn't terminate.
- [X] **Default-mode Snappy stream decoder allocated the declared size uncapped.** `snappy_dstream_finish` malloc'd the uncompressed length read from the block header (up to ~4 GiB from a few header bytes) before the caller saw any output, so the caller couldn't enforce its own cap. Every Snappy stream decoder now enforces the decompressed-size cap captured at creation, as single-stream mode and the raw LZ4/LZO decoders do; a stream over the cap fails with `CU_ERR_SIZE_LIMIT`.
- [ ] **Misleading bz2 error after trailing bytes (default mode).** After a complete bz2 stream followed by extra bytes in the same write, `decompress` returns the full output but `finish()` fails with "bz2: sequence error" (`BZ2_bzDecompress` called again after `BZ_STREAM_END`). It should report data after the end of the stream, as the other codecs do. Single-stream mode is unaffected.
- [ ] **Default-mode stream errors drop the output decoded before them in the bindings.** The C core reports that output (`*out_len` alongside the error status). The Go `Reader` and Rust `Decompressor` return it before the error, but the C++ wrapper's `drain_loop` (and so Python) and the WASM streams throw it away when they raise. Returning it would change what the default mode raises, so it needs its own design (e.g. attaching the partial output to the exception).
- [X] **Fix comment/code mismatch in zlib.cpp:60-61** - Comment says "max 4 times" but code uses `retries = 10`
- [X] **C API discards all error context** - `catch (const std::exception& e)` returns `-1` with no way to diagnose failures
  - [X] Add error retrieval function (`compress_utils_last_error()`)
  - [X] Add thread-local error storage
- [X] **Add semantic versioning** - No `VERSION` in CMakeLists.txt or headers
  - [X] Add `project(compress-utils VERSION 0.1.0)` to CMakeLists.txt
  - [X] Add version macros to public headers (`version.hpp`)
  - [X] Add runtime version query functions

## Code Quality

- [X] **Fix span parameter const-correctness** - All algorithm headers now pass `std::span<const uint8_t>` by value (idiomatic)
  - [X] `src/algorithms/zstd/zstd.hpp`
  - [X] `src/algorithms/brotli/brotli.hpp`
  - [X] `src/algorithms/xz/xz.hpp`
  - [X] `src/algorithms/zlib/zlib.hpp`
- [X] **Fix docstring copy-paste errors** - brotli.hpp and xz.hpp incorrectly said "Zstandard"
- [X] **Extract magic numbers to constants.hpp**
  - [X] `xz.cpp:36` - `64 * 1024` buffer size → `internal::DEFAULT_BUFFER_SIZE`
  - [X] `xz.cpp:100` - `16384` minimum buffer → `internal::MIN_DECOMP_BUFFER_SIZE`
  - [X] `zlib.cpp:53` - `data.size() * 4` initial buffer multiplier → `internal::DECOMP_BUFFER_MULTIPLIER_ZLIB`
  - [X] `zlib.cpp:61` - `10` retry count → `internal::MAX_DECOMP_RETRIES`
  - [X] Algorithm max levels (ZSTD_MAX_LEVEL, BROTLI_MAX_LEVEL, etc.)
  - [X] Helper functions MapLevel() and MapLevelZeroBased()
- [x] **Deduplicate buffer resizing logic** — every algorithm's streaming impl now shares the same `pending_append` / drain pattern, and the C++/Python bindings have a single drain loop in their stream classes.
- [X] **Remove `using namespace` from compress_utils_py.cpp** — file rewritten in Phase 4; no `using namespace` anywhere.
- [x] ~~Add allocation failure checks in C tests~~ — N/A; C tests rewritten in `tests/smoke_test.c` using `CHECK()` macros and use the library API not raw malloc.
- [x] ~~Replace hardcoded test values with named constants in `tests/smoke_test.c`~~ — N/A; `smoke_test.c` no longer exists (C tests are now split across `tests/test_*.c`).

## Build System Fixes

- [x] **Vendor upstream codec sources** (2026-07). Replaced the per-codec
  `ExternalProject_Add` (git fetch + configure at build time) with a curated,
  pre-configured tree under `third_party/`, driven by `third_party/manifest.json`
  (generated by `tools/vendor-codecs.py`) and built via `cmake/Vendor.cmake`.
  No network, no per-codec configure; builds are offline + reproducible and much
  faster to configure. C/C++/Python/WASM all migrated. Config portability solved
  with hand-authored headers (zlib `zconf.h`, snappy `config.h` +
  `snappy-stubs-public.h`) and a uniform `-D` set for liblzma. This is the shared
  substrate the Go/Rust/Zig/Swift bindings compile directly. See
  `third_party/VENDOR.md`.
- [X] Fix Windows build issues and re-add `windows-latest` to Github Actions workflows
  - [X] Build `compress-utils` and `compress-utils-static`
  - [X] Build `unit-tests` and `unit-tests-static`
  - [X] Fix `ctest`
  - [X] Build `compress-utils-c` and `compress-utils-c-static`
  - [X] Build `unit-tests-c` and `unit-tests-c-static`
  - [X] Build `xz`
- [X] Rename `compress-utils` to `compress-utils`
- [x] Ship a bundled self-contained static `.a`
- [x] Disable `ZSTD-LEGACY` & `ZSTD-MULTITHREADED` to slim the shared library — done via the vendored zstd defines (`ZSTD_LEGACY_SUPPORT=0`, `ZSTD_MULTITHREAD` dropped, `ZSTD_DISABLE_ASM`); also removes the pthread dependency so one build serves native + wasm.
- [ ] **Make the CMake build consumable** (`FetchContent` / `add_subdirectory` / `find_package`). This is the prerequisite for the Conan, vcpkg and Homebrew entries under Package Managers: each recipe is essentially `cmake --install` plus a Config file, so do this first.
  - [ ] Install to `GNUInstallDirs` (`CMAKE_INSTALL_LIBDIR` etc.) instead of the hardcoded `${CMAKE_SOURCE_DIR}/dist/c`, so a package manager's prefix / `DESTDIR` is honored. Keep `dist/c` as an opt-in layout for the release workflows that rely on it.
  - [ ] `install(EXPORT)` plus a `compress_utilsConfig.cmake` / `ConfigVersion.cmake` (`CMakePackageConfigHelpers`) for `find_package(compress_utils)`, and a pkg-config file.
  - [ ] Namespaced alias targets (`compress_utils::compress_utils` for shared, `compress_utils::static`) with the include directory on the interface.
  - [ ] Gate developer-only defaults on `PROJECT_IS_TOP_LEVEL`: tests, benchmarks, C++/Python/WASM bindings, the forced Debug build type and global `-flto` should not apply when consumed via `FetchContent`.
  - [ ] Make the static archive an easy path: today `compress_utils_static` only exists with `BUILD_STATIC_LIB`, and most C/C++ consumers want static.
  - [ ] Version without a `.git` directory: `FetchContent` and release tarballs fall back to the hardcoded `DEFAULT_VERSION` (0.11.1), which drifts. Add a `CU_VERSION` override or a version file written at release time.
  - [ ] Option to link system codecs (`find_package` for zlib, zstd, ...) instead of the vendored `third_party/` copies, so Conan/vcpkg recipes don't duplicate libraries consumers already have. Needed for distros too; same duplicate-symbol concern as the Rust static-lib limitation.
  - [ ] Document `FetchContent` and `find_package` usage in the root README.

## Optimizations

- [X] Support iterative builds in `cibuildwheel` (via separated Python binding project & shared core lib project)
- [X] Add source wheel distribution for unsupported Python wheel configurations
- [X] Split CI/CD pipelines hierarchically
- [X] Support free-threaded Python

## Documentation

- [X] **Document thread-safety guarantees** (documented in compress_utils.hpp and compress_utils_stream.hpp)
  - [X] Functional API is thread-safe (stateless)
  - [X] Compressor class requires external synchronization for shared instances
  - [X] Streaming classes are NOT thread-safe (documented in header)
- [X] **Document compression level mappings per algorithm** (documented in constants.hpp)
  - [X] ZSTD: 1-10 → 2-22 (via MapLevel with ZSTD_MAX_LEVEL=22)
  - [X] Brotli: 1-10 → 0-11 (via MapLevel with BROTLI_MAX_LEVEL=11)
  - [X] zlib: 1-10 → 1-9 (capped at ZLIB_MAX_LEVEL=9)
  - [X] XZ: 1-10 → 0-9 (via MapLevelZeroBased)
- [ ] **Add API reference documentation** (Doxygen or similar)
- [ ] **Add CHANGELOG.md** for tracking releases (part of the path to 1.0 above)

## New Features

- [X] Github Workflow for artifact publishing
- [X] **Streaming compression/decompression support** (Complete)
  - [X] Design streaming API (CompressStream/DecompressStream classes)
  - [X] Implement for each algorithm using native streaming APIs (ZSTD, Brotli, zlib, XZ)
  - [X] Add Python bindings for streaming (CompressStream/DecompressStream)
  - [X] Add C bindings for streaming (compress_stream_*/decompress_stream_* functions)
  - [X] Add streaming unit tests (C++, C, and Python)
  - [X] Fix move semantics tests for streaming API (was a test bug, not implementation bug)
- [X] Cross-language performance testbench
- [X] **Single-stream decoding** (`CU_DSTREAM_SINGLE`): stop at the end of the first frame/member/stream and hand back the input after it (`eof` / `unused`), in C, C++, Python, Rust, Go and WASM. Lets callers like zpickle read one message and carry on with what follows.
- [ ] Standalone CLI executable
- [ ] Multi-file input/output (archiving) via `zip` and `tar.*`
- [ ] Async/multi-threaded compression support

## Bindings (implementation, tooling, tests & ci/cd updates)

- [X] `c++` (Main Lib)
- [X] `c`
- [X] `go`
- [X] `js/ts` (WebAssembly via Emscripten)
- [X] `python` (3.11 - 3.15)
- [X] `rust`
- [ ] `cli` (standalone command-line tool) — deferred; if one new surface is added, this is the cheapest and most visible, but it comes after the "Next up" items.

Planned language bindings, in priority order (each new binding multiplies the maintenance matrix, so keep each one's tests and benchmark minimal, and check what the ecosystem already offers before starting). All consume the C ABI in `include/compress_utils.h` (30 `CU_API` symbols):

- [ ] `java` / `kotlin` — largest audience and a real gap (JVM compression libraries are mostly single-codec JNI wrappers). Use the Panama FFM API (Java 22+) with `jextract` over the header rather than JNI. The real work is Maven Central packaging with per-platform native libs in the jar. Kotlin consumes the Java artifact.
- [ ] `csharp` / `.NET` — P/Invoke over the C ABI; NuGet `runtimes/<rid>/native/` layout. Best reach for the effort after Java.
- [ ] `swift` — SwiftPM can consume a C target directly over the vendored sources (`module.modulemap` plus an idiomatic wrapper). Aimed at iOS/macOS users who want Brotli/zstd/lz4 in one package.
- [ ] `zig` — cheapest to add: `@cImport` plus `build.zig.zon` over the vendored sources, and the zig toolchain is already a WASM build dependency. Small audience; check what `std` already covers first.
- [ ] `ruby` — FFI gem over the shared library, or a native extension over the vendored sources (decide).
- [ ] `php` — FFI (`ext-ffi`) versus a C extension distributed via PECL/PIE (decide; FFI is often disabled on shared hosts).
- [ ] `lua` — C module over the vendored sources, packaged on LuaRocks (LuaJIT FFI as an alternative).
- [ ] `r` — `.Call` package over the vendored sources. Check CRAN's size and bundled-source policy before committing.

Not planned: Node.js native addon (N-API). The WASM binding covers Node and the browser, and Node's built-in zlib/brotli/zstd cover the common cases.

## Algorithms

See [docs/adding-an-algorithm.md](docs/adding-an-algorithm.md) for the
end-to-end checklist (C core → build → bindings → tests → benchmarks → docs).

- [X] `brotli`
- [X] `bzip2`
- [X] `gzip`
- [X] `lz4`
- [X] `snappy` — **switched from google/snappy (C++) to the pure-C port
  andikleen/snappy-c (2026-07)** so the whole release library is pure C (no
  libc++/libstdc++ in any binding — native shared/static, Rust, Go, WASM all
  verified). Wire format identical (byte-for-byte + both-direction interop).
  google/snappy is kept at `third_party/snappy-oracle` as the C++ differential
  test oracle only (`tests/test_snappy_oracle`), never shipped.
- [X] `snappy` — **replaced the andikleen/snappy-c port with our own first-party C implementation (2026-10)**, `src/algorithms/snappy/core` (a self-contained compressor and decompressor with its own `LICENSE` and `PROVENANCE.md`; no vendored upstream, so `snappy` left `codec-versions.json`, `manifest.json` and `tools/vendor-codecs.py`). The snappy-c decoder was ~2x and its encoder ~1.4x slower than Google's C++; the new core is within ~10% of it (decode and encode, large files and 1-4 KB messages) at the same ratio. Still pure C (NEON/SSE2 intrinsics for the overlapping 16-byte copy only, portable scalar paths). Output bytes differ from snappy-c's but are valid Snappy, and are identical on every target and compiler (one multiplicative match hash; a hardware CRC32C hash gained at most 4% and was dropped). A second round retuned the decoder for gcc 13-15, which compiled the first version badly (a literal-versus-copy select became a mispredicted branch): under gcc -O2 on x86-64 (Sapphire Rapids) and aarch64 (Graviton4) decode is now 1.2-2.0x snappy-c's on every file and block size measured and within ~10% of clang, with no `goto` left in the core. Known gap: gcc 13 at `-O3` on x86-64 still turns that select into a branch (about 20% slower on text than its `-O2`, still ~35% faster than snappy-c). Verified against the oracle in both directions plus differential accept/reject over corrupted inputs (`tests/test_snappy_core_diff`), the fuzz harness under ASan/UBSan, and every binding's tests.
- [X] `xz/lzma`
- [X] `zlib`
- [X] `zstd`
- [ ] `snappy-framed` (deferred — add only on demand). *Decision (2026-07-08): support the raw Snappy block format only; do NOT add the framing (`.sz`) format now.*

## Package Managers

C/C++ package managers all depend on "Make the CMake build consumable" under Build System Fixes. Suggested order: vcpkg, Conan, Homebrew.

- [ ] `c` -> `vcpkg` — portfile plus the CMake Config file; the registry PR process is lighter than Conan Center's, so likely first.
- [ ] `c++` -> `vcpkg` — same package; adds the `compress_utils_cpp` INTERFACE target export.
- [ ] `c` -> `conan` — write a `conanfile.py` recipe in this repo first, then upstream it to conan-center-index (review takes weeks). Expose an option to use system codecs instead of the vendored ones.
- [ ] `c++` -> `conan` — same recipe as the C package, plus the `compress_utils_cpp` INTERFACE target export.
- [ ] `c` / `c++` -> `homebrew` — formula or own tap over a tagged release tarball; a tap avoids review.
- [ ] `c` / `c++` -> Linux distros (`apt`/`rpm`) — deferred until someone asks. Debian and Fedora policy forbids bundled zlib/zstd/etc., so this needs the system-codecs option and likely unbundling of the patched Snappy C port and the LZO decompressor.
- [X] `go` -> `pkg.go`
- [X] `js/ts` -> `npm`
- [X] `python` -> `pypi`
- [x] `rust` -> `cargo` — `build_and_test_rust.yml` publishes to crates.io on a
  `v*` tag (a tag-gated `publish` job in the same workflow, matching the WASM/
  Python pattern; verifies Cargo.toml == tag first) plus a `cargo publish
  --dry-run` package gate on every run. Packaged crate is 7.1 MiB / 1.7 MiB
  compressed (well under the 10 MiB limit). Published since 2026-07 (0.11.1 is live on crates.io).
- [ ] `java` / `kotlin` -> `maven` (Maven Central)
- [ ] `csharp` -> `nuget`
- [ ] `swift` -> SwiftPM
- [ ] `zig` -> `build.zig.zon` package
- [ ] `ruby` -> `rubygems`
- [ ] `php` -> `packagist` (FFI) or `pecl`/PIE (extension)
- [ ] `lua` -> `luarocks`
- [ ] `r` -> `cran`
- [ ] `cli-macos` -> `homebrew`
- [ ] `cli-linux` -> `apt`/`rpm`

