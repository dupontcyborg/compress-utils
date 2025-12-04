#ifndef COMPRESS_UTILS_STREAM_HPP_
#define COMPRESS_UTILS_STREAM_HPP_

#include "algorithms.hpp"
#include "symbol_exports.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace compress_utils {

/**
 * @brief Streaming compression class for incremental data compression
 *
 * This class allows compressing data in chunks without loading the entire
 * dataset into memory. Useful for large files or streaming data.
 *
 * Thread-safety: NOT thread-safe. Use separate instances for different threads.
 *
 * Example usage:
 * @code
 *   CompressStream stream(Algorithm::ZSTD, 5);
 *   while (has_more_data) {
 *       auto chunk = read_chunk();
 *       auto compressed = stream.Compress(chunk);
 *       write_output(compressed);
 *   }
 *   auto final = stream.Finish();
 *   write_output(final);
 * @endcode
 */
class EXPORT CompressStream {
public:
    /**
     * @brief Construct a new compression stream
     *
     * @param algorithm Compression algorithm to use
     * @param level Compression level (1 = fastest; 10 = smallest; default = 3)
     * @throws std::invalid_argument if algorithm is unsupported or level is out of range
     */
    explicit CompressStream(Algorithm algorithm, int level = 3);

    /**
     * @brief Destructor - releases algorithm-specific resources
     */
    ~CompressStream();

    // Non-copyable
    CompressStream(const CompressStream&) = delete;
    CompressStream& operator=(const CompressStream&) = delete;

    // Movable
    CompressStream(CompressStream&& other) noexcept;
    CompressStream& operator=(CompressStream&& other) noexcept;

    /**
     * @brief Compress a chunk of input data
     *
     * Feed input data to the compressor. Returns any compressed output that
     * is available. The returned data may be empty if the compressor is
     * buffering internally.
     *
     * @param data Input data chunk to compress
     * @return std::vector<uint8_t> Compressed output (may be empty)
     * @throws std::runtime_error if compression fails or stream is already finished
     */
    std::vector<uint8_t> Compress(std::span<const uint8_t> data);

    /**
     * @brief Finish compression and flush any remaining data
     *
     * Must be called after all input has been fed to get the final compressed
     * output. After calling Finish(), the stream cannot be used again.
     *
     * @return std::vector<uint8_t> Final compressed output
     * @throws std::runtime_error if finishing fails or stream is already finished
     */
    std::vector<uint8_t> Finish();

    /**
     * @brief Check if the stream has been finished
     * @return true if Finish() has been called
     */
    bool IsFinished() const;

    /**
     * @brief Get the algorithm being used
     * @return Algorithm The compression algorithm
     */
    Algorithm algorithm() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * @brief Streaming decompression class for incremental data decompression
 *
 * This class allows decompressing data in chunks without loading the entire
 * compressed dataset into memory.
 *
 * Thread-safety: NOT thread-safe. Use separate instances for different threads.
 *
 * Example usage:
 * @code
 *   DecompressStream stream(Algorithm::ZSTD);
 *   while (has_more_data) {
 *       auto chunk = read_compressed_chunk();
 *       auto decompressed = stream.Decompress(chunk);
 *       write_output(decompressed);
 *   }
 *   auto final = stream.Finish();
 *   write_output(final);
 * @endcode
 */
class EXPORT DecompressStream {
public:
    /**
     * @brief Construct a new decompression stream
     *
     * @param algorithm Decompression algorithm to use
     * @throws std::invalid_argument if algorithm is unsupported
     */
    explicit DecompressStream(Algorithm algorithm);

    /**
     * @brief Destructor - releases algorithm-specific resources
     */
    ~DecompressStream();

    // Non-copyable
    DecompressStream(const DecompressStream&) = delete;
    DecompressStream& operator=(const DecompressStream&) = delete;

    // Movable
    DecompressStream(DecompressStream&& other) noexcept;
    DecompressStream& operator=(DecompressStream&& other) noexcept;

    /**
     * @brief Decompress a chunk of compressed data
     *
     * Feed compressed data to the decompressor. Returns any decompressed
     * output that is available.
     *
     * @param data Compressed data chunk to decompress
     * @return std::vector<uint8_t> Decompressed output (may be empty)
     * @throws std::runtime_error if decompression fails or stream is already finished
     */
    std::vector<uint8_t> Decompress(std::span<const uint8_t> data);

    /**
     * @brief Finish decompression and verify stream completeness
     *
     * Must be called after all compressed input has been fed. Verifies that
     * the compressed stream was complete and returns any remaining output.
     *
     * @return std::vector<uint8_t> Final decompressed output
     * @throws std::runtime_error if stream is incomplete or already finished
     */
    std::vector<uint8_t> Finish();

    /**
     * @brief Check if the stream has been finished
     * @return true if Finish() has been called
     */
    bool IsFinished() const;

    /**
     * @brief Get the algorithm being used
     * @return Algorithm The decompression algorithm
     */
    Algorithm algorithm() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace compress_utils

#endif  // COMPRESS_UTILS_STREAM_HPP_
