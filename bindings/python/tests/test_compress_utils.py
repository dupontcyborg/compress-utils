import unittest
import random
import compress_utils as comp

# Sample test data
SAMPLE_DATA = b"Hello World"
EMPTY_DATA = b""
SINGLE_BYTE_DATA = b"A"
LARGE_DATA = bytes([random.randint(0, 255) for _ in range(1024 * 1024)])  # 1MB random data
REPETITIVE_DATA = b"A" * (1024 * 1024)  # 1MB repetitive data

# Dynamically populate available algorithms from the `Algorithm` enum in compress_utils
AVAILABLE_ALGORITHMS = [algo.name.lower() for algo in comp.Algorithm.__members__.values()]

def generate_random_data(size_in_bytes):
    """Generate random binary data of a given size."""
    return bytes(random.randint(0, 255) for _ in range(size_in_bytes))

# Test data types
TEST_DATA_TYPES = {
    "sample_data": SAMPLE_DATA,
    "empty_data": EMPTY_DATA,
    "single_byte_data": SINGLE_BYTE_DATA,
    "large_data": LARGE_DATA,
    "repetitive_data": REPETITIVE_DATA,
}


class TestCompressionUtils(unittest.TestCase):
    """Unit tests for compress-utils using functional and OOP approaches."""

    @staticmethod
    def check_compression_and_decompression(test_case, algorithm, data, level=None):
        """Helper to compress and decompress data, checking consistency."""

        # Functional API Test
        compressed_data = comp.compress(data, algorithm, level) if level else comp.compress(data, algorithm)
        decompressed_data = comp.decompress(compressed_data, algorithm)
        
        # Assert decompressed data matches original
        test_case.assertEqual(decompressed_data, data, f"Functional API failed for {algorithm}")

        # OOP API Test
        compressor = comp.compressor(algorithm)
        compressed_data = compressor.compress(data, level) if level else compressor.compress(data)
        decompressed_data = compressor.decompress(compressed_data)
        
        # Assert decompressed data matches original
        test_case.assertEqual(decompressed_data, data, f"OOP API failed for {algorithm}")


# Dynamically create test methods
def add_test_methods():
    for algorithm in AVAILABLE_ALGORITHMS:
        for data_type, data in TEST_DATA_TYPES.items():
            test_name = f"test_{algorithm}_{data_type}"
            test_func = lambda self, alg=algorithm, dt=data: TestCompressionUtils.check_compression_and_decompression(self, alg, dt)
            setattr(TestCompressionUtils, test_name, test_func)
        
        # Add compression level tests
        for level in [1, 5, 10]:
            test_name = f"test_{algorithm}_sample_data_level_{level}"
            test_func = lambda self, alg=algorithm, lvl=level: TestCompressionUtils.check_compression_and_decompression(self, alg, SAMPLE_DATA, lvl)
            setattr(TestCompressionUtils, test_name, test_func)

add_test_methods()


class TestStreamingAPI(unittest.TestCase):
    """Unit tests for the streaming compression/decompression API."""

    @staticmethod
    def stream_compress(algorithm, data, chunk_size=1024, level=3):
        """Compress data using streaming API in chunks."""
        stream = comp.CompressStream(algorithm, level)
        result = bytearray()

        for i in range(0, len(data), chunk_size):
            chunk = data[i:i + chunk_size]
            output = stream.compress(chunk)
            result.extend(output)

        final = stream.finish()
        result.extend(final)
        return bytes(result)

    @staticmethod
    def stream_decompress(algorithm, data, chunk_size=1024):
        """Decompress data using streaming API in chunks."""
        stream = comp.DecompressStream(algorithm)
        result = bytearray()

        for i in range(0, len(data), chunk_size):
            chunk = data[i:i + chunk_size]
            output = stream.decompress(chunk)
            result.extend(output)

        final = stream.finish()
        result.extend(final)
        return bytes(result)

    def test_streaming_basic(self):
        """Test basic streaming compression and decompression."""
        for algorithm in AVAILABLE_ALGORITHMS:
            with self.subTest(algorithm=algorithm):
                compressed = self.stream_compress(algorithm, SAMPLE_DATA, chunk_size=4)
                self.assertGreater(len(compressed), 0)

                decompressed = self.stream_decompress(algorithm, compressed, chunk_size=4)
                self.assertEqual(decompressed, SAMPLE_DATA)

    def test_streaming_single_byte_chunks(self):
        """Test streaming with single-byte chunks."""
        for algorithm in AVAILABLE_ALGORITHMS:
            with self.subTest(algorithm=algorithm):
                compressed = self.stream_compress(algorithm, SAMPLE_DATA, chunk_size=1)
                decompressed = self.stream_decompress(algorithm, compressed, chunk_size=1)
                self.assertEqual(decompressed, SAMPLE_DATA)

    def test_streaming_large_data(self):
        """Test streaming with larger data."""
        for algorithm in AVAILABLE_ALGORITHMS:
            with self.subTest(algorithm=algorithm):
                # Use smaller data for XZ to avoid long test times
                test_data = LARGE_DATA[:256 * 1024] if algorithm == 'xz' else LARGE_DATA
                compressed = self.stream_compress(algorithm, test_data, chunk_size=64 * 1024, level=1)
                decompressed = self.stream_decompress(algorithm, compressed, chunk_size=32 * 1024)
                self.assertEqual(decompressed, test_data)

    def test_streaming_empty_data(self):
        """Test streaming with empty data."""
        for algorithm in AVAILABLE_ALGORITHMS:
            with self.subTest(algorithm=algorithm):
                compressed = self.stream_compress(algorithm, EMPTY_DATA, chunk_size=4)
                decompressed = self.stream_decompress(algorithm, compressed, chunk_size=4)
                self.assertEqual(decompressed, EMPTY_DATA)

    def test_streaming_is_finished(self):
        """Test is_finished property of streams."""
        for algorithm in AVAILABLE_ALGORITHMS:
            with self.subTest(algorithm=algorithm):
                stream = comp.CompressStream(algorithm, 3)
                self.assertFalse(stream.is_finished())

                stream.compress(SAMPLE_DATA)
                self.assertFalse(stream.is_finished())

                stream.finish()
                self.assertTrue(stream.is_finished())

    def test_streaming_algorithm_property(self):
        """Test algorithm property of streams."""
        for algorithm in AVAILABLE_ALGORITHMS:
            with self.subTest(algorithm=algorithm):
                cstream = comp.CompressStream(algorithm, 3)
                self.assertEqual(cstream.algorithm.name.lower(), algorithm)

                dstream = comp.DecompressStream(algorithm)
                self.assertEqual(dstream.algorithm.name.lower(), algorithm)

    def test_streaming_compression_levels(self):
        """Test streaming with different compression levels."""
        for algorithm in AVAILABLE_ALGORITHMS:
            for level in [1, 5, 9]:
                with self.subTest(algorithm=algorithm, level=level):
                    compressed = self.stream_compress(algorithm, SAMPLE_DATA, chunk_size=4, level=level)
                    decompressed = self.stream_decompress(algorithm, compressed, chunk_size=4)
                    self.assertEqual(decompressed, SAMPLE_DATA)

    def test_streaming_compress_after_finish_raises(self):
        """Test that compressing after finish raises an error."""
        for algorithm in AVAILABLE_ALGORITHMS:
            with self.subTest(algorithm=algorithm):
                stream = comp.CompressStream(algorithm, 3)
                stream.compress(SAMPLE_DATA)
                stream.finish()

                with self.assertRaises(RuntimeError):
                    stream.compress(SAMPLE_DATA)

    def test_streaming_double_finish_raises(self):
        """Test that calling finish twice raises an error."""
        for algorithm in AVAILABLE_ALGORITHMS:
            with self.subTest(algorithm=algorithm):
                stream = comp.CompressStream(algorithm, 3)
                stream.compress(SAMPLE_DATA)
                stream.finish()

                with self.assertRaises(RuntimeError):
                    stream.finish()

    def test_streaming_invalid_compression_level(self):
        """Test that invalid compression levels raise errors."""
        for algorithm in AVAILABLE_ALGORITHMS:
            with self.subTest(algorithm=algorithm):
                with self.assertRaises(ValueError):
                    comp.CompressStream(algorithm, 0)
                with self.assertRaises(ValueError):
                    comp.CompressStream(algorithm, 11)

    def test_streaming_repetitive_data(self):
        """Test streaming with highly compressible repetitive data."""
        for algorithm in AVAILABLE_ALGORITHMS:
            with self.subTest(algorithm=algorithm):
                test_data = REPETITIVE_DATA[:100 * 1024]  # 100KB of A's
                compressed = self.stream_compress(algorithm, test_data, chunk_size=8 * 1024)

                # Repetitive data should compress very well
                self.assertLess(len(compressed), len(test_data) // 10,
                              f"Repetitive data should compress to less than 10% for {algorithm}")

                decompressed = self.stream_decompress(algorithm, compressed, chunk_size=8 * 1024)
                self.assertEqual(decompressed, test_data)


# Run the tests if executed as a standalone script
if __name__ == "__main__":
    unittest.main()
