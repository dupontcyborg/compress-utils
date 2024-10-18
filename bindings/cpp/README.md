# Compression Utils - C++ API

## Basic Usage - OOP

```cpp
#include "compression_utils.hpp"

// Select algorithm
compression_utils::Algorithm algorithm = compression_utils::Algorithms::ZSTD;

// Create Compressor object
compression_utils::Compressor compressor(algorithm);

// Compress data
std::vector<uint8_t> compressed_data = compressor.Compress(data);

// Compress data with a compression level (1-10)
std::vector<uint8_t> compressed_data = compressor.Compress(data, 5);

// Decompress data
std::vector<uint8_t> decompressed_data = compressor.Decompress(compressed_data);
```

## Basic Usage - Functional

```cpp
#include "compression_utils_func.hpp"

// Select algorithm
compression_utils::Algorithm algorithm = compression_utils::Algorithms::ZSTD;

// Compress data
std::vector<uint8_t> compressed_data = compression_utils::Compress(data, algorithm);

// Compress data with a compression level (1-10)
std::vector<uint8_t> compressed_data = compression_utils::Compress(data, algorithm, 5);

// Decompress data
std::vector<uint8_t> decompressed_data = compression_utils::Decompress(compressed_data, algorithm);
```