# Compress Utils - C API

`compress-utils` aims to simplify data compression by offering a unified interface for various algorithms and languages, while maintaining best-in-class performance. 

These docs cover the C binding. The API is simple and universal across all [available algorithms](/README.md#built-in-compression-algorithms).

## Table of Contents

- [Usage](#usage)
    - [Setup](#setup)
        - [Includes](#includes)
        - [Selecting Algorithm](#selecting-algorithm)
    - [Compression](#compression)
    - [Decompression](#decompression)

## Usage

### Setup

#### Includes

The entire `compress-utils` library is available through a single header:

```c
#include "compress_utils.h"
```

### Selecting Algorithm

Before calling `compress()` or `decompress()`, you must select a compression algorithm from the `Algorithms` enum:

```c
// Select algorithm
Algorithm algorithm = ZSTD;
```

### Compression

To compress data from a `uint8_t*` pointer, you can call `compress()` via:

```c
// Compress data
uint8_t* comp_data = NULL;
int level = 3;  // Compression level: 1 (fastest) to 10 (smallest)
int64_t comp_size = compress(data, data_size, &comp_data, algorithm, level);

// Check if compression succeeded
if (comp_size == -1) {
    // Handle compression error
}

// Clean up
free(comp_data);
```

Note that `compress()` will allocate memory at the `comp_data` pointer, so be sure to free that memory when you've finished using it.

### Decompression

To decompress data from a `uint8_t*` pointer, you can call `decompress()` via:

```c
// Decompress data
uint8_t* decomp_data = NULL;
int64_t decomp_size = decompress(comp_data, comp_size, &decomp_data, algorithm);

// Check if decompression succeeded
if (decomp_size == -1) {
    // Handle decompression error
}

free(decomp_data);
```

Note that `decompress()` will allocate memory at the `decomp_data` pointer, so be sure to free that memory when you've finished using it.