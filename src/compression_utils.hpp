#ifndef COMPRESSION_UTILS_HPP_
#define COMPRESSION_UTILS_HPP_

#include "algorithms.hpp"

#include <cstdint>
#include <vector>

// Platform-specific macros for exporting and importing symbols
#if defined(_WIN32) || defined(_WIN64)
#if defined(COMPRESSION_UTILS_EXPORTS)
#define EXPORT __declspec(dllexport)  // Export when building the DLL
#else
#define EXPORT __declspec(dllimport)  // Import when using the DLL
#endif
#else
#define EXPORT __attribute__((visibility("default")))  // For non-Windows platforms (Linux/macOS)
#endif

namespace compression_utils {

// OOP Interface

/**
 * @brief Compressor class that provides compression and decompression functionalities
 *
 * The class provides two methods, Compress and Decompress, that can be used to compress and
 * decompress
 */
class EXPORT Compressor {
   public:
    /**
     * @brief Construct a new Compressor object
     *
     * @param algorithm Compression algorithm to use
     */
    explicit Compressor(const Algorithm algorithm);

    /**
     * @brief Compresses the input data using the specified algorithm
     *
     * @param data Input data to compress
     * @param level Compression level (1 = fastest; 10 = smallest; default = 3)
     * @return std::vector<uint8_t> Compressed data
     * @throws std::runtime_error if the compression fails
     */
    std::vector<uint8_t> Compress(const std::vector<uint8_t>& data, int level = 3);

    /**
     * @brief Compresses the input data using the specified algorithm
     *
     * @param data Pointer to the input data
     * @param size Size of the input data
     * @param level Compression level (1 = fastest; 10 = smallest; default = 3)
     * @return std::vector<uint8_t>
     * @throws std::runtime_error if the compression fails
     */
    std::vector<uint8_t> Compress(const uint8_t* data, size_t size, int level = 3);

    /**
     * @brief Decompresses the input data using the specified algorithm
     *
     * @param data Input data to decompress
     * @return std::vector<uint8_t> Decompressed data
     * @throws std::runtime_error if the decompression fails
     */
    std::vector<uint8_t> Decompress(const std::vector<uint8_t>& data);

    /**
     * @brief Decompresses the input data using the specified algorithm
     *
     * @param data Pointer to the input data to decompress
     * @param size Size of the input data
     * @return std::vector<uint8_t> Decompressed data
     * @throws std::runtime_error if the decompression fails
     */
    std::vector<uint8_t> Decompress(const uint8_t* data, size_t size);

   private:
    Algorithm algorithm_;
};

}  // namespace compression_utils

#endif  // COMPRESSION_UTILS_HPP_