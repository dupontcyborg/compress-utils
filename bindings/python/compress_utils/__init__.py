# compress_utils — Python binding for the compress-utils C library.
#
# Surface preserved from the legacy binding:
#   compress(data, algorithm, level=5)
#   decompress(data, algorithm)
#   Algorithm (enum with lowercase members: zstd, brotli, zlib, bz2, lz4, xz, lzma)
#   CompressStream / DecompressStream
#
# Removed (was a smell — wrapper around free functions with no cached state):
#   compressor(algorithm)  →  use compress()/decompress() directly, or
#                             CompressStream/DecompressStream for streaming.
#
# Added:
#   version()                       → "MAJOR.MINOR.PATCH"
#   is_available(algorithm)         → bool
#   set_max_decompressed_size(b)    → cap one-shot decompression

from .compress_utils_py import (
    Algorithm,
    CompressStream,
    DecompressStream,
    CompressError,
    compress,
    decompress,
    is_available,
    set_max_decompressed_size,
    version,
)

__all__ = [
    "Algorithm",
    "CompressStream",
    "DecompressStream",
    "CompressError",
    "compress",
    "decompress",
    "is_available",
    "set_max_decompressed_size",
    "version",
]
