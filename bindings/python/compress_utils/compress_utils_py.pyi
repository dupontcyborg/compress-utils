from typing import Union, Iterator, ByteString, overload

class Algorithm:
    """Enum representing the available compression algorithms."""

    # Algorithm values (these will only exist if the algorithm is compiled in)
    brotli: 'Algorithm'
    lzma: 'Algorithm'
    xz: 'Algorithm'
    zlib: 'Algorithm'
    zstd: 'Algorithm'

    # Make the enum iterable
    @staticmethod
    def __iter__() -> Iterator['Algorithm']: ...

class compressor:
    """Class-based interface for compression/decompression."""

    def __init__(self, algorithm: Union[Algorithm, str]) -> None:
        """
        Initialize a compressor with the specified algorithm.
        
        Parameters:
            algorithm: The compression algorithm to use (Algorithm enum or string)
        """
        ...

    def compress(self, data: ByteString, level: int = 3) -> bytes:
        """
        Compress data with optional compression level.
        
        Parameters:
            data: Binary data to compress
            level: Compression level (1=fastest, 10=best compression)
            
        Returns:
            Compressed data as bytes
        """
        ...

    def decompress(self, data: ByteString) -> bytes:
        """
        Decompress data.
        
        Parameters:
            data: Compressed binary data
            
        Returns:
            Decompressed data as bytes
        """
        ...

def compress(data: ByteString, algorithm: Union[Algorithm, str], level: int = 3) -> bytes:
    """
    Compress data using an algorithm and optional level.
    
    Parameters:
        data: Binary data to compress
        algorithm: The compression algorithm to use (Algorithm enum or string)
        level: Compression level (1=fastest, 10=best compression)
        
    Returns:
        Compressed data as bytes
    """
    ...

def decompress(data: ByteString, algorithm: Union[Algorithm, str]) -> bytes:
    """
    Decompress data using an algorithm.
    
    Parameters:
        data: Compressed binary data
        algorithm: The compression algorithm to use (Algorithm enum or string)
        
    Returns:
        Decompressed data as bytes
    """
    ...