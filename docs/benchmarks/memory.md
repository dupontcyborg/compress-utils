# Memory: the C core against native libraries

How much memory one compress or decompress needs, measured for the C core and the native libraries on Linux: for example xz level 6 compresses dickens one-shot with 93 MiB over the output on c8g for compress-utils against 93 MiB for plain liblzma.

What is measured: peak resident set size of one call minus what the call had to write (the compressed bytes when compressing; the decompressed bytes one-shot or a 64 KiB window when streaming), in a fresh process per measurement, in MiB. Three cases: dickens one-shot (10.2 MB), dickens streaming in 64 KiB chunks, and one 4 KiB message (the first 4 KiB of dickens: the fixed cost of one call on a small message). Levels 1, 6 and 10 are our levels; competitors use the matching native level. One sample per cell, C only, Linux only.

Caveats: resident set size counts touched pages only, so a large dictionary or table the codec never fills does not show; the peak includes allocator slack and the codec's own input and output copies; `malloc_trim` between phases is glibc-only; there are no allocation counts, no per-context footprint with many live contexts, and nothing for the other bindings (in WASM the heap is the constraint, in Python, Go and Rust the runtime's allocator adds its own). Variants of a library (a reused context, a fitted xz dictionary) are listed as separate rows. See [shortcomings](../benchmark-shortcomings.md#what-is-not-measured).

## AWS c8g.xlarge (Graviton4, arm64)

### Compress, c8g: MiB over the output written

| Format | Library | L1 one-shot | L1 stream | L1 4 KiB | L6 one-shot | L6 stream | L6 4 KiB | L10 one-shot | L10 stream | L10 4 KiB |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **zstd** | **compress-utils** | **2.0** | **3.7** | **0.50** | **35** | **38** | **0.63** | **258** | **719** | **0.62** |
| zstd | libzstd | 1.8 | 3.0 | 0.27 | 34 | 38 | 0.46 | 257 | 719 | 0.45 |
| zstd | libzstd (reused context) | 2.5 | - | 0.25 | 34 | - | 0.37 | 259 | - | 0.37 |
| **brotli** | **compress-utils** | **2.8** | **1.5** | **0.42** | **37** | **38** | **3.4** | **75** | **75** | **3.6** |
| brotli | libbrotli | 2.0 | 2.1 | 0.42 | 38 | 38 | 4.7 | 77 | 75 | 3.6 |
| **zlib** | **compress-utils** | **2.5** | **1.9** | **0.51** | **1.7** | **0.97** | **0.53** | **2.3** | **1.5** | **0.37** |
| zlib | ISA-L | 1.3 | 0.46 | 0.24 | - | - | - | - | - | - |
| zlib | libdeflate | 0.95 | - | 0.21 | 1.6 | - | 0.29 | 1.0 | - | 0.29 |
| zlib | miniz | 1.8 | 2.0 | 0.19 | 1.1 | 2.0 | 0.19 | 2.6 | 1.5 | 0.19 |
| zlib | zlib | 1.2 | 1.5 | 0.39 | 2.3 | 2.2 | 0.46 | 0.69 | 2.2 | 0.47 |
| zlib | zlib-ng | 1.8 | 1.0 | 0.26 | 2.1 | 2.1 | 0.28 | 1.5 | 1.2 | 0.20 |
| zlib | zlib-rs | 1.8 | 2.0 | 0.47 | 1.6 | 2.1 | 0.28 | 0.91 | 0.86 | 0.47 |
| **gzip** | **compress-utils** | **2.2** | **2.1** | **0.51** | **2.3** | **1.5** | **0.53** | **2.3** | **0.94** | **0.53** |
| gzip | ISA-L | 1.8 | 2.1 | 0.24 | - | - | - | - | - | - |
| gzip | libdeflate | 0.70 | - | 0.21 | 2.1 | - | 0.29 | 1.8 | - | 0.29 |
| gzip | zlib | 0.79 | 0.77 | 0.39 | 1.3 | 1.4 | 0.37 | 1.4 | 2.1 | 0.46 |
| gzip | zlib-ng | 0.49 | 1.6 | 0.26 | 0.84 | 1.6 | 0.28 | 1.4 | 1.2 | 0.28 |
| gzip | zlib-rs | 2.2 | 1.3 | 0.28 | 1.4 | 1.0 | 0.47 | 1.3 | 2.3 | 0.47 |
| **deflate** | **compress-utils** | **0.80** | **3.9** | **0.43** | **2.9** | **1.1** | **0.53** | **0.94** | **2.0** | **0.53** |
| deflate | ISA-L | 0.91 | 1.8 | 0.24 | - | - | - | - | - | - |
| deflate | libdeflate | 0.57 | - | 0.21 | 0.74 | - | 0.29 | 1.4 | - | 0.29 |
| deflate | miniz | 2.1 | 0.64 | 0.19 | 2.0 | 0.70 | 0.19 | 1.8 | 1.1 | 0.19 |
| deflate | zlib | 2.2 | 1.7 | 0.39 | 1.3 | 0.84 | 0.46 | 1.6 | 3.8 | 0.47 |
| deflate | zlib-ng | 1.3 | 0.40 | 0.27 | 0.53 | 1.7 | 0.28 | 2.1 | 1.9 | 0.28 |
| deflate | zlib-rs | 1.6 | 1.9 | 0.47 | 1.2 | 2.3 | 0.47 | 1.3 | 1.6 | 0.47 |
| **xz** | **compress-utils** | **4.6** | **4.2** | **0.63** | **93** | **91** | **2.8** | **123** | **156** | **0.31** |
| xz | liblzma | 3.8 | 4.2 | 0.69 | 93 | 93 | 15 | 156 | 156 | 63 |
| xz | LZMA SDK | 7.7 | 8.2 | 4.2 | 123 | 156 | 2.0 | 123 | 347 | 4.2 |
| xz | liblzma (fitted dictionary) | 3.0 | - | 0.40 | 93 | - | 0.58 | 123 | - | 0.68 |
| **lz4** | **compress-utils** | **2.1** | **2.5** | **0.39** | **0.82** | **1.2** | **0.69** | **1.2** | **1.2** | **0.70** |
| lz4 | liblz4 | 1.8 | 1.2 | 0.20 | 0.67 | 1.6 | 0.49 | 1.9 | 2.3 | 2.5 |
| **bz2** | **compress-utils** | **2.3** | **4.9** | **0.38** | **6.2** | **6.4** | **0.35** | **8.1** | **7.7** | **0.35** |
| bz2 | libbz2 | 2.3 | 2.1 | 0.36 | 4.9 | 6.0 | 0.35 | 7.1 | 7.4 | 0.35 |
| bz2 | libbz2-rs | 1.3 | 2.0 | 1.1 | 4.9 | 5.3 | 0.37 | 7.6 | 10 | 0.12 |
| **snappy** | **compress-utils** | **0.54** | **18** | **0.20** | **1.8** | **20** | **0.20** | **0.54** | **21** | **0.20** |
| snappy | google-snappy | 0.23 | - | 0.14 | 0.85 | - | 0.13 | 0.92 | - | 0.14 |
| snappy | snappy-core (our core, called directly) | 1.2 | - | 0.08 | 1.6 | - | 0.08 | 1.8 | - | 0.07 |

### Decompress, c8g: MiB over the output written

| Format | Library | L1 one-shot | L1 stream | L1 4 KiB | L6 one-shot | L6 stream | L6 4 KiB | L10 one-shot | L10 stream | L10 4 KiB |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **zstd** | **compress-utils** | **0.06** | **2.7** | **0.06** | **0.09** | **4.4** | **0.06** | **0.08** | **12** | **0.06** |
| zstd | libzstd | 0.06 | 3.3 | 0.06 | 0.03 | 4.3 | 0.00 | 0.02 | 11 | 0.00 |
| zstd | libzstd (reused context) | 2.1 | - | 0.05 | 0.03 | - | 0.02 | 0.03 | - | 0.02 |
| **brotli** | **compress-utils** | **4.5** | **6.5** | **0.13** | **4.3** | **5.0** | **0.14** | **4.3** | **6.2** | **0.14** |
| brotli | libbrotli | 4.3 | 5.2 | 0.07 | 4.3 | 4.1 | 0.07 | 4.5 | 4.7 | 0.08 |
| **zlib** | **compress-utils** | **0.06** | **0.00** | **0.07** | **0.06** | **0.03** | **0.07** | **0.06** | **0.00** | **2.1** |
| zlib | ISA-L | 0.01 | 0.00 | 0.01 | - | - | - | - | - | - |
| zlib | libdeflate | 0.01 | - | 0.01 | 0.01 | - | 0.01 | 0.01 | - | 0.01 |
| zlib | miniz | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 |
| zlib | zlib | 0.06 | 0.05 | 0.07 | 0.00 | 0.03 | 0.01 | 0.01 | 0.00 | 0.01 |
| zlib | zlib-ng | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.01 | 0.00 | 0.03 | 2.0 |
| zlib | zlib-rs | 0.00 | 0.00 | 0.00 | 0.01 | 0.04 | 2.0 | 0.00 | 0.00 | 0.00 |
| **gzip** | **compress-utils** | **0.07** | **0.00** | **0.07** | **0.06** | **0.00** | **0.07** | **0.06** | **0.00** | **0.07** |
| gzip | ISA-L | 0.01 | 0.00 | 0.01 | - | - | - | - | - | - |
| gzip | libdeflate | 0.01 | - | 0.01 | 0.01 | - | 0.00 | 0.01 | - | 0.00 |
| gzip | zlib | 0.06 | 0.05 | 0.07 | 0.00 | 0.00 | 0.01 | 0.00 | 0.00 | 0.01 |
| gzip | zlib-ng | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.01 | 0.00 | 0.00 | 0.01 |
| gzip | zlib-rs | 0.00 | 0.00 | 0.02 | 0.01 | 0.00 | 0.00 | 0.00 | 0.04 | 0.00 |
| **deflate** | **compress-utils** | **0.06** | **2.0** | **2.1** | **0.00** | **0.00** | **0.07** | **0.07** | **0.00** | **0.07** |
| deflate | ISA-L | 0.01 | 0.00 | 0.01 | - | - | - | - | - | - |
| deflate | libdeflate | 0.01 | - | 0.01 | 0.01 | - | 0.01 | 0.01 | - | 0.01 |
| deflate | miniz | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.03 | 0.00 |
| deflate | zlib | 0.06 | 0.05 | 0.07 | 0.00 | 0.00 | 0.01 | 0.00 | 0.00 | 0.01 |
| deflate | zlib-ng | 0.01 | 0.00 | 0.00 | 0.00 | 0.00 | 0.01 | 0.00 | 0.00 | 0.01 |
| deflate | zlib-rs | 0.00 | 0.00 | 0.00 | 0.04 | 0.00 | 0.00 | 0.00 | 2.0 | 0.00 |
| **xz** | **compress-utils** | **0.25** | **0.33** | **0.08** | **9.5** | **8.3** | **0.08** | **13** | **10** | **0.06** |
| xz | liblzma | 0.19 | 0.27 | 0.02 | 8.3 | 9.5 | 2.0 | 11 | 10 | 0.04 |
| xz | LZMA SDK | 0.00 | 0.00 | 0.00 | 0.00 | 11 | 0.00 | 2.0 | 11 | 0.00 |
| xz | liblzma (fitted dictionary) | 0.19 | - | 0.02 | 8.9 | - | 0.02 | 12 | - | 0.02 |
| **lz4** | **compress-utils** | **0.11** | **0.12** | **0.07** | **0.00** | **0.05** | **0.00** | **1.9** | **0.00** | **0.00** |
| lz4 | liblz4 | 0.11 | 0.07 | 0.01 | 0.06 | 0.00 | 0.00 | 2.1 | 0.00 | 2.0 |
| **bz2** | **compress-utils** | **2.1** | **0.00** | **0.08** | **4.1** | **2.1** | **2.1** | **4.7** | **4.3** | **2.1** |
| bz2 | libbz2 | 0.02 | 0.00 | 0.02 | 3.0 | 3.3 | 2.0 | 4.0 | 4.4 | 2.0 |
| bz2 | libbz2-rs | 2.1 | 2.0 | 0.00 | 2.3 | 3.3 | 3.5 | 3.6 | 4.5 | 4.2 |
| **snappy** | **compress-utils** | **0.06** | **18** | **0.06** | **0.06** | **19** | **0.06** | **0.06** | **18** | **0.06** |
| snappy | google-snappy | 0.00 | - | 0.00 | 0.00 | - | 0.00 | 0.00 | - | 0.00 |
| snappy | snappy-core (our core, called directly) | 0.06 | - | 0.06 | 0.06 | - | 0.06 | 0.06 | - | 0.06 |

## AWS c8i.xlarge (Intel Xeon 6975P-C, x86-64)

### Compress, c8i: MiB over the output written

| Format | Library | L1 one-shot | L1 stream | L1 4 KiB | L6 one-shot | L6 stream | L6 4 KiB | L10 one-shot | L10 stream | L10 4 KiB |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **zstd** | **compress-utils** | **1.0** | **2.1** | **0.63** | **33** | **37** | **0.66** | **257** | **715** | **0.67** |
| zstd | libzstd | 0.81 | 1.9 | 0.36 | 33 | 37 | 0.60 | 257 | 715 | 0.62 |
| zstd | libzstd (reused context) | 0.86 | - | 0.43 | 33 | - | 0.46 | 257 | - | 0.50 |
| **brotli** | **compress-utils** | **1.2** | **0.85** | **0.61** | **37** | **38** | **3.8** | **73** | **73** | **2.1** |
| brotli | libbrotli | 1.1 | 0.86 | 0.61 | 37 | 38 | 3.5 | 73 | 73 | 1.9 |
| **zlib** | **compress-utils** | **0.65** | **0.65** | **0.48** | **0.68** | **0.67** | **0.49** | **0.71** | **0.71** | **0.48** |
| zlib | ISA-L | 0.62 | 0.60 | 0.37 | - | - | - | - | - | - |
| zlib | libdeflate | 0.33 | - | 0.27 | 0.71 | - | 0.34 | 0.69 | - | 0.35 |
| zlib | miniz | 0.34 | 0.22 | 0.25 | 0.34 | 0.34 | 0.13 | 0.24 | 0.24 | 0.13 |
| zlib | zlib | 0.53 | 0.53 | 0.36 | 0.56 | 0.56 | 0.43 | 0.47 | 0.34 | 0.37 |
| zlib | zlib-ng | 0.42 | 0.42 | 0.25 | 0.45 | 0.45 | 0.26 | 0.37 | 0.36 | 0.26 |
| zlib | zlib-rs | 0.44 | 0.43 | 0.51 | 0.52 | 0.66 | 0.51 | 0.68 | 0.69 | 0.51 |
| **gzip** | **compress-utils** | **0.65** | **0.65** | **0.48** | **0.68** | **0.67** | **0.48** | **0.59** | **0.71** | **0.48** |
| gzip | ISA-L | 0.62 | 0.62 | 0.37 | - | - | - | - | - | - |
| gzip | libdeflate | 0.33 | - | 0.26 | 0.71 | - | 0.34 | 0.69 | - | 0.34 |
| gzip | zlib | 0.59 | 0.54 | 0.39 | 0.57 | 0.61 | 0.40 | 0.41 | 0.50 | 0.40 |
| gzip | zlib-ng | 0.42 | 0.42 | 0.25 | 0.45 | 0.45 | 0.25 | 0.36 | 0.36 | 0.26 |
| gzip | zlib-rs | 0.49 | 0.55 | 0.45 | 0.64 | 0.52 | 0.51 | 0.57 | 0.70 | 0.56 |
| **deflate** | **compress-utils** | **0.65** | **0.65** | **0.48** | **0.68** | **0.68** | **0.48** | **0.59** | **0.71** | **0.48** |
| deflate | ISA-L | 0.60 | 0.61 | 0.37 | - | - | - | - | - | - |
| deflate | libdeflate | 0.32 | - | 0.26 | 0.71 | - | 0.34 | 0.68 | - | 0.34 |
| deflate | miniz | 0.35 | 0.34 | 0.25 | 0.34 | 0.35 | 0.13 | 0.24 | 0.24 | 0.13 |
| deflate | zlib | 0.53 | 0.52 | 0.24 | 0.61 | 0.56 | 0.24 | 0.35 | 0.34 | 0.41 |
| deflate | zlib-ng | 0.42 | 0.42 | 0.25 | 0.45 | 0.45 | 0.26 | 0.37 | 0.36 | 0.25 |
| deflate | zlib-rs | 0.44 | 0.49 | 0.57 | 0.63 | 0.64 | 0.51 | 0.57 | 0.55 | 0.51 |
| **xz** | **compress-utils** | **2.8** | **3.0** | **0.64** | **91** | **91** | **0.88** | **119** | **140** | **0.89** |
| xz | liblzma | 2.7 | 2.7 | 0.80 | 90 | 90 | 6.6 | 140 | 140 | 7.5 |
| xz | LZMA SDK | 6.3 | 6.3 | 4.3 | 120 | 154 | 2.8 | 120 | 346 | 2.8 |
| xz | liblzma (fitted dictionary) | 2.6 | - | 0.38 | 90 | - | 0.63 | 119 | - | 0.63 |
| **lz4** | **compress-utils** | **0.45** | **0.56** | **0.39** | **0.63** | **0.75** | **0.60** | **0.59** | **0.84** | **0.61** |
| lz4 | liblz4 | 0.39 | 0.39 | 0.33 | 0.46 | 0.52 | 0.49 | 0.48 | 0.72 | 0.43 |
| **bz2** | **compress-utils** | **1.2** | **1.4** | **0.40** | **4.7** | **4.7** | **0.40** | **6.6** | **6.7** | **0.40** |
| bz2 | libbz2 | 1.2 | 1.2 | 0.40 | 4.7 | 4.6 | 0.40 | 6.5 | 6.6 | 0.34 |
| bz2 | libbz2-rs | 0.97 | 1.0 | 0.19 | 4.4 | 4.4 | 0.19 | 6.4 | 6.4 | 0.19 |
| **snappy** | **compress-utils** | **0.30** | **16** | **0.25** | **0.30** | **16** | **0.25** | **0.30** | **16** | **0.25** |
| snappy | google-snappy | 0.12 | - | 0.14 | 0.13 | - | 0.17 | 0.12 | - | 0.14 |
| snappy | snappy-core (our core, called directly) | 0.19 | - | 0.14 | 0.19 | - | 0.14 | 0.18 | - | 0.14 |

### Decompress, c8i: MiB over the output written

| Format | Library | L1 one-shot | L1 stream | L1 4 KiB | L6 one-shot | L6 stream | L6 4 KiB | L10 one-shot | L10 stream | L10 4 KiB |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| **zstd** | **compress-utils** | **0.22** | **1.5** | **0.21** | **0.15** | **4.3** | **0.14** | **0.15** | **9.8** | **0.14** |
| zstd | libzstd | 0.10 | 1.5 | 0.21 | 0.09 | 4.3 | 0.11 | 0.03 | 9.6 | 0.07 |
| zstd | libzstd (reused context) | 0.04 | - | 0.06 | 0.05 | - | 0.02 | 0.03 | - | 0.02 |
| **brotli** | **compress-utils** | **4.0** | **4.1** | **0.02** | **4.2** | **4.1** | **0.07** | **4.1** | **4.2** | **0.02** |
| brotli | libbrotli | 4.0 | 4.1 | 0.08 | 4.3 | 4.1 | 0.07 | 4.2 | 4.1 | 0.08 |
| **zlib** | **compress-utils** | **0.07** | **0.12** | **0.10** | **0.07** | **0.14** | **0.11** | **0.07** | **0.14** | **0.10** |
| zlib | ISA-L | 0.00 | 0.00 | 0.00 | - | - | - | - | - | - |
| zlib | libdeflate | 0.01 | - | 0.00 | 0.01 | - | 0.00 | 0.01 | - | 0.00 |
| zlib | miniz | 0.01 | 0.03 | 0.11 | 0.01 | 0.03 | 0.01 | 0.01 | 0.04 | 0.01 |
| zlib | zlib | 0.01 | 0.03 | 0.05 | 0.01 | 0.03 | 0.11 | 0.01 | 0.09 | 0.05 |
| zlib | zlib-ng | 0.01 | 0.03 | 0.00 | 0.01 | 0.03 | 0.03 | 0.01 | 0.03 | 0.01 |
| zlib | zlib-rs | 0.04 | 0.04 | 0.02 | 0.02 | 0.04 | 0.01 | 0.02 | 0.04 | 0.01 |
| **gzip** | **compress-utils** | **0.01** | **0.06** | **0.04** | **0.01** | **0.08** | **0.04** | **0.01** | **0.08** | **0.04** |
| gzip | ISA-L | 0.00 | 0.00 | 0.00 | - | - | - | - | - | - |
| gzip | libdeflate | 0.01 | - | 0.00 | 0.01 | - | 0.00 | 0.01 | - | 0.00 |
| gzip | zlib | 0.01 | 0.03 | 0.11 | 0.01 | 0.03 | 0.11 | 0.01 | 0.03 | 0.11 |
| gzip | zlib-ng | 0.01 | 0.03 | 0.00 | 0.01 | 0.03 | 0.01 | 0.01 | 0.03 | 0.03 |
| gzip | zlib-rs | 0.02 | 0.04 | 0.02 | 0.02 | 0.04 | 0.01 | 0.02 | 0.04 | 0.01 |
| **deflate** | **compress-utils** | **0.07** | **0.12** | **0.11** | **0.07** | **0.14** | **0.10** | **0.07** | **0.14** | **0.10** |
| deflate | ISA-L | 0.00 | 0.00 | 0.00 | - | - | - | - | - | - |
| deflate | libdeflate | 0.01 | - | 0.00 | 0.01 | - | 0.00 | 0.01 | - | 0.00 |
| deflate | miniz | 0.01 | 0.03 | 0.12 | 0.01 | 0.03 | 0.01 | 0.01 | 0.04 | 0.01 |
| deflate | zlib | 0.01 | 0.03 | 0.07 | 0.01 | 0.03 | 0.07 | 0.07 | 0.09 | 0.11 |
| deflate | zlib-ng | 0.01 | 0.03 | 0.00 | 0.01 | 0.03 | 0.01 | 0.01 | 0.03 | 0.01 |
| deflate | zlib-rs | 0.05 | 0.04 | 0.02 | 0.04 | 0.04 | 0.02 | 0.04 | 0.04 | 0.02 |
| **xz** | **compress-utils** | **0.34** | **0.39** | **0.09** | **8.1** | **8.1** | **0.09** | **9.8** | **9.8** | **0.09** |
| xz | liblzma | 0.28 | 0.27 | 0.04 | 8.0 | 8.0 | 0.03 | 9.7 | 9.6 | 0.02 |
| xz | LZMA SDK | 0.02 | 0.07 | 0.02 | 0.02 | 9.6 | 0.02 | 0.02 | 9.6 | 0.02 |
| xz | liblzma (fitted dictionary) | 0.28 | - | 0.02 | 8.0 | - | 0.02 | 9.7 | - | 0.02 |
| **lz4** | **compress-utils** | **0.00** | **0.23** | **0.07** | **0.11** | **0.28** | **0.07** | **0.11** | **0.28** | **0.07** |
| lz4 | liblz4 | 0.03 | 0.12 | 0.07 | 0.11 | 0.19 | 0.07 | 0.11 | 0.12 | 0.07 |
| **bz2** | **compress-utils** | **0.42** | **0.47** | **0.05** | **2.3** | **2.4** | **0.05** | **3.5** | **3.5** | **0.05** |
| bz2 | libbz2 | 0.42 | 0.41 | 0.05 | 2.3 | 2.3 | 0.05 | 3.5 | 3.5 | 0.05 |
| bz2 | libbz2-rs | 0.42 | 0.41 | 0.16 | 2.3 | 2.3 | 0.16 | 3.5 | 3.5 | 0.16 |
| **snappy** | **compress-utils** | **0.06** | **16** | **0.05** | **0.06** | **16** | **0.05** | **0.06** | **16** | **0.05** |
| snappy | google-snappy | 0.00 | - | 0.00 | 0.00 | - | 0.00 | 0.00 | - | 0.01 |
| snappy | snappy-core (our core, called directly) | 0.06 | - | 0.06 | 0.06 | - | 0.06 | 0.00 | - | 0.06 |

---

Generated by benchmarks/publish.py from run 2026-10-29, commit c3ae4b7 plus driver fixes; do not edit by hand.
