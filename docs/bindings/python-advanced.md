# compress-utils for Python: advanced guide

Details that the [package README](../../bindings/python/README.md) leaves out: raw blocks, stopping at the end of a stream, type checking and threading.

## Raw blocks and `expected_size`

Raw block formats such as those inside Parquet and ORC pages don't record their decompressed size. Pass `expected_size`, an upper bound on the output, and the result holds only the bytes actually decoded:

```python
page = cu.decompress(block, "lz4_raw", expected_size=uncompressed_page_size)
```

Without `expected_size`, `lz4_raw` blocks are decoded with the incremental stream decoder, which is slower, so pass it when you know the size. Every other algorithm accepts it too, which skips the size probe and decodes straight into a buffer of that size.

## Stopping at the end of a stream

By default a `DecompressStream` decodes concatenated zstd/LZ4 frames, gzip members and xz streams as one stream. When a compressed stream is followed by other data (the next message on a socket, another record in the same file), pass `single_stream=True`. Decoding stops at the end of the first frame, member or stream, `eof` turns `True`, and `unused_data` holds the input that came after the end, as with the standard library's `bz2.BZ2Decompressor`, `lzma.LZMADecompressor` and `compression.zstd.ZstdDecompressor`:

```python
ds = DecompressStream("zstd", single_stream=True)
message = b""
while not ds.eof:
    chunk = f.read(65536)
    if not chunk:
        ds.finish()                         # raises CompressError: the stream was cut short
    message += ds.decompress(chunk)
f.seek(-len(ds.unused_data), io.SEEK_CUR)   # give back what was read past the end
```

After the end, `decompress()` with more data raises `EndOfStreamError`, a subclass of both `CompressError` and `EOFError`, without consuming it. The standard library's decompressor objects stop at the first stream by default, but here it is opt-in, because `DecompressStream` decodes concatenated frames as one stream by default. `lz4_raw` (which has no end marker) and `lzo` don't support it. Single-stream Snappy emits nothing until its whole block has arrived, and rejects a block declaring more than `set_max_decompressed_size` (as set when the stream was created).

For applications that round-trip many small payloads with the same algorithm, prefer the functional API over creating a new `CompressStream` per payload.

## Type checking

The package ships PEP 561 type information:

```python
import compress_utils as cu
reveal_type(cu.compress)           # → (data: Buffer, algorithm: object, level: int = 5) -> bytes
reveal_type(cu.Algorithm.zstd)     # → Algorithm
```

The stubs are generated from the compiled module at build time, so they match the binding signatures.

## Concurrency details

On regular Python, calls with at least 256 KiB of input (or of recorded output, for formats that store their size) release the GIL, so other Python threads keep running and several threads can compress or decompress in parallel. Smaller calls keep the GIL, since getting it back can cost more than the call itself. On the free-threaded builds (`python3.14t`, `python3.15t`) calls on different threads run in parallel whatever their size.

Don't resize a `bytearray` while it's being passed to a call; Python raises `BufferError` if you try.

While another thread runs pure Python, each call that released the GIL can wait up to the interpreter's switch interval (5 ms by default) to get it back, as with the standard library's `zlib`. Larger chunks (1 MiB or more) make that wait a small share of each call, and `sys.setswitchinterval(0.0005)` shortens it.
