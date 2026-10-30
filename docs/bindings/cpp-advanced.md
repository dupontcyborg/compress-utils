# compress-utils for C++: advanced guide

Details that the [README](../../bindings/cpp/README.md) leaves out: raw blocks and stopping at the end of a stream.

## Raw blocks and output capacity

Raw block formats such as those inside Parquet and ORC pages don't record their decompressed size. Pass an output capacity, an upper bound on the output, and the result holds only the bytes actually decoded:

```cpp
auto page = cu::decompress(cu::Algorithm::Lz4Raw, block, uncompressed_page_size);
```

Without a capacity, `decompress` decodes `Lz4Raw` blocks with the incremental stream decoder, which is slower, so pass one when you know the size. Every other algorithm accepts it too, which skips the size probe and decodes straight into a buffer of that size.

## Stopping at the end of a stream

By default a `DecompressStream` decodes concatenated zstd/LZ4 frames, gzip members and xz streams as one stream. When a compressed stream is followed by other data (the next message on a socket, another record in the same file), construct it with `cu::StreamMode::Single`. Decoding stops at the end of the first frame, member or stream, `eof()` turns true, and `unused()` returns the input that came after the end:

```cpp
cu::DecompressStream ds(cu::Algorithm::Zstd, cu::StreamMode::Single);
auto message = ds.write(received);   // decodes up to the end of the first frame
if (ds.eof()) {
    auto next = ds.unused();         // the bytes after it; valid until the next call on ds
}
```

After `eof()`, `write()` with more data throws `cu::Error` with `CU_ERR_STREAM_FINISHED` without consuming it, while an empty `write()` and `finish()` return nothing. Raw LZ4 (which has no end marker) and LZO don't support it, and throw `CU_ERR_UNSUPPORTED_ALGO`. Single-stream Snappy emits nothing until its whole block has arrived, and rejects a block declaring more than `cu::set_max_decompressed_size` (as set when the stream was created) with `CU_ERR_SIZE_LIMIT`. From C, create the stream with `cu_decompress_stream_create_ex(algo, CU_DSTREAM_SINGLE, &stream)` and query it with `cu_decompress_stream_eof` and `cu_decompress_stream_unused`; `include/compress_utils.h` documents the exact protocol.
