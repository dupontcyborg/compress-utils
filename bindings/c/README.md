# Compression Utils - C API

`compression-utils` aims to simplify data compression by offering a unified interface for various algorithms and languages, while maintaining best-in-class performance. 

These docs cover the C binding.

## Basic Usage

```c
#include "compression_utils.h"

// Select algorithm
Algorithm algorithm = ZSTD;

// Compress data
uint8_t* comp_data = NULL;
int level = 3;  // Compression level: 1 (fastest) to 10 (smallest)
int64_t comp_size = compress(data, data_size, &comp_data, algorithm, level);

// Check if compression succeeded
if (comp_size == -1) {
    // Handle compression error
}

// Decompress data
uint8_t* decompressed_data = NULL;
int64_t decompressed_size = decompress(comp_data, comp_size, &decompressed_data, algorithm);

// Check if decompression succeeded
if (decompressed_size == -1) {
    // Handle decompression error
}

// Clean up
free(comp_data);
free(decompressed_data);
```