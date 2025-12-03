#include "compress_utils_stream.hpp"
#include "helpers.hpp"

#include <gtest/gtest.h>
#include <numeric>
#include <stdexcept>

const std::vector<uint8_t> SAMPLE_DATA = {'H', 'e', 'l', 'l', 'o', ' ', 'W', 'o', 'r', 'l', 'd'};

class StreamingTest : public ::testing::TestWithParam<compress_utils::Algorithm> {};

// Helper to compress data in chunks using streaming API
std::vector<uint8_t> StreamCompress(compress_utils::Algorithm algorithm, const std::vector<uint8_t>& data,
                                    size_t chunk_size, int level = 3) {
    compress_utils::CompressStream stream(algorithm, level);
    std::vector<uint8_t> result;

    for (size_t i = 0; i < data.size(); i += chunk_size) {
        size_t this_chunk = std::min(chunk_size, data.size() - i);
        std::span<const uint8_t> chunk(data.data() + i, this_chunk);
        auto output = stream.Compress(chunk);
        result.insert(result.end(), output.begin(), output.end());
    }

    auto final_output = stream.Finish();
    result.insert(result.end(), final_output.begin(), final_output.end());
    return result;
}

// Helper to decompress data in chunks using streaming API
std::vector<uint8_t> StreamDecompress(compress_utils::Algorithm algorithm, const std::vector<uint8_t>& data,
                                      size_t chunk_size) {
    compress_utils::DecompressStream stream(algorithm);
    std::vector<uint8_t> result;

    for (size_t i = 0; i < data.size(); i += chunk_size) {
        size_t this_chunk = std::min(chunk_size, data.size() - i);
        std::span<const uint8_t> chunk(data.data() + i, this_chunk);
        auto output = stream.Decompress(chunk);
        result.insert(result.end(), output.begin(), output.end());
    }

    auto final_output = stream.Finish();
    result.insert(result.end(), final_output.begin(), final_output.end());
    return result;
}

// Test basic streaming compression and decompression
TEST_P(StreamingTest, BasicStreamCompressDecompress) {
    auto algorithm = GetParam();

    // Compress with streaming API
    auto compressed = StreamCompress(algorithm, SAMPLE_DATA, 4);
    ASSERT_FALSE(compressed.empty()) << "Streaming compression produced empty output";

    // Decompress with streaming API
    auto decompressed = StreamDecompress(algorithm, compressed, 4);
    EXPECT_EQ(decompressed, SAMPLE_DATA) << "Streaming decompression doesn't match original data";
}

// Test streaming with single-byte chunks
TEST_P(StreamingTest, SingleByteChunks) {
    auto algorithm = GetParam();

    // Compress one byte at a time
    auto compressed = StreamCompress(algorithm, SAMPLE_DATA, 1);
    ASSERT_FALSE(compressed.empty());

    // Decompress one byte at a time
    auto decompressed = StreamDecompress(algorithm, compressed, 1);
    EXPECT_EQ(decompressed, SAMPLE_DATA);
}

// Test streaming with data larger than internal buffers
TEST_P(StreamingTest, LargeDataStreaming) {
    auto algorithm = GetParam();

    // Generate 1MB of random data
    auto large_data = GenerateData(1024 * 1024);

    // If XZ, reduce size to avoid long test times
    if (algorithm == compress_utils::Algorithm::XZ) {
        large_data.resize(256 * 1024);
    }

    // Compress in 64KB chunks
    auto compressed = StreamCompress(algorithm, large_data, 64 * 1024, 1);
    ASSERT_FALSE(compressed.empty());

    // Decompress in 32KB chunks
    auto decompressed = StreamDecompress(algorithm, compressed, 32 * 1024);
    EXPECT_EQ(decompressed, large_data);
}

// Test empty data streaming
TEST_P(StreamingTest, EmptyDataStreaming) {
    auto algorithm = GetParam();
    const std::vector<uint8_t> empty_data;

    compress_utils::CompressStream compress_stream(algorithm, 3);
    auto compressed = compress_stream.Compress(empty_data);
    auto final_compressed = compress_stream.Finish();

    // Concatenate the outputs
    compressed.insert(compressed.end(), final_compressed.begin(), final_compressed.end());

    // Decompress the empty stream
    compress_utils::DecompressStream decompress_stream(algorithm);
    auto decompressed = decompress_stream.Decompress(compressed);
    auto final_decompressed = decompress_stream.Finish();
    decompressed.insert(decompressed.end(), final_decompressed.begin(), final_decompressed.end());

    EXPECT_EQ(decompressed, empty_data);
}

// Test that IsFinished() returns correct state
TEST_P(StreamingTest, IsFinishedState) {
    auto algorithm = GetParam();

    compress_utils::CompressStream compress_stream(algorithm, 3);
    EXPECT_FALSE(compress_stream.IsFinished());

    compress_stream.Compress(SAMPLE_DATA);
    EXPECT_FALSE(compress_stream.IsFinished());

    compress_stream.Finish();
    EXPECT_TRUE(compress_stream.IsFinished());

    compress_utils::DecompressStream decompress_stream(algorithm);
    EXPECT_FALSE(decompress_stream.IsFinished());
}

// Test that algorithm() returns the correct algorithm
TEST_P(StreamingTest, AlgorithmProperty) {
    auto algorithm = GetParam();

    compress_utils::CompressStream compress_stream(algorithm, 3);
    EXPECT_EQ(compress_stream.algorithm(), algorithm);

    compress_utils::DecompressStream decompress_stream(algorithm);
    EXPECT_EQ(decompress_stream.algorithm(), algorithm);
}

// Test that compressing after Finish() throws
TEST_P(StreamingTest, CompressAfterFinishThrows) {
    auto algorithm = GetParam();

    compress_utils::CompressStream stream(algorithm, 3);
    stream.Compress(SAMPLE_DATA);
    stream.Finish();

    EXPECT_THROW(stream.Compress(SAMPLE_DATA), std::runtime_error);
}

// Test that calling Finish() twice throws
TEST_P(StreamingTest, DoubleFinishThrows) {
    auto algorithm = GetParam();

    compress_utils::CompressStream stream(algorithm, 3);
    stream.Compress(SAMPLE_DATA);
    stream.Finish();

    EXPECT_THROW(stream.Finish(), std::runtime_error);
}

// Test invalid compression level
TEST_P(StreamingTest, InvalidCompressionLevel) {
    auto algorithm = GetParam();

    EXPECT_THROW(compress_utils::CompressStream(algorithm, 0), std::invalid_argument);
    EXPECT_THROW(compress_utils::CompressStream(algorithm, 11), std::invalid_argument);
}

// Test move semantics for CompressStream
TEST_P(StreamingTest, MoveCompressStream) {
    auto algorithm = GetParam();

    compress_utils::CompressStream stream1(algorithm, 3);
    stream1.Compress(SAMPLE_DATA);

    // Move construct
    compress_utils::CompressStream stream2(std::move(stream1));
    auto compressed = stream2.Finish();
    EXPECT_FALSE(compressed.empty());
    EXPECT_TRUE(stream2.IsFinished());
}

// Test move semantics for DecompressStream
TEST_P(StreamingTest, MoveDecompressStream) {
    auto algorithm = GetParam();

    // Create decompress stream
    compress_utils::DecompressStream stream1(algorithm);

    // Move construct - verify the move compiles and doesn't crash
    compress_utils::DecompressStream stream2(std::move(stream1));

    // Verify the moved-to stream has the correct algorithm
    EXPECT_EQ(stream2.algorithm(), algorithm);
    EXPECT_FALSE(stream2.IsFinished());
}

// Test compression levels with streaming
TEST_P(StreamingTest, CompressionLevels) {
    auto algorithm = GetParam();

    for (int level = 1; level <= 9; ++level) {
        auto compressed = StreamCompress(algorithm, SAMPLE_DATA, 4, level);
        ASSERT_FALSE(compressed.empty()) << "Streaming compression failed at level " << level;

        auto decompressed = StreamDecompress(algorithm, compressed, 4);
        EXPECT_EQ(decompressed, SAMPLE_DATA) << "Streaming decompression failed at level " << level;
    }
}

// Test repetitive data streaming (should compress well)
TEST_P(StreamingTest, RepetitiveDataStreaming) {
    auto algorithm = GetParam();

    auto repetitive_data = GenerateRepetitiveData(100 * 1024, 'A');  // 100KB of 'A's

    auto compressed = StreamCompress(algorithm, repetitive_data, 8 * 1024);
    ASSERT_FALSE(compressed.empty());

    // Repetitive data should compress very well
    EXPECT_LT(compressed.size(), repetitive_data.size() / 10)
        << "Repetitive data should compress to less than 10% of original size";

    auto decompressed = StreamDecompress(algorithm, compressed, 8 * 1024);
    EXPECT_EQ(decompressed, repetitive_data);
}

// Conditionally instantiate the test suite
#if defined(INCLUDE_BROTLI) || defined(INCLUDE_XZ) || defined(INCLUDE_ZLIB) || defined(INCLUDE_ZSTD)
INSTANTIATE_TEST_SUITE_P(StreamingTests, StreamingTest, ::testing::ValuesIn(GetAlgorithms()),
                         AlgorithmToString);
#else
TEST(StreamingTest, NoAlgorithmsAvailable) {
    GTEST_SKIP() << "No compression algorithms were included in the build.";
}
#endif
