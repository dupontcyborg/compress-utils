from typing import Union, Iterator, ByteString

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

class CompressStream:
    """
    Streaming compression class for incremental data compression.

    This class allows compressing data in chunks without loading the entire
    dataset into memory. Useful for large files or streaming data.

    Example:
        stream = CompressStream('zstd', level=5)
        while has_more_data:
            compressed = stream.compress(chunk)
            output.write(compressed)
        output.write(stream.finish())
    """

    def __init__(self, algorithm: Union[Algorithm, str], level: int = 3) -> None:
        """
        Create a new compression stream.

        Parameters:
            algorithm: The compression algorithm to use (Algorithm enum or string)
            level: Compression level (1=fastest, 10=best compression)
        """
        ...

    def compress(self, data: ByteString) -> bytes:
        """
        Compress a chunk of data.

        Parameters:
            data: Binary data chunk to compress

        Returns:
            Compressed output (may be empty if data is being buffered)
        """
        ...

    def finish(self) -> bytes:
        """
        Finish compression and flush any remaining data.

        Must be called after all input has been fed to get the final compressed output.
        After calling finish(), the stream cannot be used again.

        Returns:
            Final compressed output
        """
        ...

    def is_finished(self) -> bool:
        """
        Check if the stream has been finished.

        Returns:
            True if finish() has been called
        """
        ...

    @property
    def algorithm(self) -> Algorithm:
        """Get the compression algorithm being used."""
        ...

class DecompressStream:
    """
    Streaming decompression class for incremental data decompression.

    This class allows decompressing data in chunks without loading the entire
    compressed dataset into memory.

    Example:
        stream = DecompressStream('zstd')
        while has_more_data:
            decompressed = stream.decompress(chunk)
            output.write(decompressed)
        stream.finish()
    """

    def __init__(self, algorithm: Union[Algorithm, str]) -> None:
        """
        Create a new decompression stream.

        Parameters:
            algorithm: The decompression algorithm to use (Algorithm enum or string)
        """
        ...

    def decompress(self, data: ByteString) -> bytes:
        """
        Decompress a chunk of compressed data.

        Parameters:
            data: Compressed data chunk to decompress

        Returns:
            Decompressed output
        """
        ...

    def finish(self) -> bytes:
        """
        Finish decompression and verify stream completeness.

        Must be called after all compressed input has been fed.

        Returns:
            Any remaining decompressed output
        """
        ...

    def is_finished(self) -> bool:
        """
        Check if the stream has been finished.

        Returns:
            True if finish() has been called
        """
        ...

    @property
    def algorithm(self) -> Algorithm:
        """Get the decompression algorithm being used."""
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
