# Streaming issues found while integrating into mbox-md

Found on 2026-09-30 against compress-utils 0.8.0 (zstd v1.5.7, zlib v1.3.2), using a real 6 GB `.mbox.zst` (15.4 GB decompressed) and small synthetic inputs. Ordered by payoff.

## 1. Decode cost grows with the square of the input chunk size

- **Cause:** when the 64 KB output page fills, the stream copies the whole unread rest of the input into `pending` and then `memmove`s it forward after every page: `src/algorithms/zstd/zstd.c:453`, `:479`; `src/algorithms/zlib/deflate_backend.h:325`. The deflate path also copies every input chunk into `pending` first (`deflate_backend.h:345`). So each call costs roughly chunk size × output pages.
- **Measured** (zstd, 500 MB of compressed input held in memory, output throughput):

  | Chunk | 16–256 KB | 1 MB | 4 MB | 16 MB |
  |---|---|---|---|---|
  | MB/s | ~2,000 | 1,709 | 1,194 | 537 |

  The zstd CLI does 791 MB/s on the same data, so with small chunks compress-utils is already faster.
- **Fix:**
  - Track a read offset into `pending` instead of `memmove`ing after each page, and compact only when appending.
  - In `bindings/cpp/include/compress_utils.hpp` (`drain_loop`, both classes), decode straight into the growing `std::vector` instead of through a 64 KB scratch buffer.
- **Done when:** a 16 MB chunk decodes as fast as a 64 KB one.

## 2. Concatenated zstd frames and gzip members are silently dropped

- **Cause:**
  - **zstd:** `zstd.c:477` (and `:450` in the pending path) does `if (r == 0) { st->frame_done = 1; break; }`, so input after the first frame in the same call is discarded, and `pending_len` is cleared at `:461`. A later `write()` fails with "write after frame end".
  - **gzip:** `deflate_backend.h:317–330`. On `Z_STREAM_END` it sets `stream_end` and discards the remaining input with `pending_len = 0`.
- **Effect:** `cat a.gz b.gz`, `pigz` output, `pzstd` output, and concatenated `.zst` files lose data without an error when the boundary falls inside one call, and fail when it falls in a later call. Reproduce with `DecompressStream("zstd").decompress(cu.compress(a, "zstd") + cu.compress(b, "zstd"))`, which returns only `a`. gzip behaves the same.
- **Fix:**
  - **zstd:** treat `r == 0` as "at a frame boundary" and keep feeding; `ZSTD_decompressStream` starts the next frame, including skippable frames. `finish()` reports truncation only when not at a boundary.
  - **gzip:** on `Z_STREAM_END`, call `inflateReset()` and continue with the remaining input, as the `gzip` CLI and Python's `gzip` do. For the zlib wrapper, keep trailing data as an error, but raise it rather than discarding it.
- **Tests:** concatenations fed in one call, and split at every offset.

## 3. The Python binding never releases the GIL

- **Cause:** `bindings/python/compress_utils_py.cpp` has no `py::gil_scoped_release`, so every `compress` / `decompress` / stream `write` / `finish` call holds the GIL for its whole duration.
- **Measured:** decoding 1.5 GB in a worker thread takes 4.4 s alone and 8.8 s next to a busy Python thread, which only gets in between calls.
- **Fix:**
  - Wrap the C++ calls in `py::gil_scoped_release`: take the `buffer_info` before releasing, and build `py::bytes` after re-acquiring.
  - Stream objects then need a `std::mutex`, or a documented "not thread-safe" note.

## 4. Wheels ship without `py.typed`

- **Cause:** `.gitignore` line 48 is `py.typed`, so `bindings/python/compress_utils/py.typed` was never committed. Local builds include it; CI wheels built from a clean checkout don't. mypy then reports `import-untyped`, despite the README.
- **Fix:** delete the `.gitignore` line, commit the empty `py.typed`, and add a CI check that the built wheel contains it.

## 5. zstd window and threading options

- **Long-distance matching:** there is no option for it, and it matters a lot for mailbox-like data with far-apart repeats. On a 500 MB slice of the archive, `zstd -10 --long=27` reaches 3.70× at 268 MB/s, while compress-utils level 5 reaches 3.00× at 133 MB/s.
  - **Proposal:** a C function `cu_compress_stream_create_ex(algo, level, const cu_stream_options_t*)` with `window_log`, `long_distance`, and `threads`, mapped to `ZSTD_c_windowLog`, `ZSTD_c_enableLongDistanceMatching`, and `ZSTD_c_nbWorkers`.
  - In Python: `CompressStream("zstd", 5, window_log=27, long=True, threads=0)`.
  - Default `window_log` to 27 when `long=True`: every zstd decoder reads that without extra flags.
- **Multithreading:** `lib/compress/zstdmt_compress.c`, `pool.c`, and `threading.c` are compiled, but `third_party/manifest.json` doesn't define `ZSTD_MULTITHREAD`, so zstd would reject `nbWorkers`. Define it and link pthreads for native builds, not WASM.
- **Large-window decoding:** the decoder rejects windows over 128 MB. A file made with `zstd --long=30` fails with "Frame requires too much memory for decoding". Set `ZSTD_DCtx_setParameter(ds, ZSTD_d_windowLogMax, 31)` in `zstd_dstream_create`; memory is only allocated when a frame asks for it.

## 6. Smaller items

- **Level cliff:** `cu_scale_level` maps 5 → zstd 11, 6 → 13, and 10 → 22. From 13 up, zstd uses much slower match finders: 133 MB/s at user level 5, 24 MB/s at 6, and 6 MB/s at 10, for about 3% better ratio. Consider a hand-tuned table.
- **Assembly disabled:** `ZSTD_DISABLE_ASM` in the manifest turns off zstd's x86-64 assembly Huffman decoder, which slows decoding on x86-64 Linux and Windows. Arm isn't affected. Consider enabling it for native x86-64 wheels.
- **No macOS x86_64 wheel:** Intel Macs build from the sdist, which needs a compiler.
- **Unverified:** `zstd_dstream_finish` doesn't call `ZSTD_decompressStream` to flush output zstd may still hold when `pending_len == 0`. Full-archive round trips came out byte-identical, so this may never trigger, but it's worth a test.

## Workarounds in mbox-md to remove once fixed

- **Guards against concatenated input (#2):** a zstd frame walk before decoding, and a gzip ISIZE check after (`src/mbox_md/_compression.py`).
- **GIL (#3):** a lowered `sys.setswitchinterval` while decoding.
- **Types (#4):** a mypy `follow_untyped_imports` override.
- **Chunk size (#1):** mbox-md should feed 256 KB chunks until this is fixed.
