# Compress Utils - C++ API

`compress-utils` aims to simplify data compression by offering a unified interface for various algorithms and languages, while maintaining best-in-class performance. 

These docs cover the C++ binding (not really a binding, per se, as the original library is written in C++).

The API is simple and universal across all [available algorithms](/README.md#built-in-compression-algorithms). In C++, there are two flavors:

- [Object-Oriented (OOP)](#oop-api)
- [Functional](#functional-api)

## Table of Contents

- [Installation](#installation)
- [Available Algorithms](#available-algorithms)
- [OOP API](#oop-api)
    - [Setup](#setup)
        - [Includes](#includes)
        - [Selecting Algorithm](#selecting-algorithm)
        - [Creating a Compressor Object](#creating-a-compressor-object)
    - [Compression](#compression)
        - [From a Vector](#from-a-vector)
        - [From a Pointer](#from-a-pointer)
    - [Decompression](#decompression)
        - [From a Vector](#from-a-vector-1)
        - [From a Pointer](#from-a-pointer-1)
- [Functional API](#functional-api)
    - [Setup](#setup-1)
        - [Includes](#includes-1)
        - [Selecting Algorithm](#selecting-algorithm-1)
    - [Compression](#compression-1)
        - [From a Vector](#from-a-vector-2)
        - [From a Pointer](#from-a-pointer-2)
    - [Decompression](#decompression-1)
        - [From a Vector](#from-a-vector-3)
        - [From a Pointer](#from-a-pointer-3)
        
## Installation

_TBD_
## Available Algorithms

The following compression algorithms are available (depending on your build configuration):

- **brotli**
- **lzma**
- **xz**
- **zlib**
- **zstd**

To check which algorithms are available in your installation, you check the `algorithms.hpp` header:

```cpp
/**
 * @brief Enum class that defines the available compression algorithms
 */
enum class Algorithm {
    BROTLI,
    LZMA,
    XZ,
    ZLIB,
    ZSTD
};
```

## OOP API

### Setup

#### Includes

To use the OOP API, include the main header:

```cpp
#include "compress_utils.hpp"
```

#### Selecting Algorithm

Before constructing the `Compressor` class, you must select a compression algorithm from the `Algorithms` enum:

```cpp
// Select algorithm
compress_utils::Algorithm algorithm = compress_utils::Algorithms::ZSTD;
```

#### Creating a Compressor Object

To create a `Compressor` object, simply pass the algorithm:

```cpp
// Create Compressor object
compress_utils::Compressor compressor(algorithm);
```

For conciseness, this can also be done inline with algorithm selection:

```cpp
// Create Compressor object
compress_utils::Compressor compressor(compress_utils::Algorithms::ZSTD);
```

### Compression

#### From a Vector

To compress data from a `std::vector<uint8_t>` you can call `Compress()` via:

```cpp
// Compress data from a vector
std::vector<uint8_t> compressed_data = compressor.Compress(data);
```

You can also set a compression level, between 1-10:

```cpp
// Compress data with a compression level (1-10)
int level = 5;
std::vector<uint8_t> compressed_data = compressor.Compress(data, level);
```

#### From a Pointer

You can also compress data from a pointer & size integer, to avoid copying the original data if it's not in a `std::vector`:

```cpp
// Compress data from a pointer & size
std::vector<uint8_t> compressed_data = compressor.Compress(data_ptr, data_size);
```

Similarly, you can also set a compression level:

```cpp
// Compress data from a pointer & size with a compression level (1-10)
int level = 5;
std::vector<uint8_t> compressed_data = compress_utils::Compress(data, algorithm, level);
```

### Decompression

#### From a Vector

To decompress data from a `std::vector<uint8_t>` you can call `Decompress()` via:

```cpp
// Decompress data
std::vector<uint8_t> decompressed_data = compressor.Decompress(compressed_data);
```

#### From a Pointer

Decompression is also available by passing a pointer and size:

```cpp
// Decompress data from a pointer & size
std::vector<uint8_t> compressed_data = compressor.Decompress(data_ptr, data_size);
```

## Functional API

### Setup

#### Includes

To use the functional API, include the functional header:

```cpp
#include "compress_utils_func.hpp"
```

#### Selecting Algorithm

Before calling `Compress()` or `Decompress()`, you must select a compression algorithm from the `Algorithms` enum:

```cpp
// Select algorithm
compress_utils::Algorithm algorithm = compress_utils::Algorithms::ZSTD;
```

### Compression

#### From a Vector

To compress data from a `std::vector<uint8_t>` you can call `Compress()` via:

```cpp
// Compress data from a vector
std::vector<uint8_t> compressed_data = compress_utils::Compress(data, algorithm);
```

You can also set a compression level, between 1-10:

```cpp
// Compress data from a vector with a compression level (1-10)
int level = 5;
std::vector<uint8_t> compressed_data = compress_utils::Compress(data, algorithm, level);
```

#### From a Pointer

You can also compress data from a pointer & size integer, to avoid copying the original data if it's not in a `std::vector`:

```cpp
// Compress data from a pointer & size
std::vector<uint8_t> compressed_data = compress_utils::Compress(data_ptr, data_size, algorithm);
```

Similarly, you can also set a compression level:

```cpp
// Compress data from a pointer & size with a compression level (1-10)
int level = 5;
std::vector<uint8_t> compressed_data = compress_utils::Compress(data, algorithm, level);
```

### Decompression

#### From a Vector

To decompress data from a `std::vector<uint8_t>` you can call `Decompress()` via:

```cpp
// Decompress data from a vector
std::vector<uint8_t> decompressed_data = compress_utils::Decompress(compressed_data, algorithm);
```

#### From a Pointer

Decompression is also available by passing a pointer and size:

```cpp
// Decompress data from a pointer & size
std::vector<uint8_t> compressed_data = compress_utils::Decompress(data_ptr, data_size, algorithm);
```