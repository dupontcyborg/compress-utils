# Benchmark note: decompress output sizing

Found during the libdeflate spike (worktree `compression-utils-libdeflate`, write-up in `docs/libdeflate-spike.md` there) and resolved in the harness on 2026-10-29. Related to `docs/benchmark-shortcomings.md`.

## The problem

The harness used to hand every decompressor an output buffer of exactly the original size, in one-shot, message-set and streaming modes. That is the best case and applies only to callers who already have the size. zlib, gzip and raw deflate do not record a usable output size (zlib has only an Adler-32, gzip's ISIZE is the last member's size mod 2^32, raw deflate has nothing), so a default binding streams them; libdeflate, which only has a whole-buffer API, was measured on a path our bindings do not take. The streaming mode also offered one output window the size of the whole file, which flatters inflate.

## The rule now

Nothing is told the output size. Each implementation uses what the compressed bytes record, or the library's own unknown-size API.

- **C one-shot and message sets.** A codec with a `size_hint` function (zstd, lz4 frame and snappy sizes, compress-utils through `cu_decompress_size_hint`, which succeeds for zstd, lz4, snappy and single-Stream xz but not for zlib, gzip, deflate, brotli or bz2) starts from the hint, capped at 1 GiB. Everything else starts from `max(64 KiB, 4 x input)` and, when the library reports insufficient output space (`BENCH_NEED_MORE`), doubles the buffer and restarts the decode from scratch. The restart cost is deliberately inside the timing. A message set recomputes the starting size per message and reuses the physical buffer, so malloc is not timed. Records carry `decompress_size_hint` (`codec` only when the hint was usable on every call of the last timed pass, else `none`) and `decompress_retries` (retries in the last timed pass, the whole set for message sets).
- **C streaming.** The input is fed in chunks (64 KiB by default) and the output goes into one reused window of `max(chunk, 64 KiB)`; each drained piece is passed to a sink that counts it. Verification is one extra, untimed decode whose sink compares against the original. In stream mode the record says `none`, since the hint is not used.
- **Python, Go, Rust, JS.** One-shot and message sets call the library's plain decompress with no size or capacity (no `expected_size`, `bufsize`, `output_len`, `max_output_size`, `DecompressWithCapacity`, `decompress_with_capacity`, `out` buffers). Streaming discards output as it is produced (Go and Rust through a fixed 64 KiB buffer). Python and JS drop each chunk as it arrives rather than reading through a bounded window, because those libraries expose none. Records carry `decompress_size_known: false`; they do not carry the C-only fields.

## What to expect

- Known-size formats (zstd, lz4, snappy) are unaffected.
- gzip's ISIZE is not used by anyone, in the harness or in our library: it is only the last member's size mod 2^32, a guess rather than a recorded size, so every gzip row starts like zlib and raw deflate. (libdeflate's own `gzip` program uses it; its library API does not. At the time of writing libdeflate's gzip decompress was only marginally faster than its zlib, which has no such hint.)
- Whole-buffer libraries on formats without a size (zlib, raw deflate, bz2, xz, brotli) pay for the starting guess. The compress-utils rows follow whatever the library does for unknown sizes; once the core `cu_decompress_alloc` lands, the C driver's compress-utils codecs should call it for formats without a hint, which is the intended before-and-after comparison.
- Results from before this change are not comparable for decompress.
