# Acknowledgements

This project would not be possible without the remarkable work of other engineers in the data compression space. `compress-utils` incorporates such code from the following third-party projects. Their full license texts are in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md), which ships with every package.

## Brotli

- **Repository**: https://github.com/google/brotli.git
- **License**: MIT License
- **Authors**: Google and contributors

## zlib-ng (zlib, gzip and raw DEFLATE)

- **Repository**: https://github.com/zlib-ng/zlib-ng
- **License**: zlib License
- **Authors**: zlib-ng contributors, building on zlib by Jean-loup Gailly and Mark Adler

## bzip2

- **Repository**: https://gitlab.com/bzip2/bzip2
- **License**: bzip2 License (BSD-style)
- **Author**: Julian Seward

## LZ4

- **Repository**: https://github.com/lz4/lz4
- **License**: BSD 2-Clause License (library)
- **Authors**: Yann Collet and contributors

## Zstandard (zstd)

- **Repository**: https://github.com/facebook/zstd
- **License**: BSD 3-Clause License
- **Authors**: Yann Collet and contributors

## XZ Utils

- **Repository**: https://github.com/tukaani-project/xz
- **License**: BSD 0-Clause License
- **Authors**: Lasse Collin, Tukaani and contributors

## Snappy

The shipped codec is our own pure-C implementation of the Snappy raw block format (`src/algorithms/snappy/core`; it keeps the library free of a C++ runtime dependency). Its design follows Google's snappy and Andi Kleen's C port of it, whose BSD 3-Clause notices it retains. The reference C++ implementation is also used as the differential test oracle (`third_party/snappy-oracle`) and is never distributed.

- **Format and design**: https://github.com/google/snappy — BSD 3-Clause License, Google and contributors
- **Earlier C port (replaced by the first-party core)**: https://github.com/andikleen/snappy-c — BSD 3-Clause License, Andi Kleen and contributors
- **Provenance**: see `src/algorithms/snappy/core/PROVENANCE.md` and `LICENSE`

## lzokay (LZO1X decoder)

- **Repository**: https://github.com/AxioDL/lzokay
- **License**: MIT License
- **Author**: Jack Andersen
- **Adaptation**: Decoder-only C port of revision `db2df1fcbebc2ed06c10f727f72567d40f06a2be`; see `third_party/lzokay/PROVENANCE.md` and `LICENSE`.
