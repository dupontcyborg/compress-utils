"""
Python bindings for compress-utils library (C-core).
"""
from __future__ import annotations
import typing
import typing_extensions
__all__: list[str] = ['Algorithm', 'CompressError', 'CompressStream', 'DecompressStream', 'brotli', 'bz2', 'compress', 'decompress', 'deflate', 'gzip', 'is_available', 'lz4', 'lz4_raw', 'lzma', 'lzo', 'set_max_decompressed_size', 'snappy', 'version', 'xz', 'zlib', 'zstd']
class Algorithm:
    """
    Members:
    
      zstd
    
      brotli
    
      zlib
    
      bz2
    
      lz4
    
      xz
    
      lzma
    
      snappy
    
      gzip
    
      deflate
    
      lz4_raw
    
      lzo
    """
    __members__: typing.ClassVar[dict[str, Algorithm]]
    brotli: typing.ClassVar[Algorithm]
    bz2: typing.ClassVar[Algorithm]
    deflate: typing.ClassVar[Algorithm]
    gzip: typing.ClassVar[Algorithm]
    lz4: typing.ClassVar[Algorithm]
    lz4_raw: typing.ClassVar[Algorithm]
    lzma: typing.ClassVar[Algorithm]
    lzo: typing.ClassVar[Algorithm]
    snappy: typing.ClassVar[Algorithm]
    xz: typing.ClassVar[Algorithm]
    zlib: typing.ClassVar[Algorithm]
    zstd: typing.ClassVar[Algorithm]
    @typing.overload
    def __eq__(self, other: Algorithm) -> bool:
        ...
    @typing.overload
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __int__(self) -> int:
        ...
    @typing.overload
    def __ne__(self, other: Algorithm) -> bool:
        ...
    @typing.overload
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class CompressStream:
    """
    Streaming compression. Feed chunks via .compress(b); .flush() delivers
    everything so far and keeps the stream open; .finish() ends it.
    Safe to share between threads: calls on one stream run one at a time.
    """
    def __init__(self, algorithm: typing.Any, level: typing.SupportsInt | typing.SupportsIndex = 5) -> None:
        ...
    def compress(self, data: typing_extensions.Buffer) -> bytes:
        ...
    def finish(self) -> bytes:
        ...
    def flush(self) -> bytes:
        """
        Return the bytes a decoder needs to recover everything compressed so far,
        keeping the stream open for more data. Unlike zlib's flush(), this does
        not end the stream; use finish() for that. Each flush costs a few bytes.
        Raises CompressError for snappy and bz2, which can't flush mid-stream.
        """
class DecompressStream:
    """
    Streaming decompression. Feed chunks via .decompress(b); end with .finish().
    Safe to share between threads: calls on one stream run one at a time.
    """
    def __init__(self, algorithm: typing.Any) -> None:
        ...
    def decompress(self, data: typing_extensions.Buffer) -> bytes:
        ...
    def finish(self) -> bytes:
        ...
class CompressError(Exception):
    pass
def compress(data: typing_extensions.Buffer, algorithm: typing.Any, level: typing.SupportsInt | typing.SupportsIndex = 5) -> bytes:
    """
    Compress bytes/buffer using the given algorithm (string or Algorithm).
    """
def decompress(data: typing_extensions.Buffer, algorithm: typing.Any, expected_size: typing.SupportsInt | typing.SupportsIndex | None = None) -> bytes:
    """
    Decompress bytes/buffer using the given algorithm.
    
    expected_size is an upper bound on the output size; the result holds the
    bytes actually decoded. It skips the size probe, and for lz4_raw it skips
    the much slower streaming fallback.
    """
def is_available(algorithm: typing.Any) -> bool:
    ...
def set_max_decompressed_size(bytes: typing.SupportsInt | typing.SupportsIndex) -> None:
    ...
def version() -> str:
    ...
brotli: Algorithm
bz2: Algorithm
deflate: Algorithm
gzip: Algorithm
lz4: Algorithm
lz4_raw: Algorithm
lzma: Algorithm
lzo: Algorithm
snappy: Algorithm
xz: Algorithm
zlib: Algorithm
zstd: Algorithm
