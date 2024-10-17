#include <gtest/gtest.h>
#include "compression_utils.hpp"  // Your Compressor class

using namespace compression_utils;

// Dummy data for testing (adjust for real test data as needed)
const std::vector<uint8_t> sample_data = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};

class CompressorTest : public ::testing::TestWithParam<Algorithm> {
   protected:
    Compressor compressor{GetParam()};  // GetParam() provides the algorithm (ZLIB or ZSTD)
};

// Helper function to ensure the data is decompressed correctly
void CheckCompressionAndDecompression(Compressor& compressor, const std::vector<uint8_t>& data) {
    // Compress the data
    std::vector<uint8_t> compressed_data = compressor.Compress(data, 5);
    ASSERT_FALSE(compressed_data.empty()) << "Compression failed, compressed data is empty.";

    // Decompress the data
    std::vector<uint8_t> decompressed_data = compressor.Decompress(compressed_data);
    ASSERT_EQ(decompressed_data, data) << "Decompression failed, data doesn't match the original.";
}

// Parameterized test for compression and decompression
TEST_P(CompressorTest, CompressDecompress) {
    CheckCompressionAndDecompression(compressor, sample_data);
}

// Parameterized test for handling empty data
TEST_P(CompressorTest, CompressDecompressEmpty) {
    std::vector<uint8_t> empty_data;
    CheckCompressionAndDecompression(compressor, empty_data);
}

// Conditionally instantiate the test suite only if there are algorithms to test
#if defined(INCLUDE_ZLIB) || defined(INCLUDE_ZSTD)
    INSTANTIATE_TEST_SUITE_P(
        CompressionTests,  // Test suite name
        CompressorTest,
        ::testing::Values(
#ifdef INCLUDE_ZLIB
            Algorithm::ZLIB,
#endif
#ifdef INCLUDE_ZSTD
            Algorithm::ZSTD
#endif
        )
    );
#else
    TEST(CompressorTest, NoAlgorithmsAvailable) {
        GTEST_SKIP() << "No compression algorithms were included in the build.";
    }
#endif
