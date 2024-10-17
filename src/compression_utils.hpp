#ifndef COMPRESSION_UTILS_HPP_
#define COMPRESSION_UTILS_HPP_

#include "algorithms.hpp"

#include <string>
#include <vector>

// Platform-specific macros for exporting and importing symbols
#if defined(_WIN32) || defined(_WIN64)
    #if defined(COMPRESSION_UTILS_EXPORTS)
        #define COMPRESSION_API __declspec(dllexport)  // Export when building the DLL
    #else
        #define COMPRESSION_API __declspec(dllimport)  // Import when using the DLL
    #endif
#else
    #define COMPRESSION_API __attribute__((visibility("default")))  // For non-Windows platforms (Linux/macOS)
#endif

namespace compression_utils {

// OOP Interface

/**
 * @brief Compressor class that provides compression and decompression functionalities
 *
 * The class provides two methods, Compress and Decompress, that can be used to compress and
 * decompress
 */
class COMPRESSION_API Compressor {
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
     */
    std::vector<uint8_t> Compress(const std::vector<uint8_t>& data, int level = 3);

    /**
     * @brief Decompresses the input data using the specified algorithm
     *
     * @param data Input data to decompress
     * @return std::vector<uint8_t> Decompressed data
     */
    std::vector<uint8_t> Decompress(const std::vector<uint8_t>& data);

   private:
    Algorithm algorithm_;
};

}  // namespace compression_utils

#endif  // COMPRESSION_UTILS_HPP_