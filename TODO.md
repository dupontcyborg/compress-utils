# Compress Utils TODO

## Next up (2026-10)

Direction: stop adding breadth and find out where the library wins. Likely positioning is "every format, including the browser, hardened against untrusted input", to be confirmed by the ecosystem benchmarks.

- [x] **Fix the decompressed-size cap bypass** (see Bugs; fixed 2026-10-22).
- [x] **README: untrusted-input section** (2026-10-22): root README "Untrusted input" section (output cap for every format, stream limits, bounded decoder memory, final errors, `.xz`-only decoding, fuzzing in CI with a regression corpus, pinned offline builds), linked from each binding's "Errors and limits".
- [x] **Deflate backend: zlib, gzip and raw deflate now run on zlib-ng 2.3.3** (2026-10), in its zlib-compatible mode, with per-ISA wrappers instead of a configure step (see `third_party/VENDOR.md`). Level 6 compresses about 3x and decompresses about 2x faster on every machine measured: Apple M-series (49 to 118 MB/s, 453 to 750 MB/s on silesia-mini), Graviton4 (20 to 63, 339 to 615 on dickens), AMD Zen3 (17 to 52, 319 to 655) and Intel Sapphire Rapids (18 to 56, 275 to 610); up to 3.3x compress on 1-4 KB messages at level 1. Runtime dispatch (NEON, AVX2, AVX-512, PCLMUL, ARMv8 CRC) is worth +30% to +54% on decompression and nothing on compression at levels 1 and 6; hardware CRC adds about 4% to gzip decompression. Verified through the C, Go (cgo) and Rust (cc-rs) builds on all three machines, including a plain `-march=x86-64` build where all the AVX code comes from the per-file target wrappers; output is byte-identical between arm64 and x86-64 for zlib. **User-visible:** output bytes differ from madler zlib at every level; level 1 is zlib-ng's quick strategy (about 24% larger output on text for about 2x the speed); the level map is now 1-10 to native 1-7, 9, 9, 9 (native 8 is within 0.1% of 7, and 9 is about 0.5% larger than 7 on text but about 2% smaller on binary), so levels 8-10 are one setting. Zlib-only native library +34 KiB (arm64); WASM zlib 86.9 to 104.4 KiB (size budgets raised). Not run: MSVC/Windows (CI only).
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

## Bugs & Critical Issues

- [x] **Nonterminal flush never finished with an output buffer under 7 bytes (zlib, gzip, deflate; fixed 2026-10-25).** A flush that exactly filled the caller's buffer returned `CU_ERR_BUF_TOO_SMALL`, and the drain call made zlib append another flush marker, so the caller received markers forever. Found by the new `fuzz_roundtrip` harness (the same hang on madler zlib and zlib-ng). `dfl_cstream_pump` now asks `deflatePending` whether the flush is complete. Regression inputs in `tests/fuzz/regressions-roundtrip/`.

- [x] **The decompressed-size cap was bypassed for formats without a recorded size (fixed 2026-10-22).** `set_max_decompressed_size` (default 1 GiB) was only enforced when the size probe succeeded (zstd, lz4, Snappy). For zlib, gzip, deflate, brotli, bz2 and xz, one-shot `decompress()` fell back to an uncapped streaming decode in C++, Python, Go and WASM: with a 1 MiB cap, a 102-byte brotli and a 110-byte bz2 input each decompressed to 64 MiB. Separately, `cu_decompress_size_hint` returned the declared size without checking the cap, so bindings allocated an attacker-chosen size before `cu_decompress` rejected it. Fix: a per-stream output limit in the C core (`cu_decompress_stream_set_max_output`, exposed in every binding as `set_max_output` / `max_output` / `SetMaxOutput` / `maxOutput`), which every binding's one-shot fallback sets to the global cap (`cu_get_max_decompressed_size`, now public); and the size hint returns `CU_ERR_SIZE_LIMIT` above the cap. Rust's binding-side cap mirror was removed, so every binding enforces the cap the same way. Tests per codec in C, C++, Python, Go, Rust and WASM.
- [ ] **xz `size_hint` contradicts the header docs.** `include/compress_utils.h` says XZ size is available via the stream footer/index, but `xz_decompress_size_hint` returns `CU_ERR_SIZE_UNKNOWN` (TODO in `src/algorithms/xz/xz.c`). Either parse the index or fix the docs.
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
- [ ] Add CMake package config for `find_package(compress_utils)` support

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
- [ ] `java` — deferred until someone asks for it; each new binding multiplies the maintenance matrix.
- [ ] `swift` — deferred, same reason.
- [ ] `cli` (standalone command-line tool) — deferred; if one new surface is added, this is the cheapest and most visible, but it comes after the "Next up" items.

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

- [ ] `c` -> `conan` (low priority — publish the C library to Conan Center for C consumers; requires writing a `conanfile.py` recipe + registering with conan-center-index)
- [ ] `c++` -> `conan` (low priority — same as above for the C++ header-only binding; shares most of the recipe with the C package, just adds the `compress_utils_cpp` INTERFACE target export)
- [X] `go` -> `pkg.go`
- [X] `js/ts` -> `npm`
- [X] `python` -> `pypi`
- [x] `rust` -> `cargo` — `build_and_test_rust.yml` publishes to crates.io on a
  `v*` tag (a tag-gated `publish` job in the same workflow, matching the WASM/
  Python pattern; verifies Cargo.toml == tag first) plus a `cargo publish
  --dry-run` package gate on every run. Packaged crate is 7.1 MiB / 1.7 MiB
  compressed (well under the 10 MiB limit). Published since 2026-07 (0.11.1 is live on crates.io).
- [ ] `java` -> `maven`
- [ ] `swift` -> ?
- [ ] `cli-macos` -> `homebrew`
- [ ] `cli-linux` -> `apt`/`rpm`

