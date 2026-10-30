# compress-utils for Go: advanced guide

Details that the [package README](../../bindings/go/README.md) leaves out: raw blocks, stopping at the end of a stream, and the cgo call cost.

## Raw blocks and output capacity

Raw block formats such as those inside Parquet and ORC pages don't record their decompressed size. `DecompressWithCapacity` takes an upper bound on the output and returns only the bytes actually decoded:

```go
page, err := cu.DecompressWithCapacity(cu.Lz4Raw, block, uncompressedPageSize)
```

Without a capacity, `Decompress` decodes `Lz4Raw` blocks with the incremental stream decoder, which is slower, so pass one when you know the size. Every other algorithm accepts a capacity too, which skips the size probe and decodes straight into a buffer of that size.

## Stopping at the end of a stream

By default a `Reader` decodes concatenated zstd/LZ4 frames, gzip members and xz streams as one stream. When a compressed stream is followed by other data (another message on a connection, the next record in a file), call `Multistream(false)` before the first `Read`, as with `compress/gzip`. `Read` then returns `io.EOF` at the end of the first frame, member or stream, and `Unused()` returns the bytes the `Reader` read from its source past that end:

```go
r, _ := cu.NewReader(conn, cu.Zstd)
r.Multistream(false)
msg, _ := io.ReadAll(r)                                  // the first message only
next := io.MultiReader(bytes.NewReader(r.Unused()), conn) // carry on from where it ended
```

Unlike `gzip.Reader`, which avoids reading past the end by pulling one byte at a time through an `io.ByteReader`, this `Reader` reads its source in chunks and hands the extra bytes back through `Unused()`, so always continue from `Unused()` before the source. `Multistream` panics if called after `Read`. `Lz4Raw` (which has no end marker) and `Lzo` don't support it, and their first `Read` returns an `*Error` with `Code` `ErrUnsupported`. Snappy emits nothing until its whole block has arrived.

## cgo and thread pinning

Every call crosses the cgo boundary, and each call pins its goroutine to an OS thread for its duration, because the C core reports error details through thread-local state. The pin is released when the call returns. The crossing and the pin are a fixed cost per call, so they matter most for very small inputs and for the fastest codecs; for large inputs on slow codecs they are lost in the noise.

`Decompress` recovers the size from the wire format when the codec records it (zstd, xz, lz4, snappy) and otherwise has the C core grow the output buffer as it decodes, copying once into the result.
