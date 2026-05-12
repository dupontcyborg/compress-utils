"""
Python bindings for compress-utils library (C-core).
"""
from __future__ import annotations
import typing
import typing_extensions
__all__: list[str] = ['Algorithm', 'CompressError', 'CompressStream', 'DecompressStream', 'brotli', 'bz2', 'compress', 'decompress', 'is_available', 'lz4', 'lzma', 'set_max_decompressed_size', 'version', 'xz', 'zlib', 'zstd']
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
    """
    __members__: typing.ClassVar[dict[str, Algorithm]]  # value = {'zstd': <Algorithm.zstd: 0>, 'brotli': <Algorithm.brotli: 1>, 'zlib': <Algorithm.zlib: 2>, 'bz2': <Algorithm.bz2: 3>, 'lz4': <Algorithm.lz4: 4>, 'xz': <Algorithm.xz: 5>, 'lzma': <Algorithm.lzma: 6>}
    brotli: typing.ClassVar[Algorithm]  # value = <Algorithm.brotli: 1>
    bz2: typing.ClassVar[Algorithm]  # value = <Algorithm.bz2: 3>
    lz4: typing.ClassVar[Algorithm]  # value = <Algorithm.lz4: 4>
    lzma: typing.ClassVar[Algorithm]  # value = <Algorithm.lzma: 6>
    xz: typing.ClassVar[Algorithm]  # value = <Algorithm.xz: 5>
    zlib: typing.ClassVar[Algorithm]  # value = <Algorithm.zlib: 2>
    zstd: typing.ClassVar[Algorithm]  # value = <Algorithm.zstd: 0>
    def __eq__(self, other: typing.Any) -> bool:
        ...
    def __getstate__(self) -> int:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __init__(self, value: int) -> None:
        ...
    def __int__(self) -> int:
        ...
    def __ne__(self, other: typing.Any) -> bool:
        ...
    def __repr__(self) -> str:
        ...
    def __setstate__(self, state: int) -> None:
        ...
    def __str__(self) -> str:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def value(self) -> int:
        ...
class CompressError(Exception):
    pass
class CompressStream:
    """
    Streaming compression. Feed chunks via .compress(b); flush with .finish().
    """
    def __init__(self, algorithm: typing.Any, level: int = 5) -> None:
        ...
    def compress(self, data: typing_extensions.Buffer) -> bytes:
        ...
    def finish(self) -> bytes:
        ...
class DecompressStream:
    """
    Streaming decompression. Feed chunks via .decompress(b); flush with .finish().
    """
    def __init__(self, algorithm: typing.Any) -> None:
        ...
    def decompress(self, data: typing_extensions.Buffer) -> bytes:
        ...
    def finish(self) -> bytes:
        ...
def compress(data: typing_extensions.Buffer, algorithm: typing.Any, level: int = 5) -> bytes:
    """
    Compress bytes/buffer using the given algorithm (string or Algorithm).
    """
def decompress(data: typing_extensions.Buffer, algorithm: typing.Any) -> bytes:
    """
    Decompress bytes/buffer using the given algorithm.
    """
def is_available(algorithm: typing.Any) -> bool:
    ...
def set_max_decompressed_size(bytes: int) -> None:
    ...
def version() -> str:
    ...
brotli: Algorithm  # value = <Algorithm.brotli: 1>
bz2: Algorithm  # value = <Algorithm.bz2: 3>
lz4: Algorithm  # value = <Algorithm.lz4: 4>
lzma: Algorithm  # value = <Algorithm.lzma: 6>
xz: Algorithm  # value = <Algorithm.xz: 5>
zlib: Algorithm  # value = <Algorithm.zlib: 2>
zstd: Algorithm  # value = <Algorithm.zstd: 0>
