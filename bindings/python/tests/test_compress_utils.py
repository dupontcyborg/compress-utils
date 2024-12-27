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

# Run the tests if executed as a standalone script
if __name__ == "__main__":
    unittest.main()
