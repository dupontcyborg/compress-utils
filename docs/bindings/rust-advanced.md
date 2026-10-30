# compress-utils for Rust: advanced guide

Details that the [crate README](../../bindings/rust/README.md) leaves out: raw blocks and stopping at the end of a stream.

## Raw blocks and output capacity

Raw block formats such as those inside Parquet and ORC pages don't record their decompressed size. `decompress_with_capacity` takes an upper bound on the output and returns only the bytes actually decoded:

```rust
let page = compress_utils::decompress_with_capacity(Algorithm::Lz4Raw, &block, page_size)?;
```

Without a capacity, `decompress` decodes `Lz4Raw` blocks with the incremental stream decoder, which is slower, so pass one when you know the size. Every other algorithm accepts a capacity too, which skips the size probe and decodes straight into a buffer of that size.

## Stopping at the end of a stream

By default a `Decompressor` decodes concatenated zstd/LZ4 frames, gzip members and xz streams as one stream, and expects the source to end where the compressed data does. When something else follows it, such as the next message on a socket or another record in the same file, use `Decompressor::single_stream`. It stops at the end of the first frame, and `read` then returns `Ok(0)`:

```rust
use std::io::{Cursor, Read};
use compress_utils::{Algorithm, Decompressor};

let mut d = Decompressor::single_stream(src, Algorithm::Zstd)?;
let mut message = Vec::new();
d.read_to_end(&mut message)?;

// It reads ahead in chunks, so take back what it read past the end, then
// carry on with the source.
let (src, unused) = d.into_parts()?;
let mut rest = Cursor::new(unused).chain(src);
```

`is_eof()` reports whether the end has been decoded, and `unused()` borrows the bytes read past it. Read until `read` returns `Ok(0)` before calling `into_parts()`, even if you stop at the end of your own data (as `read_exact` does): until then part of the frame and the bytes after it can still be inside the decompressor, so `into_parts()` returns an `IntoPartsError`, whose `into_inner()` hands the decompressor back to keep reading. Raw LZ4 (which has no end marker) and LZO don't support it, and return `Status::UnsupportedAlgo`. Single-stream Snappy emits nothing until its whole block has arrived, and rejects a block declaring more than `set_max_decompressed_size` (as set when the decompressor was created) with `Status::SizeLimit`.
