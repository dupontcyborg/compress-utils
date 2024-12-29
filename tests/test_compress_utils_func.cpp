#include "compress_utils_func.hpp"
#include "helpers.hpp"

#include <gtest/gtest.h>
#include <stdexcept>

const std::vector<uint8_t> SAMPLE_DATA = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};

class FunctionalCompressorTest : public ::testing::TestWithParam<compress_utils::Algorithm> {
   protected:
    compress_utils::Algorithm algorithm = GetParam();
};

// Helper function to ensure the data is decompressed correctly
void CheckCompressionAndDecompression(compress_utils::Algorithm& algorithm,
                                      const std::vector<uint8_t>& data, int level = 3) {
    // Check with vector input
    std::vector<uint8_t> compressed_data = compress_utils::Compress(data, algorithm, level);
    ASSERT_FALSE(compressed_data.empty()) << "Compression failed, compressed data is empty.";
    std::vector<uint8_t> decompressed_data =
        compress_utils::Decompress(compressed_data, algorithm);
    ASSERT_EQ(decompressed_data, data) << "Decompression failed, data doesn't match the original.";

    // Check with pointer input
    compressed_data = compress_utils::Compress(data.data(), data.size(), algorithm, level);
    ASSERT_FALSE(compressed_data.empty()) << "Compression failed, compressed data is empty.";
    decompressed_data =
        compress_utils::Decompress(compressed_data.data(), compressed_data.size(), algorithm);
    ASSERT_EQ(decompressed_data, data) << "Decompression failed, data doesn't match the original.";
}

// Parameterized test for compression and decompression
TEST_P(FunctionalCompressorTest, CompressDecompress) {
    CheckCompressionAndDecompression(algorithm, SAMPLE_DATA);
}

// Test compression and decompression of empty data
TEST_P(FunctionalCompressorTest, CompressDecompressEmpty) {
    const std::vector<uint8_t> empty_data;
    CheckCompressionAndDecompression(algorithm, empty_data);
}

// Test compression and decompression of small inputs
TEST_P(FunctionalCompressorTest, CompressDecompress1B) {
    const std::vector<uint8_t> small_data = {'A'};
    CheckCompressionAndDecompression(algorithm, small_data);
}

// Test compression and decompression of medium inputs
TEST_P(FunctionalCompressorTest, CompressDecompress1MB) {
    auto large_data = GenerateData(1024 * 1024);  // 1 MB of random data
    CheckCompressionAndDecompression(algorithm, large_data);
}

// Test compression and decompression of large inputs
TEST_P(FunctionalCompressorTest, CompressDecompress32MB) {
    auto large_data = GenerateData(1024 * 1024 * 32);            // 32 MB of random data

    // If XZ, reduce size to 4MB to avoid long test times
    if (algorithm == compress_utils::Algorithm::XZ) {
        large_data.resize(1024 * 1024 * 4);
    }
    
    CheckCompressionAndDecompression(algorithm, large_data, 1);  // Use fastest compression level
}

// Test invalid compression level handling
TEST_P(FunctionalCompressorTest, InvalidCompressionLevel) {
    const std::vector<uint8_t> data = {'D', 'a', 't', 'a'};
    EXPECT_THROW(compress_utils::Compress(data, algorithm, 0),
                 std::invalid_argument);  // Invalid level (0)
    EXPECT_THROW(compress_utils::Compress(data, algorithm, 11),
                 std::invalid_argument);  // Invalid level (11)
}

// Test behavior with corrupted compressed data
TEST_P(FunctionalCompressorTest, CorruptedCompressedData) {
    const std::vector<uint8_t> corrupted_data = {'C', 'o', 'r', 'r', 'u', 'p', 't', 'e', 'd'};
    EXPECT_THROW(compress_utils::Decompress(corrupted_data, algorithm),
                 std::runtime_error);  // Should throw
}

// Test compression and decompression of repetitive data
TEST_P(FunctionalCompressorTest, CompressDecompressRepetitiveData) {
    auto repetitive_data = GenerateRepetitiveData(1024 * 1024, 'A');  // 1 MB of repetitive 'A'
    CheckCompressionAndDecompression(algorithm, repetitive_data);
}

// Test every compression level
TEST_P(FunctionalCompressorTest, CompressionLevels) {
    for (int level = 1; level <= 9; ++level) {
        auto compressed_data = compress_utils::Compress(SAMPLE_DATA, algorithm, level);
        ASSERT_FALSE(compressed_data.empty()) << "Compression failed, compressed data is empty.";
        auto decompressed_data = compress_utils::Decompress(compressed_data, algorithm);
        ASSERT_EQ(decompressed_data, SAMPLE_DATA)
            << "Decompression failed, data doesn't match the original.";
    }
}

// Test compressing already compressed data
TEST_P(FunctionalCompressorTest, CompressCompressedData) {
    auto compressed_data = compress_utils::Compress(SAMPLE_DATA, algorithm, 5);
    ASSERT_FALSE(compressed_data.empty()) << "Compression failed, compressed data is empty.";
    auto double_compressed_data = compress_utils::Compress(compressed_data, algorithm, 5);
    ASSERT_FALSE(double_compressed_data.empty()) << "Compression failed, compressed data is empty.";
    auto decompressed_data = compress_utils::Decompress(double_compressed_data, algorithm);
    ASSERT_EQ(decompressed_data, compressed_data)
        << "Decompression failed, data doesn't match the original.";
}

// Conditionally instantiate the test suite only if there are algorithms to test
#if defined(INCLUDE_BROTLI) || defined(INCLUDE_XZ) || defined(INCLUDE_ZLIB) || defined(INCLUDE_ZSTD)
INSTANTIATE_TEST_SUITE_P(
    FunctionalCompressionTests,  // Test suite name
    FunctionalCompressorTest,
    ::testing::ValuesIn(GetAlgorithms()),  // Use ValuesIn to avoid trailing comma issues
    AlgorithmToString                      // Custom name generator function
);
#else
TEST(FunctionalCompressorTest, NoAlgorithmsAvailable) {
    GTEST_SKIP() << "No compression algorithms were included in the build.";
}
#endif
