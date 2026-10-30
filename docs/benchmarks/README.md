# compress-utils benchmarks

Throughput and compression ratio of compress-utils, in the C core and in every binding, against the libraries a developer would otherwise use in each language, measured on two cloud machines on 2026-10-29 (commit c3ae4b7 plus driver fixes); where compress-utils is behind is listed below.

## Pages

- [C core](c.md): the C library against native libraries (libzstd, libbrotli, liblz4, liblzma, LZMA SDK, libbz2, zlib, zlib-ng, libdeflate, ISA-L, miniz, zlib-rs, google-snappy).
- [Python](python.md): the Python binding against the standard library, cramjam, zstandard, brotli and lz4.
- [Go](go.md): the Go binding against the standard library, klauspost/compress, andybalholm/brotli, pierrec/lz4, ulikunitz/xz and dsnet/compress.
- [Rust](rust.md): the Rust binding against flate2 (miniz_oxide and zlib-rs), zstd, brotli, bzip2, xz2, lz4_flex and snap.
- [WASM](wasm.md): the WASM binding on Node and Bun against fflate, pako, brotli-wasm, zstd-wasm, fzstd, xz-decompress, lz4js, snappyjs and `CompressionStream`.
- [Bindings against the C core](bindings.md): what each binding costs relative to the C core, per format.
- [Memory](memory.md): peak memory of one compress or decompress, C core against native libraries.

## Headline results

All numbers are dickens (10.2 MB of English text), one-shot, level 6, on c8g unless stated; every other input, level and machine is on the pages above. Competitors run at the native level matched to ours by compression ratio, and only libraries within 5% of our ratio count as comparable.

### The C core against the fastest native library

Each cell names the fastest comparable library at level 6 (libdeflate and ISA-L only for the deflate formats, LZMA SDK for xz, and so on) and its speed divided by ours: above 1.00x it is faster than compress-utils, below 1.00x compress-utils is ahead of every comparable library. Library variants (a reused context, a fitted dictionary) are not counted here and are on the [C page](c.md).

| Format | c8g compress | c8g decompress | c8i compress | c8i decompress |
| :--- | ---: | ---: | ---: | ---: |
| zstd | libzstd 1.04x | libzstd 1.03x | libzstd 1.00x | libzstd 1.01x |
| brotli | libbrotli 1.13x | libbrotli 1.10x | libbrotli 1.05x | libbrotli 1.04x |
| zlib | libdeflate 1.01x | libdeflate 1.35x | libdeflate 1.20x | libdeflate 1.38x |
| gzip | libdeflate 1.01x | libdeflate 1.37x | libdeflate 1.20x | libdeflate 1.36x |
| deflate | libdeflate 1.01x | libdeflate 1.35x | libdeflate 1.21x | libdeflate 1.39x |
| xz | LZMA SDK 1.03x | LZMA SDK 1.06x | LZMA SDK 1.12x | liblzma 1.00x |
| lz4 | liblz4 0.99x | liblz4 1.01x | liblz4 1.00x | liblz4 1.00x |
| bz2 | libbz2-rs 1.06x | libbz2-rs 1.09x | libbz2-rs 1.01x | libbz2-rs 1.03x |
| snappy | google-snappy 1.08x | google-snappy 1.06x | google-snappy 1.10x | google-snappy 0.77x |

### Each binding against its ecosystem

For every format and direction (compress, decompress) with a comparable peer in the language's ecosystem, compress-utils is compared with the fastest peer at level 6. Cells are ahead / level (within 10%) / behind; the last number is the cells counted. A peer outside 5% of our ratio is not counted (klauspost zstd, for one).

| Binding | c8g: ahead / level / behind | c8i: ahead / level / behind |
| :--- | ---: | ---: |
| Python | 7 / 11 / 0 of 18 | 9 / 7 / 2 of 18 |
| Go | 9 / 1 / 3 of 13 | 10 / 0 / 3 of 13 |
| Rust | 11 / 5 / 0 of 16 | 10 / 6 / 0 of 16 |
| WASM (Node) | 11 / 1 / 0 of 12 | 11 / 1 / 0 of 12 |
| WASM (Bun) | 11 / 1 / 0 of 12 | 10 / 2 / 0 of 12 |

### Each binding against the C core

Binding speed divided by C core speed on dickens one-shot: the median over formats, with the range in brackets (geometric mean over levels for each format). See [bindings](bindings.md) for every cell.

| Binding | c8g compress | c8g decompress | c8i compress | c8i decompress |
| :--- | ---: | ---: | ---: | ---: |
| Python | 1.00 (0.97 to 1.06) | 0.99 (0.96 to 1.01) | 0.96 (0.71 to 1.00) | 0.84 (0.66 to 1.00) |
| Go | 0.97 (0.93 to 1.00) | 0.95 (0.90 to 0.99) | 0.95 (0.73 to 1.00) | 0.93 (0.66 to 0.98) |
| Rust | 1.00 (0.99 to 1.06) | 1.00 (0.98 to 1.02) | 0.98 (0.98 to 1.01) | 0.98 (0.90 to 1.02) |
| WASM (Node) | 0.74 (0.65 to 0.86) | 0.74 (0.62 to 1.08) | 0.80 (0.75 to 0.94) | 0.65 (0.57 to 0.86) |
| WASM (Bun) | 0.78 (0.62 to 0.90) | 0.69 (0.54 to 0.90) | 0.78 (0.66 to 0.90) | 0.59 (0.35 to 0.81) |

## Where compress-utils is behind

Multiples are the competitor's speed divided by ours (geometric mean over levels, dickens one-shot, both machines), so above 1.00x the competitor is faster.

- **libdeflate on the deflate formats (zlib, gzip, deflate).** libdeflate compresses 1.09 to 1.20x and decompresses 1.40 to 1.50x as fast as compress-utils at ratios within a few percent of ours. The one-shot decompress figure depends on a harness choice: libdeflate has no streaming API, so when the output buffer (3 times the input at the start) is too small it restarts, and that cost is inside the timing (see the sizing note under Methodology).
- **ISA-L on the deflate formats.** ISA-L offers levels 1 to 3 only. Compress 1.12 to 2.62x, decompress 0.61 to 1.56x of our speed across both machines; its position differs by machine, so read it per machine on the [C page](c.md).
- **LZMA SDK on xz compress.** Its encoder compresses 1.43 to 1.55x as fast as ours at a matched ratio; its decompress is 0.71 to 1.07x. liblzma itself is level with us.
- **Native C against WASM.** Through the WASM binding on Node, compress-utils runs at 0.65 to 0.94x of the C core's speed on compress and 0.57 to 1.08x on decompress, by format and machine ([bindings](bindings.md)); native `node:zlib` and zstd are not in this comparison.
- **The Rust zstd crate on streaming decompress.** On dickens streaming at matched levels the crate decompresses 1.05x on c8g and 1.07x on c8i as fast as ours (geometric mean over levels).

Every library that is more than 10% faster than compress-utils on dickens one-shot on at least one machine, within 5% of our ratio on the first machine and not a library variant, is in this table (speed multiples, geometric mean over levels; `-` where the library has no such direction):

| Binding | Format | Library | c8g comp | c8g dec | c8i comp | c8i dec | Ratio vs ours |
| :--- | :--- | :--- | ---: | ---: | ---: | ---: | ---: |
| C | zlib | libdeflate | 1.09x | **1.40x** | **1.20x** | **1.49x** | 1.05 |
| C | zlib | zlib-rs | 0.89x | 1.02x | 1.00x | **1.12x** | 1.00 |
| C | gzip | libdeflate | 1.09x | **1.41x** | **1.19x** | **1.48x** | 1.05 |
| C | deflate | libdeflate | 1.09x | **1.40x** | **1.20x** | **1.50x** | 1.05 |
| C | deflate | zlib-rs | 0.89x | 1.02x | 1.00x | **1.12x** | 1.00 |
| C | xz | LZMA SDK | **1.43x** | 1.07x | **1.55x** | 0.71x | 1.00 |
| C | snappy | google-snappy | 1.08x | 1.06x | **1.10x** | 0.77x | 1.00 |
| Python | brotli | brotli (PyPI) | 1.04x | 1.01x | 1.04x | **1.10x** | 1.00 |
| Go | zstd | klauspost/compress | **2.70x** | 0.95x | **3.08x** | 0.73x | 0.95 |
| Go | zlib | klauspost/compress | 1.03x | 0.35x | **1.14x** | 0.34x | 1.02 |
| Go | gzip | klauspost/compress | 1.09x | 0.39x | **1.20x** | 0.36x | 1.02 |

## Methodology

**Inputs.** `dickens` (10.2 MB of English text, compresses about 1.7 to 3.6 times depending on codec and level) one-shot and streaming in 64 KiB chunks, `xml` (repetitive) and `x-ray` (hard to compress) one-shot, and dickens cut into 4 KB messages (256 messages, one call per message, timed over the set). All from the Silesia corpus, fetched and pinned by `benchmarks/corpus/`. All ten compress-utils levels are run in every binding.

**Decompress is given no output size.** Every driver decompresses without being told the original size: each implementation uses what the compressed bytes record (zstd, lz4 and snappy sizes, our own size hint) or its library's unknown-size API. In C, a library with no hint starts from a buffer of 3 times the input and doubles it on a miss; libraries that can only decode into a whole buffer (libdeflate, miniz, google-snappy) restart the decode with the larger buffer, libraries with a streaming decoder continue it, and both are timed. Streaming decompress writes into a reused 64 KiB window. Details and what each driver does: [benchmark-decompress-sizing.md](../benchmark-decompress-sizing.md).

**Level mapping is by ratio.** Libraries number their levels differently, so each competitor runs at the native level that gives about the same compression ratio as each of our ten levels ([levels.json](../../benchmarks/levels.json), 2% tolerance, checked by `benchmarks/check_levels.py`, which also checks that the native baseline is byte-identical to compress-utils). A competitor that has no levels (lz4_flex, snap, snappyjs, lz4js, google-snappy) is a single point and is compared with the compress-utils level closest in ratio. Libraries that cannot match our ratio range (ISA-L stops at level 3, klauspost zstd tops out lower) show only where they overlap.

**One sample per cell, and the noise rule.** Large files run one timed sample after one warmup, the 4 KB message tier three samples; nothing is pinned to a core. Treat any gap under 10% as a tie: every table marks only multiples beyond 10%, and the charts show every point so scatter is visible.

**Machines and versions.** c8g is AWS c8g.xlarge (Graviton4, arm64), Linux 7.0.0-1014-aws; c8i is AWS c8i.xlarge (Intel Xeon 6975P-C, x86-64), Linux 7.0.0-1014-aws. Single-threaded runs on one instance of each type, one day. compress-utils 0.11.2. Competitor versions as recorded in the results: google-snappy 1.3.1, isa-l 2.32.1, libbz2-rs 0.2.5, libdeflate 1.26, libdeflate-percall 1.26, lzma-sdk 26.05, miniz 3.1.2, zlib-ng 2.3.3, zlib-rs 0.6.8, brotli andybalholm/brotli v1.2.7, bzip2 dsnet/compress v0.0.1, klauspost klauspost/compress v1.20.3, lz4 pierrec/lz4 v4.1.33, stdlib go1.26.8, xz ulikunitz/xz v0.5.17, brotli-wasm 3.0.1, compression-stream bun 1.3.10 (web standard), fflate 0.8.3, fzstd 0.1.1, lz4js 0.2.0, pako 3.0.2, snappyjs 0.7.0, xz-decompress 0.2.3, zstd-wasm 0.0.27, brotli 1.2.1, cramjam 2.14.0, lz4 4.4.5, stdlib python 3.14.4, zlib 1.3.1, zstd 1.5.7, zstandard 0.25.0, brotli brotli crate 9, bzip2 bzip2 crate 0.6 (libbz2-rs-sys), flate2 flate2 1.1 (miniz_oxide), flate2-zlib-rs flate2 1.1 (zlib-rs), lz4_flex lz4_flex 0.14, snap snap 1.1, xz2 xz2 crate 0.1 (bundled liblzma 5.2), zstd zstd crate 0.14 (libzstd 1.5.7).

**Reproducing.** The harness is documented in [benchmarks/README.md](../../benchmarks/README.md). The sweep is `benchmarks/run_ecosystem.sh` (stages: c, python, go, rust, js-node, js-bun), memory is `benchmarks/memory.py`, and `python3 benchmarks/publish.py all --runs c8g=<dir> c8i=<dir>` regenerates every page, chart and README block here from the run directories. The raw run JSON (about 5 MB per machine) is not committed; it is attached to a GitHub release of this repository, and `benchmarks/published/summary.json` holds what these pages use.

## Limitations

The full list is in [benchmark-shortcomings.md](../benchmark-shortcomings.md); the ones that most affect how to read these pages:

- **Thin corpus.** Three files and one message set; streaming and 4 KB messages are dickens only. Executables, incompressible data, messages under 256 bytes and realistic message corpora (logs, JSON, protobuf) are not covered.
- **One sample per cell, no variance reported.** Treat gaps under 10% as ties; one instance of each machine type on one day, so noisy neighbours are possible.
- **Level mapping is by hand.** Where a competitor's levels cover a different ratio range, a speed multiple at a level can mislead (klauspost zstd); the charts show speed against ratio so this is visible.
- **Library variants and the harness's decompress policy are choices.** A whole-buffer-only library pays for restarts when the starting guess is too small, so its xml decompress rows depend on the file's ratio; another starting multiple would give other numbers.
- **Not measured:** multi-threading, tail latency, browsers (Node and Bun stand in), binary size outside C and JavaScript, memory outside C, and snappy streaming in Go and Rust.
- **Build flags of the competitors** were not systematically checked against each project's recommended flags and CPU dispatch.

---

Generated by benchmarks/publish.py from run 2026-10-29, commit c3ae4b7 plus driver fixes; do not edit by hand.
